// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/MemoryPressure.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/CgroupFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace::Platform;
using Pace::Testing::FakeCgroupHost;
using Pace::Testing::Unwrap;

namespace
{

constexpr auto Full = std::string_view {
    "some avg10=12.50 avg60=3.00 avg300=1.00 total=123456\nfull avg10=4.00 avg60=1.00 avg300=0.50 total=6789\n"
};
constexpr auto SomeOnly = std::string_view { "some avg10=0.25 avg60=0.00 avg300=0.00 total=42\n" };

} // namespace

TEST_CASE("ParsePsi reads the kernel's two-line format", "[platform][psi]")
{
    auto const reading = Unwrap(ParsePsi(Full));
    CHECK(reading.some.avg10 == 12.5);
    CHECK(reading.some.avg60 == 3.0);
    CHECK(reading.some.totalMicros == 123456);
    auto const full = Unwrap(reading.full);
    CHECK(full.avg10 == 4.0);
    CHECK(full.totalMicros == 6789);
}

TEST_CASE("ParsePsi tolerates a some-only document", "[platform][psi]")
{
    auto const reading = Unwrap(ParsePsi(SomeOnly));
    CHECK(reading.some.avg10 == 0.25);
    CHECK_FALSE(reading.full.has_value());
}

TEST_CASE("ParsePsi names the line it cannot read", "[platform][psi]")
{
    auto const garbage = ParsePsi("some avg10=1.00 avg60=1.00 avg300=1.00 total=1\nwhat is this\n");
    REQUIRE_FALSE(garbage.has_value());
    CHECK(garbage.error().find("what is this") != std::string::npos);

    CHECK_FALSE(ParsePsi("").has_value());
    CHECK_FALSE(ParsePsi("full avg10=1.00 avg60=1.00 avg300=1.00 total=1\n").has_value());
    CHECK_FALSE(ParsePsi("some avg10=x avg60=1.00 avg300=1.00 total=1\n").has_value());
    CHECK_FALSE(ParsePsi("some avg10=1.00 avg60=1.00 avg300=1.00 total=-1\n").has_value());
}

TEST_CASE("PsiPressureSource samples the system file and the compiles cgroup", "[platform][psi]")
{
    auto host = std::make_shared<FakeCgroupHost>();
    host->files[std::string { PsiSystemPath }] = std::string { Full };

    SECTION("no compiles path leaves it disengaged")
    {
        PsiPressureSource source { host, std::nullopt };
        auto const sample = Unwrap(source.Sample());
        CHECK(sample.system.some.avg10 == 12.5);
        CHECK_FALSE(sample.compiles.has_value());
        CHECK_FALSE(sample.compilesFault.has_value());
    }
    SECTION("a compiles cgroup is read from its memory.pressure")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.pressure"] = std::string { SomeOnly };
        PsiPressureSource source { host, std::filesystem::path { "/sys/fs/cgroup/x/compiles" } };
        auto const sample = Unwrap(source.Sample());
        CHECK(Unwrap(sample.compiles).some.avg10 == 0.25);
    }
    SECTION("an unreadable compiles file keeps the system reading and states the fault")
    {
        PsiPressureSource source { host, std::filesystem::path { "/sys/fs/cgroup/x/compiles" } };
        auto const sample = Unwrap(source.Sample());
        CHECK(sample.system.some.avg10 == 12.5);
        CHECK_FALSE(sample.compiles.has_value());
        CHECK(Unwrap(sample.compilesFault).find("/sys/fs/cgroup/x/compiles") != std::string::npos);
    }
}

