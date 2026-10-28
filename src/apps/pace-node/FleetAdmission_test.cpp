// SPDX-License-Identifier: Apache-2.0
//
// The fleet compile queue end to end (#1728): the launcher's own miss flow (`Cc::RunMiss` over the
// real `Cc::OpenAdmitSession`) against the node's real admission surface, over real loopback sockets.
//
// What is real: the launcher's session and miss flow, the framing on both ends, the node's
// `AdmissionResponder` and `AdmitForwarder`, the scheduler's queue and its estimates, and the
// connection that IS the lease. What is not: there is no compiler (a compile is a counted sleep) and no
// worker -- every request here is `LocalOnly`, so a grant places the compile on the asking machine --
// and the cache is a set of keys the launchers share. Placement on a worker is covered by the queue's
// own cases (`AdmissionQueue.*`, `AdmissionResponder.dispatchable-*`); what these cases pin is the part
// that only exists between a launcher and a node: that the SEQUENCE the two halves implement is the one
// the rules say.
//
// `FleetHarness` is not used: it replays a request into a scheduler's protocol object through a
// scripted socket, which cannot hold a connection open for the length of a compile, and holding it IS
// the property here (R5).
#include "AdmissionTestUtils.hpp"
#include "AdmitSession.hpp"
#include "MissFlow.hpp"

#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <CacheProtocol.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using namespace Pace::Node::AdmissionFixture;
using namespace std::chrono_literals;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

constexpr std::uint64_t GiB = 1024ULL * MiB;

/// What the launchers of one case share: the object cache, the number of compilers spawned, and a
/// watch over the memory the grants they hold add up to.
struct World
{
    /// @param machineBudgetBytes What the fleet's one machine states as its budget.
    explicit World(std::uint64_t machineBudgetBytes):
        budget { machineBudgetBytes }
    {
    }

    /// @param key An object key.
    /// @return Whether an earlier compile stored it.
    [[nodiscard]] bool Has(std::string const& key) const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _objects.contains(key);
    }

    /// @param key The object a compile just stored.
    void Put(std::string const& key)
    {
        auto const guard = std::scoped_lock { _mutex };
        _objects.insert(key);
    }

    /// A grant arrived: the compile it names is about to run.
    /// @param charged What the queue charged for it.
    void Granted(std::uint64_t charged)
    {
        auto const guard = std::scoped_lock { _mutex };
        ++grants;
        // A request estimated above the whole budget runs ALONE: the one grant allowed to overshoot, and
        // only when nothing else is running beside it.
        if (charged > budget ? _inUse != 0 : _inUse + charged > budget)
            ++violations;
        _inUse += charged;
        peakInUse = std::max(peakInUse, _inUse);
        if (charged > budget)
            ++ranAlone;
    }

    /// A compile and its STORE are done; the charge goes back at the next SETTLE.
    /// @param charged What it held.
    void Finished(std::uint64_t charged)
    {
        auto const guard = std::scoped_lock { _mutex };
        _inUse -= charged;
    }

    std::uint64_t const budget;
    std::atomic<int> spawns { 0 };
    // Written under the lock, read after every launcher has been joined.
    std::uint64_t peakInUse {};
    std::size_t grants {};
    std::size_t violations {};
    std::size_t ranAlone {};

  private:
    mutable std::mutex _mutex;
    std::set<std::string> _objects;
    std::uint64_t _inUse {};
};

/// What one launcher did, read on the case's thread after it is joined.
struct Launch
{
    bool servedFromCache {}; ///< The object was there before it asked, or a `Stored` grant said it would be.
    std::optional<int> code;
    std::optional<std::uint64_t> queuedMs;
    std::vector<std::string> failedOpen; ///< Why each admission failed open, in order.
    /// How long this launcher waited for its first grant, on ITS OWN steady clock: the leader's queue runs on a
    /// manual clock here, so its `queuedMs` is always zero and says nothing about whether a request waited.
    std::chrono::milliseconds waited { 0 };
};

/// How a scripted compile behaves.
struct Plan
{
    std::chrono::milliseconds duration { 0 }; ///< How long the "compiler" runs.
    std::optional<std::uint64_t> peak;        ///< The peak it measures, which teaches the queue.
};

