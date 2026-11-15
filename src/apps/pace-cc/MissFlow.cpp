// SPDX-License-Identifier: Apache-2.0
#include "MissFlow.hpp"

#include <Pace/Core/Utf8.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

namespace Pace::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Why an admission produced no report of its own.
    enum class Unplaced : std::uint8_t
    {
        FailOpen, ///< Compile as this launcher did before a queue existed.
        Reopen,   ///< Ask the queue again from the start.
        Declined, ///< The cache, asked again, settled this as a plain compile with no STORE.
        Last
    };

    /// The worker a grant named failed the compile in a way no queue answers.
    struct WorkerFailed
    {
        std::string endpoint; ///< That worker, by the endpoint it advertises.
    };

    /// The compiler was killed for memory while this invocation's one requeue was unspent, or a worker killed the
    /// requeued one (#1770). The admission that held it has ENDED unsettled: the leader reads that as a kill and
    /// raises the compile's estimate, which is what the next ask is charged.
    struct Killed
    {
        /// The worker whose machine killed it, by the endpoint it advertises; disengaged when it was killed here.
        std::optional<std::string> worker;
    };

    using Placement = std::variant<MissReport, Unplaced, WorkerFailed, Killed>;

    /// The admission waits of one invocation, summed across its openings, and what the leader is told of the
    /// compile when it is asked again.
    ///
    /// Within ONE opening the leader measures from that request's enqueue, so a grant after a requeue
    /// already counts the wait before it: an opening contributes its LAST grant's figure, and
    /// openings add up.
    class Waits
    {
      public:
        /// An opening was granted after waiting @p queuedMs since it was enqueued.
        void Granted(std::uint64_t queuedMs) noexcept
        {
            _opening = queuedMs;
        }

        /// The opening is over: its last grant's wait joins the total.
        void EndOpening() noexcept
        {
            if (_opening.has_value())
                _total = _total.value_or(0) + *_opening;
            _opening.reset();
        }

        /// @return The total; empty when no opening was granted.
        [[nodiscard]] std::optional<std::uint64_t> Total() const noexcept
        {
            return _total;
        }

        /// The leader granted this compile a place to RUN (a worker or here) -- not `Stored`, which runs nothing.
        void CompileGranted() noexcept
        {
            _compileGranted = true;
        }

        /// The compiler was killed for memory: from here on every ask is a requeue after a kill (#1770).
        void CompileKilled() noexcept
        {
            _killed = true;
        }

        /// @return Whether this invocation's one requeue after a kill is spent.
        [[nodiscard]] bool RequeueSpent() const noexcept
        {
            return _killed;
        }

        /// What every later request of this invocation tells the leader: once it granted this compile, asking
        /// again -- the local retry after a granted worker failed, the lease after an admission broke -- is the SAME
        /// compile, and the leader's demand history must not count it twice (`AskOrigin`). Once it was KILLED, the
        /// leader also places it only where its estimate fits a configured budget.
        /// @return `AfterKill` once a compile was killed, else `AfterGrant` once one was granted, else `First`.
        [[nodiscard]] CompileCacheWire::AskOrigin Origin() const noexcept
        {
            if (_killed)
                return CompileCacheWire::AskOrigin::AfterKill;
            return _compileGranted ? CompileCacheWire::AskOrigin::AfterGrant : CompileCacheWire::AskOrigin::First;
        }

      private:
        std::optional<std::uint64_t> _opening;
        std::optional<std::uint64_t> _total;
        bool _compileGranted { false };
        bool _killed { false };
    };

    /// Tell the launcher a kill moved the miss path, when it asked to hear.
    /// @param seams What to run. @param step The step taken.
    void Say(MissSeams const& seams, KillStep step)
    {
        if (seams.killed)
            seams.killed(step);
    }

    /// A session's answer, with the failure told to the launcher and the wait recorded.
    /// @param seams What to run.
    /// @param waits Where a grant's wait is recorded.
    /// @param answer What the session answered.
    /// @return @p answer.
    [[nodiscard]] std::expected<AdmitTicket, std::string> Read(MissSeams const& seams,
                                                               Waits& waits,
                                                               std::expected<AdmitTicket, std::string> answer)
    {
        if (answer.has_value())
        {
            waits.Granted(answer->queuedMs);
            if (answer->kind != CompileCacheWire::GrantKind::Stored)
                waits.CompileGranted();
            seams.granted(*answer);
        }
        else if (seams.failedOpen)
            seams.failedOpen(answer.error());
        return answer;
    }

    /// Finish a compile held under @p session: the STORE first, then `SETTLE` (R1).
    /// @param seams What to run.
    /// @param session The held admission.
    /// @param run The compile.
    /// @return The report; its wait is the caller's to fill in.
    [[nodiscard]] MissReport FinishHeld(MissSeams const& seams, IAdmitSession& session, CompileRun& run)
    {
        bool stored = false;
        auto const code = seams.finish(run, stored);
        // After the STORE and never before, so a compile waiting on this key is woken to an object
        // that is there. An exception out of `finish` skips this line and the session's destructor
        // closes the connection, which the leader reads as an abandonment.
        session.Settle(Wire::SettleRequest { .outcome = SettleOutcomeOf(code, stored), .peakBytes = run.peakMemoryBytes });
        return MissReport { .code = code, .queuedMs = std::nullopt, .peakMemoryBytes = run.peakMemoryBytes };
    }

    /// Finish a compile that ended KILLED, with nothing settled: whatever holds it ends unsettled, which the leader
    /// reads as a kill and so raises the compile's estimate -- where a `Failed` settle would teach it the killed run's
    /// peak, a LOWER bound, as the compile's measure.
    /// @param seams What to run. @param run The killed compile.
    /// @return The report: the compiler's own exit, and no peak.
    [[nodiscard]] MissReport FinishKilled(MissSeams const& seams, CompileRun& run)
    {
        Say(seams, KillStep::LocalKilledOutside);
        run.peakMemoryBytes.reset();
        bool stored = false;
        auto const code = seams.finish(run, stored);
        return MissReport { .code = code, .queuedMs = std::nullopt, .peakMemoryBytes = std::nullopt };
    }

    /// Where a grant runs its compile here: outside the ceiling only when the leader said nothing can hold it.
    /// @param ticket The grant.
    /// @return The placement to ask for.
    [[nodiscard]] LocalPlacement PlacementFor(AdmitTicket const& ticket) noexcept
    {
        return ticket.localReason == Wire::LocalGrantReason::NeverFits ? LocalPlacement::OutsideCeiling
                                                                       : LocalPlacement::UnderCeiling;
    }

    /// Run the compiler here under @p session, which a grant placed here.
    ///
    /// A run killed for memory while the requeue is unspent ends the session UNSETTLED (`Killed`), so the caller
    /// asks once more. Once it is spent, a run killed inside the ceiling runs once more outside it, under the same
    /// grant; one killed outside every ceiling ends killed.
    /// @param seams What to run. @param waits Whether the requeue is spent. @param session The held admission.
    /// @param placement Where the grant lets it run.
    /// @return The report, or `Killed` for the caller to ask again.
    [[nodiscard]] Placement RunHeld(MissSeams const& seams,
                                    Waits const& waits,
                                    IAdmitSession& session,
                                    LocalPlacement placement)
    {
        auto run = seams.compileLocal(placement);
        if (!run.killed.has_value())
            return FinishHeld(seams, session, run);
        if (!waits.RequeueSpent())
            return Killed { .worker = std::nullopt };
        if (run.inCgroup)
        {
            Say(seams, KillStep::LocalOutsideCeiling);
            run = seams.compileLocal(LocalPlacement::OutsideCeiling);
            if (!run.killed.has_value())
                return FinishHeld(seams, session, run);
        }
        return FinishKilled(seams, run);
    }

    /// What the FETCH after a `Stored` grant leads to.
    /// @param fetched How it ended.
    /// @return A report for a served hit, else what to do next.
    [[nodiscard]] Placement AfterStored(StoredFetch const& fetched)
    {
        switch (fetched.kind)
        {
            case StoredFetchKind::Served:
                return MissReport { .code = fetched.code, .queuedMs = std::nullopt, .peakMemoryBytes = std::nullopt };
            case StoredFetchKind::Declined:
                return Unplaced::Declined;
            case StoredFetchKind::Missed:
            case StoredFetchKind::Last:
                break;
        }
        return Unplaced::Reopen;
    }

    /// One opening of the admission, from the ADMIT to the end of the compile.
    ///
    /// The session lives exactly as long as this call, so every way out -- a fail open, a worker that
    /// failed the compile, a kill -- ends the connection before anything runs outside it.
    /// @param seams What to run.
    /// @param waits Where each grant's wait is recorded.
    /// @return A report when the compile ran here, else what to do next.
    [[nodiscard]] Placement Admit(MissSeams const& seams, Waits& waits)
    {
        auto session = seams.open(std::nullopt, waits.Origin());
        if (session == nullptr)
            return Unplaced::FailOpen;
        auto ticket = Read(seams, waits, session->Await());
        for (auto const refusals: std::views::iota(std::size_t { 0 }, MaxWorkerRefusals + 1))
        {
            if (!ticket.has_value())
                return Unplaced::FailOpen;
            switch (ticket->kind)
            {
                case Wire::GrantKind::Stored:
                    // Nothing is held for a deduplicated request, so there is nothing to settle: the
                    // connection ends with the session.
                    return AfterStored(seams.serveStored());
                case Wire::GrantKind::Local:
                    return RunHeld(seams, waits, *session, PlacementFor(*ticket));
                case Wire::GrantKind::Worker: {
                    auto attempt = seams.dispatchAdmitted(*ticket);
                    if (attempt.run.has_value())
                        return FinishHeld(seams, *session, *attempt.run);
                    switch (attempt.setback)
                    {
                        case WorkerSetback::Killed:
                            // Ended UNSETTLED, deliberately: the leader reads the close as a kill and raises this
                            // compile's estimate, which is what its requeue is charged (#1770).
                            return Killed { .worker = ticket->endpoint };
                        case WorkerSetback::Refused:
                            // The launcher's own stop. The queue DID answer, so the compile that now runs
                            // here is asked for as a local one and charged here -- never run in the open,
                            // which is the unbounded compile the queue exists to prevent. The grant is
                            // settled `NotStored` first, as a failed worker's is: it frees that charge and
                            // teaches the estimator nothing, where a bare close would read as a kill.
                            if (refusals == MaxWorkerRefusals)
                            {
                                session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::NotStored,
                                                                      .peakBytes = std::nullopt });
                                return WorkerFailed { .endpoint = ticket->endpoint };
                            }
                            // Back into the queue on the same connection, never a compile in the open:
                            // the leader admitted this against a ledger the worker disagreed with.
                            ticket = Read(seams, waits, session->Requeue());
                            continue;
                        case WorkerSetback::Failed:
                        case WorkerSetback::Last:
                            break;
                    }
                    // The grant charged THAT worker, which is not running the compile: this session
                    // ends here, before anything runs, and frees the charge. SETTLED rather than merely
                    // closed: a close reads as an abandonment, which teaches the leader's estimator this
                    // compile was KILLED -- a fact about the worker's failure, not about the compile.
                    // `NotStored` with no peak frees the charge and teaches nothing. Should the settle
                    // fail, the session's end still abandons, which frees the charge all the same.
                    session->Settle(
                        Wire::SettleRequest { .outcome = Wire::SettleOutcome::NotStored, .peakBytes = std::nullopt });
                    return WorkerFailed { .endpoint = ticket->endpoint };
                }
                case Wire::GrantKind::Last:
                    break;
            }
            return Unplaced::FailOpen;
        }
        return Unplaced::FailOpen;
    }

    /// Run the compiler here, held by no admission, and finish it.
    ///
    /// With no queue to wait in, a run killed inside the ceiling runs once more outside it at once, and one killed
    /// outside every ceiling ends killed (#1770).
    /// @param seams What to run.
    /// @return The report.
    [[nodiscard]] MissReport CompileUnadmitted(MissSeams const& seams)
    {
        auto run = seams.compileLocal(LocalPlacement::UnderCeiling);
        if (run.killed.has_value() && run.inCgroup)
        {
            Say(seams, KillStep::LocalOutsideCeiling);
            run = seams.compileLocal(LocalPlacement::OutsideCeiling);
        }
        if (run.killed.has_value())
            return FinishKilled(seams, run);
        bool stored = false;
        auto const code = seams.finish(run, stored);
        return MissReport { .code = code, .queuedMs = std::nullopt, .peakMemoryBytes = run.peakMemoryBytes };
    }

    /// After a worker failed the compile: ask once more, `LocalOnly` and with that worker excluded, so
    /// the compile that now runs here is charged here.
    ///
    /// The session lives exactly as long as this call, so a compile it cannot hold runs after it ends.
    /// @param seams What to run.
    /// @param waits Where the grant's wait is recorded.
    /// @param failed The worker that failed it.
    /// @return A report when the compile ran under this admission, else what to do next.
    [[nodiscard]] Placement AdmitHere(MissSeams const& seams, Waits& waits, WorkerFailed const& failed)
    {
        auto session = seams.open(std::string_view { failed.endpoint }, waits.Origin());
        if (session == nullptr)
            return Unplaced::FailOpen;
        auto const ticket = Read(seams, waits, session->Await());
        if (!ticket.has_value())
            return Unplaced::FailOpen;
        switch (ticket->kind)
        {
            case Wire::GrantKind::Local:
                return RunHeld(seams, waits, *session, PlacementFor(*ticket));
            case Wire::GrantKind::Stored: {
                // Another compile of this key stored it meanwhile. A miss is not asked about again: the
                // compile runs here, after this session has ended.
                auto placed = AfterStored(seams.serveStored());
                auto const* const unplaced = std::get_if<Unplaced>(&placed);
                if (unplaced == nullptr || *unplaced != Unplaced::Reopen)
                    return placed;
                break;
            }
            case Wire::GrantKind::Worker:
            case Wire::GrantKind::Last:
                // Not what a local-only request is answered; nothing here can hold it.
                break;
        }
        return Unplaced::FailOpen;
    }

    /// What a launcher with no queue does, which is what one that cannot reach it does.
    /// @param seams What to run.
    /// @param origin Whether a grant this invocation already received makes this lease a second ask for one compile.
    /// @return The report.
    [[nodiscard]] MissReport FailOpen(MissSeams const& seams, CompileCacheWire::AskOrigin origin)
    {
        auto run = seams.dispatchUnadmitted(origin);
        if (!run.has_value())
            return CompileUnadmitted(seams);
        bool stored = false;
        auto const code = seams.finish(*run, stored);
        return MissReport { .code = code, .queuedMs = std::nullopt, .peakMemoryBytes = run->peakMemoryBytes };
    }

    /// The report @p placed ends in, with the invocation's summed wait; nothing when it ends in none.
    /// @param placed What an opening produced.
    /// @param waits The waits so far.
    /// @return The report, a declined one included; disengaged for every other `Unplaced`.
    [[nodiscard]] std::optional<MissReport> Concluded(Placement const& placed, Waits const& waits)
    {
        if (auto const* const report = std::get_if<MissReport>(&placed))
        {
            auto out = *report;
            out.queuedMs = waits.Total();
            return out;
        }
        if (auto const* const unplaced = std::get_if<Unplaced>(&placed);
            unplaced != nullptr && *unplaced == Unplaced::Declined)
            return MissReport { .code = std::nullopt, .queuedMs = waits.Total(), .peakMemoryBytes = std::nullopt };
        return std::nullopt;
    }
} // namespace

