// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
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
/// @param family The family the node read off the banner; empty for a node that said none.
/// @param label What `ToolchainLabel` called the toolchain; empty for a node that said nothing.
/// @return One registry entry, as `LiveWorkers()` reports it.
[[nodiscard]] WorkerReport Entry(std::string fingerprint, std::string endpoint, std::string family, std::string label)
{
    auto report = WorkerReport {};
    report.info.fingerprint = std::move(fingerprint);
    report.info.endpoint = std::move(endpoint);
    report.info.toolchainFamily = std::move(family);
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

/// @param advice One family's advice.
/// @return Whether it places no machine anywhere: what an unknown ordering must say.
[[nodiscard]] bool PlacesNobody(UpgradeAdvice const& advice)
{
    return advice.nodesBehind.empty() && advice.nodesAhead.empty() && advice.nodesSameVersionOtherFingerprint.empty();
}

using Endpoints = std::vector<std::string>;
} // namespace

TEST_CASE("A toolchain label parses into its tool and numeric version", "[distributed][toolchains]")
{
    auto const gcc = Unwrap(ParseToolchainLabel("gcc 13.2.0"));
    CHECK(gcc.tool == "gcc");
    CHECK(gcc.parts == std::vector<std::uint32_t> { 13, 2, 0 });

    auto const cl = Unwrap(ParseToolchainLabel("cl 19.44.35207"));
    CHECK(cl.tool == "cl");
    CHECK(cl.parts == std::vector<std::uint32_t> { 19, 44, 35207 });

    // The cross drivers' names carry digits and dashes of their own: the tool is the WHOLE first token.
    auto const cross = Unwrap(ParseToolchainLabel("x86_64-w64-mingw32-g++-posix 13.2.0"));
    CHECK(cross.tool == "x86_64-w64-mingw32-g++-posix");
    CHECK(cross.parts == std::vector<std::uint32_t> { 13, 2, 0 });

    // The shape allows a version with no dot, though `ToolchainLabel` itself never writes one.
    auto const bare = Unwrap(ParseToolchainLabel("gcc 13"));
    CHECK(bare.tool == "gcc");
    CHECK(bare.parts == std::vector<std::uint32_t> { 13 });

    // A lone zero is a part like any other.
    CHECK(Unwrap(ParseToolchainLabel("clang 18.0.0")).parts == std::vector<std::uint32_t> { 18, 0, 0 });
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

TEST_CASE("A version part with a leading zero does not parse", "[distributed][toolchains]")
{
    // `13.02` reads as 13.2 to one reader and as something else to another: read neither way.
    CHECK_FALSE(ParseToolchainLabel("gcc 13.02").has_value());
    CHECK_FALSE(ParseToolchainLabel("cl 19.44.035207").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc 013.2.0").has_value());
    CHECK_FALSE(ParseToolchainLabel("gcc 00").has_value());
}

TEST_CASE("Without demand data the highest labelled version is the target, and machines are placed against it",
          "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc", "gcc 13.2.0"),
        Entry("fp-13", "c:1", "gcc", "gcc 13.2.0"),
        // One machine with two toolchains is ONE machine, placed once.
        Entry("fp-12", "d:1", "gcc", "gcc 12.3.0"),
        Entry("fp-clang", "d:1", "clang", "clang 18.1.8"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    REQUIRE(advice.size() == 2);

    auto const gcc = FamilyOf(advice, "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedVersion == std::optional<std::string> { "13.2.0" });
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind == Endpoints { "a:1", "d:1" });
    CHECK(gcc.nodesAhead.empty());
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
    INFO(gcc.basis);
    CHECK(gcc.basis.contains("no demand data; highest labelled version served"));
    CHECK(gcc.basis.contains("12.3.0"));
    CHECK(gcc.basis.contains("13.2.0"));

    auto const clang = FamilyOf(advice, "clang");
    CHECK(clang.recommendedFingerprint == std::optional<std::string> { "fp-clang" });
    CHECK(PlacesNobody(clang));
}

TEST_CASE("Families come from the banner: Apple clang is never compared with LLVM clang or GNU gcc",
          "[distributed][toolchains]")
{
    // Apple clang installed as `gcc` is labelled by the name it was invoked by -- `gcc 17.0.0` -- and its banner
    // says Apple clang. Grouped by the label it would be the newest gcc in the fleet and every GNU machine would be
    // told it is behind a compiler that is not gcc at all.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-gnu-13", "linux-a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-gnu-14", "linux-b:1", "gcc", "gcc 14.1.0"),
        Entry("fp-apple-as-gcc", "mac-a:1", "apple-clang", "gcc 17.0.0"),
        Entry("fp-apple", "mac-b:1", "apple-clang", "clang 16.0.0"),
        Entry("fp-llvm", "linux-a:1", "clang", "clang 18.1.8"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    REQUIRE(advice.size() == 3);

    auto const gcc = FamilyOf(advice, "gcc");
    CHECK(gcc.recommendedVersion == std::optional<std::string> { "14.1.0" });
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-gnu-14" });
    CHECK(gcc.nodesBehind == Endpoints { "linux-a:1" });

    auto const apple = FamilyOf(advice, "apple-clang");
    CHECK(apple.recommendedFingerprint == std::optional<std::string> { "fp-apple-as-gcc" });
    CHECK(apple.nodesBehind == Endpoints { "mac-b:1" });

    auto const llvm = FamilyOf(advice, "clang");
    CHECK(llvm.recommendedFingerprint == std::optional<std::string> { "fp-llvm" });
    CHECK(PlacesNobody(llvm));
    // No GNU or LLVM machine appears in another family's lists.
    for (auto const& row: advice)
        for (auto const& endpoint: row.nodesBehind)
            CHECK((row.family == "gcc") == endpoint.starts_with("linux"));
}

TEST_CASE("A toolchain that registered no family joins none and is advised on by nobody", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-13", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-unknown", "b:1", "", "gcc 14.1.0"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    REQUIRE(advice.size() == 1);
    auto const gcc = FamilyOf(advice, "gcc");
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(PlacesNobody(gcc));
}

TEST_CASE("one unparseable label in a family makes orderingUnknown and no recommendation", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-17", "a:1", "clang", "clang 17.0.6"),
        Entry("fp-18", "b:1", "clang", "clang 18.1.8"),
        Entry("fp-odd", "c:1", "clang", "clang version 18-ish"),
    };
    auto const clang = FamilyOf(AdviseUpgrades(workers, {}), "clang");
    CHECK(clang.orderingUnknown);
    CHECK_FALSE(clang.recommendedVersion.has_value());
    CHECK_FALSE(clang.recommendedFingerprint.has_value());
    CHECK(PlacesNobody(clang));
    INFO(clang.basis);
    CHECK(clang.basis.contains("clang version 18-ish"));
}

TEST_CASE("A label of the tool alone, or none, leaves its family's ordering unknown", "[distributed][toolchains]")
{
    auto const toolAlone = std::vector<WorkerReport> {
        Entry("fp-a", "a:1", "cl", "cl 19.44.35207"),
        Entry("fp-b", "b:1", "cl", "cl"),
    };
    auto const cl = FamilyOf(AdviseUpgrades(toolAlone, {}), "cl");
    CHECK(cl.orderingUnknown);
    CHECK_FALSE(cl.recommendedFingerprint.has_value());
    CHECK(PlacesNobody(cl));

    // A label withheld (too long, not text) still registered a family: its version is unknown, so nothing is ordered.
    auto const noLabel = std::vector<WorkerReport> {
        Entry("fp-a", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-b", "b:1", "gcc", ""),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(noLabel, {}), "gcc");
    CHECK(gcc.orderingUnknown);
    CHECK_FALSE(gcc.recommendedFingerprint.has_value());
    CHECK(PlacesNobody(gcc));
    CHECK(gcc.basis.contains("fp-b"));
}

TEST_CASE("One fingerprint labelled with two versions leaves its family's ordering unknown", "[distributed][toolchains]")
{
    // The same digest cannot be two versions; when its entries say so, neither is believed.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-x", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-x", "b:1", "gcc", "gcc 13.3.0"),
        Entry("fp-y", "c:1", "gcc", "gcc 12.1.0"),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK(gcc.orderingUnknown);
    CHECK_FALSE(gcc.recommendedFingerprint.has_value());
    CHECK(PlacesNobody(gcc));
    INFO(gcc.basis);
    CHECK(gcc.basis.contains("gcc 13.2.0"));
    CHECK(gcc.basis.contains("gcc 13.3.0"));
}

TEST_CASE("Versions of differing lengths in one family do not order", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-a", "a:1", "gcc", "gcc 13.2"),
        Entry("fp-b", "b:1", "gcc", "gcc 13.2.0"),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK(gcc.orderingUnknown);
    CHECK_FALSE(gcc.recommendedFingerprint.has_value());
    CHECK(PlacesNobody(gcc));
}

TEST_CASE("recommendation is the highest version the most demand uses, basis lists both", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-12", "b:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "c:1", "gcc", "gcc 13.2.0"),
        Entry("fp-14", "d:1", "gcc", "gcc 14.1.0"),
    };
    // 13.2.0 carries the most demand, so it -- and not the newest version, which almost nothing uses -- is where
    // the fleet converges. The machine on 14.1.0 is AHEAD of that, never behind it.
    auto const demand = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-12", .compiles = 40 },
        DemandRow { .fingerprint = "fp-13", .compiles = 900 },
        DemandRow { .fingerprint = "fp-14", .compiles = 3 },
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, demand), "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedVersion == std::optional<std::string> { "13.2.0" });
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind == Endpoints { "a:1", "b:1" });
    CHECK(gcc.nodesAhead == Endpoints { "d:1" });
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
    INFO(gcc.basis);
    CHECK_FALSE(gcc.basis.contains("no demand data"));
    for (std::string_view const part: { "12.3.0", "13.2.0", "14.1.0", "40", "900", "3" })
        CHECK(gcc.basis.contains(part));
}