TEST_CASE("PsiPressureSource says whether the compiles stand at their own ceiling", "[platform][psi]")
{
    auto host = std::make_shared<FakeCgroupHost>();
    host->files[std::string { PsiSystemPath }] = std::string { Full };
    auto const compiles = std::filesystem::path { "/sys/fs/cgroup/x/compiles" };
    host->files["/sys/fs/cgroup/x/compiles/memory.pressure"] = std::string { SomeOnly };
    auto const events = [](int max) {
        return "low 0\nhigh 0\nmax " + std::to_string(max) + "\noom 0\noom_kill 0\noom_group_kill 0\n";
    };

    SECTION("far below a numeric ceiling, with no max event: not at it")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "1073741824\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "8589934592\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.events"] = events(0);
        PsiPressureSource source { host, compiles };
        CHECK(Unwrap(source.Sample()).compilesAtLimit == false);
        CHECK(Unwrap(source.Sample()).compilesAtLimit == false);
    }
    SECTION("within a twentieth of the ceiling: at it")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "8200000000\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "8589934592\n";
        PsiPressureSource source { host, compiles };
        CHECK(Unwrap(source.Sample()).compilesAtLimit == true);
    }
    SECTION("no ceiling (max) is never near, however much they hold -- and is a reading, not a fault")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "8589934592\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "max\n";
        PsiPressureSource source { host, compiles };
        auto const sample = Unwrap(source.Sample());
        CHECK(sample.compilesAtLimit == false);
        CHECK_FALSE(sample.compilesLimitFault.has_value());
    }
    SECTION("a max count that rose since the previous sample: at it, though below now")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "1073741824\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "8589934592\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.events"] = events(7);
        PsiPressureSource source { host, compiles };
        // The first sample has nothing to compare with: a count already standing is no rise.
        CHECK(Unwrap(source.Sample()).compilesAtLimit == false);
        host->files["/sys/fs/cgroup/x/compiles/memory.events"] = events(9);
        CHECK(Unwrap(source.Sample()).compilesAtLimit == true);
        // Standing still again is no rise.
        CHECK(Unwrap(source.Sample()).compilesAtLimit == false);
    }
    SECTION("nothing readable: no answer at all, and why -- never a plain not-at-it")
    {
        PsiPressureSource source { host, compiles };
        auto const sample = Unwrap(source.Sample());
        CHECK_FALSE(sample.compilesAtLimit.has_value());
        REQUIRE(sample.compilesLimitFault.has_value());
        CHECK(Unwrap(sample.compilesLimitFault).contains("/sys/fs/cgroup/x/compiles"));
    }
    SECTION("memory.events alone is a reading")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.events"] = events(1);
        PsiPressureSource source { host, compiles };
        auto const sample = Unwrap(source.Sample());
        CHECK(sample.compilesAtLimit == false);
        CHECK_FALSE(sample.compilesLimitFault.has_value());
    }
    SECTION("no compiles cgroup configured: no answer and no fault")
    {
        PsiPressureSource source { host, std::nullopt };
        auto const sample = Unwrap(source.Sample());
        CHECK_FALSE(sample.compilesAtLimit.has_value());
        CHECK_FALSE(sample.compilesLimitFault.has_value());
    }
}

TEST_CASE("PsiPressureSource reports a kernel without PSI", "[platform][psi]")
{
    auto host = std::make_shared<FakeCgroupHost>();
    PsiPressureSource source { host, std::nullopt };
    CHECK_FALSE(source.Sample().has_value());
}

namespace
{

/// A PSI document stalled @p some / @p full percent over ten seconds.
std::string Psi(double some, double full)
{
    return std::format(
        "some avg10={:.2f} avg60=0.00 avg300=0.00 total=1\nfull avg10={:.2f} avg60=0.00 avg300=0.00 total=1\n", some, full);
}

/// A machine whose stall (some 15, full 14) is almost all one user service thrashing at its own 4 GiB memory.max, under
/// a slice with no limit; this node runs in system.slice/pace-node.service; and a quiet sibling with a limit too.
struct ConfinedTree
{
    std::shared_ptr<FakeCgroupHost> host = std::make_shared<FakeCgroupHost>();
    core::platform::ManualClock clock;

    ConfinedTree()
    {
        auto& files = host->files;
        files[std::string { PsiSystemPath }] = Psi(15.0, 14.0);
        for (auto const* directory: { "/sys/fs/cgroup",
                                      "/sys/fs/cgroup/user.slice",
                                      "/sys/fs/cgroup/user.slice/thrash.service",
                                      "/sys/fs/cgroup/user.slice/quiet.service",
                                      "/sys/fs/cgroup/system.slice",
                                      "/sys/fs/cgroup/system.slice/pace-node.service" })
            host->directories.insert(directory);
        Cgroup("user.slice", Psi(14.5, 13.8), "max", "9000000000");
        Cgroup("user.slice/thrash.service", Psi(14.2, 13.5), "4294967296", "4290000000");
        Cgroup("user.slice/quiet.service", Psi(0.0, 0.0), "4294967296", "100000000");
        Cgroup("system.slice", Psi(14.0, 13.0), "max", "2000000000");
        Cgroup("system.slice/pace-node.service", Psi(14.0, 13.0), "4294967296", "4290000000");
    }

