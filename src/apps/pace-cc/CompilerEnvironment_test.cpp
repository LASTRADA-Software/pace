// SPDX-License-Identifier: Apache-2.0
#include "CompilerEnvironment.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>

using namespace Pace::Cc;

namespace
{

/// A fake environment: exactly the variables a case sets, nothing inherited from the process.
[[nodiscard]] EnvironmentReader FakeEnvironment(std::map<std::string, std::string, std::less<>> variables)
{
    return [variables = std::move(variables)](std::string_view name) -> std::optional<std::string> {
        if (auto const found = variables.find(name); found != variables.end())
            return found->second;
        return std::nullopt;
    };
}

} // namespace

TEST_CASE("An empty environment contributes nothing to any driver", "[compiler-environment]")
{
    auto const none = FakeEnvironment({});
    for (auto const flavor: { Flavor::Cl, Flavor::ClangCl, Flavor::Gcc, Flavor::Clang, Flavor::Unknown })
        CHECK(ReadCompilerEnvironment(flavor, none) == CompilerEnvironment {});
}

TEST_CASE("CL and _CL_ change a cl compile and are named", "[compiler-environment]")
{
    CHECK(ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "CL", "/O2" } })).changesTheCompile == "CL");
    CHECK(ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "_CL_", "/DPROBE=x" } })).changesTheCompile == "_CL_");
    CHECK(ReadCompilerEnvironment(Flavor::ClangCl, FakeEnvironment({ { "CL", "/O2" } })).changesTheCompile == "CL");
}

TEST_CASE("A set but EMPTY CL changes nothing", "[compiler-environment]")
{
    // The distinguishing half of the case above: refusing on presence alone would cost every
    // build whose shell exports `CL=` its cache.
    CHECK_FALSE(ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "CL", "" } })).changesTheCompile.has_value());
}

TEST_CASE("A variable only another driver reads is ignored", "[compiler-environment]")
{
    // gcc does not read CL; cl does not read COMPILER_PATH. Refusing across families would turn
    // an unrelated export into a cache that never hits.
    CHECK_FALSE(ReadCompilerEnvironment(Flavor::Gcc, FakeEnvironment({ { "CL", "/O2" } })).changesTheCompile.has_value());
    CHECK_FALSE(ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "COMPILER_PATH", "/opt/x" } }))
                    .changesTheCompile.has_value());
    CHECK(ReadCompilerEnvironment(Flavor::Gcc, FakeEnvironment({ { "COMPILER_PATH", "/opt/x" } })).changesTheCompile
          == "COMPILER_PATH");
    CHECK(ReadCompilerEnvironment(Flavor::Clang, FakeEnvironment({ { "CCC_OVERRIDE_OPTIONS", "+-O0" } })).changesTheCompile
          == "CCC_OVERRIDE_OPTIONS");
}

TEST_CASE("INCLUDE is folded for the manifest key and does not refuse the compile", "[compiler-environment]")
{
    auto const read = ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "INCLUDE", R"(C:\sdk\a;C:\sdk\b)" } }));
    CHECK_FALSE(read.changesTheCompile.has_value());
    REQUIRE(read.includeSearch.size() == 1);
    CHECK(read.includeSearch.front() == R"(INCLUDE=C:\sdk\a;C:\sdk\b)");
}

TEST_CASE("Two INCLUDE values give two different contributions", "[compiler-environment]")
{
    // The property the manifest key needs from this function: what it returns MOVES with the
    // value, so a changed search path is a different manifest key (#1756).
    auto const first = ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "INCLUDE", "C:/one" } }));
    auto const second = ReadCompilerEnvironment(Flavor::Cl, FakeEnvironment({ { "INCLUDE", "C:/two" } }));
    CHECK(first.includeSearch != second.includeSearch);
}

TEST_CASE("Every row names at least one driver and no name appears twice", "[compiler-environment]")
{
    auto const rows = CompilerEnvironmentTable();
    REQUIRE_FALSE(rows.empty());
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(row.flavors != 0);
        CHECK(std::ranges::count(rows, row.name, &CompilerEnvironmentRow::name) == 1);
    }
}
