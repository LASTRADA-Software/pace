// SPDX-License-Identifier: Apache-2.0
//
// Where a cache miss compiles, and the order in which the queue is told (#1728).
//
// Against a fake `IAdmitSession` and recording seams: what is asserted is which steps ran, in what
// ORDER and how many times -- the STORE before the SETTLE, the compile never run for a deduplicated
// request, the connection held across the compile -- none of which a real socket would make easier
// to read.
#include "MissFlow.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using Pace::Testing::Unwrap;

using namespace Pace;
using namespace Pace::Cc;
namespace Wire = Pace::CompileCacheWire;

namespace
{
/// The events one miss produced, in order. Every seam and the session write here, so ORDER is a
/// property of one list rather than of several that a case would have to interleave.
struct Events
{
    std::vector<std::string> log;
    std::vector<Wire::SettleRequest> settles; ///< What every session was told, in order.

    /// @return Whether @p event happened at least once.
    [[nodiscard]] bool Saw(std::string const& event) const
    {
        return std::ranges::find(log, event) != log.end();
    }
    /// @return How many times @p event happened.
    [[nodiscard]] std::size_t Count(std::string const& event) const
    {
        return static_cast<std::size_t>(std::ranges::count(log, event));
    }
    /// @return The index of the first @p event, or the log's size.
    [[nodiscard]] std::size_t At(std::string const& event) const
    {
        return static_cast<std::size_t>(std::ranges::find(log, event) - log.begin());
    }
};

/// A session answering from a script, recording what it was asked and when it ended.
class FakeSession final: public IAdmitSession
{
  public:
    FakeSession(Events& events, std::deque<std::expected<AdmitTicket, std::string>> grants, std::string name):
        _events { &events },
        _grants { std::move(grants) },
        _name { std::move(name) }
    {
    }
    ~FakeSession() override
    {
        _events->log.push_back(_name + ":closed");
    }
    FakeSession(FakeSession const&) = delete;
    FakeSession(FakeSession&&) = delete;
    FakeSession& operator=(FakeSession const&) = delete;
    FakeSession& operator=(FakeSession&&) = delete;

    [[nodiscard]] std::expected<AdmitTicket, std::string> Await() override
    {
        _events->log.push_back(_name + ":await");
        return Next();
    }
    [[nodiscard]] std::expected<AdmitTicket, std::string> Requeue() override
    {
        _events->log.push_back(_name + ":requeue");
        return Next();
    }
    void Settle(Wire::SettleRequest const& settle) override
    {
        _events->log.push_back(_name + ":settle");
        _events->settles.push_back(settle);
    }

  private:
    [[nodiscard]] std::expected<AdmitTicket, std::string> Next()
    {
        if (_grants.empty())
            return std::unexpected { std::string { "the script ran out" } };
        auto next = std::move(_grants.front());
        _grants.pop_front();
        return next;
    }

