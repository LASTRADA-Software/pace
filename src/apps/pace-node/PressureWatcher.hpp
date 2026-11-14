// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompileMemoryMeter.hpp"
#include "HeldCharges.hpp"
#include "MachineBudget.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "PressureGovernor.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/CgroupV2.hpp>
#include <Pace/Platform/MemoryPressure.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include <core/platform/Clock.hpp>

/// @file PressureWatcher.hpp
/// Layer C of the compile out-of-memory defence, wired: the thread that samples PSI, steps the governor and acts on
/// its answer -- the compile ceiling, the youngest-first kill, the machine report and the condition.
namespace Pace::Node
{

class INodeCapacitySource;
class INodeConfigSource;
class PresenceWake;

/// What the governor is configured with at one moment: its thresholds and the allowance it never exceeds.
struct PressureSettings
{
    PressureThresholds thresholds {}; ///< Where the levels change.
    std::uint64_t configuredBytes {}; ///< The compile ceiling in force; the allowance is never above it.
    std::uint64_t floorBytes {};      ///< The least pressure lowers the allowance to: one default compile estimate.

    [[nodiscard]] bool operator==(PressureSettings const&) const = default;
};

/// Where the watcher reads its settings at every sample: both move under a running node -- the two flags reload,
/// and the configured ceiling follows `--compile-memory-limit` and the cache reserve.
class IPressureSettings
{
  public:
    IPressureSettings() = default;
    IPressureSettings(IPressureSettings const&) = delete;
    IPressureSettings(IPressureSettings&&) = delete;
    IPressureSettings& operator=(IPressureSettings const&) = delete;
    IPressureSettings& operator=(IPressureSettings&&) = delete;
    virtual ~IPressureSettings() = default;

    /// @return The settings in force now.
    [[nodiscard]] virtual PressureSettings Current() const = 0;
};

/// The governor's thresholds a configuration asks for: `--memory-pressure-yield` and `--memory-pressure-shed` (each
/// `0` turning its level off), every other figure the governor's default.
///
/// Each exit threshold keeps the default's proportion to its entry -- yield leaves at half its entry, shed at a fifth
/// -- so a moved entry keeps the hysteresis band the defaults were chosen with rather than crossing a fixed exit.
/// @param cfg The configuration in force.
/// @return The thresholds.
[[nodiscard]] PressureThresholds MemoryPressureThresholdsOf(NodeConfig const& cfg) noexcept;

/// Production's settings: the configuration in force and the capacity, read at every call.
class LivePressureSettings final: public IPressureSettings
{
  public:
    /// @param inForce The configuration in force; outlives this.
    /// @param capacity What this machine is; outlives this.
    LivePressureSettings(INodeConfigSource const& inForce, INodeCapacitySource const& capacity) noexcept;

    /// @copydoc IPressureSettings::Current
    [[nodiscard]] PressureSettings Current() const override;

  private:
    INodeConfigSource const& _inForce;
    INodeCapacitySource const& _capacity;
};

/// Whether this build's platform has pressure stall information: Linux alone.
#if defined(__linux__)
inline constexpr bool ThisPlatformHasPsi = true;
#else
inline constexpr bool ThisPlatformHasPsi = false;
#endif

/// What the watcher borrows. All of it outlives the watcher.
struct PressureWatcherParts
{
    /// Where samples come from; owned by the watcher.
    std::unique_ptr<Platform::IMemoryPressureSource> source;
    /// The thresholds and configured ceiling, read at every sample.
    IPressureSettings const& settings;
    /// Holds compiles under the allowance; null where compiles run in no cgroup, and then the watcher changes only
    /// what the machine reports.
    ICompileCeiling* ceiling;
    /// Kills the youngest compile when the governor sheds; null where compiles run in no cgroup.
    Platform::IYoungestKiller* killer;
    /// What compiles on this machine are charged, for the allowance where the kernel's own figure is not to be had.
    IHeldCharges const& held;
    /// Where the level and allowance are published for the machine report, the heartbeat and the scrape.
    PublishedPressure& published;
    /// Ended at once on a level change, so the fleet's queue hears it within one wait, not one interval.
    PresenceWake& wake;
    /// Where `memory-pressure-shedding` is answered.
    NodeConditions& conditions;
    /// Where the time in each level and the entries into it are counted.
    IMetricsSink& metrics;
    /// Where a level change, and the compiles' pressure going unreadable, are said.
    ILogger& logger;
    /// The governor's dwell and ramp clock, and the time counted per level.
    core::platform::IClock const& clock;
    /// How often a sample is taken.
    std::chrono::milliseconds poll { std::chrono::seconds { 1 } };
    /// Whether this platform has PSI at all; the build's own except in a test. Where it has none the watcher is a
    /// stated no-op and never samples.
    bool platformHasPsi { ThisPlatformHasPsi };
};

/// The node's memory-pressure watcher: one sample per `poll`, on a thread of its own -- never a reactor, and never
/// the presence loop's, whose dial can block for ten seconds.
///
/// Each step: sample; step the governor with what compiles hold; write the allowance through the ceiling when it
/// moved; kill the youngest compile when the governor asks; on a level change, end the presence wait and say so once
/// (Warn entering `Shed`, Info otherwise); publish the level and the allowance; answer the condition; count the time.
///
/// Lives in the node process only: a restart starts calm at the configured allowance.
class PressureWatcher
{
  public:
    /// @param parts What the watcher borrows; `source` is taken.
    explicit PressureWatcher(PressureWatcherParts parts);

