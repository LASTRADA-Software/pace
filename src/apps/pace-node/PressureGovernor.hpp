// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Platform/MemoryPressure.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

#include <core/platform/Clock.hpp>

/// Layer C of the compile out-of-memory defence: turning memory-pressure samples into what the node does
/// about them. A pure state machine over `Platform::PressureSample` and an injected clock -- it reads no
/// file, kills nothing and sends nothing; the watcher that drives it (and acts on its answer) is elsewhere.
namespace Pace::Node
{

/// The level the governor reports. Transmitted, so it lives on the wire (`CompileCacheWire`).
using PressureLevel = CompileCacheWire::PressureLevel;

/// The threshold that turns a level off: no PSI figure (a percentage) ever exceeds it. A disabled `shedEnter` is
/// also never reached by escalating a held yield, so `--memory-pressure-shed=0` means the node kills nothing.
inline constexpr double DisabledPressureThreshold = std::numeric_limits<double>::infinity();

/// Where the governor changes level, and how fast the allowance comes back.
///
/// `*Enter` / `*Exit` are PSI `avg10` percentages: yield on the system's `some` line, shed on its `full`
/// line. Each level has an exit threshold below its entry one, and a minimum `dwell` before it may be
/// left, so a figure hovering at a threshold cannot make the level oscillate.
///
/// **Why the defaults sit so far below systemd-oomd's.** oomd (Fedora's default) kills a whole cgroup of
/// `system.slice` or a user slice once its memory pressure has stayed above 80% for 20 s (Fedora 44's
/// per-slice defaults) -- and under the user unit that slice is the desktop, the game and the editor, not
/// merely the compiles. The node must act FIRST: a 10% `some` stall over 10 s is
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

    [[nodiscard]] bool operator==(PressureThresholds const&) const = default;
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
///   when the compiles' cgroup's `some` is below `ownShare` of it, or there is no compiles reading, or the
///   compiles have not stood at their own ceiling (`PressureSample::compilesAtLimit`) within the last `dwell`
///   -- sticky, because that evidence covers one second and the PSI averages ten. Otherwise it is the
///   ceiling working, and the level stays `Calm`. Compiles far below their limit that stall with the
///   machine are stalling on someone else's shortage -- the machine reclaims from everyone -- so that yields.
/// - **Shed** on system `full` at or above `shedEnter`, whoever stalls, or from `Yield` once a FOREIGN
///   stall (above) has held for twice `dwell` without a break: its clock restarts whenever it drops, so a
///   yield that eased and then returns is not shed at once. While shedding, `killYoungest` is true at most
///   once per `dwell` -- and never when compiles hold nothing, since there is nothing to kill.
/// - **Leaving a level** needs its figure below the exit threshold AND `dwell` since entering it, and an
///   own stall counts as below: compiles stalling against the ceiling a yield just set hold the node in
///   no level. That holds for `full` too: entering `Shed` on `full` asks nobody whose stall it is, but
///   leaving it counts `full` as eased below `shedExit` OR when the compiles' own `full` is at least
///   `ownShare` of the system's (and they stand at their ceiling) -- never when the compiles' reading or its
///   `full` line is absent. Leaving `Shed` also needs no foreign stall above `yieldEnter`, and lands in `Yield`
///   unless the stall has already eased below `yieldExit` or is the compiles' own.
/// - **A shed left on an own full stall LATCHES**: while that stall persists, `full` at or above `shedEnter`
///   does not re-enter `Shed`. The latch lifts once the full stall turns foreign or falls below `shedExit`,
///   and entry is "regardless of whose" again from then -- otherwise one own stall flaps `Shed` and `Calm`
///   every dwell and kills a compile at each entry.
/// - **The allowance**: entering `Yield` or `Shed` sets it to what compiles hold (never raising it), but never
///   below the floor -- one default compile estimate, itself never above the configured bytes. A machine whose
///   compiles held next to nothing when pressure began would otherwise report a budget no compile fits, and the
///   queue would refuse everything for the whole ramp. While `Calm` it grows by `rampPercent` of the configured
///   bytes per `rampInterval`, never past them.
///
/// Lives in the node process only: never replicated, never persisted, so a restart starts calm at the
/// configured allowance. Not thread-safe; one watcher thread steps it.
class PressureGovernor
{
  public:
    /// @param thresholds The levels' thresholds, dwell and ramp.
    /// @param configuredBytes The configured compile allowance, the ceiling the allowance never exceeds.
    /// @param floorBytes The least a yield or shed lowers the allowance to (one default compile estimate); taken
    ///                   as the configured bytes where it is above them.
    /// @param clock The time source for dwell and ramp.
    PressureGovernor(PressureThresholds thresholds,
                     std::uint64_t configuredBytes,
                     std::uint64_t floorBytes,
                     core::platform::IClock const& clock);