std::optional<std::string_view> PlanUnreachableAt(LauncherEndpoints const& endpoints,
                                                  std::function<bool(std::string_view)> const& remembered)
{
    auto const queue = AdmissionEndpointOf(endpoints);
    if (!remembered(queue))
        return std::nullopt;
    if (endpoints.scheduler.empty())
        return queue;
    if (!remembered(endpoints.scheduler))
        return std::nullopt;
    return endpoints.scheduler;
}

AskVerdict DecideAskFrom(std::string_view endpoint,
                         std::string_view cacheAddress,
                         std::optional<CacheOutcomeKind> fetched,
                         ReachabilityMemo const& memo,
                         std::chrono::system_clock::time_point now)
{
    return DecideAsk(
        endpoint, cacheAddress, fetched, !endpoint.empty() && memo.Remembers(MemoKind::SchedulerUnreached, endpoint, now));
}

bool RememberUnreachedFetch(ReachabilityMemo& memo,
                            std::string_view admissionEndpoint,
                            std::string_view cacheAddress,
                            CacheOutcomeKind fetched,
                            TransportFailure failure,
                            std::chrono::system_clock::time_point now)
{
    if (admissionEndpoint.empty() || admissionEndpoint != cacheAddress || fetched != CacheOutcomeKind::Transport
        || !MarksUnreachable(failure))
        return false;
    memo.Note(MemoKind::SchedulerUnreached, admissionEndpoint, now);
    return true;
}