    Events* _events;
    std::deque<std::expected<AdmitTicket, std::string>> _grants;
    std::string _name;
};

[[nodiscard]] AdmitTicket Grant(Wire::GrantKind kind,
                                std::uint64_t queuedMs = 0,
                                Wire::LocalGrantReason reason = Wire::LocalGrantReason::Asked)
{
    return AdmitTicket { .kind = kind,
                         .endpoint = kind == Wire::GrantKind::Worker ? "worker:1" : "",
                         .leaseToken = kind == Wire::GrantKind::Worker ? "token" : "",
                         .workerCodecs = {},
                         .chargedBytes = 1'000,
                         .queuedMs = queuedMs,
                         .lifetime = {},
                         .workerKey = {},
                         .localReason = reason,
                         .dialHint = {},
                         .receivedAt = {} };
}

/// The seams of one miss, each recording itself into `events`.
struct Harness
{
    Events events;
    /// One script per opening; an opening past the scripts yields no session.
    std::deque<std::deque<std::expected<AdmitTicket, std::string>>> openings;
    std::vector<FakeSession*> sessions; ///< The sessions handed out, while they live.
    /// What each `open` was asked about: the failed worker, or nothing for the request as described.
    std::vector<std::optional<std::string>> asked;
    /// What `serveStored` answers.
    StoredFetch storedFetch { .kind = StoredFetchKind::Missed, .code = 0 };
    /// Every grant the launcher was told of, in order.
    std::vector<AdmitTicket> granted;
    std::deque<bool> workerRefuses; ///< Per admitted dispatch: whether the worker refuses.
    std::optional<std::uint64_t> peak { 4'096 };
    bool stores { true };             ///< Whether the object reaches the cache.
    int compileExit { 0 };            ///< What the compiler said.
    bool legacyDispatches {};         ///< Whether today's lease dispatch produces a compile.
    bool admittedDispatchDeclines {}; ///< A worker that answers nothing the queue answers.

    [[nodiscard]] MissSeams Seams()
    {
        return MissSeams {
            .open = [this](std::optional<std::string_view> failedWorker) -> std::unique_ptr<IAdmitSession> {
                events.log.emplace_back("open");
                asked.push_back(failedWorker.has_value() ? std::optional<std::string> { *failedWorker } : std::nullopt);
                if (openings.empty())
                    return nullptr;
                auto script = std::move(openings.front());
                openings.pop_front();
                auto session =
                    std::make_unique<FakeSession>(events, std::move(script), "s" + std::to_string(sessions.size()));
                sessions.push_back(session.get());
                return session;
            },
            .serveStored =
                [this]() {
                    events.log.emplace_back("fetch");
                    return storedFetch;
                },
            .granted =
                [this](AdmitTicket const& ticket) {
                    events.log.emplace_back("granted");
                    granted.push_back(ticket);
                },
            .dispatchAdmitted =
                [this](AdmitTicket const&) {
                    events.log.emplace_back("dispatch-admitted");
                    AdmittedDispatch out;
                    if (!workerRefuses.empty())
                    {
                        out.workerRefused = workerRefuses.front();
                        workerRefuses.pop_front();
                    }
                    if (!out.workerRefused && !admittedDispatchDeclines)
                        out.run = CompileRun { .exitCode = compileExit, .out = {}, .err = {} };
                    return out;
                },
            .dispatchUnadmitted = [this]() -> std::optional<CompileRun> {
                events.log.emplace_back("dispatch-unadmitted");
                if (!legacyDispatches)
                    return std::nullopt;
                return CompileRun { .exitCode = compileExit, .out = {}, .err = {} };
            },
            .compileLocal =
                [this]() {
                    events.log.emplace_back("compile");
                    CompileRun run { .exitCode = compileExit, .out = {}, .err = {} };
                    run.peakMemoryBytes = peak;
                    return run;
                },
            .finish =
                [this](CompileRun&, bool& stored) {
                    // The STORE happens here, inside `finish`, and the case reads WHERE the settle
                    // landed relative to it.
                    if (stores && compileExit == 0)
                    {
                        events.log.emplace_back("store");
                        stored = true;
                    }
                    return compileExit;
                },
            .failedOpen = {},
        };
    }

    /// Queue a session that answers with @p grants.
    void Open(std::deque<std::expected<AdmitTicket, std::string>> grants)
    {
        openings.push_back(std::move(grants));
    }
};
} // namespace

TEST_CASE("Launcher.fail-open-is-todays-path", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ std::unexpected { std::string { "no leader" } } });
    harness.legacyDispatches = false;

    auto const report = RunMiss(harness.Seams());

    // Exactly what a launcher with no queue does: the lease dispatch is tried, then the compiler
    // runs here, then the object is stored -- and the queue is told nothing.
    CHECK(report.code == 0);
    CHECK_FALSE(report.queuedMs.has_value());
    CHECK(harness.events.Count("compile") == 1);
    CHECK(harness.events.Count("dispatch-unadmitted") == 1);
    CHECK(harness.events.Count("store") == 1);
    CHECK_FALSE(harness.events.Saw("s0:settle"));
    CHECK(harness.events.Count("dispatch-admitted") == 0);
    // The session is gone BEFORE the compile runs in the open, so a leader that failed us is not
    // left holding a connection for the length of a compile nobody admitted.
    CHECK(harness.events.At("s0:closed") < harness.events.At("compile"));
}

TEST_CASE("Launcher.no-session-is-todays-path", "[launcher][admission]")
{
    // Nothing configured to ask, or a node this invocation already knows is unreachable: no
    // admission is opened at all, and the compile runs exactly as it did before.
    Harness harness;
    harness.legacyDispatches = true;

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.Count("open") == 1);
    CHECK(harness.events.Count("compile") == 0); // the legacy dispatch compiled it
    CHECK(harness.events.Count("dispatch-unadmitted") == 1);
    CHECK_FALSE(harness.events.Saw("s0:await"));
}

