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
// The learner's relay is dialled plainly, as `AdmitForwarder_test.cpp` dials a loopback leader. The PROVEN leg
// -- the same relay presenting its machine's key to a leader that admits nobody for being loopback, and the
// leader charging the forwarding machine -- is `AdmitForwarder.a-proven-relay-charges-the-forwarding-machine`
// there; it needs a leader behind the node-proof surface, which these cases do not stand up.
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
#include <functional>
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
        _budget { machineBudgetBytes }
    {
    }

    /// What the grants of one case added up to, read after every launcher has been joined.
    struct Tally
    {
        std::uint64_t peakShared {}; ///< The most the compiles sharing the machine held at once.
        std::size_t maxDepth {};     ///< The deepest the leader's queue was seen.
        std::size_t leaderViolations {};
        std::size_t grants {};
        std::size_t violations {};
        std::size_t ranAlone {};
    };

    /// @return The totals so far; read them only once every launcher has been joined.
    [[nodiscard]] Tally Totals() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _totals;
    }

    /// @param view The leader's own view of its queue, so a grant is judged against what admission decided.
    void SetLeaderView(std::function<Distributed::AdmissionSnapshot()> view)
    {
        _leaderView = std::move(view);
    }

    /// Count one compiler spawned.
    void NoteSpawn() noexcept
    {
        ++_spawns;
    }

    /// @return How many compilers were spawned.
    [[nodiscard]] int Spawns() const noexcept
    {
        return _spawns.load();
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
    ///
    /// Judged twice, because each reading can miss what the other sees: this side's own tally of the grants
    /// its launchers hold, and -- when `leaderView` is set -- the LEADER's ledger as it stands now, which is
    /// what admission actually decided against. A leader that overcommits shows in the second even if the
    /// launchers' tally happens to be late.
    /// @param charged What the queue charged for it.
    void Granted(std::uint64_t charged)
    {
        // The leader is read before this side's lock is taken: it has a lock of its own.
        std::uint64_t leaderCharged = 0;
        std::size_t depth = 0;
        auto const read = static_cast<bool>(_leaderView);
        if (read)
        {
            auto const view = _leaderView();
            depth = view.depth;
            for (auto const& [machine, bytes]: view.chargedBytes)
                leaderCharged += bytes;
        }
        auto const guard = std::scoped_lock { _mutex };
        ++_totals.grants;
        auto const alone = charged > _budget;
        // A request estimated above the whole _budget runs ALONE: the one grant allowed to overshoot, and
        // only when nothing else is running beside it.
        if (alone ? _inUse != 0 : _inUse + charged > _budget)
            ++_totals.violations;
        if (read)
        {
            _totals.maxDepth = std::max(_totals.maxDepth, depth);
            // Alone means the leader charges exactly this; anything else is over _budget with more than one grant held.
            if (alone ? leaderCharged != charged : (leaderCharged > _budget && !_aloneHeld))
                ++_totals.leaderViolations;
        }
        _inUse += charged;
        if (alone)
        {
            ++_totals.ranAlone;
            _aloneHeld = true;
        }
        else if (!_aloneHeld)
            // What the compiles that share the machine held at once. The run-alone grant is left out: it is
            // bigger than any figure this is asked to exceed, and would satisfy it by itself.
            _totals.peakShared = std::max(_totals.peakShared, _inUse);
    }

    /// A compile and its STORE are done; the charge goes back at the next SETTLE.
    /// @param charged What it held.
    void Finished(std::uint64_t charged)
    {
        auto const guard = std::scoped_lock { _mutex };
        _inUse -= charged;
        if (charged > _budget)
            _aloneHeld = false;
    }

  private:
    std::uint64_t const _budget;
    /// The leader's own view of its queue; unset when a case has no leader to ask.
    std::function<Distributed::AdmissionSnapshot()> _leaderView;
    std::atomic<int> _spawns { 0 };
    Tally _totals {}; ///< Written under the lock, read after every launcher has been joined.
    mutable std::mutex _mutex;
    std::set<std::string> _objects;
    std::uint64_t _inUse {};
    bool _aloneHeld {};
};

