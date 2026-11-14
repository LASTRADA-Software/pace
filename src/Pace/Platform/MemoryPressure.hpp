// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Platform/CgroupV2.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/// Memory pressure stall information (PSI): how much of the last seconds the machine, or the compiles'
/// cgroup, spent stalled waiting for memory. Linux only; read through `ICgroupHost`, so it runs against
/// a fake in a unit test. On a kernel without PSI the source reports an error, never a calm reading.
namespace Pace::Platform
{

/// The system-wide PSI file.
inline constexpr std::string_view PsiSystemPath = "/proc/pressure/memory";

/// The per-cgroup PSI file, relative to a cgroup directory.
inline constexpr std::string_view PsiCgroupFileName = "memory.pressure";

/// How close to its `memory.max` a cgroup's `memory.current` counts as AT it, as a divisor of the ceiling: within a
/// twentieth (5%). A cgroup reclaiming against its ceiling hovers just under it; one well below it is not stalling
/// against it, whatever its stall says.
inline constexpr std::uint64_t CeilingNearDivisor = 20;

/// One `some`/`full` line of a PSI file.
struct PsiLine
{
    double avg10 = 0.0;            ///< Percent of the last 10 s stalled.
    double avg60 = 0.0;            ///< Percent of the last 60 s stalled.
    std::uint64_t totalMicros = 0; ///< Cumulative stall, microseconds.
};

/// A whole PSI file.
struct PsiReading
{
    PsiLine some {};                ///< Some tasks stalled.
    std::optional<PsiLine> full {}; ///< All tasks stalled; absent in some files on older kernels.
};

/// Parse a PSI document.
/// @param document The file's text.
/// @return The reading, or a message naming the line that could not be read.
[[nodiscard]] std::expected<PsiReading, std::string> ParsePsi(std::string_view document);

/// One look at the machine's memory pressure.
struct PressureSample
{
    PsiReading system {};                  ///< The whole machine.
    std::optional<PsiReading> compiles {}; ///< The compiles' cgroup, when there is one.

    /// Why `compiles` is absent although a compiles cgroup was configured. Disengaged when no compiles
    /// path was configured or the read succeeded, so "not configured" and "unreadable" stay distinct.
    std::optional<std::string> compilesFault {};

    /// Whether the compiles' cgroup is at its own ceiling now: its `memory.events` `max` count rose since the
    /// previous sample, or its `memory.current` stands within `1 / CeilingNearDivisor` of a numeric `memory.max`.
    ///
    /// The evidence that a stall the compiles carry is the CEILING working rather than the machine running short:
    /// compiles far below their limit stall only because the machine reclaims from everything, a game's shortage
    /// included. False where no ceiling is set (`max`) and nothing rose. DISENGAGED where no compiles cgroup is
    /// configured, or where none of it can be read (`compilesLimitFault` says why) -- not-at-limit and could-not-tell
    /// are two answers, though a reader counts both as nobody's own stall, the safe direction (a yield).
    std::optional<bool> compilesAtLimit {};

    /// Why `compilesAtLimit` is disengaged although a compiles cgroup was configured: neither its `memory.events` nor
    /// its `memory.current` and `memory.max` could be read. Disengaged when no compiles path was configured or a
    /// reading was had.
    std::optional<std::string> compilesLimitFault {};
};

/// Where pressure samples come from.
class IMemoryPressureSource
{
  public:
    IMemoryPressureSource() = default;
    IMemoryPressureSource(IMemoryPressureSource const&) = delete;
    IMemoryPressureSource(IMemoryPressureSource&&) = delete;
    IMemoryPressureSource& operator=(IMemoryPressureSource const&) = delete;
    IMemoryPressureSource& operator=(IMemoryPressureSource&&) = delete;
    virtual ~IMemoryPressureSource() = default;

    /// Take a sample.
    /// @return The sample, or why it could not be taken.
    [[nodiscard]] virtual std::expected<PressureSample, std::string> Sample() = 0;
};

/// A source reading `/proc/pressure/memory` and `<compiles>/memory.pressure`, and whether the compiles stand at
/// their ceiling from `<compiles>/memory.events`, `memory.current` and `memory.max`.
class PsiPressureSource final: public IMemoryPressureSource
{
  public:
    /// @param host The filesystem seam.
    /// @param compilesCgroup The compiles' cgroup directory, when one exists.
    PsiPressureSource(std::shared_ptr<ICgroupHost const> host, std::optional<std::filesystem::path> compilesCgroup);

    [[nodiscard]] std::expected<PressureSample, std::string> Sample() override;

  private:
    /// @param compiles The compiles' cgroup directory.
    /// @return Whether the compiles stand at their ceiling now, or why that cannot be read; updates the `max` count.
    [[nodiscard]] std::expected<bool, std::string> CompilesAtLimit(std::filesystem::path const& compiles);

    std::shared_ptr<ICgroupHost const> _host;
    std::optional<std::filesystem::path> _compiles;
    /// The compiles' `memory.events` `max` count at the previous sample; disengaged until one was read, so the
    /// first sample never reads as a rise.
    std::optional<std::uint64_t> _maxEventsSeen {};
};

} // namespace Pace::Platform
