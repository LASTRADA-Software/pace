// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/AsciiFold.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace Pace;

TEST_CASE("ASCII folding moves the capital letters and nothing else", "[core][ascii-fold]")
{
    CHECK(AsciiFold("Build-Box_7") == "build-box_7");
    CHECK(AsciiFold("") == "");
    // The neighbours of the range: '@' sits before 'A' and '[' after 'Z', so a fold off by one moves them.
    CHECK(AsciiFold("@AZ[") == "@az[");
    // Already lower case, digits and punctuation are kept.
    CHECK(AsciiFold("abc 012 -_.") == "abc 012 -_.");

    static_assert(AsciiFoldEquals("Office-PC", "office-pc"));
    static_assert(!AsciiFoldEquals("office-pc", "office-pc2"));
}

TEST_CASE("ASCII folding compares every byte above ASCII exactly", "[core][ascii-fold]")
{
    // "Ä" is C3 84 and "ä" is C3 A4: one name to a person, two to this rule -- deliberately, since a Unicode fold
    // would need tables this project does not carry.
    constexpr auto upper = std::string_view { "\xC3\x84rger" };
    constexpr auto lower = std::string_view { "\xC3\xA4rger" };
    CHECK(AsciiFold(upper) == upper);
    CHECK_FALSE(AsciiFoldEquals(upper, lower));
    // The ASCII half of a mixed name still folds.
    CHECK(AsciiFoldEquals("\xC3\xA4RGER", "\xC3\xA4rger"));
}
