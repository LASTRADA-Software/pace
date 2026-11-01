// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace::Distributed;
using Pace::Testing::Unwrap;

namespace
{
/// @param fingerprint The toolchain identity.
/// @param endpoint The machine.
/// @param label What `ToolchainLabel` called the toolchain; empty for a node that said nothing.
/// @return One registry entry, as `LiveWorkers()` reports it.
[[nodiscard]] WorkerReport Entry(std::string fingerprint, std::string endpoint, std::string label)
{
    auto report = WorkerReport {};
    report.info.fingerprint = std::move(fingerprint);
    report.info.endpoint = std::move(endpoint);
    report.info.toolchainLabel = std::move(label);
    return report;
}

/// @param advice Every family's advice.
/// @param family A family.
/// @return A copy of that family's advice, so a caller may pass a temporary; the case fails when there is none.
[[nodiscard]] UpgradeAdvice FamilyOf(std::vector<UpgradeAdvice> const& advice, std::string_view family)
{
    auto const found = std::ranges::find(advice, family, &UpgradeAdvice::family);
    REQUIRE(found != advice.end());
    return *found;
}

/// @param text Some text.
/// @param part What it must carry.
/// @return Whether it does.
[[nodiscard]] bool Carries(std::string const& text, std::string_view part)
{
    return text.contains(part);
}
} // namespace

TEST_CASE("A toolchain label parses into its family and numeric version", "[distributed][toolchains]")
{
    auto const gcc = Unwrap(ParseToolchainLabel("gcc 13.2.0"));
    CHECK(gcc.family == "gcc");
    CHECK(gcc.parts == std::vector<std::uint32_t> { 13, 2, 0 });

    auto const cl = Unwrap(ParseToolchainLabel("cl 19.44.35207"));
    CHECK(cl.family == "cl");
    CHECK(cl.parts == std::vector<std::uint32_t> { 19, 44, 35207 });

    // The cross drivers' names carry digits and dashes of their own: the family is the WHOLE first token.
    auto const cross = Unwrap(ParseToolchainLabel("x86_64-w64-mingw32-g++-posix 13.2.0"));
    CHECK(cross.family == "x86_64-w64-mingw32-g++-posix");
    CHECK(cross.parts == std::vector<std::uint32_t> { 13, 2, 0 });
}

TEST_CASE("Anything but the ToolchainLabel shape does not parse", "[distributed][toolchains]")
{
    CHECK_FALSE(ParseToolchainLabel("clang version 18-ish").has_value());
    // The tool alone -- what `ToolchainLabel` writes when the banner states no version.
    CHECK_FALSE(ParseToolchainLabel("cl").has_value());
    CHECK_FALSE(ParseToolchainLabel("").has_value());
    CHECK_FALSE(ParseToolchainLabel(" 13.2.0").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc ").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc 13..2").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc .13").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc 13.").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc 13.2.0-1ubuntu").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc  13.2.0").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc 13.2.0 ").has_value());
    // A part past what a version part can hold is not read as a smaller number.
    CHECK_FALSE(ParseToolchainLabel("gcc 99999999999.1").has_value());
}

TEST_CASE("A label's family is its first token, parsed or not", "[distributed][toolchains]")
{
    CHECK(ToolchainFamilyOf("gcc 13.2.0") == std::optional<std::string> { "gcc" });
    CHECK(ToolchainFamilyOf("cl") == std::optional<std::string> { "cl" });
    CHECK(ToolchainFamilyOf("clang version 18-ish") == std::optional<std::string> { "clang" });
    CHECK_FALSE(ToolchainFamilyOf("").has_value());
    CHECK_FALSE(ToolchainFamilyOf(" 13.2.0").has_value());
}

TEST_CASE("Without demand data the highest version is recommended and every node lacking it is behind",
          "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc 13.2.0"),
        Entry("fp-13", "c:1", "gcc 13.2.0"),
        // One machine with two toolchains is ONE node, behind once.
        Entry("fp-12", "d:1", "gcc 12.3.0"),
        Entry("fp-clang", "d:1", "clang 18.1.8"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    REQUIRE(advice.size() == 2);

    auto const& gcc = FamilyOf(advice, "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind == std::vector<std::string> { "a:1", "d:1" });
    INFO(gcc.basis);
    CHECK(Carries(gcc.basis, "no demand data; highest version served"));
    CHECK(Carries(gcc.basis, "12.3.0"));
    CHECK(Carries(gcc.basis, "13.2.0"));

    auto const& clang = FamilyOf(advice, "clang");
    CHECK(clang.recommendedFingerprint == std::optional<std::string> { "fp-clang" });
    CHECK(clang.nodesBehind.empty());
}

TEST_CASE("one unparseable label in a family makes orderingUnknown and no recommendation", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-17", "a:1", "clang 17.0.6"),
        Entry("fp-18", "b:1", "clang 18.1.8"),
        Entry("fp-odd", "c:1", "clang version 18-ish"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    auto const& clang = FamilyOf(advice, "clang");
    CHECK(clang.orderingUnknown);
    CHECK_FALSE(clang.recommendedFingerprint.has_value());
    CHECK(clang.nodesBehind.empty());
    INFO(clang.basis);
    CHECK(Carries(clang.basis, "clang version 18-ish"));
}

TEST_CASE("A label of the tool alone leaves its family's ordering unknown", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-a", "a:1", "cl 19.44.35207"),
        Entry("fp-b", "b:1", "cl"),
    };
    auto const& cl = FamilyOf(AdviseUpgrades(workers, {}), "cl");
    CHECK(cl.orderingUnknown);
    CHECK_FALSE(cl.recommendedFingerprint.has_value());
    CHECK(cl.nodesBehind.empty());
}

TEST_CASE("Versions of differing lengths in one family do not order", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-a", "a:1", "gcc 13.2"),
        Entry("fp-b", "b:1", "gcc 13.2.0"),
    };
    auto const& gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK(gcc.orderingUnknown);
    CHECK_FALSE(gcc.recommendedFingerprint.has_value());
    CHECK(gcc.nodesBehind.empty());
}