std::optional<LocalGrantNote> ExplainLocalGrant(Wire::LocalGrantReason reason, std::string_view toolchainLabel)
{
    auto const index = static_cast<std::size_t>(reason);
    if (index >= EnumeratorCount<Wire::LocalGrantReason>)
        return std::nullopt;
    auto const& row = LocalGrantTable[index];
    if (!row.records)
        return std::nullopt;
    auto const toolchain =
        toolchainLabel.empty() ? std::string { "this toolchain" } : std::format("this toolchain ({})", toolchainLabel);
    auto const* const descriptor = Wire::Describe(row.code);
    auto const name = descriptor != nullptr ? descriptor->name : std::string_view { "unknown" };
    return LocalGrantNote { .recording = RecordingFor(DispatchStatus::Declined, row.cause),
                            .line = std::format("not dispatched (rejected ({}): {}); compiling locally",
                                                name,
                                                std::vformat(row.sentence, std::make_format_args(toolchain))) };
}

std::string TreeLabelOf(std::string_view workingDirectory, std::string_view sourceRoot)
{
    auto const spelled = [](std::string_view text) {
        std::string out { text };
        std::ranges::replace(out, '\\', '/');
        return out;
    };
    if (!IsValidUtf8(workingDirectory))
        return std::string { UnnamedTreeLabel };
    auto label = spelled(workingDirectory);
    // A root that is not text names nothing to be relative to: the directory is spelled whole.
    auto root = IsValidUtf8(sourceRoot) ? spelled(sourceRoot) : std::string {};
    while (root.size() > 1 && root.ends_with('/'))
        root.pop_back();
    if (!root.empty() && label.starts_with(root)
        && (label.size() == root.size() || label[root.size()] == '/' || root.ends_with('/')))
    {
        label.erase(0, root.size());
        while (label.starts_with('/'))
            label.erase(0, 1);
        if (label.empty())
            label = ".";
    }
    if (label.empty() || label.size() > Wire::MaxAdmitTreeLabelBytes)
        return std::string { UnnamedTreeLabel };
    return label;
}