/// What one launcher did, read on the case's thread after it is joined.
struct Launch
{
    bool servedFromCache {}; ///< The object was there before it asked, or a `Stored` grant said it would be.
    std::optional<int> code;
    std::optional<std::uint64_t> queuedMs;
    std::vector<std::string> failedOpen; ///< Why each admission failed open, in order.
};

/// How a scripted compile behaves.
struct Plan
{
    std::chrono::milliseconds duration { 0 }; ///< How long the "compiler" runs.
    std::optional<std::uint64_t> peak {};     ///< The peak it measures, which teaches the queue.
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
                held = ticket.chargedBytes;
                world.Granted(held);
            },
        .dispatchAdmitted = [](Cc::AdmitTicket const&) { return Cc::AdmittedDispatch {}; },
        .dispatchUnadmitted = []() -> std::optional<Cc::CompileRun> { return std::nullopt; },
        .compileLocal =
            [&] {
                world.NoteSpawn();
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
    CHECK(world.Spawns() == 1);
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
    CHECK(world.Spawns() == 1);
    // The LEADER queued and granted it; the learner decided nothing, and dialled exactly the leader.
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionGrantedLocal) == 1);
    CHECK(learner.metrics.Read(IMetricsSink::Counter::AdmissionQueued) == 0);
    CHECK(learner.dialer.Dialled() == std::vector<std::string> { EndpointOf(leaderPort) });
    // The settle crossed the relay too: the leader holds nothing for a compile that is done.
    CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
    CHECK(WaitFor([&] { return leader.held.Held() == HeldCharges {}; }));
}

namespace
{
/// A machine with an 8 GiB budget and a queue that has been taught what two compile sizes cost: one needing
/// most of the machine, and requests needing all of it.
struct BigCompiles
{
    /// @param first The key taught as 6 GiB, which a case takes a grant for.
    /// @param whole The key taught as 8 GiB, which fits only on an idle machine.
    BigCompiles(std::string_view first, std::string_view whole):
        port { queue.Serve() }
    {
        queue.Report(8 * GiB, 8);
        Teach(port, first, 6 * GiB);
        Teach(port, whole, 8 * GiB);
    }

    /// @param key The object key.
    /// @return A session asking for it, as a launcher does.
    [[nodiscard]] std::unique_ptr<Cc::IAdmitSession> Open(std::string_view key) const
    {
        auto const request = Wire::AdmitRequest { .kind = Wire::AdmitKind::LocalOnly,
                                                  .fingerprint = {},
                                                  .cacheKey = key,
                                                  .sourcePath = "src/a.cpp",
                                                  .treeLabel = "tree",
                                                  .storable = false,
                                                  .acceptedCodecs = {},
                                                  .excluded = {},
                                                  .toolchainLabel = {} };
        return Cc::OpenAdmitSession(EndpointOf(port), Cc::Credential {}, request, Cc::DefaultAdmitBudget());
    }

    /// Ask for the whole budget on a plain connection, read bounded.
    /// @param key The key taught as 8 GiB.
    /// @return The grant.
    [[nodiscard]] Wire::AdmitGrantFields AskForAll(std::string_view key) const
    {
        Client whole { port };
        whole.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, key));
        return GrantOf(Unwrap(whole.ReadAnswer()));
    }

    Queue queue;
    std::uint16_t port;
};
} // namespace

