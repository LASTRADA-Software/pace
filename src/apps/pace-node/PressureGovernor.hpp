// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Platform/MemoryPressure.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

#include <core/platform/Clock.hpp>

/// Layer C of the compile out-of-memory defence: turning memory-pressure samples into what the node does
/// about them. A pure state machine over `Platform::PressureSample` and an injected clock -- it reads no
/// file, kills nothing and sends nothing; the watcher that drives it (and acts on its answer) is elsewhere.
namespace Pace::Node
{

/// The level the governor reports. Transmitted, so it lives on the wire (`CompileCacheWire`).
using PressureLevel = CompileCacheWire::PressureLevel;

/// Where the governor changes level, and how fast the allowance comes back.
///
/// `*Enter` / `*Exit` are PSI `avg10` percentages: yield on the system's `some` line, shed on its `full`
/// line. Each level has an exit threshold below its entry one, and a minimum `dwell` before it may be
/// left, so a figure hovering at a threshold cannot make the level oscillate.
///
/// **Why the defaults sit so far below systemd-oomd's.** oomd (Fedora's default) kills a whole user
/// slice once its memory pressure has stayed above 50-80% for 20 s -- and that slice is the desktop, the
/// game and the editor, not merely the compiles. The node must act FIRST: a 10% `some` stall over 10 s is
/// already a desktop that stutters, and 5% `full` (every non-idle task stalled at once) is one that
/// freezes. Yielding at those levels costs only admissions the fleet can place elsewhere; acting at
/// oomd's figures would mean acting after oomd has already chosen a victim. The 10 s dwell matches PSI's
/// own `avg10` window, so a level is held for at least as long as the figure that raised it describes.
///
/// **Why the ramp is 5% per 30 s.** After pressure ends the allowance starts at what compiles held then
/// -- the work that demonstrably fits beside whatever else is running -- and climbs back to its configured
/// value in ten minutes. Slow enough that a game still running is not pushed back into a stall by a
/// refill (the next yield would reset it, which is the shed/refill cycle this avoids); fast enough that
/// a machine whose game ended is back to full capacity within one long build.
struct PressureThresholds
{
    double yieldEnter { 10.0 };               ///< System `some` avg10 above which a foreign stall yields.
    double yieldExit { 5.0 };                 ///< System `some` avg10 below which `Yield` may end.
    double shedEnter { 5.0 };                 ///< System `full` avg10 at or above which the node sheds.
    double shedExit { 1.0 };                  ///< System `full` avg10 below which `Shed` may end.
    double ownShare { 0.8 };                  ///< Compiles' share of the stall at which it is their own.
    std::chrono::seconds dwell { 10 };        ///< Minimum time in a level; one shed kill per this period.
    std::chrono::seconds rampInterval { 30 }; ///< How often a calm allowance grows.
    std::uint32_t rampPercent { 5 };          ///< Of the configured bytes, per `rampInterval`.
};

/// What the governor decided on one sample.
struct GovernorDecision
{
    PressureLevel level;          ///< The level after this sample.
    std::uint64_t allowanceBytes; ///< What compiles may hold at once now; never above the configured bytes.
    bool killYoungest;            ///< Kill the youngest running compile now.
    bool levelChanged;            ///< This sample moved the level.
};

/// The pressure state machine: Calm, Yield, Shed, with hysteresis, dwell and an adaptive allowance.
///
/// - **A stall that is the compiles' own is not a yield.** System `some` above `yieldEnter` counts only
///   when the compiles' cgroup's `some` is below `ownShare` of it, or there is no compiles reading.
///   Otherwise it is the ceiling working, and the level stays `Calm`.
/// - **Shed** on system `full` at or above `shedEnter`, whoever stalls, or once `Yield` has been held for
///   twice `dwell` with system `some` still above `yieldEnter`. While shedding, `killYoungest` is true at
///   most once per `dwell` -- and never when compiles hold nothing, since there is nothing to kill.
/// - **Leaving a level** needs its figure below the exit threshold AND `dwell` since entering it. Leaving
///   `Shed` also needs system `some` no longer above `yieldEnter` -- the figure a held yield sheds on --
///   and lands in `Yield` unless that figure is already below `yieldExit`.
/// - **The allowance**: entering `Yield` or `Shed` sets it to what compiles hold (never raising it); while
///   `Calm` it grows by `rampPercent` of the configured bytes per `rampInterval`, never past them.
///
/// Lives in the node process only: never replicated, never persisted, so a restart starts calm at the
/// configured allowance. Not thread-safe; one watcher thread steps it.
class PressureGovernor
{
  public:
    /// @param thresholds The levels' thresholds, dwell and ramp.
    /// @param configuredBytes The configured compile allowance, the ceiling the allowance never exceeds.
    /// @param clock The time source for dwell and ramp.
    PressureGovernor(PressureThresholds thresholds, std::uint64_t configuredBytes, core::platform::IClock const& clock);

    /// Read one sample.
    /// @param sample The machine's pressure, and the compiles' when there is a compiles cgroup.
    /// @param compilesHoldBytes What the running compiles hold now.
    /// @return The level, the allowance, and whether to kill the youngest compile.
    [[nodiscard]] GovernorDecision Step(Platform::PressureSample const& sample, std::uint64_t compilesHoldBytes);

    /// Change the configured allowance. Lowering it clamps the allowance; raising it never raises the
    /// allowance, which ramps toward the new value from where it stands.
    /// @param configuredBytes The new configured allowance.
    void Reconfigure(std::uint64_t configuredBytes);

  private:
    /// Move to @p level at @p now, setting the allowance from what compiles hold.
    void Enter(PressureLevel level, core::platform::SteadyTimePoint now, std::uint64_t compilesHoldBytes);

    /// Grow a calm allowance by every ramp interval elapsed since the last growth.
    void Ramp(core::platform::SteadyTimePoint now);

    /// @return One ramp step: `rampPercent` of the configured bytes, without overflowing.
    [[nodiscard]] std::uint64_t RampStepBytes() const noexcept;

    PressureThresholds _thresholds;
    std::uint64_t _configuredBytes;
    core::platform::IClock const& _clock;
    PressureLevel _level = PressureLevel::Calm;
    std::uint64_t _allowanceBytes;
    core::platform::SteadyTimePoint _enteredAt;
    core::platform::SteadyTimePoint _rampAnchor;
    std::optional<core::platform::SteadyTimePoint> _lastKill;
};

} // namespace Pace::Node
