// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
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
using Choices = std::map<std::string, std::string>;
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
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-13" } });
    CHECK(gcc.nodesBehind == Endpoints { "a:1", "d:1" });
    CHECK(gcc.nodesAhead.empty());
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
    INFO(gcc.basis);
    CHECK(gcc.basis.contains("no demand data; highest labelled version served"));
    CHECK(gcc.basis.contains("12.3.0"));
    CHECK(gcc.basis.contains("13.2.0"));

    auto const clang = FamilyOf(advice, "clang");
    CHECK(clang.recommendedFingerprints == Choices { { "clang", "fp-clang" } });
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
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-gnu-14" } });
    CHECK(gcc.nodesBehind == Endpoints { "linux-a:1" });

    auto const apple = FamilyOf(advice, "apple-clang");
    CHECK(apple.recommendedFingerprints == Choices { { "gcc", "fp-apple-as-gcc" } });
    CHECK(apple.nodesBehind == Endpoints { "mac-b:1" });

    auto const llvm = FamilyOf(advice, "clang");
    CHECK(llvm.recommendedFingerprints == Choices { { "clang", "fp-llvm" } });
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
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-13" } });
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
    CHECK(clang.recommendedFingerprints.empty());
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
    CHECK(cl.recommendedFingerprints.empty());
    CHECK(PlacesNobody(cl));

    // A label withheld (too long, not text) still registered a family: its version is unknown, so nothing is ordered.
    auto const noLabel = std::vector<WorkerReport> {
        Entry("fp-a", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-b", "b:1", "gcc", ""),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(noLabel, {}), "gcc");
    CHECK(gcc.orderingUnknown);
    CHECK(gcc.recommendedFingerprints.empty());
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
    CHECK(gcc.recommendedFingerprints.empty());
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
    CHECK(gcc.recommendedFingerprints.empty());
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
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-13" } });
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
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-13" } });
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
    CHECK(gcc.recommendedFingerprints.empty());
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
    CHECK(broken.recommendedFingerprints == Choices { { "gcc", "fp-13-y" } });
    // Not behind: a:1 runs the recommended VERSION, under another build of it.
    CHECK(broken.nodesBehind.empty());
    CHECK(broken.nodesAhead.empty());
    CHECK(broken.nodesSameVersionOtherFingerprint == Endpoints { "a:1" });

    auto const tied = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-13-x", .compiles = 70 },
        DemandRow { .fingerprint = "fp-13-y", .compiles = 70 },
    };
    auto const stillTied = FamilyOf(AdviseUpgrades(workers, tied), "gcc");
    CHECK(stillTied.recommendedFingerprints.empty());
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
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-13" } });
    CHECK(gcc.nodesBehind.empty());
    CHECK(gcc.nodesAhead == Endpoints { "b:1" });
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
}

TEST_CASE("gcc and cc at one version are one install under two drivers: a fingerprint each, and nobody another build",
          "[distributed][toolchains]")
{
    // What every Linux host serves: `gcc` and `cc` are one GCC under two names, and two fingerprints because each
    // banner names its own driver. Chosen per driver, both are recommended; chosen across drivers, neither was.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-gcc", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-cc", "a:1", "gcc", "cc 13.2.0"),
        Entry("fp-gcc", "b:1", "gcc", "gcc 13.2.0"),
        Entry("fp-cc", "b:1", "gcc", "cc 13.2.0"),
        // A machine serving only one of the two drivers is not "another build": it lacks a driver, not a version.
        Entry("fp-cc", "c:1", "gcc", "cc 13.2.0"),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedVersion == std::optional<std::string> { "13.2.0" });
    CHECK(gcc.recommendedFingerprints == Choices { { "cc", "fp-cc" }, { "gcc", "fp-gcc" } });
    CHECK(PlacesNobody(gcc));
    CHECK_FALSE(gcc.basis.contains("toolchain_diff"));
}

