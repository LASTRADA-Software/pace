// SPDX-License-Identifier: Apache-2.0
#include "MachineBudget.hpp"
#include "NodeAnnounce.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace Pace::Node
{

namespace
{
    namespace Wire = Pace::CompileCacheWire;

    /// The subject a budget refusal is filed under in `SchedulerReachability`, so a scheduler that
    /// records this machine's presence and refuses its budget is two setbacks and not one.
    constexpr std::string_view BudgetSubject = "machine-budget";

    /// What this machine's compiles may be charged when the operator named nothing.
    /// @param cfg The parsed configuration.
    /// @param capacity What this machine is.
    /// @return 75% of what is left after the node's own reserve, never below the default estimate.
    [[nodiscard]] std::uint64_t DerivedBudgetOf(NodeConfig const& cfg, Distributed::NodeCapacity const& capacity) noexcept
    {
        auto const reserved = std::min(capacity.reservedMemoryBytes, capacity.totalMemoryBytes);
        auto const usable = capacity.totalMemoryBytes - reserved;
        // Divided before multiplied would round a small machine to nothing; the product cannot wrap, for
        // a machine's memory is far below 2^64 / 100.
        return std::max(usable * MachineBudgetSharePercent / 100, CompileEstimateDefaultOf(cfg));
    }

    /// The report's half of the round: say it on one connection.
    class BudgetAnnouncement final: public IAnnouncement
    {
      public:
        BudgetAnnouncement(Wire::MachineBudgetReport const& report,
                           ILogger& logger,
                           SchedulerReachability& reachability) noexcept:
            _report { report },
            _logger { logger },
            _reachability { reachability }
        {
        }

        [[nodiscard]] AnnounceOutcome Attempt(core::net::ISocket& client, std::string_view endpoint) override
        {
            auto const sent = Cc::SendMachineBudget(client, _report);
            if (sent.has_value())
            {
                _accepted = true;
                if (auto const back = _reachability.Succeeded(AnnounceStage::Announcement, endpoint, BudgetSubject);
                    back.has_value())
                    _logger.Log(back->level, back->message);
                return AnnounceOutcome { .accepted = 1, .leader = std::nullopt };
            }

            // A refusal naming a LEADER is the redirect this round follows at once, not a setback:
            // Debug, and never the tracker, or every election spends a scheduler's Warn.
            if (sent.error().leader.has_value())
            {
                auto const reason = std::string_view { sent.error().reason };
                _logger.Log(LogLevel::Debug,
                            std::vformat(SchedulerOutcomeRowOf(SchedulerOutcome::BudgetRefused).failure,
                                         std::make_format_args(endpoint, BudgetSubject, reason)));
            }
            else
            {
                // A stall or a lost peer after the socket connected is this scheduler being
                // unreachable, not a refusal of the budget: `PresenceAnnouncement`'s split, for its reason.
                auto const outcome = sent.error().kind == Cc::AnnounceRefusalKind::Transport
                                         ? SchedulerOutcome::Unreachable
                                         : SchedulerOutcome::BudgetRefused;
                auto const subject = outcome == SchedulerOutcome::BudgetRefused ? BudgetSubject : std::string_view {};
                auto const said = _reachability.Failed(
                    outcome, SchedulerFailure { .endpoint = endpoint, .subject = subject, .reason = sent.error().reason });
                _logger.Log(said.level, said.message);
            }
            return AnnounceOutcome { .accepted = 0, .leader = sent.error().leader };
        }

        /// @return Whether a scheduler recorded the report.
        [[nodiscard]] bool Accepted() const noexcept
        {
            return _accepted;
        }

      private:
        Wire::MachineBudgetReport const& _report;
        ILogger& _logger;
        SchedulerReachability& _reachability;
        bool _accepted = false;
    };
} // namespace

Distributed::MachineReport MachineBudgetOf(NodeConfig const& cfg,
                                           Distributed::NodeCapacity const& capacity,
                                           HeldCharges const& held) noexcept
{
    return Distributed::MachineReport {
        .memoryBudgetBytes = cfg.machineMemoryBudgetBytes.value_or(DerivedBudgetOf(cfg, capacity)),
        // Zero cores is "did not say", and one is the answer `OfferableSlots` gives for it: a machine
        // that reports no slot is one the queue could never run a compile on.
        .slots = cfg.machineSlots.value_or(std::max<std::uint32_t>(capacity.logicalCores, 1)),
        .heldSlots = held.slots,
        .heldBytes = held.bytes,
    };
}

void MeasuredPeaks::OnPeak(Wire::EstimateKeyFields const& key, std::uint64_t peakBytes)
{
    if (key.cacheKey.empty() || key.cacheKey.size() > Wire::MaxLeaseKeyBytes
        || key.sourcePath.size() > Wire::MaxAdmitPathBytes
        || key.compilerIdentity.size() > Wire::MaxToolchainFingerprintBytes)
        return;
    auto const guard = std::scoped_lock { _mutex };
    // One entry per key, and the newest figure, at the newest position.
    std::erase_if(_peaks, [&key](MeasuredPeak const& held) { return held.first == key; });
    _peaks.emplace_back(key, peakBytes);
    Trim();
}

std::vector<MeasuredPeak> MeasuredPeaks::Take()
{
    auto const guard = std::scoped_lock { _mutex };
    return std::exchange(_peaks, {});
}

void MeasuredPeaks::Restore(std::vector<MeasuredPeak> undelivered)
{
    auto const guard = std::scoped_lock { _mutex };
    // What was measured since is newer than anything that failed to go: keep it, and drop the
    // undelivered figure of a key that has one.
    std::erase_if(undelivered, [this](MeasuredPeak const& old) {
        return std::ranges::any_of(_peaks, [&old](MeasuredPeak const& now) { return now.first == old.first; });
    });
    _peaks.insert(_peaks.begin(), std::make_move_iterator(undelivered.begin()), std::make_move_iterator(undelivered.end()));
    Trim();
}

void MeasuredPeaks::Trim()
{
    if (_peaks.size() > Wire::MaxMachineBudgetPeaks)
        _peaks.erase(_peaks.begin(), _peaks.end() - static_cast<std::ptrdiff_t>(Wire::MaxMachineBudgetPeaks));
}

Wire::MachineBudgetReport MachineBudgetToWire(Distributed::MachineReport const& report, std::vector<MeasuredPeak> peaks)
{
    return Wire::MachineBudgetReport { .memoryBudgetBytes = report.memoryBudgetBytes,
                                       .slots = report.slots,
                                       .heldSlots = report.heldSlots,
                                       .heldBytes = report.heldBytes,
                                       .peaks = std::move(peaks) };
}

bool SendMachineBudgetOnce(MachineBudgetRound const& round, SchedulerLink& link, IEndpointDialer& dialer)
{
    // Read per round, never captured: what is held is the one figure here that moves between rounds.
    auto taken = round.peaks.Take();
    auto const report = MachineBudgetToWire(MachineBudgetOf(round.cfg, round.capacity, round.held.Held()), taken);

    BudgetAnnouncement announcement { report, round.logger, round.reachability };
    (void) DialAndAnnounce(
        link, round.reachability, dialer, round.logger, announcement, AnnounceProof { .prover = round.prover });
    if (!announcement.Accepted())
        // Nobody recorded them: offered again next round rather than lost with the failure.
        round.peaks.Restore(std::move(taken));
    return announcement.Accepted();
}

} // namespace Pace::Node