TEST_CASE("Launcher.local-grant-compiles-then-stores-then-settles", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Local, 750) });

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(report.queuedMs == std::optional<std::uint64_t> { 750 });
    CHECK(report.peakMemoryBytes == std::optional<std::uint64_t> { 4'096 });
    REQUIRE(harness.sessions.size() == 1);
    // The order is the assertion: the STORE is visible before the queue wakes the compiles waiting on
    // this key (R1), and the connection is held across the whole compile (the connection is the lease).
    CHECK(harness.events.log
          == std::vector<std::string> { "open", "s0:await", "granted", "compile", "store", "s0:settle", "s0:closed" });
}

TEST_CASE("Launcher.settle-carries-how-the-compile-ended-and-its-peak", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Local) });
    auto expected = Wire::SettleOutcome::Stored;

    SECTION("stored")
    {
        expected = Wire::SettleOutcome::Stored;
    }
    SECTION("nothing was stored")
    {
        harness.stores = false;
        expected = Wire::SettleOutcome::NotStored;
    }
    SECTION("the compile failed")
    {
        harness.compileExit = 1;
        expected = Wire::SettleOutcome::Failed;
    }

    auto const report = RunMiss(harness.Seams());

    REQUIRE(harness.events.settles.size() == 1);
    CHECK(harness.events.settles.front().outcome == expected);
    // Always the measured peak when there is one, a failed compile's included.
    CHECK(harness.events.settles.front().peakBytes == std::optional<std::uint64_t> { 4'096 });
    CHECK(report.peakMemoryBytes == std::optional<std::uint64_t> { 4'096 });
}

TEST_CASE("Launcher.settle-states-no-peak-when-none-was-measured", "[launcher][admission]")
{
    // Absent is not zero: a compile that measured nothing must not tell the queue it used nothing.
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Local) });
    harness.peak = std::nullopt;

    (void) RunMiss(harness.Seams());

    REQUIRE(harness.events.settles.size() == 1);
    CHECK_FALSE(harness.events.settles.front().peakBytes.has_value());
}

TEST_CASE("Launcher.stored-grant-fetches-and-skips-compile", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Stored, 300) });
    harness.storedFetch = StoredFetch { .kind = StoredFetchKind::Served, .code = 0 };

    auto const report = RunMiss(harness.Seams());

    // The cached-hit path ran, so there is no compile, no dispatch and no STORE.
    CHECK(report.code == 0);
    CHECK(report.queuedMs == std::optional<std::uint64_t> { 300 });
    CHECK(harness.events.Count("fetch") == 1);
    CHECK(harness.events.Count("compile") == 0);
    CHECK(harness.events.Count("dispatch-admitted") == 0);
    CHECK(harness.events.Count("dispatch-unadmitted") == 0);
    CHECK(harness.events.Count("store") == 0);
}

TEST_CASE("Launcher.stored-grant-miss-requeues-once", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Stored) });
    harness.Open({ Grant(Wire::GrantKind::Local) });

    auto const report = RunMiss(harness.Seams());

    // The first session is destroyed and the flow runs once more from the start; the second grant
    // compiles.
    CHECK(report.code == 0);
    CHECK(harness.events.Count("open") == 2);
    CHECK(harness.events.Count("fetch") == 1);
    CHECK(harness.events.Count("compile") == 1);
    CHECK(harness.events.At("s0:closed") < harness.events.At("s1:await"));
    CHECK(harness.events.Saw("s1:settle"));
    CHECK_FALSE(harness.events.Saw("s0:settle"));
}

TEST_CASE("Launcher.queued-time-sums-every-admission-of-the-invocation", "[launcher][admission]")
{
    Harness harness;
    std::optional<std::uint64_t> expected;

    SECTION("a stored miss and the opening after it")
    {
        harness.Open({ Grant(Wire::GrantKind::Stored, 300) });
        harness.Open({ Grant(Wire::GrantKind::Local, 750) });
        expected = 1'050;
    }
    SECTION("a requeue on one connection is the leader's one measurement, never counted twice")
    {
        // The leader measures a requeued request from its first enqueue, so the later grant already
        // holds the wait before it.
        harness.Open({ Grant(Wire::GrantKind::Worker, 100), Grant(Wire::GrantKind::Worker, 400) });
        harness.workerRefuses = { true, false };
        expected = 400;
    }
    SECTION("a stored miss that then fails open keeps the wait it had")
    {
        harness.Open({ Grant(Wire::GrantKind::Stored, 300) });
        harness.Open({ std::unexpected { std::string { "no leader" } } });
        expected = 300;
    }

    auto const report = RunMiss(harness.Seams());

    CHECK(report.queuedMs == expected);
}

