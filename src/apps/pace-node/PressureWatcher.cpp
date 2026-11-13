// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "LiveNodeConfig.hpp"
#include "NodePresenceTier.hpp"
#include "PressureWatcher.hpp"

#include <Pace/Core/EnumTable.hpp>

#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace Pace::Node
{

namespace
{
    /// Each exit threshold as a share of its entry, the defaults' own proportions: yield 10 -> 5, shed 5 -> 1.
    constexpr double YieldExitShare = 0.5;
    constexpr double ShedExitShare = 0.2;

    /// One level: what it is called where it is said, and the two counters its time and its entries go to --
    /// absent for `Calm`, which is the rest of the time and has no row of its own.
    struct PressureLevelRow
    {
        PressureLevel level;                          ///< The enumerator this row describes.
        std::string_view name;                        ///< As a log line and the condition's detail spell it.
        std::optional<IMetricsSink::Counter> seconds; ///< Whole seconds held.
        std::optional<IMetricsSink::Counter> entries; ///< Times entered.
    };

    /// Every level, in enumerator order.
    constexpr EnumTable<PressureLevel, PressureLevelRow> PressureLevelTable { {
        { .level = PressureLevel::Calm, .name = "calm", .seconds = std::nullopt, .entries = std::nullopt },
        { .level = PressureLevel::Yield,
          .name = "yield",
          .seconds = IMetricsSink::Counter::NodeMemoryPressureYieldSeconds,
          .entries = IMetricsSink::Counter::NodeMemoryPressureYieldEntries },
        { .level = PressureLevel::Shed,
          .name = "shed",
          .seconds = IMetricsSink::Counter::NodeMemoryPressureShedSeconds,
          .entries = IMetricsSink::Counter::NodeMemoryPressureShedEntries },
    } };
    static_assert(RowsInEnumeratorOrder(PressureLevelTable, &PressureLevelRow::level),
                  "PressureLevelTable must hold one row per PressureLevel, in enumerator order");

    /// @return @p level's row.
    [[nodiscard]] PressureLevelRow const& RowOf(PressureLevel level) noexcept
    {
        return PressureLevelTable[static_cast<std::size_t>(level)];
    }

    /// A configured percentage as a threshold: zero turns the level off.
    /// @param percent The flag's value.
    /// @return The threshold.
    [[nodiscard]] double ThresholdOf(std::uint32_t percent) noexcept
    {
        return percent == 0 ? DisabledPressureThreshold : static_cast<double>(percent);
    }
} // namespace

PressureThresholds MemoryPressureThresholdsOf(NodeConfig const& cfg) noexcept
{
    auto thresholds = PressureThresholds {};
    thresholds.yieldEnter = ThresholdOf(cfg.memoryPressureYieldPercent.value_or(DefaultPressureYieldPercent));
    thresholds.yieldExit = thresholds.yieldEnter * YieldExitShare;
    thresholds.shedEnter = ThresholdOf(cfg.memoryPressureShedPercent.value_or(DefaultPressureShedPercent));
    thresholds.shedExit = thresholds.shedEnter * ShedExitShare;
    return thresholds;
}

LivePressureSettings::LivePressureSettings(INodeConfigSource const& inForce, INodeCapacitySource const& capacity) noexcept:
    _inForce { inForce },
    _capacity { capacity }
{
}

PressureSettings LivePressureSettings::Current() const
{
    auto const cfg = _inForce.Current();
    return PressureSettings { .thresholds = MemoryPressureThresholdsOf(cfg),
                              .configuredBytes = CompileCeilingOf(cfg, _capacity.Current()) };
}

PressureWatcher::PressureWatcher(PressureWatcherParts parts):
    _source { std::move(parts.source) },
    _settings { parts.settings },
    _ceiling { parts.ceiling },
    _killer { parts.killer },
    _held { parts.held },
    _published { parts.published },
    _wake { parts.wake },
    _conditions { parts.conditions },
    _metrics { parts.metrics },
    _logger { parts.logger },
    _clock { parts.clock },
    _poll { parts.poll },
    _applied { _settings.Current() },
    _governor { _applied.thresholds, _applied.configuredBytes, _clock },
    _counted { _clock.now() }
{
}

PressureWatcher::~PressureWatcher()
{
    Stop();
}

std::expected<void, std::string> PressureWatcher::Start(bool launch)
{
    if (auto const first = _source->Sample(); !first.has_value())
    {
        auto reason = std::format("memory pressure cannot be read ({})", first.error());
        _logger.Logf(LogLevel::Warn,
                     "{}: this node does not yield or shed its compiles when the machine runs short of memory",
                     reason);
        _conditions.NotEvaluated(NodeCondition::MemoryPressureShedding, reason);
        return std::unexpected { std::move(reason) };
    }

    _conditions.Clear(NodeCondition::MemoryPressureShedding);
    _published.Publish(MachinePressure { .level = PressureLevel::Calm, .allowanceBytes = _applied.configuredBytes });
    _logger.Logf(LogLevel::Info,
                 "memory pressure watched: yields above {}% (PSI some), sheds at {}% (PSI full); {}",
                 _applied.thresholds.yieldEnter,
                 _applied.thresholds.shedEnter,
                 _ceiling != nullptr
                     ? "a yield holds compiles under what they held, and a shed kills the youngest"
                     : "compiles run in no cgroup of this node's own, so it changes only what this machine reports -- "
                       "no compile is held back by a ceiling or killed");
    _counted = _clock.now();
    if (launch)
        _thread = std::jthread { [this](std::stop_token const& stop) { Loop(stop); } };
    return {};
}

void PressureWatcher::Stop()
{
    if (_thread.joinable())
    {
        _thread.request_stop();
        _thread.join();
    }
}

void PressureWatcher::Loop(std::stop_token const& stop)
{
    // A wait the stop token ends, never a sleep: a SIGTERM must not sit out a poll.
    auto mutex = std::mutex {};
    auto wake = std::condition_variable_any {};
    while (!stop.stop_requested())
    {
        StepOnce();
        auto lock = std::unique_lock { mutex };
        static_cast<void>(wake.wait_for(lock, stop, _poll, [] { return false; }));
    }
}

void PressureWatcher::StepOnce()
{
    CountTime(_clock.now());

    // Read at every sample: both flags reload, and the configured ceiling follows a reload and the cache reserve.
    if (auto const settings = _settings.Current(); settings != _applied)
    {
        if (settings.thresholds != _applied.thresholds)
            _governor.Retune(settings.thresholds);
        if (settings.configuredBytes != _applied.configuredBytes)
            _governor.Reconfigure(settings.configuredBytes);
        _applied = settings;
    }

    auto const sample = _source->Sample();
    if (!sample.has_value())
    {
        // Said on the transition, never per sample: the governor holds its level until a sample arrives.
        if (!std::exchange(_sampleFaultSaid, true))
            _logger.Logf(LogLevel::Warn, "memory pressure cannot be read now ({}); holding the level", sample.error());
        return;
    }
    if (std::exchange(_sampleFaultSaid, false))
        _logger.Log(LogLevel::Info, "memory pressure can be read again");
    NoteCompilesFault(sample->compilesFault);

    auto const decision = _governor.Step(*sample, HoldingBytes());
    if (_ceiling != nullptr && _allowanceWritten != decision.allowanceBytes)
    {
        _ceiling->Apply(decision.allowanceBytes);
        _allowanceWritten = decision.allowanceBytes;
    }
    // A shed with nothing running kills nothing and changes nothing else: the governor asks again next dwell.
    if (decision.killYoungest && _killer != nullptr)
        static_cast<void>(_killer->KillYoungest());
    // Published BEFORE the presence loop is woken, so the round it runs reads the new level.
    _published.Publish(MachinePressure { .level = decision.level, .allowanceBytes = decision.allowanceBytes });
    if (decision.levelChanged)
        OnLevelChanged(decision, *sample);
    _level = decision.level;
}

void PressureWatcher::NoteCompilesFault(std::optional<std::string> const& fault)
{
    if (fault.has_value() && !_compilesFaultSaid)
    {
        _compilesFaultSaid = true;
        _logger.Logf(LogLevel::Warn,
                     "the compiles' own memory pressure cannot be read ({}), so a stall they cause cannot be told from "
                     "one something else causes: every stall counts as foreign until it can",
                     *fault);
    }
    else if (!fault.has_value() && _compilesFaultSaid)
    {
        _compilesFaultSaid = false;
        _logger.Log(LogLevel::Info, "the compiles' own memory pressure can be read again");
    }
}

void PressureWatcher::CountTime(core::platform::SteadyTimePoint now)
{
    auto const elapsed = now - std::exchange(_counted, now);
    _uncounted += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    auto const whole = std::chrono::duration_cast<std::chrono::seconds>(_uncounted);
    if (whole.count() <= 0)
        return;
    _uncounted -= whole;
    if (auto const counter = RowOf(_level).seconds; counter.has_value())
        _metrics.Increment(*counter, static_cast<std::uint64_t>(whole.count()));
}

void PressureWatcher::OnLevelChanged(GovernorDecision const& decision, Platform::PressureSample const& sample)
{
    if (auto const counter = RowOf(decision.level).entries; counter.has_value())
        _metrics.Increment(*counter);
    // The fleet's queue must hear it within one wait: the report's 20 s cadence is too slow to yield in time.
    _wake.RequestRound(PresenceWakeReason::Pressure);
    auto const full = sample.system.full.has_value() ? sample.system.full->avg10 : 0.0;
    auto const consequence = [&]() -> std::string_view {
        switch (decision.level)
        {
            case PressureLevel::Calm:
                return "compiles are admitted again, and their allowance grows back toward the configured ceiling";
            case PressureLevel::Yield:
                return "no new compile is admitted on this machine, its own local ones included";
            case PressureLevel::Shed:
                return _killer != nullptr ? "nothing is admitted, and the youngest compile is killed once per dwell"
                                          : "nothing is admitted; no compile can be killed without a cgroup of the "
                                            "node's own";
            case PressureLevel::Last:
                break;
        }
        return {};
    }();
    _logger.Logf(decision.level == PressureLevel::Shed ? LogLevel::Warn : LogLevel::Info,
                 "memory pressure: {} (system some {:.1f}%, full {:.1f}%); compiles may hold {} bytes; {}",
                 RowOf(decision.level).name,
                 sample.system.some.avg10,
                 full,
                 decision.allowanceBytes,
                 consequence);
    AnswerCondition(decision);
}

void PressureWatcher::AnswerCondition(GovernorDecision const& decision)
{
    if (decision.level == PressureLevel::Calm)
    {
        _conditions.Clear(NodeCondition::MemoryPressureShedding);
        return;
    }
    _conditions.Raise(NodeCondition::MemoryPressureShedding,
                      std::format("level {}: compiles may hold {} bytes on this machine{}",
                                  RowOf(decision.level).name,
                                  decision.allowanceBytes,
                                  _ceiling != nullptr ? "" : " (reported only: no ceiling holds them to it)"));
}

std::uint64_t PressureWatcher::HoldingBytes() const
{
    // The kernel's figure where there is one: an allowance below what compiles are resident in would have the
    // ceiling reclaim -- and kill -- at once, which is the shed's job, not the yield's.
    if (_ceiling != nullptr)
        if (auto const held = _ceiling->HeldBytes(); held.has_value())
            return *held;
    return _held.Held().bytes;
}

} // namespace Pace::Node