    /// Read one sample.
    /// @param sample The machine's pressure, and the compiles' when there is a compiles cgroup.
    /// @param compilesHoldBytes What the running compiles hold now.
    /// @return The level, the allowance, and whether to kill the youngest compile.
    [[nodiscard]] GovernorDecision Step(Platform::PressureSample const& sample, std::uint64_t compilesHoldBytes);

    /// Change the configured allowance and its floor. Lowering the configured bytes clamps the allowance. Raising
    /// them raises an allowance that stands AT the old configured value -- never lowered by pressure, or fully
    /// ramped back -- at once; one a yield or shed lowered stays where it stands (lifted to a raised floor) and
    /// ramps toward the new value.
    /// @param configuredBytes The new configured allowance.
    /// @param floorBytes The new floor; taken as the configured bytes where it is above them.
    void Reconfigure(std::uint64_t configuredBytes, std::uint64_t floorBytes);

    /// Change the thresholds, dwell and ramp from the next sample on. The level held and its clocks stay: a
    /// reload moves where the next change happens, never the state the machine is in.
    /// @param thresholds The new thresholds.
    void Retune(PressureThresholds const& thresholds);

    /// @return The allowance now, as the last `Step` or `Reconfigure` left it.
    [[nodiscard]] std::uint64_t AllowanceBytes() const noexcept;

  private:
    /// Move to @p level at @p now, setting the allowance from what compiles hold.
    void Enter(PressureLevel level, core::platform::SteadyTimePoint now, std::uint64_t compilesHoldBytes);

    /// Grow a calm allowance by every ramp interval elapsed since the last growth.
    void Ramp(core::platform::SteadyTimePoint now);

    /// @return One ramp step: `rampPercent` of the configured bytes, without overflowing.
    [[nodiscard]] std::uint64_t RampStepBytes() const noexcept;

    /// @return The floor in force: the configured one, never above the configured bytes.
    [[nodiscard]] std::uint64_t FloorBytes() const noexcept;

    PressureThresholds _thresholds;
    std::uint64_t _configuredBytes;
    std::uint64_t _floorBytes; ///< As configured; `FloorBytes()` caps it at `_configuredBytes`.
    core::platform::IClock const& _clock;
    PressureLevel _level = PressureLevel::Calm;
    std::uint64_t _allowanceBytes;
    core::platform::SteadyTimePoint _enteredAt;
    core::platform::SteadyTimePoint _rampAnchor;
    std::optional<core::platform::SteadyTimePoint> _lastKill;
    std::optional<core::platform::SteadyTimePoint> _foreignStallSince; ///< Since when the foreign stall has held.
    bool _ownFreezeLatched { false }; ///< A shed was left on the compiles' own full stall, which has not lifted since.
    /// When the compiles were last seen at their own ceiling; counts as evidence for one `dwell` after.
    std::optional<core::platform::SteadyTimePoint> _atLimitSeen;
};

} // namespace Pace::Node
