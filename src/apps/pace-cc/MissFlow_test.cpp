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

[[nodiscard]] AdmitTicket Grant(Wire::GrantKind kind, std::uint64_t queuedMs = 0)
{
    return AdmitTicket { .kind = kind,
                         .endpoint = kind == Wire::GrantKind::Worker ? "worker:1" : "",
                         .leaseToken = kind == Wire::GrantKind::Worker ? "token" : "",
                         .workerCodecs = {},
                         .chargedBytes = 1'000,
                         .queuedMs = queuedMs,
                         .receivedAt = {} };
}

/// The seams of one miss, each recording itself into `events`.
struct Harness
{
    Events events;
    /// One script per opening; an opening past the scripts yields no session.
    std::deque<std::deque<std::expected<AdmitTicket, std::string>>> openings;
    std::vector<FakeSession*> sessions; ///< The sessions handed out, while they live.
    std::optional<int> storedFetch;     ///< What `serveStored` answers.
    std::deque<bool> workerRefuses;     ///< Per admitted dispatch: whether the worker refuses.
    std::optional<std::uint64_t> peak { 4'096 };
    bool stores { true };             ///< Whether the object reaches the cache.
    int compileExit { 0 };            ///< What the compiler said.
    bool legacyDispatches {};         ///< Whether today's lease dispatch produces a compile.
    bool admittedDispatchDeclines {}; ///< A worker that answers nothing the queue answers.

    [[nodiscard]] MissSeams Seams()
    {
        return MissSeams {
            .open = [this]() -> std::unique_ptr<IAdmitSession> {
                events.log.push_back("open");
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
                    events.log.push_back("fetch");
                    return storedFetch;
                },
            .dispatchAdmitted =
                [this](AdmitTicket const&) {
                    events.log.push_back("dispatch-admitted");
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
                events.log.push_back("dispatch-unadmitted");
                if (!legacyDispatches)
                    return std::nullopt;
                return CompileRun { .exitCode = compileExit, .out = {}, .err = {} };
            },
            .compileLocal =
                [this]() {
                    events.log.push_back("compile");
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
                        events.log.push_back("store");
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
          == std::vector<std::string> { "open", "s0:await", "compile", "store", "s0:settle", "s0:closed" });
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
    harness.storedFetch = 0;

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
          == std::vector<std::string> { "open", "s0:await", "dispatch-admitted", "store", "s0:settle", "s0:closed" });
    // No local compile, so no peak: the worker's own measurement is its to report.
    CHECK_FALSE(report.peakMemoryBytes.has_value());
}

TEST_CASE("Launcher.worker-grant-the-worker-did-not-run-compiles-here-holding-the-grant", "[launcher][admission]")
{
    // An unreachable worker, a refused argument, a crossed reply: nothing a queue answers, so the
    // compile runs here, as it always has -- and the grant is still held until it is stored.
    Harness harness;
    harness.Open({ Grant(Wire::GrantKind::Worker) });
    harness.admittedDispatchDeclines = true;

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.log
          == std::vector<std::string> {
              "open", "s0:await", "dispatch-admitted", "compile", "store", "s0:settle", "s0:closed" });
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

TEST_CASE("Launcher.a-queue-that_never-converges-fails-open-after-a-bound", "[launcher][admission]")
{
    Harness harness;
    std::deque<std::expected<AdmitTicket, std::string>> grants;
    for ([[maybe_unused]] auto const i: std::views::iota(std::size_t { 0 }, MaxWorkerRefusals + 2))
    {
        grants.push_back(Grant(Wire::GrantKind::Worker));
        harness.workerRefuses.push_back(true);
    }
    harness.Open(std::move(grants));

    auto const report = RunMiss(harness.Seams());

    CHECK(report.code == 0);
    CHECK(harness.events.Count("dispatch-admitted") == MaxWorkerRefusals + 1);
    CHECK(harness.events.Count("s0:requeue") == MaxWorkerRefusals);
    CHECK(harness.events.Count("compile") == 1); // the launcher's own stop, in the open
    CHECK_FALSE(harness.events.Saw("s0:settle"));
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
    CHECK(TreeLabelOf("/work/src/build", "/work/\xFF") == UnnamedTreeLabel);
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
