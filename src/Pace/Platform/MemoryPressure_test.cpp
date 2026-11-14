// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/MemoryPressure.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
        CHECK_FALSE(Unwrap(source.Sample()).compilesAtLimit);
        CHECK_FALSE(Unwrap(source.Sample()).compilesAtLimit);
    }
    SECTION("within a twentieth of the ceiling: at it")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "8200000000\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "8589934592\n";
        PsiPressureSource source { host, compiles };
        CHECK(Unwrap(source.Sample()).compilesAtLimit);
    }
    SECTION("no ceiling (max) is never near, however much they hold")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "8589934592\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "max\n";
        PsiPressureSource source { host, compiles };
        CHECK_FALSE(Unwrap(source.Sample()).compilesAtLimit);
    }
    SECTION("a max count that rose since the previous sample: at it, though below now")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.current"] = "1073741824\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.max"] = "8589934592\n";
        host->files["/sys/fs/cgroup/x/compiles/memory.events"] = events(7);
        PsiPressureSource source { host, compiles };
        // The first sample has nothing to compare with: a count already standing is no rise.
        CHECK_FALSE(Unwrap(source.Sample()).compilesAtLimit);
        host->files["/sys/fs/cgroup/x/compiles/memory.events"] = events(9);
        CHECK(Unwrap(source.Sample()).compilesAtLimit);
        // Standing still again is no rise.
        CHECK_FALSE(Unwrap(source.Sample()).compilesAtLimit);
    }
    SECTION("nothing readable: not at it, which counts every stall as foreign")
    {
        PsiPressureSource source { host, compiles };
        CHECK_FALSE(Unwrap(source.Sample()).compilesAtLimit);
    }
}

TEST_CASE("PsiPressureSource reports a kernel without PSI", "[platform][psi]")
{
    auto host = std::make_shared<FakeCgroupHost>();
    PsiPressureSource source { host, std::nullopt };
    CHECK_FALSE(source.Sample().has_value());
}