TEST_CASE("Two versions with equal demand recommend the higher", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc", "gcc 13.2.0"),
    };
    auto const demand = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-12", .compiles = 50 },
        DemandRow { .fingerprint = "fp-13", .compiles = 50 },
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, demand), "gcc");
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind == Endpoints { "a:1" });
    CHECK(gcc.nodesAhead.empty());
}

TEST_CASE("One version under two fingerprints names no fingerprint, but still places machines by version",
          "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13-x", "b:1", "gcc", "gcc 13.2.0"),
        Entry("fp-13-y", "c:1", "gcc", "gcc 13.2.0"),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedVersion == std::optional<std::string> { "13.2.0" });
    CHECK_FALSE(gcc.recommendedFingerprint.has_value());
    // The version is known, so the machine below it is behind whichever 13.2.0 it installs.
    CHECK(gcc.nodesBehind == Endpoints { "a:1" });
    CHECK(gcc.nodesAhead.empty());
    // With no fingerprint named, nobody is "the other fingerprint".
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
    INFO(gcc.basis);
    CHECK(gcc.basis.contains("fp-13-x"));
    CHECK(gcc.basis.contains("fp-13-y"));
    CHECK(gcc.basis.contains("toolchain_diff"));
}

TEST_CASE("Demand that clearly favours one of two same-version fingerprints names it, and the other is the same "
          "version built differently",
          "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-13-x", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-13-y", "b:1", "gcc", "gcc 13.2.0"),
    };
    auto const decisive = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-13-x", .compiles = 10 },
        DemandRow { .fingerprint = "fp-13-y", .compiles = 70 },
    };
    auto const broken = FamilyOf(AdviseUpgrades(workers, decisive), "gcc");
    CHECK(broken.recommendedFingerprint == std::optional<std::string> { "fp-13-y" });
    // Not behind: a:1 runs the recommended VERSION, under another build of it.
    CHECK(broken.nodesBehind.empty());
    CHECK(broken.nodesAhead.empty());
    CHECK(broken.nodesSameVersionOtherFingerprint == Endpoints { "a:1" });

    auto const tied = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-13-x", .compiles = 70 },
        DemandRow { .fingerprint = "fp-13-y", .compiles = 70 },
    };
    auto const stillTied = FamilyOf(AdviseUpgrades(workers, tied), "gcc");
    CHECK_FALSE(stillTied.recommendedFingerprint.has_value());
    CHECK(PlacesNobody(stillTied));
    CHECK(stillTied.basis.contains("toolchain_diff"));
}

TEST_CASE("A machine is placed by the highest version of the family it serves", "[distributed][toolchains]")
{
    // b:1 serves 12 AND 14 with the target at 13 (the most demand): it already runs something newer, so it is
    // ahead, not behind; a:1 serves the target alongside an older one and is placed nowhere.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-12", "b:1", "gcc", "gcc 12.3.0"),
        Entry("fp-14", "b:1", "gcc", "gcc 14.1.0"),
    };
    auto const demand = std::vector<DemandRow> { DemandRow { .fingerprint = "fp-13", .compiles = 100 } };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, demand), "gcc");
    CHECK(gcc.recommendedFingerprint == std::optional<std::string> { "fp-13" });
    CHECK(gcc.nodesBehind.empty());
    CHECK(gcc.nodesAhead == Endpoints { "b:1" });
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
}

TEST_CASE("No workers give no advice", "[distributed][toolchains]")
{
    CHECK(AdviseUpgrades(std::span<WorkerReport const> {}, {}).empty());
}
