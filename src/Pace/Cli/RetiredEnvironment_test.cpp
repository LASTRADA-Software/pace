// SPDX-License-Identifier: Apache-2.0
#include <Pace/Cli/RetiredEnvironment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using Pace::EnvironmentNameCase;
using Pace::RetiredPrefix;
using Pace::Testing::Unwrap;

namespace
{

/// The names a retired-name search found, as text.
std::vector<std::string_view> NamesOf(std::vector<Pace::RetiredName> const& retired)
{
    std::vector<std::string_view> names;
    names.reserve(retired.size());
    for (auto const& name: retired)
        names.push_back(name.name);
    return names;
}

/// @p text in lower case, built at run time so the old name is not spelled lower-case here.
std::string Lowered(std::string_view text)
{
    std::string lowered(text);
    std::ranges::transform(
        lowered, lowered.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return lowered;
}

} // namespace

TEST_CASE("RetiredEnvironment: a FASTCACHE_ name is noticed, and only one that starts with it", "[cli][retired-environment]")
{
    // MY_FASTCACHE_X CONTAINS the retired prefix without starting with it, and PACE_ADDR is the
    // spelling that replaced it: neither is a retired name.
    constexpr auto names = std::to_array<std::string_view>({ "PATH", "FASTCACHE_ADDR", "PACE_ADDR", "MY_FASTCACHE_X" });
    for (auto const nameCase: { EnvironmentNameCase::Sensitive, EnvironmentNameCase::Insensitive })
    {
        auto const retired = Pace::RetiredEnvironmentNames(names, Pace::RetiredEnvironmentPrefixes, nameCase);
        CHECK(NamesOf(retired) == std::vector<std::string_view> { "FASTCACHE_ADDR" });
    }
}

TEST_CASE("RetiredEnvironment: a name in another letter case is retired exactly where the host folds case",
          "[cli][retired-environment]")
{
    auto const lowered = Lowered("FASTCACHE_ADDR");
    auto const names = std::to_array<std::string_view>({ lowered, "PACE_ADDR" });

    CHECK(Pace::RetiredEnvironmentNames(names, Pace::RetiredEnvironmentPrefixes, EnvironmentNameCase::Sensitive).empty());
    auto const folded =
        Pace::RetiredEnvironmentNames(names, Pace::RetiredEnvironmentPrefixes, EnvironmentNameCase::Insensitive);
    CHECK(NamesOf(folded) == std::vector<std::string_view> { lowered });

    // The platform fact itself: Windows' environment block folds case and POSIX's does not.
#if defined(_WIN32)
    STATIC_CHECK(Pace::HostEnvironmentNameCase == EnvironmentNameCase::Insensitive);
#else
    STATIC_CHECK(Pace::HostEnvironmentNameCase == EnvironmentNameCase::Sensitive);
#endif
}

TEST_CASE("RetiredEnvironment: the notice names every retired variable and what it is called now",
          "[cli][retired-environment]")
{
    constexpr auto none = std::to_array<std::string_view>({ "PATH", "PACE_ADDR" });
    REQUIRE_FALSE(
        Pace::RetiredEnvironmentNotice(
            "pace-cc", Pace::RetiredEnvironmentNames(none, Pace::RetiredEnvironmentPrefixes, EnvironmentNameCase::Sensitive))
            .has_value());

    constexpr auto one = std::to_array<std::string_view>({ "FASTCACHE_ADDR" });
    CHECK(
        Unwrap(Pace::RetiredEnvironmentNotice(
            "pace-cc", Pace::RetiredEnvironmentNames(one, Pace::RetiredEnvironmentPrefixes, EnvironmentNameCase::Sensitive)))
        == "pace-cc: FASTCACHE_ADDR is no longer read; rename it to PACE_ADDR (docs/upgrading-to-pace.md)");

    constexpr auto two = std::to_array<std::string_view>({ "FASTCACHE_ADDR", "FASTCACHE_SCHEDULER" });
    CHECK(Unwrap(Pace::RetiredEnvironmentNotice(
              "pace-node",
              Pace::RetiredEnvironmentNames(two, Pace::RetiredEnvironmentPrefixes, EnvironmentNameCase::Sensitive)))
          == "pace-node: FASTCACHE_ADDR, FASTCACHE_SCHEDULER are no longer read; rename them to PACE_ADDR, "
             "PACE_SCHEDULER (docs/upgrading-to-pace.md)");

    // The owned-names entry point the binaries call is the same decision.
    auto const owned = std::vector<std::string> { "HOME", "FASTCACHE_ADDR" };
    CHECK(Unwrap(Pace::RetiredEnvironmentNoticeFor("pace-cc", owned))
          == "pace-cc: FASTCACHE_ADDR is no longer read; rename it to PACE_ADDR (docs/upgrading-to-pace.md)");
}

TEST_CASE("RetiredEnvironment: each name's replacement comes from the row it matched, not the first row",
          "[cli][retired-environment]")
{
    // Two rows, so a replacement taken from the table's FIRST row rather than the matched one
    // names OLDER_X wrongly.
    constexpr auto rows = std::to_array<RetiredPrefix>({
        RetiredPrefix { .retired = "OLDER_", .replacement = "NEWER_" },
        RetiredPrefix { .retired = "FASTCACHE_", .replacement = "PACE_" },
    });
    constexpr auto names = std::to_array<std::string_view>({ "FASTCACHE_ADDR", "OLDER_X" });
    auto const retired = Pace::RetiredEnvironmentNames(names, rows, EnvironmentNameCase::Sensitive);
    REQUIRE(retired.size() == 2);
    CHECK(retired[0].Replacement() == "PACE_ADDR");
    CHECK(retired[1].Replacement() == "NEWER_X");
    CHECK(Unwrap(Pace::RetiredEnvironmentNotice("pace-cc", retired))
          == "pace-cc: FASTCACHE_ADDR, OLDER_X are no longer read; rename them to PACE_ADDR, NEWER_X "
             "(docs/upgrading-to-pace.md)");
}