    void Cgroup(std::string const& relative, std::string psi, std::string max, std::string current)
    {
        auto const base = "/sys/fs/cgroup/" + relative;
        host->files[base + "/memory.pressure"] = std::move(psi);
        host->files[base + "/memory.max"] = std::move(max) + "\n";
        host->files[base + "/memory.high"] = "max\n";
        host->files[base + "/memory.current"] = std::move(current) + "\n";
        host->files[base + "/memory.events"] = "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n";
    }

    [[nodiscard]] PsiPressureSource Source(ConfinedScanBounds bounds = Bounds())
    {
        return PsiPressureSource { host, std::nullopt, std::make_unique<ConfinedStallScan>(host, std::move(bounds), clock) };
    }

    [[nodiscard]] static ConfinedScanBounds Bounds()
    {
        auto bounds = ConfinedScanBounds {};
        bounds.excluded = { "/sys/fs/cgroup/system.slice/pace-node.service" };
        return bounds;
    }
};

} // namespace

TEST_CASE("PsiPressureSource finds the cgroup stalling at its own limit, and none of this node's",
          "[platform][psi][confined]")
{
    ConfinedTree tree;
    auto source = tree.Source();
    // The walk keeps it by its limit; a first look has no count to compare with, so it is not AT it yet.
    auto const first = Unwrap(source.Sample());
    auto const& kept = Unwrap(first.confined);
    REQUIRE(kept.size() == 1);
    CHECK(kept.front().cgroup == "/sys/fs/cgroup/user.slice/thrash.service");
    CHECK(kept.front().limited);
    CHECK_FALSE(kept.front().atLimit);
    CHECK_FALSE(first.confinedFault.has_value());

    // The kernel counts it reaching its memory.max: at it now.
    tree.host->files["/sys/fs/cgroup/user.slice/thrash.service/memory.events"] = "low 0\nhigh 0\nmax 9\noom 0\noom_kill 0\n";
    auto const rose = Unwrap(Unwrap(source.Sample()).confined);
    REQUIRE(rose.size() == 1);
    CHECK(rose.front().atLimit);
    CHECK(rose.front().psi.some.avg10 == 14.2);
}

TEST_CASE("PsiPressureSource reports no cgroup at its limit without the kernel counting it there",
          "[platform][psi][confined]")
{
    ConfinedTree tree;
    SECTION("within a twentieth of its limit, with no event rising: it stalls on nothing of its own")
    {
        auto source = tree.Source();
        for ([[maybe_unused]] auto const sample: std::views::iota(0, 3))
        {
            auto const confined = Unwrap(Unwrap(source.Sample()).confined);
            CHECK(std::ranges::none_of(confined, &ConfinedStall::atLimit));
        }
    }
    SECTION("no limit at all: not even a candidate")
    {
        tree.host->files["/sys/fs/cgroup/user.slice/thrash.service/memory.max"] = "max\n";
        auto source = tree.Source();
        CHECK(Unwrap(Unwrap(source.Sample()).confined).empty());
    }
}

TEST_CASE("PsiPressureSource keeps no cgroup that carries only the full share", "[platform][psi][confined]")
{
    // In a machine-wide freeze every busy cgroup's `full` reads about the system's: a cgroup at its own limit with
    // the system's `full` but little of its `some` owns nothing, and is no candidate.
    ConfinedTree tree;
    tree.host->files["/sys/fs/cgroup/user.slice/thrash.service/memory.pressure"] = Psi(5.0, 13.5);
    auto source = tree.Source();
    CHECK(Unwrap(Unwrap(source.Sample()).confined).empty());
}

TEST_CASE("PsiPressureSource counts a memory.high a cgroup is throttled at, by its events", "[platform][psi][confined]")
{
    ConfinedTree tree;
    auto const base = std::string { "/sys/fs/cgroup/user.slice/thrash.service" };
    tree.host->files[base + "/memory.max"] = "max\n";
    tree.host->files[base + "/memory.high"] = "2000000000\n";
    auto source = tree.Source();
    auto const kept = Unwrap(Unwrap(source.Sample()).confined);
    REQUIRE(kept.size() == 1);
    CHECK_FALSE(kept.front().atLimit);

    // The kernel counted it throttled at memory.high since: at it. Standing still again: not.
    tree.host->files[base + "/memory.events"] = "low 0\nhigh 7\nmax 0\noom 0\noom_kill 0\n";
    auto const rose = Unwrap(Unwrap(source.Sample()).confined);
    REQUIRE(rose.size() == 1);
    CHECK(rose.front().atLimit);
    auto const still = Unwrap(Unwrap(source.Sample()).confined);
    REQUIRE(still.size() == 1);
    CHECK_FALSE(still.front().atLimit);
}

