// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pace
{

/// One retired spelling: a prefix nothing reads any more, and the one that replaced it.
struct RetiredPrefix
{
    std::string_view retired;     ///< e.g. "FASTCACHE_"
    std::string_view replacement; ///< e.g. "PACE_"
};

/// The environment prefixes the rename to Pace retired (#1713 removes this table in 0.8.0).
///
/// Not a bridge: no value under a retired name is read. A retired name is only NOTICED, so
/// that a build still exporting `FASTCACHE_ADDR` is told it no longer distributes rather
/// than silently compiling alone.
inline constexpr std::array RetiredEnvironmentPrefixes { RetiredPrefix { .retired = "FASTCACHE_", .replacement = "PACE_" } };

/// Whether two environment variable names that differ only in letter case name one variable.
/// Private to this process: never transmitted, never persisted.
enum class EnvironmentNameCase : std::uint8_t
{
    Sensitive,   ///< POSIX: `Path` and `PATH` are two variables.
    Insensitive, ///< Windows: the environment block folds case, so `Path` IS `PATH`.
};

/// This host's answer, as a platform fact rather than a condition at each comparison: on
/// Windows a build exporting the old address variable in lower case has set `FASTCACHE_ADDR`,
/// and a notice that missed it would be the silence it exists to end.
inline constexpr EnvironmentNameCase HostEnvironmentNameCase =
#if defined(_WIN32)
    EnvironmentNameCase::Insensitive;
#else
    EnvironmentNameCase::Sensitive;
#endif

/// A name that starts with a retired prefix, and the row it matched.
struct RetiredName
{
    std::string_view name; ///< The variable as the environment spells it.
    RetiredPrefix row;     ///< The row whose `retired` prefix it starts with.

    /// What the variable is called now: the matched row's replacement, then the rest of the name.
    /// @return e.g. `PACE_ADDR` for `FASTCACHE_ADDR`.
    [[nodiscard]] std::string Replacement() const
    {
        return std::format("{}{}", row.replacement, name.substr(row.retired.size()));
    }
};

/// Whether @p name starts with @p prefix under @p nameCase. ASCII folding only, because every
/// retired prefix is ASCII and so is every name that can match one.
/// @param name A variable name.
/// @param prefix A retired prefix.
/// @param nameCase How the host compares names.
/// @return True when the name starts with the prefix.
[[nodiscard]] constexpr bool StartsWithRetiredPrefix(std::string_view name,
                                                     std::string_view prefix,
                                                     EnvironmentNameCase nameCase) noexcept
{
    if (nameCase == EnvironmentNameCase::Sensitive)
        return name.starts_with(prefix);
    auto const fold = [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    };
    return name.size() >= prefix.size() && std::ranges::equal(name.substr(0, prefix.size()), prefix, {}, fold, fold);
}

/// The names that start with a retired prefix.
///
/// Header-only and pure -- no environment access inside -- because both `pace-cc`, which
/// does not link the Pace library, and `pace-node` ask it, each over the names its own
/// seam read (`EnvironmentVariableNames`).
/// @param names Every environment variable NAME this process sees.
/// @param rows The retired prefixes; `RetiredEnvironmentPrefixes` outside a test.
/// @param nameCase How the host compares names; `HostEnvironmentNameCase` outside a test.
/// @return The names that start with a retired prefix, in input order, each with its row.
[[nodiscard]] inline std::vector<RetiredName> RetiredEnvironmentNames(std::span<std::string_view const> names,
                                                                      std::span<RetiredPrefix const> rows,
                                                                      EnvironmentNameCase nameCase)
{
    std::vector<RetiredName> retired;
    for (auto const name: names)
        for (auto const& row: rows)
            if (StartsWithRetiredPrefix(name, row.retired, nameCase))
            {
                retired.push_back(RetiredName { .name = name, .row = row });
                break; // the first row that matches is the name's row
            }
    return retired;
}

/// The one stderr line a process prints while retired names are set.
/// @param program The program's name, e.g. `pace-cc`.
/// @param retired What `RetiredEnvironmentNames` returned.
/// @return The line, naming each variable and what it is called now, or a disengaged
///         optional when nothing retired is set.
[[nodiscard]] inline std::optional<std::string> RetiredEnvironmentNotice(std::string_view program,
                                                                         std::span<RetiredName const> retired)
{
    if (retired.empty())
        return std::nullopt;
    auto listed = std::string {};
    auto renamed = std::string {};
    for (auto const& name: retired)
    {
        std::string_view const separator = listed.empty() ? "" : ", ";
        listed += std::format("{}{}", separator, name.name);
        renamed += std::format("{}{}", separator, name.Replacement());
    }
    return std::format("{}: {} {} no longer read; rename {} to {} (docs/upgrading-to-pace.md)",
                       program,
                       listed,
                       retired.size() == 1 ? "is" : "are",
                       retired.size() == 1 ? "it" : "them",
                       renamed);
}

/// `RetiredEnvironmentNames` then `RetiredEnvironmentNotice`, over owned names as
/// `EnvironmentVariableNames` returns them, against this build's table and this host's case.
/// @param program The program's name.
/// @param names Every environment variable name.
/// @return The line to print, or a disengaged optional.
[[nodiscard]] inline std::optional<std::string> RetiredEnvironmentNoticeFor(std::string_view program,
                                                                            std::span<std::string const> names)
{
    std::vector<std::string_view> const views(names.begin(), names.end());
    auto const retired = RetiredEnvironmentNames(views, RetiredEnvironmentPrefixes, HostEnvironmentNameCase);
    return RetiredEnvironmentNotice(program, retired);
}

} // namespace Pace