TEST_CASE("Two builds under ONE driver at the target are still another build", "[distributed][toolchains]")
{
    // The narrowing is to the driver, not away from the question: two `gcc` 13.2.0 fingerprints remain competitors.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-gcc-x", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-gcc-y", "b:1", "gcc", "gcc 13.2.0"),
        Entry("fp-cc", "a:1", "gcc", "cc 13.2.0"),
    };
    auto const demand = std::vector<DemandRow> { DemandRow { .fingerprint = "fp-gcc-x", .compiles = 30 } };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, demand), "gcc");
    CHECK(gcc.recommendedFingerprints == Choices { { "cc", "fp-cc" }, { "gcc", "fp-gcc-x" } });
    CHECK(gcc.nodesSameVersionOtherFingerprint == Endpoints { "b:1" });
    CHECK(gcc.nodesBehind.empty());
}

TEST_CASE("Version-suffixed drivers are one driver, and their versions are still compared", "[distributed][toolchains]")
{
    // `gcc-13` and `gcc-14` are the driver `gcc` at two versions: ordered within the family, the older machine
    // behind -- and a machine with only `cc` is behind `gcc 14` too, since the FAMILY places machines.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-13", "a:1", "gcc", "gcc-13 13.3.0"),
        Entry("fp-14", "b:1", "gcc", "gcc-14 14.2.0"),
        Entry("fp-cc", "c:1", "gcc", "cc 13.3.0"),
    };
    auto const gcc = FamilyOf(AdviseUpgrades(workers, {}), "gcc");
    CHECK_FALSE(gcc.orderingUnknown);
    CHECK(gcc.recommendedVersion == std::optional<std::string> { "14.2.0" });
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-14" } });
    CHECK(gcc.nodesBehind == Endpoints { "a:1", "c:1" });
    CHECK(gcc.nodesAhead.empty());
    CHECK(gcc.nodesSameVersionOtherFingerprint.empty());
}

TEST_CASE("Apple's aliases are one family row, one choice per driver, and nobody another build", "[distributed][toolchains]")
{
    // macOS answers `cc`, `gcc` and `clang` with one Apple clang: one family, one fingerprint, whatever name each
    // machine's toolchain was first served under.
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-apple", "mac-a:1", "apple-clang", "cc 17.0.0"),
        Entry("fp-apple", "mac-b:1", "apple-clang", "gcc 17.0.0"),
        Entry("fp-apple", "mac-c:1", "apple-clang", "clang 17.0.0"),
    };
    auto const advice = AdviseUpgrades(workers, {});
    REQUIRE(advice.size() == 1);
    auto const apple = FamilyOf(advice, "apple-clang");
    CHECK_FALSE(apple.orderingUnknown);
    CHECK(apple.recommendedFingerprints == Choices { { "cc", "fp-apple" }, { "clang", "fp-apple" }, { "gcc", "fp-apple" } });
    CHECK(PlacesNobody(apple));
}

TEST_CASE("No workers give no advice", "[distributed][toolchains]")
{
    CHECK(AdviseUpgrades(std::span<WorkerReport const> {}, {}).empty());
}

TEST_CASE("Demand decides only by more than the folded compiles, and counts are then printed as floors",
          "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc", "gcc 13.2.0"),
    };
    auto const demand = std::vector<DemandRow> {
        DemandRow { .fingerprint = "fp-12", .compiles = 10 },
        DemandRow { .fingerprint = "fp-13", .compiles = 4 },
    };
    // A lead of 6: 5 folded compiles cannot reverse it, 6 can (a tie goes to nobody's lead).
    auto const decided = FamilyOf(
        AdviseUpgrades(workers, demand, DemandContext { .unattributable = 5, .weighed = true, .notWeighedBecause = {} }),
        "gcc");
    CHECK(decided.recommendedVersion == std::optional<std::string> { "12.3.0" });
    CHECK(decided.basis.contains("at least 10 compiles of 14 compiles named"));
    CHECK(decided.nodesLackingRecommended == Endpoints { "b:1" });
    auto const undecided = FamilyOf(
        AdviseUpgrades(workers, demand, DemandContext { .unattributable = 6, .weighed = true, .notWeighedBecause = {} }),
        "gcc");
    CHECK(undecided.recommendedVersion == std::optional<std::string> { "13.2.0" });
    CHECK(undecided.basis.contains("unattributable demand (folded or unnamed) of 6 compiles could change the order"));
    CHECK(undecided.nodesLackingRecommended == Endpoints { "a:1" });
    // Nothing folded: exact, and no floor wording.
    CHECK_FALSE(
        FamilyOf(
            AdviseUpgrades(workers, demand, DemandContext { .unattributable = 0, .weighed = true, .notWeighedBecause = {} }),
            "gcc")
            .basis.contains("at least"));
}