TEST_CASE("Launcher.a-refetch-the-cache-settles-as-a-plain-compile-takes-that-path", "[launcher][admission]")
{
    // The FETCH after a `Stored` grant met a hit this machine cannot write: the cache decided, with no
    // STORE. That is not a miss, so it is never asked about again; the caller takes today's plain path.
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Stored, 300) });
    harness.Open({ Grant(Wire::GrantKind::Local) });
    harness.storedFetch = StoredFetch { .kind = StoredFetchKind::Declined, .code = 0 };

    auto const report = RunMiss(harness.Seams());

    CHECK_FALSE(report.code.has_value());
    CHECK(report.queuedMs == std::optional<std::uint64_t> { 300 });
    CHECK(harness.events.Count("open") == 1);
    CHECK(harness.events.Count("compile") == 0);
    CHECK(harness.events.Count("dispatch-unadmitted") == 0);
    CHECK(harness.events.Count("store") == 0);
}

TEST_CASE("Launcher.a-second-stored-miss-fails-open", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Stored) });
    harness.Open({ Grant(Wire::GrantKind::Stored) });
    // A third opening would compile, and must never be reached.
    harness.Open({ Grant(Wire::GrantKind::Local) });

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.Count("open") == 2);
    CHECK(harness.events.Count("fetch") == 2);
    CHECK(harness.events.Count("dispatch-unadmitted") == 1);
    CHECK(harness.events.Count("compile") == 1);
    CHECK_FALSE(harness.events.Saw("s2:await"));
    CHECK_FALSE(harness.events.Saw("s1:settle"));
}

TEST_CASE("Launcher.worker-grant-dispatches-then-stores-then-settles", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Worker) });

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.log
          == std::vector<std::string> {
              "open", "s0:await", "granted", "dispatch-admitted", "store", "s0:settle", "s0:closed" });
    // No local compile, so no peak: the worker's own measurement is its to report.
    CHECK_FALSE(report.peakMemoryBytes.has_value());
}

TEST_CASE("Launcher.a-worker-that-failed-the-compile-gives-its-grant-back-and-asks-local-only", "[launcher][admission]")
{
    // An unreachable worker, a failed dispatch preprocess, a discarded result: nothing a queue answers.
    // The worker grant charged THAT machine, which is not running the compile, so the session is settled
    // NotStored and closed -- freeing the charge without teaching the estimator a kill -- and a
    // local-only admission excluding that worker holds the compile here.
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Worker) });
    harness.Open({ Grant(Wire::GrantKind::Local) });
    harness.admittedDispatchDeclines = true;

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.log
          == std::vector<std::string> { "open",
                                        "s0:await",
                                        "granted",
                                        "dispatch-admitted",
                                        "s0:settle",
                                        "s0:closed",
                                        "open",
                                        "s1:await",
                                        "granted",
                                        "compile",
                                        "store",
                                        "s1:settle",
                                        "s1:closed" });
    // The worker's grant is settled NotStored with no peak: freed, and no measurement claimed.
    REQUIRE(harness.events.settles.size() == 2);
    CHECK(harness.events.settles[0].outcome == Wire::SettleOutcome::NotStored);
    CHECK_FALSE(harness.events.settles[0].peakBytes.has_value());
    REQUIRE(harness.asked.size() == 2);
    CHECK_FALSE(harness.asked[0].has_value());
    CHECK(harness.asked[1] == std::optional<std::string> { "worker:1" });
    // Both grants were told; the second asked to run here, so it carries no refusal to record.
    REQUIRE(harness.granted.size() == 2);
    CHECK(harness.granted[1].kind == Wire::GrantKind::Local);
    CHECK(harness.granted[1].localReason == Wire::LocalGrantReason::Asked);
}

