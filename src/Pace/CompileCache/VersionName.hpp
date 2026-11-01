// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <string_view>

/// @file VersionName.hpp
/// Which text is a version, asked by the launcher and by the scheduler alike.
///
/// **Header-only and dependency-free**, because `pace-cc` does not link the `Pace` library and includes this the
/// way it includes `CompileCacheWire.hpp`. One predicate rather than one per side: the launcher asks it which SDK
/// subdirectory is a kit version, which `gcc-` suffix is a version and which banner token is the version a label
/// carries; the upgrade report asks it which suffix to drop from a driver name. Two spellings would be two sets of
/// rules for what counts as a version, and `gcc-ar` would one day be a version of `gcc` on one side only.

namespace Pace::CompileCache
{

/// Whether a name is made only of digits and dots, with at least one digit.
///
/// One predicate for every question that must answer alike: which subdirectory of a Windows SDK's `Include` is a
/// kit version (`10.0.26100.0` yes, `KitsRoot10` and the GUID-named values no), which suffix on a compiler name is
/// a version (`gcc-13` yes, `gcc-ar` no), and which banner token `Cc::ToolchainLabel` reports as the version.
/// @param name The candidate.
/// @return True when it could be a version.
[[nodiscard]] constexpr bool LooksLikeVersion(std::string_view name) noexcept
{
    return !name.empty() && std::ranges::any_of(name, [](char c) { return c >= '0' && c <= '9'; })
           && std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

/// A driver name with its version suffix dropped: `gcc-13` is `gcc`, `g++-14` is `g++`,
/// `powerpc64le-linux-gnu-g++-14` is `powerpc64le-linux-gnu-g++` -- while `gcc-ar` stays `gcc-ar`, because `ar` is
/// not a version (`LooksLikeVersion`).
///
/// What the upgrade report calls the same DRIVER: two installs of one family under one driver name, whatever
/// version each carries.
/// @param tool A normalized compiler name, as a toolchain label's first token carries it.
/// @return The name without a trailing `-<version>`; @p tool unchanged when it has none.
[[nodiscard]] constexpr std::string_view DriverWithoutVersion(std::string_view tool) noexcept
{
    auto const dash = tool.rfind('-');
    if (dash == std::string_view::npos || dash == 0 || !LooksLikeVersion(tool.substr(dash + 1)))
        return tool;
    return tool.substr(0, dash);
}

static_assert(DriverWithoutVersion("gcc-13") == "gcc");
static_assert(DriverWithoutVersion("g++-14") == "g++");
static_assert(DriverWithoutVersion("clang-20") == "clang");
static_assert(DriverWithoutVersion("gcc-ar") == "gcc-ar");
static_assert(DriverWithoutVersion("gcc") == "gcc");
static_assert(DriverWithoutVersion("clang-cl") == "clang-cl");
static_assert(DriverWithoutVersion("powerpc64le-linux-gnu-g++-14") == "powerpc64le-linux-gnu-g++");

} // namespace Pace::CompileCache