/// One launcher's cache miss: the FETCH it makes first, then the miss flow as `main.cpp` runs it.
///
/// Asserts nothing, for it runs on a thread of its own.
/// @param world The shared cache, counters and watch.
/// @param endpoint The node's `0xFC` endpoint the queue is asked at.
/// @param key The object this invocation compiles.
/// @param plan How its compiler behaves.
[[nodiscard]] Launch Invoke(World& world, std::string const& endpoint, std::string const& key, Plan plan = {})
{
    Launch out;
    auto const began = std::chrono::steady_clock::now();
    if (world.Has(key))
    {
        out.servedFromCache = true;
        out.code = 0;
        return out;
    }
    auto const request = Wire::AdmitRequest { .kind = Wire::AdmitKind::LocalOnly,
                                              .fingerprint = {},
                                              .cacheKey = key,
                                              .sourcePath = key,
                                              .treeLabel = "tree",
                                              .storable = true,
                                              .acceptedCodecs = {},
                                              .excluded = {},
                                              .toolchainLabel = {} };
    std::uint64_t held = 0;
    auto const seams = Cc::MissSeams {
        .open = [&](std::optional<std::string_view>) -> std::unique_ptr<Cc::IAdmitSession> {
            return Cc::OpenAdmitSession(endpoint, Cc::Credential {}, request, Cc::DefaultAdmitBudget());
        },
        .serveStored =
            [&] {
                if (!world.Has(key))
                    return Cc::StoredFetch { .kind = Cc::StoredFetchKind::Missed, .code = 0 };
                out.servedFromCache = true;
                return Cc::StoredFetch { .kind = Cc::StoredFetchKind::Served, .code = 0 };
            },
        .granted =
            [&](Cc::AdmitTicket const& ticket) {
                if (ticket.kind == Wire::GrantKind::Stored)
                    return;
                if (out.waited == 0ms)
                    out.waited =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
                held = ticket.chargedBytes;
                world.Granted(held);
            },
        .dispatchAdmitted = [](Cc::AdmitTicket const&) { return Cc::AdmittedDispatch {}; },
        .dispatchUnadmitted = []() -> std::optional<Cc::CompileRun> { return std::nullopt; },
        .compileLocal =
            [&] {
                ++world.spawns;
                std::this_thread::sleep_for(plan.duration);
                Cc::CompileRun run { .exitCode = 0, .out = {}, .err = {} };
                run.peakMemoryBytes = plan.peak;
                return run;
            },
        .finish =
            [&](Cc::CompileRun&, bool& stored) {
                world.Put(key);
                stored = true;
                // The STORE is visible; the SETTLE that follows is what hands the charge back.
                world.Finished(std::exchange(held, 0));
                return 0;
            },
        .failedOpen = [&](std::string const& reason) { out.failedOpen.push_back(reason); },
    };
    auto const report = Cc::RunMiss(seams);
    out.code = report.code;
    out.queuedMs = report.queuedMs;
    return out;
}

/// Run @p count launchers at once and collect what each did. Every thread is joined before this returns.
/// @param count How many.
/// @param launch What launcher `i` does; asserts nothing.
/// @return Their results, by index.
template <typename Fn>
[[nodiscard]] std::vector<Launch> RunTogether(std::size_t count, Fn const& launch)
{
    std::vector<Launch> results(count);
    std::latch go { 1 };
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        threads.emplace_back([&, index] {
            go.wait();
            results[index] = launch(index);
        });
    go.count_down();
    for (auto& thread: threads)
        thread.join();
    return results;
}

/// The node teaches the queue what a key costs, as a launcher whose compile measured it would: one
/// admission, settled `Failed` with the peak.
/// @param port The leader's port. @param key The object key. @param peak The measured peak.
void Teach(std::uint16_t port, std::string_view key, std::uint64_t peak)
{
    Client client { port };
    // Its own source path: the queue's estimate for a key it has not seen falls back to the SHAPE of its
    // source, so forty teachers sharing one path would teach each other's figures to the next.
    client.Send(Wire::EncodeAdmit(Wire::AdmitRequest { .kind = Wire::AdmitKind::LocalOnly,
                                                       .fingerprint = {},
                                                       .cacheKey = key,
                                                       .sourcePath = key,
                                                       .treeLabel = "tree",
                                                       .storable = false,
                                                       .acceptedCodecs = {},
                                                       .excluded = {},
                                                       .toolchainLabel = {} }));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    client.Send(SettleFrame(Wire::SettleOutcome::Failed, peak));
    REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
}

} // namespace