TEST_CASE("Launcher.a-worker-failure-with-no-local-grant-to-be-had-compiles-here-unadmitted", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Worker) });
    harness.admittedDispatchDeclines = true;
    std::size_t expectedOpens = 2;

    SECTION("nothing to ask")
    {
        // No second script: the second opening yields no session.
    }
    SECTION("the second admission fails open")
    {
        harness.Open({ std::unexpected { std::string { "connection lost" } } });
    }
    SECTION("the second admission is answered stored and the object is not there")
    {
        harness.Open({ Grant(Wire::GrantKind::Stored) });
    }

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.Count("open") == expectedOpens);
    CHECK(harness.events.Count("compile") == 1);
    // Never the fleet a second way: the worker's failure was this compile's dispatch.
    CHECK(harness.events.Count("dispatch-unadmitted") == 0);
    // The failed worker's grant is settled before its session closes; nothing else is.
    REQUIRE(harness.events.Saw("s0:settle"));
    CHECK(harness.events.At("s0:settle") < harness.events.At("s0:closed"));
    CHECK(harness.events.settles.front().outcome == Wire::SettleOutcome::NotStored);
    CHECK_FALSE(harness.events.Saw("s1:settle"));
    // Every session ended before the compile ran outside it.
    CHECK(harness.events.At("s0:closed") < harness.events.At("compile"));
    if (harness.events.Saw("s1:closed"))
        CHECK(harness.events.At("s1:closed") < harness.events.At("compile"));
}

TEST_CASE("Launcher.a-local-grant-is-told-to-the-launcher-before-the-compile", "[launcher][admission]")
{
    // Ruling 11: a dispatchable request the fleet placed nowhere is recorded as the fleet's refusal.
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Local, 0, Wire::LocalGrantReason::NoWorker) });

    (void) RunMiss(harness.Seams());

    REQUIRE(harness.granted.size() == 1);
    CHECK(harness.granted[0].localReason == Wire::LocalGrantReason::NoWorker);
    CHECK(harness.events.At("granted") < harness.events.At("compile"));
}

TEST_CASE("Launcher.a-queue-that-never-answered-is-told-no-grant", "[launcher][admission]")
{
    // Ruling 12's cost, held to its truth: with no scheduler named, a refusal this launcher made before
    // asking is recorded only once a queue GRANTS something. A node that refuses the verb -- one too old
    // to know it (`unimplemented-verb`), or a cache daemon -- grants nothing, so nothing is recorded and
    // the dispatch axis stays `NotConfigured`.
    Harness harness;
    harness.Open({ std::unexpected { std::string { "rejected (unimplemented-verb): this build serves no admission" } } });
    std::vector<std::string> recorded;
    auto seams = harness.Seams();
    std::optional<std::string> pending { "not dispatchable (an argument no worker passes); compiling locally" };
    seams.granted = [&](AdmitTicket const&) {
        if (pending.has_value())
            recorded.push_back(*std::exchange(pending, std::nullopt));
    };

    auto const report = RunMiss(seams);

    CHECK(report.code == 0);
    CHECK(harness.events.Count("compile") == 1);
    CHECK(recorded.empty());
    CHECK(pending.has_value());

    SECTION("control: a queue that grants is told, once, before the compile")
    {
        Harness granting;
        granting.Open({ Grant(Wire::GrantKind::Stored), Grant(Wire::GrantKind::Local) });
        granting.Open({ Grant(Wire::GrantKind::Local) });
        auto grantingSeams = granting.Seams();
        auto const told = grantingSeams.granted;
        grantingSeams.granted = [&](AdmitTicket const& ticket) {
            told(ticket);
            if (pending.has_value())
                recorded.push_back(*std::exchange(pending, std::nullopt));
        };

        (void) RunMiss(grantingSeams);

        CHECK(recorded.size() == 1);
        // Every grant is told, the `Stored` one included -- a queue that answered at all.
        CHECK(granting.granted.size() == 2);
        CHECK(granting.events.At("granted") < granting.events.At("compile"));
    }
}

