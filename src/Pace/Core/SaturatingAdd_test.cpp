// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/SaturatingAdd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("SaturatingAdd adds, and saturates instead of wrapping", "[core][saturating]")
{
    constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    CHECK(Pace::SaturatingAdd(2, 3) == 5);
    CHECK(Pace::SaturatingAdd(0, Max) == Max);
    CHECK(Pace::SaturatingAdd(Max, 0) == Max);
    CHECK(Pace::SaturatingAdd(Max - 1, 1) == Max);
    CHECK(Pace::SaturatingAdd(Max - 1, 2) == Max);
    CHECK(Pace::SaturatingAdd(Max, Max) == Max);
    CHECK_FALSE(Pace::IsSaturated(Max - 1));
    CHECK(Pace::IsSaturated(Max));
}
