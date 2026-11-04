// SPDX-License-Identifier: Apache-2.0
#include "McpToolSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

using namespace Pace::Node::McpSupport;

TEST_CASE("Rfc3339 renders the instants a four-digit year holds and nothing else", "[pace-node][mcp][rfc3339]")
{
    CHECK(Rfc3339(0) == std::optional<std::string> { "1970-01-01T00:00:00Z" });
    CHECK(Rfc3339(1'700'000'000'000) == std::optional<std::string> { "2023-11-14T22:13:20Z" });
    CHECK(Rfc3339(Rfc3339LastMillis) == std::optional<std::string> { "9999-12-31T23:59:59Z" });
    // Outside it the instant is absent: no throw, no wrapped text.
    CHECK_FALSE(Rfc3339(-1).has_value());
    CHECK_FALSE(Rfc3339(Rfc3339LastMillis + 1).has_value());
    CHECK_FALSE(Rfc3339(std::numeric_limits<std::int64_t>::max()).has_value());
    CHECK_FALSE(Rfc3339(std::numeric_limits<std::int64_t>::min()).has_value());
}