TEST_CASE("Launcher.a-local-grant-reads-as-the-refusal-the-lease-path-records", "[launcher][admission]")
{
    // The wording `dist-compile-e2e` asserts, and the tally the lease path moves for the same fleet.
    auto const noWorker = ExplainLocalGrant(Wire::LocalGrantReason::NoWorker, "gcc 14.2.0");
    REQUIRE(noWorker.has_value());
    CHECK(Unwrap(noWorker).line.starts_with(
        "not dispatched (rejected (no-worker): no worker serves this toolchain (gcc 14.2.0)"));
    CHECK(Unwrap(noWorker).line.ends_with("; compiling locally"));
    CHECK(Unwrap(noWorker).recording.outcome == RecordingFor(DispatchStatus::Declined, DeclineCause::NoToolchain).outcome);
    CHECK(Unwrap(noWorker).recording.reason == RecordingFor(DispatchStatus::Declined, DeclineCause::NoToolchain).reason);

    auto const excluded = ExplainLocalGrant(Wire::LocalGrantReason::AllExcluded, {});
    REQUIRE(excluded.has_value());
    CHECK(Unwrap(excluded).line.starts_with("not dispatched (rejected (no-worker): every worker serving this toolchain is"));
    CHECK(Unwrap(excluded).recording.reason == RecordingFor(DispatchStatus::Declined, DeclineCause::NoToolchain).reason);

    auto const busy = ExplainLocalGrant(Wire::LocalGrantReason::NoRoom, "clang 19");
    REQUIRE(busy.has_value());
    CHECK(Unwrap(busy).line.starts_with("not dispatched (rejected (no-capacity):"));
    CHECK(Unwrap(busy).recording.reason == RecordingFor(DispatchStatus::Declined, DeclineCause::NoCapacity).reason);
    // Two different tallies: a fleet that is too small is not a fleet with nobody on the toolchain.
    CHECK(Unwrap(busy).recording.reason != Unwrap(noWorker).recording.reason);

    // A request that asked to run here was refused nothing.
    CHECK_FALSE(ExplainLocalGrant(Wire::LocalGrantReason::Asked, "gcc").has_value());
    CHECK_FALSE(ExplainLocalGrant(Wire::LocalGrantReason::Last, "gcc").has_value());
}

TEST_CASE("Launcher.worker-refusal-settles-and-requeues", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Worker), Grant(Wire::GrantKind::Worker) });
    harness.workerRefuses = { true, false };

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    // One connection: the refusal goes back into the queue on it (`Requeue`, which sends the
    // SETTLE{WorkerRefused} and asks again), and the second grant dispatches and settles.
    CHECK(harness.events.Count("open") == 1);
    CHECK(harness.events.Count("s0:requeue") == 1);
    CHECK(harness.events.Count("dispatch-admitted") == 2);
    CHECK(harness.events.Count("compile") == 0); // never a compile in the open after a refusal
    CHECK(harness.events.Count("s0:settle") == 1);
    CHECK(harness.events.At("s0:requeue") < harness.events.At("store"));
}

TEST_CASE("Launcher.a-queue-that-never-converges-runs-here-charged-after-a-bound", "[launcher][admission]")
{
    // The launcher's own stop is not a fail-open: the queue answered every time, so the compile it now
    // runs here is asked for local-only and charged here, never run in the open with nothing charged.
    Harness harness;
    std::deque<std::expected<AdmitTicket, std::string>> grants;
    for ([[maybe_unused]] auto const i: std::views::iota(std::size_t { 0 }, MaxWorkerRefusals + 2))
    {
        grants.emplace_back(Grant(Wire::GrantKind::Worker));
        harness.workerRefuses.push_back(true);
    }
    harness.Open(std::move(grants));
    harness.Open({ Grant(Wire::GrantKind::Local) });

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.Count("dispatch-admitted") == MaxWorkerRefusals + 1);
    CHECK(harness.events.Count("s0:requeue") == MaxWorkerRefusals);
    CHECK(harness.events.Count("compile") == 1);
    CHECK(harness.events.Count("dispatch-unadmitted") == 0);
    // The last worker grant is settled before its session ends, and the compile runs under the second.
    REQUIRE(harness.events.Saw("s0:settle"));
    CHECK(harness.events.At("s0:settle") < harness.events.At("s0:closed"));
    CHECK(harness.events.settles.front().outcome == Wire::SettleOutcome::NotStored);
    CHECK(harness.events.At("s1:await") < harness.events.At("compile"));
    CHECK(harness.events.At("compile") < harness.events.At("s1:settle"));
    REQUIRE(harness.asked.size() == 2);
    CHECK(harness.asked[1] == std::optional<std::string> { "worker:1" });
}

TEST_CASE("Launcher.a-session-that-fails-while-requeueing-fails-open", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Worker), std::unexpected { std::string { "connection lost" } } });
    harness.workerRefuses = { true };

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.Count("compile") == 1);
    CHECK_FALSE(harness.events.Saw("s0:settle"));
}

