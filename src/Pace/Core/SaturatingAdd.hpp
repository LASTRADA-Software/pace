// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>

namespace Pace
{

/// @file SaturatingAdd.hpp
/// Addition of counts that came from OUTSIDE this process, where wrapping would turn a huge figure into a small one.
///
/// A count read from another node's document is attacker- or bug-sized: summing two of them modulo 2^64 can make
/// "more than anything" read as "almost nothing", and a decision made on that is a confident wrong answer. A sum
/// that saturates stays the largest figure there is, which every caller can recognise with `IsSaturated`.

/// @param first A count.
/// @param second A count.
/// @return Their sum, or `UINT64_MAX` when it does not fit.
[[nodiscard]] constexpr std::uint64_t SaturatingAdd(std::uint64_t first, std::uint64_t second) noexcept
{
    constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    return second > Max - first ? Max : first + second;
}

/// @param count A count produced by `SaturatingAdd`.
/// @return Whether it is the ceiling, i.e. the true figure may be larger and is unknown.
[[nodiscard]] constexpr bool IsSaturated(std::uint64_t count) noexcept
{
    return count == std::numeric_limits<std::uint64_t>::max();
}

} // namespace Pace