    ~PressureWatcher();

    PressureWatcher(PressureWatcher const&) = delete;
    PressureWatcher& operator=(PressureWatcher const&) = delete;
    PressureWatcher(PressureWatcher&&) = delete;
    PressureWatcher& operator=(PressureWatcher&&) = delete;

    /// Check that PSI can be read, answer the condition, publish calm, say how the watcher runs -- and, unless
    /// @p launch says not to, begin the thread.
    ///
    /// **Without PSI the watcher does not run** -- on another platform, or on a kernel that offers none: a stated
    /// no-op, the reason logged once at Info, the condition answered not evaluated (never raised), nothing published
    /// (absent, which no reader takes for calm), and the reason returned. Without a ceiling it runs on system PSI
    /// alone, changing only what the machine reports, and its start line says so.
    /// @param launch Whether to begin the thread; a test steps by hand instead.
    /// @return Nothing once running, or why the watcher cannot run here.
    [[nodiscard]] std::expected<void, std::string> Start(bool launch = true);

    /// One step, as the thread takes it every `poll`. Public so a case drives it with a manual clock and fakes.
    void StepOnce();

    /// Stop the thread, if it runs, and wait for it. Idempotent; the destructor calls it.
    void Stop();

  private:
    /// The thread's body.
    /// @param stop Requested by `Stop`.
    void Loop(std::stop_token const& stop);

    /// Say the compiles' pressure reading going unreadable, or readable again -- on the transition only.
    /// @param fault The sample's `compilesFault`.
    void NoteCompilesFault(std::optional<std::string> const& fault);

    /// Count the time since the previous step against the level held through it.
    /// @param now This step's time.
    void CountTime(core::platform::SteadyTimePoint now);

    /// Act on a level change: count it, wake the presence loop, say it, answer the condition.
    /// @param decision What the governor decided.
    /// @param sample What it decided on.
    void OnLevelChanged(GovernorDecision const& decision, Platform::PressureSample const& sample);

    /// Answer `memory-pressure-shedding` for @p decision.
    /// @param decision What the governor decided.
    void AnswerCondition(GovernorDecision const& decision);

    /// @return What compiles hold now: the larger of what they are charged (the budget's figure, local leases
    ///         included) and what `compiles/` is resident in where a ceiling reads it.
    [[nodiscard]] std::uint64_t HoldingBytes() const;

    std::unique_ptr<Platform::IMemoryPressureSource> _source;
    IPressureSettings const& _settings;
    ICompileCeiling* _ceiling;
    Platform::IYoungestKiller* _killer;
    IHeldCharges const& _held;
    PublishedPressure& _published;
    PresenceWake& _wake;
    NodeConditions& _conditions;
    IMetricsSink& _metrics;
    ILogger& _logger;
    core::platform::IClock const& _clock;
    std::chrono::milliseconds _poll;
    bool _platformHasPsi;

    PressureSettings _applied;                    ///< The settings the governor was last given.
    PressureGovernor _governor;                   ///< Stepped by the watcher's thread only.
    PressureLevel _level { PressureLevel::Calm }; ///< The level held since the last step.
    /// The lowered allowance last handed to the ceiling; disengaged for none, which is what the keeper starts with.
    std::optional<std::uint64_t> _allowanceWritten {};
    core::platform::SteadyTimePoint _counted; ///< Up to when the time per level has been counted.
    std::chrono::nanoseconds _uncounted {};   ///< The part of a second not yet counted; carried across a change.
    bool _compilesFaultSaid { false };        ///< The compiles' reading is said to be unreadable.
    bool _sampleFaultSaid { false };          ///< A failed system sample is said.

    /// **Declared LAST**: the body touches every member above, so they are built before it starts and destroyed
    /// only after it has been stopped and joined.
    std::jthread _thread;
};

} // namespace Pace::Node
