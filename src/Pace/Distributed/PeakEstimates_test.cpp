// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/PeakEstimates.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace Pace::Distributed;

namespace
{
constexpr std::uint64_t GiB = 1ULL << 30;
constexpr std::uint64_t MiB = 1ULL << 20;
constexpr std::uint64_t Default = 2 * GiB;

EstimateKey Key(std::string cacheKey, std::string path = "a.cpp", std::string identity = "fp")
{
    return EstimateKey { std::move(cacheKey), std::move(path), std::move(identity) };
}
} // namespace

TEST_CASE("PeakEstimates.unknown-is-default", "[distributed][estimates]")
{
    PeakEstimates const estimates { Default, 1024 };
    CHECK(estimates.Estimate(Key("k")) == Default);
    CHECK(estimates.DefaultBytes() == Default);
}

TEST_CASE("PeakEstimates.exact-key-wins", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1"), 3 * GiB);
    CHECK(estimates.Estimate(Key("k1")) == 3 * GiB);
}

TEST_CASE("PeakEstimates.path-and-identity-fallback", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1"), 3 * GiB);
    CHECK(estimates.Estimate(Key("k2")) == 3 * GiB);
}

TEST_CASE("PeakEstimates.fallback-keyed-on-identity", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1"), 3 * GiB);
    CHECK(estimates.Estimate(Key("k2", "a.cpp", "other")) == Default);
}

TEST_CASE("PeakEstimates.empty-path-skips-fallback", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1", ""), 3 * GiB);
    CHECK(estimates.Estimate(Key("k2", "")) == Default);
    CHECK(estimates.Estimate(Key("k1", "")) == 3 * GiB);
}

TEST_CASE("PeakEstimates.fallback-takes-max", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1"), 1 * GiB);
    estimates.Record(Key("k2"), 4 * GiB);
    CHECK(estimates.Estimate(Key("k3")) == 4 * GiB);
}

TEST_CASE("PeakEstimates.latest-measurement-for-exact-key", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1"), 4 * GiB);
    estimates.Record(Key("k1"), 1 * GiB);
    CHECK(estimates.Estimate(Key("k1")) == 1 * GiB);
    CHECK(estimates.Estimate(Key("k2")) == 4 * GiB);
}

TEST_CASE("PeakEstimates.killed-raises-to-default", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 1024 };
    estimates.Record(Key("k1"), 512 * MiB);
    estimates.RecordKilled(Key("k1"));
    CHECK(estimates.Estimate(Key("k1")) == Default);

    estimates.Record(Key("k2", "b.cpp"), 5 * GiB);
    estimates.RecordKilled(Key("k2", "b.cpp"));
    CHECK(estimates.Estimate(Key("k2", "b.cpp")) == 5 * GiB);

    estimates.RecordKilled(Key("never-seen", "c.cpp"));
    CHECK(estimates.Estimate(Key("never-seen", "c.cpp")) == Default);
}

TEST_CASE("PeakEstimates.bounded", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 2 };
    estimates.Record(Key("k1"), 3 * GiB);
    estimates.Record(Key("k2"), 1 * GiB);
    estimates.Record(Key("k3"), 1 * GiB);
    // k1 was evicted as an exact key; the path still remembers the maximum.
    CHECK(estimates.Estimate(Key("k1")) == 3 * GiB);
    CHECK(estimates.Estimate(Key("k2")) == 1 * GiB);
    CHECK(estimates.Estimate(Key("k3")) == 1 * GiB);
}

TEST_CASE("PeakEstimates.bounded-evicts-least-recent-shape", "[distributed][estimates]")
{
    PeakEstimates estimates { Default, 2 };
    estimates.Record(Key("k1", "a.cpp"), 3 * GiB);
    estimates.Record(Key("k2", "b.cpp"), 3 * GiB);
    estimates.Record(Key("k3", "c.cpp"), 3 * GiB);
    CHECK(estimates.Estimate(Key("x", "a.cpp")) == Default);
    CHECK(estimates.Estimate(Key("x", "c.cpp")) == 3 * GiB);
}