TEST_CASE("FleetAdmission.six-identical-requests-compile-once", "[node][admission][fleet]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(8 * GiB, 16);
    World world { 8 * GiB };
    auto const endpoint = EndpointOf(port);

    auto const results =
        RunTogether(6, [&](std::size_t) { return Invoke(world, endpoint, "same-object", Plan { .duration = 400ms }); });

    // One compiler ran; the other five were told it would be stored, or found it stored, and fetched it.
    CHECK(world.spawns.load() == 1);
    CHECK(std::ranges::count_if(results, [](Launch const& one) { return one.servedFromCache; }) == 5);
    CHECK(std::ranges::all_of(results, [](Launch const& one) { return one.code == 0; }));
    // Nothing failed open: every one of them was answered by the queue.
    CHECK(std::ranges::all_of(results, [](Launch const& one) { return one.failedOpen.empty(); }));
    // At least one of them waited on the compile in flight -- or the case proved nothing about the queue --
    // and each wait was counted as the dedup it was.
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionDeduplicated) >= 1);
    CHECK(WaitFor([&] { return queue.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
}

TEST_CASE("FleetAdmission.learner-is-queued-through-its-own-node", "[node][admission][fleet][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    leader.Report(8 * GiB, 8);
    // A node that runs no scheduler: it relays its launchers' admissions to the leader over one connection.
    Follower learner { EndpointOf(leaderPort) };
    auto const learnerPort = learner.Serve();
    World world { 8 * GiB };

    // The launcher is configured with the learner's endpoint ALONE; it has never heard of the leader.
    auto const result = Invoke(world, EndpointOf(learnerPort), "learner-object", Plan { .duration = 50ms });

    CHECK(result.code == 0);
    CHECK(result.failedOpen.empty());
    CHECK(result.queuedMs.has_value());
    CHECK(world.spawns.load() == 1);
    // The LEADER queued and granted it; the learner decided nothing, and dialled exactly the leader.
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionGrantedLocal) == 1);
    CHECK(learner.metrics.Read(IMetricsSink::Counter::AdmissionQueued) == 0);
    CHECK(learner.dialer.Dialled() == std::vector<std::string> { EndpointOf(leaderPort) });
    // The settle crossed the relay too: the leader holds nothing for a compile that is done.
    CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
    CHECK(WaitFor([&] { return leader.held.Held() == HeldCharges {}; }));
}

