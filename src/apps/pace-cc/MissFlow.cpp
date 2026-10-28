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

    using Placement = std::variant<MissReport, Unplaced, WorkerFailed>;

    /// The admission waits of one invocation, summed across its openings.
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

      private:
        std::optional<std::uint64_t> _opening;
        std::optional<std::uint64_t> _total;
    };

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
    /// failed the compile -- ends the connection before anything runs outside it.
    /// @param seams What to run.
    /// @param waits Where each grant's wait is recorded.
    /// @return A report when the compile ran here, else what to do next.
    [[nodiscard]] Placement Admit(MissSeams const& seams, Waits& waits)
    {
        auto session = seams.open(std::nullopt);
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
                case Wire::GrantKind::Local: {
                    auto run = seams.compileLocal();
                    return FinishHeld(seams, *session, run);
                }
                case Wire::GrantKind::Worker: {
                    auto attempt = seams.dispatchAdmitted(*ticket);
                    if (attempt.workerRefused)
                    {
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
                    }
                    if (attempt.run.has_value())
                        return FinishHeld(seams, *session, *attempt.run);
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
    /// @param seams What to run.
    /// @return The report.
    [[nodiscard]] MissReport CompileUnadmitted(MissSeams const& seams)
    {
        auto run = seams.compileLocal();
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
        auto session = seams.open(std::string_view { failed.endpoint });
        if (session == nullptr)
            return Unplaced::FailOpen;
        auto const ticket = Read(seams, waits, session->Await());
        if (!ticket.has_value())
            return Unplaced::FailOpen;
        switch (ticket->kind)
        {
            case Wire::GrantKind::Local: {
                auto run = seams.compileLocal();
                return FinishHeld(seams, *session, run);
            }
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
    /// @return The report.
    [[nodiscard]] MissReport FailOpen(MissSeams const& seams)
    {
        auto run = seams.dispatchUnadmitted();
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
    // Two openings: the first, and one more for a `Stored` grant whose object is not there.
    for ([[maybe_unused]] auto const opening: std::views::iota(0, 2))
    {
        auto placed = Admit(seams, waits);
        waits.EndOpening();
        if (auto report = Concluded(placed, waits); report.has_value())
            return *report;
        if (auto const* const failed = std::get_if<WorkerFailed>(&placed))
        {
            auto here = AdmitHere(seams, waits, *failed);
            waits.EndOpening();
            if (auto report = Concluded(here, waits); report.has_value())
                return *report;
            // The worker's own failure was recorded where it happened; this compile runs here, held by
            // nothing, rather than trying the fleet a second way.
            auto report = CompileUnadmitted(seams);
            report.queuedMs = waits.Total();
            return report;
        }
        if (std::get<Unplaced>(placed) == Unplaced::FailOpen)
            break;
    }
    auto report = FailOpen(seams);
    report.queuedMs = waits.Total();
    return report;
}

} // namespace Pace::Cc