TEST_CASE("cc and c++ name a policy, so a family of that name is never ordered", "[distributed][toolchains]")
{
    // `/usr/bin/c++` is Apple clang on macOS and GNU on Linux: the label's version numbers belong to two
    // different products, and comparing 17 against 13 would recommend a different compiler, not a newer one.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-apple", "mac:1", "c++ 17.0.0"),
        Entry("fp-gnu", "linux:1", "c++ 13.2.0"),
        Entry("fp-cc", "linux:1", "cc 13.2.0"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    for (auto const family: { std::string_view { "c++" }, std::string_view { "cc" } })
    {
        auto const& row = FamilyOf(advice, family);
        CHECK(row.orderingUnknown);
        CHECK_FALSE(row.recommendedFingerprint.has_value());
        CHECK(row.nodesBehind.empty());
        INFO(row.basis);
        CHECK(Carries(row.basis, "policy"));
    }
}

TEST_CASE("recommendation is the highest version the most demand uses, basis lists both", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc 12.3.0"),
        Entry("fp-12", "b:1", "gcc 12.3.0"),
        Entry("fp-13", "c:1", "gcc 13.2.0"),
        Entry("fp-14", "d:1", "gcc 14.1.0"),
    };
    // 13.2.0 carries the most demand, so it -- and not the newest version, which almost nothing uses -- is where
    // the fleet converges.
    auto const demand = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-12", .compiles = 40 },
        DemandRow { .fingerprint = "fp-13", .compiles = 900 },
        DemandRow { .fingerprint = "fp-14", .compiles = 3 },
    };
    auto const& gcc = FamilyOf(AdviseUpgrades(workers, demand), "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind == std::vector<std::string> { "a:1", "b:1", "d:1" });
    INFO(gcc.basis);
    CHECK_FALSE(Carries(gcc.basis, "no demand data"));
    for (std::string_view const part: { "12.3.0", "13.2.0", "14.1.0", "40", "900", "3" })
        CHECK(Carries(gcc.basis, part));
}

TEST_CASE("Two versions with equal demand recommend the higher", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc 13.2.0"),
    };
    auto const demand = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-12", .compiles = 50 },
        DemandRow { .fingerprint = "fp-13", .compiles = 50 },
    };
    auto const& gcc = FamilyOf(AdviseUpgrades(workers, demand), "gcc");
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind == std::vector<std::string> { "a:1" });
}

TEST_CASE("One version under two fingerprints recommends neither, and names both and toolchain_diff",
          "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc 12.3.0"),
        Entry("fp-13-x", "b:1", "gcc 13.2.0"),
        Entry("fp-13-y", "c:1", "gcc 13.2.0"),
    };
    auto const& gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK_FALSE(gcc.recommendedFingerprint.has_value());
    CHECK(gcc.nodesBehind.empty());
    INFO(gcc.basis);
    CHECK(Carries(gcc.basis, "fp-13-x"));
    CHECK(Carries(gcc.basis, "fp-13-y"));
    CHECK(Carries(gcc.basis, "toolchain_diff"));
}

TEST_CASE("Demand that clearly favours one of two same-version fingerprints recommends it", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-13-x", "a:1", "gcc 13.2.0"),
        Entry("fp-13-y", "b:1", "gcc 13.2.0"),
    };
    auto const decisive = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-13-x", .compiles = 10 },
        DemandRow { .fingerprint = "fp-13-y", .compiles = 70 },
    };
    auto const& broken = FamilyOf(AdviseUpgrades(workers, decisive), "gcc");
    CHECK(broken.recommendedFingerprint == std::optional<std::string> { "fp-13-y" });
    CHECK(broken.nodesBehind == std::vector<std::string> { "a:1" });

    auto const tied = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-13-x", .compiles = 70 },
        DemandRow { .fingerprint = "fp-13-y", .compiles = 70 },
    };
    auto const& stillTied = FamilyOf(AdviseUpgrades(workers, tied), "gcc");
    CHECK_FALSE(stillTied.recommendedFingerprint.has_value());
    CHECK(stillTied.nodesBehind.empty());
    CHECK(Carries(stillTied.basis, "toolchain_diff"));
}

TEST_CASE("Families are never compared with each other, and an unlabelled toolchain joins none", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-g13", "a:1", "g++-13 13.3.0"),
        Entry("fp-g14", "b:1", "g++-14 14.2.0"),
        Entry("fp-pinned", "c:1", ""),
    };
    auto const advice = AdviseUpgrades(workers, {});
    REQUIRE(advice.size() == 2);
    CHECK(FamilyOf(advice, "g++-13").nodesBehind.empty());
    CHECK(FamilyOf(advice, "g++-14").nodesBehind.empty());
    CHECK(std::ranges::none_of(
        advice, [](UpgradeAdvice const& row) { return std::ranges::contains(row.nodesBehind, std::string { "c:1" }); }));
}

TEST_CASE("No workers give no advice", "[distributed][toolchains]")
{
    CHECK(AdviseUpgrades(std::span<WorkerReport const> {}, {}).empty());
}