TEST_CASE("FleetAdmission.killed-client-does-not-shrink-budget", "[node][admission][fleet]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(8 * GiB, 8);
    auto const endpoint = EndpointOf(port);
    // A compile that needs most of the machine, and requests that need all of it.
    Teach(port, "big-settled", 6 * GiB);
    Teach(port, "big-killed", 6 * GiB);
    Teach(port, "whole-1", 8 * GiB);
    Teach(port, "whole-2", 8 * GiB);

    auto const request = [](std::string_view key) {
        return Wire::AdmitRequest { .kind = Wire::AdmitKind::LocalOnly,
                                    .fingerprint = {},
                                    .cacheKey = key,
                                    .sourcePath = "src/a.cpp",
                                    .treeLabel = "tree",
                                    .storable = false,
                                    .acceptedCodecs = {},
                                    .excluded = {},
                                    .toolchainLabel = {} };
    };

    SECTION("control: a client that settles gives the budget back")
    {
        auto session = Cc::OpenAdmitSession(endpoint, Cc::Credential {}, request("big-settled"), Cc::DefaultAdmitBudget());
        auto const ticket = Unwrap(session->Await());
        REQUIRE(ticket.chargedBytes == 6 * GiB);
        session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::NotStored, .peakBytes = 6 * GiB });
        REQUIRE(WaitFor([&] { return queue.service.AdmissionSnapshotNow().chargedBytes.empty(); }));

        Client whole { port };
        whole.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "whole-1"));
        auto const grant = GrantOf(Unwrap(whole.ReadAnswer()));
        CHECK(grant.kind == Wire::GrantKind::Local);
        CHECK(grant.chargedBytes == 8 * GiB);
    }

    SECTION("a client killed holding a grant gives the budget back")
    {
        auto session = Cc::OpenAdmitSession(endpoint, Cc::Credential {}, request("big-killed"), Cc::DefaultAdmitBudget());
        auto const ticket = Unwrap(session->Await());
        REQUIRE(ticket.chargedBytes == 6 * GiB);
        REQUIRE(queue.service.AdmissionSnapshotNow().chargedBytes.size() == 1);
        // `kill -9`: the connection ends, and no SETTLE was ever sent.
        session.reset();

        // A request that needs the whole budget fits only once that charge is gone; read bounded, so a
        // release that never happened is a red case and not a hung one.
        Client whole { port };
        whole.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "whole-2"));
        auto const grant = GrantOf(Unwrap(whole.ReadAnswer()));
        CHECK(grant.kind == Wire::GrantKind::Local);
        CHECK(grant.chargedBytes == 8 * GiB);
        CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);
    }
}

TEST_CASE("FleetAdmission.node-stopped-behaves-as-today", "[node][admission][fleet]")
{
    // Nothing listens at this endpoint: the node is stopped.
    auto const endpoint = EndpointOf(FreePort());
    World world { 8 * GiB };

    auto const result = Invoke(world, endpoint, "no-node-object", Plan { .duration = 10ms });

    // The compile ran here, was stored, and the exit code is the compiler's; nothing was ever queued.
    CHECK(result.code == 0);
    CHECK(world.spawns.load() == 1);
    CHECK(world.Has("no-node-object"));
    CHECK_FALSE(result.queuedMs.has_value());
    // And the launcher said why: the one reason a node that is down is remembered by.
    CHECK(result.failedOpen == std::vector<std::string> { Cc::AdmitUnreachedReason() });
}

TEST_CASE("FleetAdmission.memory-never-exceeds-budget", "[node][admission][fleet]")
{
    constexpr std::size_t Compiles = 40;
    constexpr std::uint64_t Budget = 8 * GiB;
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(Budget, 64);
    World world { Budget };
    auto const endpoint = EndpointOf(port);

    // Forty keys whose measured peaks are 1..5 GiB, and one more that is larger than the machine.
    auto const keys = std::views::iota(std::size_t { 0 }, Compiles)
                      | std::views::transform([](std::size_t index) { return std::format("object-{}", index); })
                      | std::ranges::to<std::vector>();
    auto const peakOf = [](std::size_t index) -> std::uint64_t {
        return index == 17 ? 11 * GiB : (1 + (index * 7) % 5) * GiB;
    };
    for (auto const index: std::views::iota(std::size_t { 0 }, Compiles))
        Teach(port, keys[index], peakOf(index));

    auto const results = RunTogether(Compiles, [&](std::size_t index) {
        return Invoke(world, endpoint, keys[index], Plan { .duration = 20ms, .peak = peakOf(index) });
    });

    CHECK(world.grants == Compiles);
    CHECK(world.spawns.load() == static_cast<int>(Compiles));
    CHECK(std::ranges::all_of(results, [](Launch const& one) { return one.code == 0 && one.failedOpen.empty(); }));
    // The invariant: at every grant, what the grants held added up to no more than the budget, bar one
    // that is estimated above the whole of it and so runs alone.
    CHECK(world.violations == 0);
    CHECK(world.ranAlone == 1);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionRanAlone) == 1);
    // The figures are not vacuous: the machine was loaded to a real fraction of its budget, and most of
    // the forty had to wait their turn for it.
    CHECK(world.peakInUse >= 5 * GiB);
    CHECK(std::ranges::count_if(results, [](Launch const& one) { return one.waited >= 15ms; }) >= 10);
    CHECK(WaitFor([&] { return queue.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
}
