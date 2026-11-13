// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/MemoryPressure.hpp>

#include <catch2/catch_test_macros.hpp>

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
    }
    SECTION("a compiles cgroup is read from its memory.pressure")
    {
        host->files["/sys/fs/cgroup/x/compiles/memory.pressure"] = std::string { SomeOnly };
        PsiPressureSource source { host, std::filesystem::path { "/sys/fs/cgroup/x/compiles" } };
        auto const sample = Unwrap(source.Sample());
        CHECK(Unwrap(sample.compiles).some.avg10 == 0.25);
    }
    SECTION("an unreadable compiles file is an error, not a calm reading")
    {
        PsiPressureSource source { host, std::filesystem::path { "/sys/fs/cgroup/x/compiles" } };
        CHECK_FALSE(source.Sample().has_value());
    }
}

TEST_CASE("PsiPressureSource reports a kernel without PSI", "[platform][psi]")
{
    auto host = std::make_shared<FakeCgroupHost>();
    PsiPressureSource source { host, std::nullopt };
    CHECK_FALSE(source.Sample().has_value());
}