MissReport RunMiss(MissSeams const& seams)
{
    Waits waits;
    // Each pass is one opening. Bounded however the answers fall: two openings of the request as described (the
    // second for a `Stored` grant whose object is not there), one requeue after a kill, and the local-only asks a
    // failed or twice-killed worker leads to.
    constexpr auto MaxOpenings = 6;
    auto storedReopens = 0;
    std::optional<WorkerFailed> here; // engaged once the compile is to run here, charged here
    for ([[maybe_unused]] auto const opening: std::views::iota(0, MaxOpenings))
    {
        auto placed = here.has_value() ? AdmitHere(seams, waits, *here) : Admit(seams, waits);
        waits.EndOpening();
        if (auto report = Concluded(placed, waits); report.has_value())
            return *report;
        if (auto const* const killed = std::get_if<Killed>(&placed))
        {
            if (!waits.RequeueSpent())
            {
                // The one requeue (#1770): the same request, asked for after a kill, so the leader places it only
                // where what it is now estimated to need fits -- here or on a worker, now or once room frees.
                waits.CompileKilled();
                Say(seams, killed->worker.has_value() ? KillStep::WorkerRequeued : KillStep::LocalRequeued);
                continue;
            }
            // Spent: a worker killed the requeued compile too. It compiles here, charged here, rather than loop.
            // (A kill HERE after the requeue never comes back as `Killed`: `RunHeld` decides it in place.)
            Say(seams, KillStep::WorkerKilledTwice);
            here = WorkerFailed { .endpoint = killed->worker.value_or(std::string {}) };
            continue;
        }
        if (auto const* const failed = std::get_if<WorkerFailed>(&placed))
        {
            here = *failed;
            continue;
        }
        if (here.has_value())
        {
            // The worker's own failure was recorded where it happened; this compile runs here, held by
            // nothing, rather than trying the fleet a second way.
            auto report = CompileUnadmitted(seams);
            report.queuedMs = waits.Total();
            return report;
        }
        if (std::get<Unplaced>(placed) == Unplaced::FailOpen || ++storedReopens > 1)
            break;
    }
    auto report = FailOpen(seams, waits.Origin());
    report.queuedMs = waits.Total();
    return report;
}

} // namespace Pace::Cc
