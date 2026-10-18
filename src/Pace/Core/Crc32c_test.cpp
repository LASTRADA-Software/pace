// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Bytes.hpp>
#include <Pace/Core/Crc32c.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

TEST_CASE("CRC32C of empty input is zero", "[crc32c]")
{
    REQUIRE(Pace::Crc32c::Compute({}) == 0U);
}

TEST_CASE("CRC32C standard test vectors", "[crc32c]")
{
    // RFC 3720 / iSCSI test vectors for CRC-32C.
    // "123456789" -> 0xE3069283
    auto const sv = std::string_view { "123456789" };
    REQUIRE(Pace::Crc32c::Compute(Pace::AsBytes(sv)) == 0xE3069283U);
}

TEST_CASE("CRC32C streaming equals one-shot", "[crc32c]")
{
    auto const sv = std::string_view { "the quick brown fox jumps over the lazy dog" };
    auto const oneShot = Pace::Crc32c::Compute(Pace::AsBytes(sv));

    auto state = Pace::Crc32c::Seed;
    auto const first = Pace::AsBytes(sv.substr(0, 16));
    auto const second = Pace::AsBytes(sv.substr(16));
    Pace::Crc32c::Update(state, first);
    Pace::Crc32c::Update(state, second);
    REQUIRE(Pace::Crc32c::Finalise(state) == oneShot);
}

TEST_CASE("CRC32C detects single-bit changes", "[crc32c]")
{
    std::string_view const a = "pace-kv";
    std::string_view const b = "PaceKv";
    REQUIRE(Pace::Crc32c::Compute(Pace::AsBytes(a)) != Pace::Crc32c::Compute(Pace::AsBytes(b)));
}
