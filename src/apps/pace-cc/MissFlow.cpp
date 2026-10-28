// SPDX-License-Identifier: Apache-2.0
#include "MissFlow.hpp"

#include <Pace/Core/Utf8.hpp>

#include <algorithm>
#include <ranges>
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
        Last
    };

    using Placement = std::variant<MissReport, Unplaced>;

    /// Finish a compile held under @p session: the STORE first, then `SETTLE` (R1).
    /// @param seams What to run.
    /// @param session The held admission.
    /// @param ticket What it granted.
    /// @param run The compile.
    /// @return The report.
    [[nodiscard]] MissReport FinishHeld(MissSeams const& seams,
                                        IAdmitSession& session,
                                        AdmitTicket const& ticket,
                                        CompileRun& run)
    {
        bool stored = false;
        auto const code = seams.finish(run, stored);
        // After the STORE and never before, so a compile waiting on this key is woken to an object
        // that is there. An exception out of `finish` skips this line and the session's destructor
        // closes the connection, which the leader reads as an abandonment.
        session.Settle(Wire::SettleRequest { .outcome = SettleOutcomeOf(code, stored), .peakBytes = run.peakMemoryBytes });
        return MissReport { .code = code, .queuedMs = ticket.queuedMs, .peakMemoryBytes = run.peakMemoryBytes };
    }

    /// One opening of the admission, from the ADMIT to the end of the compile.
    ///
    /// The session lives exactly as long as this call, so every way out -- a fail open included --
    /// ends the connection before anything runs in the open.
    /// @param seams What to run.
    /// @return A report when the compile ran here, else what to do next.
    [[nodiscard]] Placement Admit(MissSeams const& seams)
    {
        auto session = seams.open();
        if (session == nullptr)
            return Unplaced::FailOpen;
        auto ticket = session->Await();
        auto const failed = [&](std::expected<AdmitTicket, std::string> const& answer) {
            if (!answer.has_value() && seams.failedOpen)
                seams.failedOpen(answer.error());
        };
        failed(ticket);
        for (auto const refusals: std::views::iota(std::size_t { 0 }, MaxWorkerRefusals + 1))
        {
            if (!ticket.has_value())
                return Unplaced::FailOpen;
            switch (ticket->kind)
            {
                case Wire::GrantKind::Stored: {
                    // Nothing is held for a deduplicated request, so there is nothing to settle: the
                    // connection ends with the session.
                    if (auto const code = seams.serveStored(); code.has_value())
                        return MissReport { .code = *code, .queuedMs = ticket->queuedMs, .peakMemoryBytes = std::nullopt };
                    return Unplaced::Reopen;
                }
                case Wire::GrantKind::Local: {
                    auto run = seams.compileLocal();
                    return FinishHeld(seams, *session, *ticket, run);
                }
                case Wire::GrantKind::Worker: {
                    auto attempt = seams.dispatchAdmitted(*ticket);
                    if (attempt.workerRefused)
                    {
                        // The launcher's own stop: the connection ends, which abandons the request.
                        if (refusals == MaxWorkerRefusals)
                            return Unplaced::FailOpen;
                        // Back into the queue on the same connection, never a compile in the open:
                        // the leader admitted this against a ledger the worker disagreed with.
                        ticket = session->Requeue();
                        failed(ticket);
                        continue;
                    }
                    auto run = attempt.run.has_value() ? std::move(*attempt.run) : seams.compileLocal();
                    return FinishHeld(seams, *session, *ticket, run);
                }
                case Wire::GrantKind::Last:
                    break;
            }
            return Unplaced::FailOpen;
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
            run = seams.compileLocal();
        bool stored = false;
        auto const code = seams.finish(*run, stored);
        return MissReport { .code = code, .queuedMs = std::nullopt, .peakMemoryBytes = run->peakMemoryBytes };
    }
} // namespace

std::string TreeLabelOf(std::string_view workingDirectory, std::string_view sourceRoot)
{
    auto const spelled = [](std::string_view text) {
        std::string out { text };
        std::ranges::replace(out, '\\', '/');
        return out;
    };
    if (!IsValidUtf8(workingDirectory) || !IsValidUtf8(sourceRoot))
        return std::string { UnnamedTreeLabel };
    auto label = spelled(workingDirectory);
    auto root = spelled(sourceRoot);
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
    // Two openings: the first, and one more for a `Stored` grant whose object is not there.
    for ([[maybe_unused]] auto const opening: std::views::iota(0, 2))
    {
        auto placed = Admit(seams);
        if (auto* const report = std::get_if<MissReport>(&placed))
            return *report;
        if (std::get<Unplaced>(placed) == Unplaced::FailOpen)
            break;
    }
    return FailOpen(seams);
}

} // namespace Pace::Cc
