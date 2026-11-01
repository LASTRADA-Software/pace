// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace Pace
{

/// @file AsciiFold.hpp
/// Case folding over ASCII alone: what two labels a person types are compared by when "the same name in another case"
/// must count as the same name.
///
/// **ASCII only, and every other byte compared exactly.** `A`-`Z` fold to `a`-`z` and nothing else moves: a byte at or
/// above 0x80 is part of a multi-byte UTF-8 sequence and is kept as it is, so `Ä` and `ä` stay two names. Unicode
/// case folding would need tables this project does not carry (a dependency), and a fold that knew some scripts and
/// not others would answer "same name" for one pair and "different" for its neighbour with nothing to say why. A
/// narrow rule that is the same everywhere is the one a refusal can explain.
///
/// Locale-free by construction: `std::tolower` reads the C locale's `LC_CTYPE`, which a process may change, and two
/// nodes judging one name under two locales would disagree about whether it is taken.

/// The byte @p ch with an ASCII capital letter folded to lower case.
/// @param ch A byte.
/// @return `ch + 32` for `A`-`Z`; @p ch unchanged otherwise, every byte at or above 0x80 included.
[[nodiscard]] constexpr char AsciiFoldByte(char ch) noexcept
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// @p text with every ASCII capital folded to lower case, and every other byte kept.
/// @param text Any bytes.
/// @return The folded copy, of the same length.
[[nodiscard]] inline std::string AsciiFold(std::string_view text)
{
    auto folded = std::string { text };
    std::ranges::transform(folded, folded.begin(), AsciiFoldByte);
    return folded;
}

/// Whether @p a and @p b are one name under `AsciiFold`.
/// @param a A name.
/// @param b Another.
/// @return True when they differ, if at all, only in the case of ASCII letters.
[[nodiscard]] constexpr bool AsciiFoldEquals(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, {}, AsciiFoldByte, AsciiFoldByte);
}

} // namespace Pace