TEST_CASE("Launcher.an-exception-ends-the-connection-and-sends-no-settle", "[launcher][admission]")
{
    // A launcher that did not finish is an abandonment, and the leader reads the connection's end as
    // exactly that: a SETTLE here would tell it the compile ended well.
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Local) });
    auto seams = harness.Seams();
    seams.finish = [](CompileRun&, bool&) -> int {
        throw std::runtime_error { "the store blew up" };
    };

    CHECK_THROWS_AS(RunMiss(seams), std::runtime_error);

    CHECK_FALSE(harness.events.Saw("s0:settle"));
    CHECK(harness.events.Saw("s0:closed"));
}

TEST_CASE("Launcher.settle-outcomes", "[launcher][admission]")
{
    using Wire::SettleOutcome;
    CHECK(SettleOutcomeOf(0, true) == SettleOutcome::Stored);
    CHECK(SettleOutcomeOf(0, false) == SettleOutcome::NotStored);
    CHECK(SettleOutcomeOf(1, true) == SettleOutcome::Failed);
    CHECK(SettleOutcomeOf(1, false) == SettleOutcome::Failed);
}

TEST_CASE("Launcher.a-stopped-node-adds-no-admit-dial-after-a-failed-fetch", "[launcher][admission]")
{
    // "A node that is stopped is exactly as before": the FETCH to this endpoint just learned it, and a
    // refused connect on Windows can take the whole connect budget -- per translation unit.
    constexpr std::string_view node = "127.0.0.1:6674";

    // Not served: a transport failure, and a node that answered by REFUSING us (it is there, and the
    // queue behind it would refuse the same way).
    CHECK(DecideAsk(node, node, CacheOutcomeKind::Transport, false) == AskVerdict::FetchNotServed);
    CHECK(DecideAsk(node, node, CacheOutcomeKind::Rejected, false) == AskVerdict::FetchNotServed);
    // The control: a node that served the fetch is asked.
    CHECK(DecideAsk(node, node, CacheOutcomeKind::Miss, false) == AskVerdict::Ask);
    CHECK(DecideAsk(node, node, CacheOutcomeKind::Hit, false) == AskVerdict::Ask);
    // A scheduler elsewhere says nothing about the cache's node, so the fetch is no evidence about it.
    CHECK(DecideAsk("sched:6675", node, CacheOutcomeKind::Transport, false) == AskVerdict::Ask);
    // No fetch at all (an uncacheable compile) is no evidence either.
    CHECK(DecideAsk(node, node, std::nullopt, false) == AskVerdict::Ask);
    // What a launcher a moment ago learned.
    CHECK(DecideAsk(node, node, std::nullopt, true) == AskVerdict::RememberedDown);
    CHECK(DecideAsk("sched:6675", node, CacheOutcomeKind::Miss, true) == AskVerdict::RememberedDown);
    // Nothing configured.
    CHECK(DecideAsk({}, node, CacheOutcomeKind::Miss, false) == AskVerdict::NoEndpoint);
}

namespace
{
/// A memo store kept in memory: what one launcher process writes, the next one reads.
class MemoryMemoStore final: public IMemoStore
{
  public:
    [[nodiscard]] std::optional<std::string> Read() const override
    {
        return _text;
    }
    bool Write(std::string_view text) override
    {
        _text = std::string { text };
        return true;
    }

  private:
    std::optional<std::string> _text;
};
} // namespace

TEST_CASE("Launcher.a-passthrough-after-an-unreached-fetch-adds-no-admit-dial", "[launcher][admission]")
{
    // A stopped node must cost an uncacheable compile what it did before a queue existed. Such a
    // compile makes no FETCH of its own, so what it knows is what the build's last FETCH left behind.
    constexpr std::string_view node = "127.0.0.1:6674";
    auto const now = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };
    MemoryMemoStore store;

    // One launcher: its FETCH reached nothing at the node the queue is asked at.
    auto fetching = ReachabilityMemo::Load(store);
    auto noted = RememberUnreachedFetch(fetching, node, node, CacheOutcomeKind::Transport, TransportFailure::Unreached, now);
    fetching.SaveIfChanged(store);

    // The next one -- a fresh process -- runs a passthrough: no FETCH, only the memo.
    auto const passthrough = ReachabilityMemo::Load(store);
    std::size_t dials = 0;
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Local) });
    auto seams = harness.Seams();
    auto const dial = seams.open;
    seams.open = [&](std::optional<std::string_view> failedWorker) -> std::unique_ptr<IAdmitSession> {
        if (DecideAskFrom(node, node, std::nullopt, passthrough, now + std::chrono::seconds { 1 }) != AskVerdict::Ask)
            return nullptr;
        ++dials;
        return dial(failedWorker);
    };

    auto const report = RunMiss(seams);

    CHECK(noted);
    CHECK(dials == 0);
    CHECK(report.code == 0);
    CHECK(harness.events.Count("compile") == 1);
    CHECK_FALSE(report.queuedMs.has_value());
}

