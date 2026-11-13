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

/// A source reading `/proc/pressure/memory` and `<compiles>/memory.pressure`.
class PsiPressureSource final: public IMemoryPressureSource
{
  public:
    /// @param host The filesystem seam.
    /// @param compilesCgroup The compiles' cgroup directory, when one exists.
    PsiPressureSource(std::shared_ptr<ICgroupHost const> host, std::optional<std::filesystem::path> compilesCgroup);

    [[nodiscard]] std::expected<PressureSample, std::string> Sample() override;

  private:
    std::shared_ptr<ICgroupHost const> _host;
    std::optional<std::filesystem::path> _compiles;
};

} // namespace Pace::Platform
