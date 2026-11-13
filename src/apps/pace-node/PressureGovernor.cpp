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
} // namespace

PressureGovernor::PressureGovernor(PressureThresholds thresholds,
                                   std::uint64_t configuredBytes,
                                   core::platform::IClock const& clock):
    _thresholds { thresholds },
    _configuredBytes { configuredBytes },
    _clock { clock },
    _allowanceBytes { configuredBytes },
    _enteredAt { clock.now() },
    _rampAnchor { _enteredAt }
{
}

GovernorDecision PressureGovernor::Step(Platform::PressureSample const& sample, std::uint64_t compilesHoldBytes)
{
    auto const now = _clock.now();
    auto const previous = _level;
    auto const& thresholds = _thresholds;

    auto const systemSome = sample.system.some.avg10;
    // An absent `full` line (an older kernel) is no freeze reported: shedding then comes only from a held yield.
    auto const systemFull = sample.system.full ? sample.system.full->avg10 : 0.0;

    // Compared as a product, never a ratio: a zero system figure divides nothing and yields no NaN.
    auto const ownStall = sample.compiles && sample.compiles->some.avg10 >= thresholds.ownShare * systemSome;
    auto const foreignStall = systemSome > thresholds.yieldEnter && !ownStall;
    // A stall the compiles carry is the ceiling working, so it never counts as one to stay for.
    auto const yieldEased = ownStall || systemSome < thresholds.yieldExit;
    // Entering a shed on `full` asks nobody whose stall it is; leaving one does, as leaving a yield does on
    // `some`. Only a compiles `full` line that was READ can be an own stall: absent is never the compiles'.
    auto const freezing = systemFull >= thresholds.shedEnter;
    auto const ownFull =
        sample.compiles && sample.compiles->full && sample.compiles->full->avg10 >= thresholds.ownShare * systemFull;
    auto const freezeEased = ownFull || systemFull < thresholds.shedExit;
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
                Enter(yieldEased ? PressureLevel::Calm : PressureLevel::Yield, now, compilesHoldBytes);
            break;
        case PressureLevel::Last:
            break;
    }

    if (_level == PressureLevel::Calm)
        Ramp(now);

    // One kill per dwell, and none while compiles hold nothing: a kill with no victim spends no period.
    auto const killDue = !_lastKill || now - *_lastKill >= thresholds.dwell;
    auto const killYoungest = _level == PressureLevel::Shed && compilesHoldBytes > 0 && killDue;
    if (killYoungest)
        _lastKill = now;

    return {
        .level = _level, .allowanceBytes = _allowanceBytes, .killYoungest = killYoungest, .levelChanged = _level != previous
    };
}

void PressureGovernor::Reconfigure(std::uint64_t configuredBytes)
{
    // An allowance at the configured value was never adapted: it IS the configuration, and follows it either way.
    auto const adapted = _allowanceBytes < _configuredBytes;
    _configuredBytes = configuredBytes;
    _allowanceBytes = adapted ? std::min(_allowanceBytes, _configuredBytes) : _configuredBytes;
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
        _allowanceBytes = std::min(_allowanceBytes, compilesHoldBytes);
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

std::uint64_t PressureGovernor::RampStepBytes() const noexcept
{
    auto const percent = std::uint64_t { _thresholds.rampPercent };
    // Split so `configured × percent` cannot overflow; equal to `configured × percent / 100` where that fits.
    return ((_configuredBytes / PercentDenominator) * percent)
           + ((_configuredBytes % PercentDenominator) * percent / PercentDenominator);
}

} // namespace Pace::Node