TEST_CASE("Launcher.only-a-fetch-that-reached-nothing-at-the-queue-endpoint-is-remembered", "[launcher][admission]")
{
    constexpr std::string_view node = "127.0.0.1:6674";
    auto const now = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };
    auto const remembers = [&](std::string_view endpoint, CacheOutcomeKind kind, TransportFailure failure) {
        auto memo = ReachabilityMemo::Parse({});
        auto const noted = RememberUnreachedFetch(memo, endpoint, node, kind, failure, now);
        CHECK(noted == memo.Remembers(MemoKind::SchedulerUnreached, endpoint, now));
        return noted;
    };

    CHECK(remembers(node, CacheOutcomeKind::Transport, TransportFailure::Unreached));
    // A peer that was reached is up, whatever then went wrong: the memo errs narrow.
    CHECK_FALSE(remembers(node, CacheOutcomeKind::Transport, TransportFailure::PeerLost));
    CHECK_FALSE(remembers(node, CacheOutcomeKind::Transport, TransportFailure::Expired));
    CHECK_FALSE(remembers(node, CacheOutcomeKind::Rejected, TransportFailure::None));
    CHECK_FALSE(remembers(node, CacheOutcomeKind::Miss, TransportFailure::None));
    // A queue asked elsewhere learns nothing from this FETCH.
    CHECK_FALSE(remembers("sched:6675", CacheOutcomeKind::Transport, TransportFailure::Unreached));
    CHECK_FALSE(remembers({}, CacheOutcomeKind::Transport, TransportFailure::Unreached));
}

TEST_CASE("Launcher.tree-label-is-the-working-directory-under-the-source-root", "[launcher][admission]")
{
    CHECK(TreeLabelOf("/work/src/build/debug", "/work/src") == "build/debug");
    CHECK(TreeLabelOf("/work/src/build/debug", "/work/src/") == "build/debug");
    CHECK(TreeLabelOf("D:\\work\\src\\build", "D:/work/src") == "build");
    CHECK(TreeLabelOf("/work/src", "/work/src") == ".");
    // A sibling that merely shares a prefix is not under the root.
    CHECK(TreeLabelOf("/work/srcother/build", "/work/src") == "/work/srcother/build");
    // Outside the root, or no root: spelled whole.
    CHECK(TreeLabelOf("/elsewhere/build", "/work/src") == "/elsewhere/build");
    CHECK(TreeLabelOf("/elsewhere/build", "") == "/elsewhere/build");
}

TEST_CASE("Launcher.tree-label-never-refuses-a-compile", "[launcher][admission]")
{
    // R14: display only. Text the leader would refuse is replaced, never sent, truncated or repaired.
    CHECK(TreeLabelOf("/work/src/\xC3\x28", "/work/src") == UnnamedTreeLabel);
    // A root that is not text names nothing to be relative to; the directory still names the tree.
    CHECK(TreeLabelOf("/work/src/build", "/work/\xFF") == "/work/src/build");
    CHECK(TreeLabelOf("", "/work/src") == UnnamedTreeLabel);
    auto const tooLong = "/work/src/" + std::string(Wire::MaxAdmitTreeLabelBytes + 1, 'x');
    CHECK(TreeLabelOf(tooLong, "/work/src") == UnnamedTreeLabel);
    // At the bound exactly, it travels.
    auto const atBound = std::string(Wire::MaxAdmitTreeLabelBytes, 'x');
    CHECK(TreeLabelOf("/work/src/" + atBound, "/work/src") == atBound);
}

TEST_CASE("Launcher.a-fail-open-is-reported-to-the-launcher", "[launcher][admission]")
{
    Harness harness;
    harness.Open({ std::unexpected { std::string { "admission unreached" } } });
    std::vector<std::string> told;
    auto seams = harness.Seams();
    seams.failedOpen = [&](std::string const& reason) {
        told.push_back(reason);
    };

    (void) RunMiss(seams);

    CHECK(told == std::vector<std::string> { "admission unreached" });
}
