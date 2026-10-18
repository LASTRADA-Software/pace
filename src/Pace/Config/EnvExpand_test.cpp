// SPDX-License-Identifier: Apache-2.0
#include <Pace/Config/EnvExpand.hpp>
#include <Pace/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using Pace::ConfigErrorCode;
using Pace::ExpandEnvironmentVariables;
using Pace::Testing::ScopedEnv;

TEST_CASE("EnvExpand: input without references is returned unchanged", "[config][env]")
{
    auto const r = ExpandEnvironmentVariables("/var/lib/pace-kv/cache", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "/var/lib/pace-kv/cache");
}

TEST_CASE("EnvExpand: bare $NAME ends at the first non-name character", "[config][env]")
{
    ScopedEnv const env { "PACE_TEST_ROOT", "C:/ProgramData" };

    auto const r = ExpandEnvironmentVariables("$PACE_TEST_ROOT/pace/cache", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "C:/ProgramData/pace/cache");
}

TEST_CASE("EnvExpand: braced ${NAME} allows names the bare form cannot spell", "[config][env]")
{
    ScopedEnv const env { "PACE_TEST_ODD", "/opt" };

    // The brace form is what makes Windows' ProgramFiles(x86) expressible.
    auto const r = ExpandEnvironmentVariables("${PACE_TEST_ODD}/pace", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "/opt/pace");
}

TEST_CASE("EnvExpand: adjacent references concatenate", "[config][env]")
{
    ScopedEnv const a { "PACE_TEST_A", "one" };
    ScopedEnv const b { "PACE_TEST_B", "two" };

    auto const r = ExpandEnvironmentVariables("${PACE_TEST_A}${PACE_TEST_B}", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "onetwo");
}

TEST_CASE("EnvExpand: $$ is a literal dollar", "[config][env]")
{
    auto const r = ExpandEnvironmentVariables("/tmp/a$$b", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "/tmp/a$b");
}

TEST_CASE("EnvExpand: a set-but-empty variable expands to empty", "[config][env]")
{
    // Distinct from unset: an operator who exported an empty value meant it.
#if !defined(_WIN32)
    ScopedEnv const env { "PACE_TEST_EMPTY", "" };

    auto const r = ExpandEnvironmentVariables("[$PACE_TEST_EMPTY]", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "[]");
#endif
}

TEST_CASE("EnvExpand: an unset variable is an error, not an empty string", "[config][env]")
{
    auto const r = ExpandEnvironmentVariables("$PACE_TEST_DEFINITELY_UNSET/cache", "storage_path");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ConfigErrorCode::UndefinedVariable);
    REQUIRE(r.error().field == "storage_path");
    REQUIRE(r.error().context.contains("PACE_TEST_DEFINITELY_UNSET"));
}

TEST_CASE("EnvExpand: malformed references are rejected", "[config][env]")
{
    SECTION("unterminated brace")
    {
        auto const r = ExpandEnvironmentVariables("${PACE_TEST_A/cache", "storage_path");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ConfigErrorCode::ParseError);
    }
    SECTION("empty brace")
    {
        auto const r = ExpandEnvironmentVariables("${}/cache", "storage_path");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ConfigErrorCode::ParseError);
    }
    SECTION("trailing dollar")
    {
        auto const r = ExpandEnvironmentVariables("/cache$", "storage_path");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ConfigErrorCode::ParseError);
    }
    SECTION("dollar before a non-name character")
    {
        auto const r = ExpandEnvironmentVariables("/cache/$/x", "storage_path");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ConfigErrorCode::ParseError);
    }
}

TEST_CASE("EnvExpand: Windows %NAME% is left alone", "[config][env]")
{
    // Deliberately unsupported: a bare '%' is valid in a path, so rewriting it
    // would be worse than not handling it.
    auto const r = ExpandEnvironmentVariables("%ProgramData%/pace", "storage_path");
    REQUIRE(r.has_value());
    REQUIRE(*r == "%ProgramData%/pace");
}