TEST_CASE("Each basis opens with its own case, and none says no demand data once data was read", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc", "gcc 13.2.0"),
    };
    auto const demand = std::vector<DemandRow> { DemandRow { .fingerprint = "fp-12", .compiles = 9 },
                                                 DemandRow { .fingerprint = "fp-13", .compiles = 1 } };
    // Not recorded.
    CHECK(FamilyOf(AdviseUpgrades(workers, {}), "gcc").basis.starts_with("no demand data;"));
    // Read, covering the window, nothing for this family.
    auto const zero = FamilyOf(
        AdviseUpgrades(workers, {}, DemandContext { .unattributable = 0, .weighed = true, .notWeighedBecause = {} }), "gcc");
    CHECK(zero.basis.starts_with("no compiles were recorded for this family in the window;"));
    CHECK(zero.recommendedVersion == std::optional<std::string> { "13.2.0" });
    // Read but not weighed: the reason leads, the rows are ignored.
    auto const unweighed =
        FamilyOf(AdviseUpgrades(workers,
                                demand,
                                DemandContext { .unattributable = 0,
                                                .weighed = false,
                                                .notWeighedBecause =
                                                    "coverage is insufficient (3 minute(s) observed, 1440 required)" }),
                 "gcc");
    CHECK(unweighed.basis.starts_with("demand was read but does not decide (coverage is insufficient (3 minute(s)"));
    CHECK(unweighed.recommendedVersion == std::optional<std::string> { "13.2.0" });
    // Decides.
    CHECK(FamilyOf(AdviseUpgrades(
                       workers, demand, DemandContext { .unattributable = 0, .weighed = true, .notWeighedBecause = {} }),
                   "gcc")
              .basis.starts_with("highest version the most demand uses: 12.3.0 (9 compiles of 10 compiles named)"));
    // Does not decide: nothing leads.
    auto const tie = std::vector<DemandRow> { DemandRow { .fingerprint = "fp-12", .compiles = 5 },
                                              DemandRow { .fingerprint = "fp-13", .compiles = 5 } };
    CHECK(FamilyOf(
              AdviseUpgrades(workers, tie, DemandContext { .unattributable = 0, .weighed = true, .notWeighedBecause = {} }),
              "gcc")
              .basis.starts_with("demand does not decide this family: no version leads the others;"));
}

TEST_CASE("lacks_recommended names a machine on another build of the recommended version", "[distributed][toolchains]")
{
    auto const workers = std::vector<WorkerReport> {
        Entry("fp-13a", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-13b", "b:1", "gcc", "gcc 13.2.0"),
    };
    auto const demand = std::vector<DemandRow> { DemandRow { .fingerprint = "fp-13a", .compiles = 8 },
                                                 DemandRow { .fingerprint = "fp-13b", .compiles = 2 } };
    auto const gcc = FamilyOf(
        AdviseUpgrades(workers, demand, DemandContext { .unattributable = 0, .weighed = true, .notWeighedBecause = {} }),
        "gcc");
    CHECK(gcc.recommendedFingerprints == Choices { { "gcc", "fp-13a" } });
    CHECK(gcc.nodesBehind.empty());
    CHECK(gcc.nodesAhead.empty());
    CHECK(gcc.nodesSameVersionOtherFingerprint == Endpoints { "b:1" });
    CHECK(gcc.nodesLackingRecommended == Endpoints { "b:1" });
}