TEST_CASE("FleetAdmission.a-client-that-settles-gives-the-budget-back", "[node][admission][fleet]")
{
    // The control for the case below: the same sequence with a SETTLE, which must pass whatever is done to
    // the release a dead client relies on.
    BigCompiles machine { "big-settled", "whole-1" };

    auto session = machine.Open("big-settled");
    auto const ticket = Unwrap(session->Await());
    REQUIRE(ticket.chargedBytes == 6 * GiB);
    session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::NotStored, .peakBytes = 6 * GiB });
    REQUIRE(WaitFor([&] { return machine.queue.service.AdmissionSnapshotNow().chargedBytes.empty(); }));

    auto const grant = machine.AskForAll("whole-1");
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(grant.chargedBytes == 8 * GiB);
}

TEST_CASE("FleetAdmission.killed-client-does-not-shrink-budget", "[node][admission][fleet]")
{
    BigCompiles machine { "big-killed", "whole-2" };

    auto session = machine.Open("big-killed");
    auto const ticket = Unwrap(session->Await());
    REQUIRE(ticket.chargedBytes == 6 * GiB);
    REQUIRE(machine.queue.service.AdmissionSnapshotNow().chargedBytes.size() == 1);
    // `kill -9`: the connection ends, and no SETTLE was ever sent.
    session.reset();

    // A request that needs the whole budget fits only once that charge is gone; read bounded, so a
    // release that never happened is a red case and not a hung one.
    auto const grant = machine.AskForAll("whole-2");
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(grant.chargedBytes == 8 * GiB);
    CHECK(Count(machine.queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);
}

TEST_CASE("FleetAdmission.node-stopped-behaves-as-today", "[node][admission][fleet]")
{
    // Nothing listens at this endpoint: the node is stopped.
    auto const endpoint = EndpointOf(FreePort());
    World world { 8 * GiB };

    auto const result = Invoke(world, endpoint, "no-node-object", Plan { .duration = 10ms });

    // The compile ran here, was stored, and the exit code is the compiler's; nothing was ever queued.
    CHECK(result.code == 0);
    CHECK(world.Spawns() == 1);
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
    world.SetLeaderView([&] { return queue.service.AdmissionSnapshotNow(); });
    auto const endpoint = EndpointOf(port);

    // Forty keys whose measured peaks are 1..5 GiB, and one more that is larger than the machine.
    // Collected by hand: clang over libstdc++ 14 cannot pipe into `std::ranges::to` (see `ReachabilityMemo.cpp`).
    std::vector<std::string> keys;
    keys.reserve(Compiles);
    for (auto const index: std::views::iota(std::size_t { 0 }, Compiles))
        keys.push_back(std::format("object-{}", index));
    auto const peakOf = [](std::size_t index) -> std::uint64_t {
        return index == 17 ? 11 * GiB : (1 + ((index * 7) % 5)) * GiB;
    };
    for (auto const index: std::views::iota(std::size_t { 0 }, Compiles))
        Teach(port, keys[index], peakOf(index));

    auto const results = RunTogether(Compiles, [&](std::size_t index) {
        return Invoke(world, endpoint, keys[index], Plan { .duration = 20ms, .peak = peakOf(index) });
    });

    CHECK(world.Totals().grants == Compiles);
    CHECK(world.Spawns() == static_cast<int>(Compiles));
    CHECK(std::ranges::all_of(results, [](Launch const& one) { return one.code == 0 && one.failedOpen.empty(); }));
    // The invariant, read two ways at every grant: what the launchers hold adds up to no more than the budget,
    // and so does what the LEADER has charged -- bar the one grant estimated above the whole budget, which
    // runs alone and which the leader must be charging by itself.
    CHECK(world.Totals().violations == 0);
    CHECK(world.Totals().leaderViolations == 0);
    CHECK(world.Totals().ranAlone == 1);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionRanAlone) == 1);
    // Not vacuous. No single compile is above 5 GiB, so sharing the machine up to 6 GiB or more means at
    // least two ran at once -- and the run-alone grant is not counted towards it. And the leader had
    // requests waiting behind the ones it granted.
    CHECK(world.Totals().peakShared >= 6 * GiB);
    CHECK(world.Totals().maxDepth > 0);
    CHECK(WaitFor([&] { return queue.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
}
