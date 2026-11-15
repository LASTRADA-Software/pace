// SPDX-License-Identifier: Apache-2.0
#include "PressureGovernor.hpp"

#include <algorithm>
#include <cmath>

namespace Pace::Node
{

namespace
{
    /// Percent's denominator, for the ramp step.
    constexpr std::uint64_t PercentDenominator = 100;

    /// How many dwell periods `Yield` is held, with the system still stalled, before it escalates to `Shed`.
    constexpr int YieldDwellsBeforeShed = 2;

    /// How many dwell periods at-limit evidence is held after it was last seen. It must OUTLIVE the shed's first exit
    /// check strictly, never meet it: that check comes at the first sample at least one dwell after entering, which a
    /// real loop (step, then wait one poll) lands a little past the dwell -- and a shed can be entered on `full` from
    /// a sample carrying no evidence, a poll after the evidence was last seen. A window of exactly one dwell expired
    /// in both arrangements a hair before the check, read the compiles' own stall as foreign, held the shed and
    /// killed a second compile. Twice the dwell covers both with a whole dwell to spare; the cost is that a stall
    /// stays the compiles' own for up to twenty seconds after they last touched their ceiling, which only keeps a
    /// stall they caused from being shed on.
    constexpr int EvidenceDwells = 2;
} // namespace

PressureGovernor::PressureGovernor(PressureThresholds thresholds,
                                   std::uint64_t configuredBytes,
                                   std::uint64_t floorBytes,
                                   core::platform::IClock const& clock):
    _thresholds { thresholds },
    _configuredBytes { configuredBytes },
    _floorBytes { floorBytes },
    _clock { clock },
    _allowanceBytes { configuredBytes },
    _enteredAt { clock.now() },
    _rampAnchor { _enteredAt }
{
}

GovernorDecision PressureGovernor::Step(Platform::PressureSample const& sample,
                                        std::uint64_t compilesHoldBytes,
                                        std::optional<std::uint32_t> runningCompiles)
{
    auto const now = _clock.now();
    auto const previous = _level;
    auto const& thresholds = _thresholds;

    auto const systemSome = sample.system.some.avg10;
    // An absent `full` line (an older kernel) is no freeze reported: shedding then comes only from a held yield.
    auto const systemFull = sample.system.full ? sample.system.full->avg10 : 0.0;

    // A stall is the compiles' own only when they carry it AND stand at their own ceiling: compiles far below their
    // limit stall because the machine reclaims from everyone, a game's shortage included -- which is a yield.
    // Compared as a product, never a ratio: a zero system figure divides nothing and yields no NaN.
    //
    // The evidence is STICKY for `EvidenceDwells` dwells: it describes the last second, while the PSI averages it is
    // weighed against describe the last ten. A shed that just killed a compile leaves the survivors below the ceiling for a
    // moment while `avg10` still carries the stall they caused; instant evidence would read that as someone else's, hold the
    // shed and kill again. Unreadable evidence is none at all: never the compiles' own, sticky or not.
    if (sample.compilesAtLimit == true)
        _atLimitSeen = now;
    auto const atLimit = sample.compilesAtLimit.has_value() && _atLimitSeen.has_value()
                         && now - *_atLimitSeen < EvidenceDwells * thresholds.dwell;
    auto const ownStall = atLimit && sample.compiles && sample.compiles->some.avg10 >= thresholds.ownShare * systemSome;
    // A stall confined to another cgroup's own limit (its evidence held as the compiles' is): the largest that carries
    // the share ALONE. Two that split it are the machine's -- a sum would overstate PSI's union of overlapping stalls.
    auto const confined = ConfinedCarriers(sample, now, systemSome, systemFull);
    auto const foreignStall = systemSome > thresholds.yieldEnter && !ownStall && !confined.some;
    // A stall the compiles carry is the ceiling working, so it never counts as one to stay for; nor one confined.
    auto const yieldEased = ownStall || confined.some.has_value() || systemSome < thresholds.yieldExit;
    // Entering a shed on `full` asks nobody whose stall it is; leaving one does, as leaving a yield does on
    // `some`. Only a compiles `full` line that was READ can be an own stall: absent is never the compiles'.
    auto const ownFull = atLimit && sample.compiles && sample.compiles->full
                         && sample.compiles->full->avg10 >= thresholds.ownShare * systemFull;
    auto const freezeEased = ownFull || confined.full.has_value() || systemFull < thresholds.shedExit;
    // A shed left on the compiles' own full stall stays left: re-entering needs that stall to turn foreign or to
    // fall below the exit first, or the same own stall re-enters at once and kills one compile per dwell.
    if (_ownFreezeLatched && (!ownFull || systemFull < thresholds.shedExit))
        _ownFreezeLatched = false;
    auto const freezing = systemFull >= thresholds.shedEnter && !_ownFreezeLatched && !confined.full;
    auto const dwellSpent = now - _enteredAt >= thresholds.dwell;

    // Escalation needs the foreign stall SUSTAINED: its clock restarts whenever the stall drops.
    if (!foreignStall)
        _foreignStallSince.reset();
    else if (!_foreignStallSince)
        _foreignStallSince = now;
    auto const foreignStallSustained =
        _foreignStallSince && now - *_foreignStallSince >= YieldDwellsBeforeShed * thresholds.dwell;

    switch (_level)
    {
        case PressureLevel::Calm:
            if (freezing)
                Enter(PressureLevel::Shed, now, compilesHoldBytes);
            else if (foreignStall)
                Enter(PressureLevel::Yield, now, compilesHoldBytes);
            break;
        case PressureLevel::Yield:
            if (freezing || (foreignStallSustained && !std::isinf(thresholds.shedEnter)))
                Enter(PressureLevel::Shed, now, compilesHoldBytes);
            else if (dwellSpent && yieldEased)
                Enter(PressureLevel::Calm, now, compilesHoldBytes);
            break;
        case PressureLevel::Shed:
            // Held while a freeze or a foreign stall persists; stepping down lands where the stall says.
            if (dwellSpent && freezeEased && !foreignStall)
            {
                // Eased only because the full stall is the compiles' own: latch, so it cannot re-enter by itself.
                _ownFreezeLatched = systemFull >= thresholds.shedExit;
                Enter(yieldEased ? PressureLevel::Calm : PressureLevel::Yield, now, compilesHoldBytes);
            }
            break;
        case PressureLevel::Last:
            break;
    }

    if (_level == PressureLevel::Calm)
        Ramp(now);

    // One kill per dwell, and none while compiles hold nothing: a kill with no victim spends no period. Nor of the sole
    // compile within the allowance: the ceiling bounds it, and its requeue would be admitted, killed and run again.
    auto const killDue = !_lastKill || now - *_lastKill >= thresholds.dwell;
    auto const shedKill = [&] {
        if (_level != PressureLevel::Shed)
            return ShedKill::NotShedding;
        if (compilesHoldBytes == 0)
            return ShedKill::NothingRuns;
        if (runningCompiles == 1U && compilesHoldBytes <= _allowanceBytes)
            return ShedKill::SpareSoleWithinAllowance;
        return killDue ? ShedKill::KillYoungest : ShedKill::NotDue;
    }();
    auto const killYoungest = RowOf(shedKill).kills;
    if (killYoungest)
        _lastKill = now;

    // Named only where it changed the answer: a confined stall too small to yield on is nothing to say. With the
    // levels' own hysteresis -- from above the entry threshold, held down to the exit one for the cgroup already
    // named -- so a stall hovering at a threshold does not start and end the discount, and its line, every sample.
    auto const naming = [&](std::optional<Platform::ConfinedStall> const& carrier) {
        return carrier.has_value() && _discounting == carrier->cgroup;
    };
    auto discounted = std::optional<Platform::ConfinedStall> {};
    if (confined.full
        && (systemFull >= thresholds.shedEnter || (naming(confined.full) && systemFull >= thresholds.shedExit)))
        discounted = confined.full;
    else if (confined.some && !ownStall
             && (systemSome > thresholds.yieldEnter || (naming(confined.some) && systemSome >= thresholds.yieldExit)))
        discounted = confined.some;
    _discounting = discounted.transform([](Platform::ConfinedStall const& stall) { return stall.cgroup; });

    return { .level = _level,
             .allowanceBytes = _allowanceBytes,
             .killYoungest = killYoungest,
             .shedKill = shedKill,
             .levelChanged = _level != previous,
             .discounted = std::move(discounted) };
}

PressureGovernor::Carriers PressureGovernor::ConfinedCarriers(Platform::PressureSample const& sample,
                                                              core::platform::SteadyTimePoint now,
                                                              double systemSome,
                                                              double systemFull)
{
    auto carriers = Carriers {};
    if (!sample.confined)
    {
        // Unreadable or unconfigured: no confined stall, and nothing remembered to make one of later.
        _confinedSeen.clear();
        return carriers;
    }
    // Forget what the source no longer reports; refresh what stands at its limit now.
    std::erase_if(_confinedSeen, [&](auto const& entry) {
        return std::ranges::none_of(*sample.confined, [&](auto const& stall) { return stall.cgroup == entry.first; });
    });
    for (auto const& stall: *sample.confined)
        if (stall.atLimit)
            _confinedSeen[stall.cgroup] = now;

    auto const share = _thresholds.ownShare;
    // An absent `full` line carries no freeze.
    auto const fullOf = [](Platform::ConfinedStall const& stall) {
        return stall.psi.full ? stall.psi.full->avg10 : 0.0;
    };
    auto const someOf = [](Platform::ConfinedStall const& stall) {
        return stall.psi.some.avg10;
    };
    for (auto const& stall: *sample.confined)
    {
        auto const seen = _confinedSeen.find(stall.cgroup);
        if (seen == _confinedSeen.end() || now - seen->second >= EvidenceDwells * _thresholds.dwell)
            continue;
        // `some` decides OWNERSHIP. PSI `full` is "every non-idle task of this cgroup stalled", so in a machine-wide
        // freeze every busy cgroup reads about the system's figure, one at its own limit included: `full` alone would
        // discount a genuine freeze. Only a cgroup that carries the `some` share may have its `full` discounted.
        if (systemSome <= 0.0 || someOf(stall) < share * systemSome)
            continue;
        if (someOf(stall) > carriers.some.transform(someOf).value_or(0.0))
            carriers.some = stall;
        if (systemFull > 0.0 && fullOf(stall) >= share * systemFull
            && fullOf(stall) > carriers.full.transform(fullOf).value_or(0.0))
            carriers.full = stall;
    }
    return carriers;
}

void PressureGovernor::Reconfigure(std::uint64_t configuredBytes, std::uint64_t floorBytes)
{
    // An allowance at the configured value was never adapted: it IS the configuration, and follows it either way.
    auto const adapted = _allowanceBytes < _configuredBytes;
    _configuredBytes = configuredBytes;
    _floorBytes = floorBytes;
    _allowanceBytes = adapted ? std::clamp(_allowanceBytes, FloorBytes(), _configuredBytes) : _configuredBytes;
}

std::uint64_t PressureGovernor::AllowanceBytes() const noexcept
{
    return _allowanceBytes;
}

void PressureGovernor::Retune(PressureThresholds const& thresholds)
{
    _thresholds = thresholds;
}

void PressureGovernor::Enter(PressureLevel level, core::platform::SteadyTimePoint now, std::uint64_t compilesHoldBytes)
{
    _level = level;
    _enteredAt = now;
    if (level == PressureLevel::Calm)
        _rampAnchor = now;
    else
        // Never raised, and never lowered below the floor: one default compile must still fit, or the queue refuses
        // everything here for the whole ramp.
        _allowanceBytes = std::min(_allowanceBytes, std::max(compilesHoldBytes, FloorBytes()));
}

void PressureGovernor::Ramp(core::platform::SteadyTimePoint now)
{
    if (_thresholds.rampInterval <= std::chrono::seconds::zero())
    {
        // No interval to wait: a calm allowance is the configured one.
        _allowanceBytes = _configuredBytes;
        return;
    }
    auto const intervals = (now - _rampAnchor) / _thresholds.rampInterval;
    if (intervals <= 0)
        return;
    _rampAnchor += intervals * _thresholds.rampInterval;
    auto const steps = static_cast<std::uint64_t>(intervals);

    auto const headroom = _configuredBytes - std::min(_allowanceBytes, _configuredBytes);
    auto const step = RampStepBytes();
    if (step == 0)
        return;
    // Saturating: `steps × step` past the headroom is the headroom, and is never computed when it would overflow.
    auto const growth = steps > headroom / step ? headroom : steps * step;
    _allowanceBytes = std::min(_allowanceBytes, _configuredBytes) + growth;
}

std::uint64_t PressureGovernor::FloorBytes() const noexcept
{
    return std::min(_floorBytes, _configuredBytes);
}

std::uint64_t PressureGovernor::RampStepBytes() const noexcept
{
    auto const percent = std::uint64_t { _thresholds.rampPercent };
    // Split so `configured × percent` cannot overflow; equal to `configured × percent / 100` where that fits.
    return ((_configuredBytes / PercentDenominator) * percent)
           + ((_configuredBytes % PercentDenominator) * percent / PercentDenominator);
}

} // namespace Pace::Node
