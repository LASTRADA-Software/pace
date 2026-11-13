// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

/// @file OomVictimTestSupport.hpp
/// What the OOM-victim tests share across the launcher's and the node's test binaries: where the test shim is, and
/// how a spawned child's score is read back. Linux only, as the shim is.
namespace Pace::Cc::OomVictimTesting
{

#if defined(__linux__)

/// The shell line the property is read through: the compiler's own score, then that of a child it starts at once.
inline constexpr auto ReadScoreTwice = R"(cat /proc/self/oom_score_adj; sh -c "cat /proc/self/oom_score_adj")";

/// The path of the test shim, which the build places beside every test executable that spawns through it
/// (`oom-victim-exec-shim`; `src/apps/pace-cc/CMakeLists.txt` says why it is not named through the test
/// environment).
/// @return The path; the case is skipped when the shim was not built there.
[[nodiscard]] inline std::string ShimPath()
{
    auto failure = std::error_code {};
    auto const self = std::filesystem::read_symlink("/proc/self/exe", failure);
    if (failure)
        SKIP("/proc/self/exe cannot be read, so the shim beside this executable cannot be found");
    auto const shim = self.parent_path() / "oom-victim-exec-shim";
    if (!std::filesystem::is_regular_file(shim, failure))
        SKIP("oom-victim-exec-shim was not built beside " + self.string());
    return shim.string();
}

/// This process's own OOM score adjustment, as its children inherit it.
/// @return The value's text, without the newline.
[[nodiscard]] inline std::string OwnScore()
{
    std::ifstream file { "/proc/self/oom_score_adj" };
    auto text = std::string {};
    std::getline(file, text);
    return text;
}

/// The lines of @p text.
/// @param text What a child printed.
/// @return Each line, without its newline.
[[nodiscard]] inline std::vector<std::string> LinesOf(std::string const& text)
{
    auto stream = std::istringstream { text };
    auto lines = std::vector<std::string> {};
    auto line = std::string {};
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

#endif

} // namespace Pace::Cc::OomVictimTesting