TEST_CASE("PsiPressureSource judges a cgroup's own limit by its local events, not its children's",
          "[platform][psi][confined]")
{
    // A slice with a numeric limit, whose child keeps reaching the child's own limit: the hierarchical memory.events
    // rises in the slice too, the local one does not. The slice is not at ITS limit.
    ConfinedTree tree;
    auto const slice = std::string { "/sys/fs/cgroup/user.slice" };
    tree.host->files[slice + "/memory.max"] = "60000000000\n";
    tree.host->files[slice + "/memory.events.local"] = "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n";
    auto source = tree.Source();
    auto const atSlice = [&] {
        auto const confined = Unwrap(Unwrap(source.Sample()).confined);
        auto const found = std::ranges::find(confined, std::filesystem::path { slice }, &ConfinedStall::cgroup);
        REQUIRE(found != confined.end());
        return found->atLimit;
    };
    CHECK_FALSE(atSlice());
    tree.host->files[slice + "/memory.events"] = "low 0\nhigh 0\nmax 40\noom 0\noom_kill 0\n";
    CHECK_FALSE(atSlice());
}

TEST_CASE("PsiPressureSource walks the cgroup tree only while the machine stalls, and once per interval",
          "[platform][psi][confined]")
{
    ConfinedTree tree;
    auto bounds = ConfinedTree::Bounds();
    auto scan = std::make_unique<ConfinedStallScan>(tree.host, bounds, tree.clock);
    auto const* walks = scan.get();
    PsiPressureSource source { tree.host, std::nullopt, std::move(scan) };

    static_cast<void>(Unwrap(source.Sample()));
    static_cast<void>(Unwrap(source.Sample()));
    CHECK(walks->Walks() == 1);
    tree.clock.advance(std::chrono::seconds { 10 });
    static_cast<void>(Unwrap(source.Sample()));
    CHECK(walks->Walks() == 2);

    // A calm machine: nothing to attribute, so no walk however long it has been.
    tree.host->files[std::string { PsiSystemPath }] = Psi(0.0, 0.0);
    tree.clock.advance(std::chrono::seconds { 60 });
    static_cast<void>(Unwrap(source.Sample()));
    CHECK(walks->Walks() == 2);
}

TEST_CASE("PsiPressureSource prunes a subtree that carries too little, and stops at its bound", "[platform][psi][confined]")
{
    ConfinedTree tree;
    SECTION("a slice carrying little hides nothing below it")
    {
        tree.host->files["/sys/fs/cgroup/user.slice/memory.pressure"] = Psi(1.0, 1.0);
        auto source = tree.Source();
        CHECK(Unwrap(Unwrap(source.Sample()).confined).empty());
    }
    SECTION("a bound of one level sees only the slices")
    {
        auto bounds = ConfinedTree::Bounds();
        bounds.maxDepth = 1;
        auto source = tree.Source(bounds);
        CHECK(Unwrap(Unwrap(source.Sample()).confined).empty());
    }
}

TEST_CASE("PsiPressureSource says why the cgroup tree cannot be read", "[platform][psi][confined]")
{
    ConfinedTree tree;
    tree.host->listRefusals["/sys/fs/cgroup"] = std::errc::permission_denied;
    auto source = tree.Source();
    auto const sample = Unwrap(source.Sample());
    CHECK(sample.system.some.avg10 == 15.0);
    CHECK_FALSE(sample.confined.has_value());
    CHECK(Unwrap(sample.confinedFault).contains("/sys/fs/cgroup"));
}

TEST_CASE("ConfinedScanBoundsFor leaves out this process's own cgroup and the compiles'", "[platform][psi][confined]")
{
    FakeCgroupHost host;
    host.files["/proc/self/cgroup"] = "0::/system.slice/pace-node.service/node\n";
    auto const bounds = ConfinedScanBoundsFor(host, std::filesystem::path { "/sys/fs/cgroup/x/compiles" }, 0.8);
    CHECK(bounds.excluded
          == std::vector<std::filesystem::path> { "/sys/fs/cgroup/system.slice/pace-node.service/node",
                                                  "/sys/fs/cgroup/x/compiles" });
    CHECK(bounds.share == 0.8);
}
