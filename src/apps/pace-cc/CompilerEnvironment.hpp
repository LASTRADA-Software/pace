// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pace::Cc
{

/// The environment variables a compiler driver reads as part of its INPUT, and what the
/// launcher does about each (#1693, #1756).
///
/// ## Why the environment is an input at all
///
/// A key is a function of what the compiler reads, and a driver reads more than its command
/// line. `cl` prepends `CL` and appends `_CL_` to its arguments, so `CL=/O2` and `CL=/Od`
/// produce different objects from one command line; clang's `CCC_OVERRIDE_OPTIONS` edits the
/// argument list outright; `COMPILER_PATH` and `GCC_EXEC_PREFIX` choose which `cc1` and `as`
/// a GNU driver runs. None of them reaches the key, so two builds differing only there SHARE a
/// key, and the second is served the first's object under a zero exit code.
///
/// ## Two answers, and why the stronger one is a refusal
///
/// - **`ChangesTheCompile`** — the variable adds arguments or picks the programs that run.
///   Such a compile is **not cached**, and that is a deliberate choice over folding the value
///   into the key: an argument arriving through the environment is invisible to every check the
///   launcher makes of the command line (`CL=/Zi` writes a shared PDB that a hit cannot
///   reproduce; the dispatch allowlist never sees it), and a WORKER does not inherit the
///   client's environment, so a dispatched compile would build without it and file the result
///   under the client's key. Keying the value closes the first hazard and neither of the others.
///   The refusal costs a local compile and names the variable.
/// - **`SearchesIncludes`** — the variable decides only where headers are FOUND (`INCLUDE`,
///   `CPATH`, ...). The preprocessed key already covers it, because it hashes the headers'
///   contents; the direct-mode MANIFEST does not, since its key is computed before any header
///   is opened. Such a variable is folded into the manifest key by name and value, so changing
///   it is a manifest miss and the ordinary preprocessed key then decides (#1756).
///
/// Unset and EMPTY are one answer: a driver reading an empty `CL` adds nothing.
///
/// Private to the launcher, never transmitted or persisted, so its enumerators carry no
/// explicit values.
enum class EnvironmentEffect : std::uint8_t
{
    ChangesTheCompile, ///< Adds arguments or selects subprograms: the compile is not cached.
    SearchesIncludes,  ///< Moves include resolution only: folded into the manifest key.
};

/// One variable a driver reads, which drivers read it, and what it does.
struct CompilerEnvironmentRow
{
    std::string_view name;    ///< The variable, spelled exactly as the driver reads it.
    std::uint8_t flavors {};  ///< Bit set of `FlavorBit` values naming the drivers that read it.
    EnvironmentEffect effect; ///< What the launcher does when it is set.
};

/// The bit a `Flavor` occupies in `CompilerEnvironmentRow::flavors`.
/// @param flavor The driver flavour.
/// @return A single-bit mask; `Flavor::Unknown` maps to no bit.
[[nodiscard]] constexpr std::uint8_t FlavorBit(Flavor flavor) noexcept
{
    return flavor == Flavor::Unknown ? std::uint8_t { 0 } : static_cast<std::uint8_t>(1U << static_cast<unsigned>(flavor));
}

/// Every variable a supported driver reads as compile input. Adding one is adding a row.
/// @return The table, in no meaningful order.
[[nodiscard]] std::span<CompilerEnvironmentRow const> CompilerEnvironmentTable() noexcept;

/// Reads one environment variable; `std::nullopt` when it is unset. Injected so the decision is
/// testable without mutating the process environment.
using EnvironmentReader = std::function<std::optional<std::string>(std::string_view)>;

/// What the environment contributes to one compile.
struct CompilerEnvironment
{
    /// The first variable that changes the compile, when one is set; such a compile is not cached.
    std::optional<std::string_view> changesTheCompile;

    /// `NAME=value` for every include-search variable that is set, in table order, for the
    /// manifest key. Empty when none is set, so a build that sets none keys exactly as before.
    std::vector<std::string> includeSearch;

    [[nodiscard]] friend bool operator==(CompilerEnvironment const&, CompilerEnvironment const&) = default;
};

/// Read the rows of `CompilerEnvironmentTable()` that apply to @p flavor.
/// @param flavor The driver about to run, after any banner correction.
/// @param read   The environment.
/// @return What the environment contributes; see `CompilerEnvironment`.
[[nodiscard]] CompilerEnvironment ReadCompilerEnvironment(Flavor flavor, EnvironmentReader const& read);

} // namespace Pace::Cc
