// SPDX-License-Identifier: Apache-2.0
#include <Pace/Protocol/NodeStatusView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::CompileCacheWire;
using Pace::Testing::Unwrap;

TEST_CASE("A cache that lacks a tier reports it absent, and a node with no cache reports no tiers at all",
          "[node-status-view][node-status-detail]")
{
    auto runtime = NodeRuntimeFields {};
    CHECK_FALSE(NodeStatusView::TiersOf(runtime).has_value());

    runtime.tiers =
        NodeTiersFields { NodeTierFields { .entries = 3, .bytesUsed = 30, .bytesLimit = 0, .hits = 0, .misses = 0 } };
    auto const read = NodeStatusView::TiersOf(runtime);
    REQUIRE(read.has_value());
    auto const& tiers = Unwrap(read);
    REQUIRE(tiers.size() == StorageTierTable.size());
    CHECK(tiers[0].name == "memory");
    CHECK(tiers[0].stats.has_value());
    // The disk position was not sent: a tier this cache does not have, never one holding nothing.
    CHECK(tiers[1].name == "disk");
    CHECK_FALSE(tiers[1].stats.has_value());
}

TEST_CASE("A hit ratio of no lookups is no reading, and a zero budget is unbounded",
          "[node-status-view][node-status-detail]")
{
    auto stats = NodeTierFields { .entries = 0, .bytesUsed = 0, .bytesLimit = 0, .hits = 0, .misses = 0 };
    CHECK_FALSE(NodeStatusView::HitRatio(stats).has_value());
    CHECK_FALSE(NodeStatusView::BudgetBytes(stats).has_value());

    stats.hits = 3;
    stats.misses = 1;
    stats.bytesLimit = 4096;
    CHECK(NodeStatusView::HitRatio(stats) == std::optional { 0.75 });
    CHECK(NodeStatusView::BudgetBytes(stats) == std::optional<std::uint64_t> { 4096 });

    stats.hits = 0;
    CHECK(NodeStatusView::HitRatio(stats) == std::optional { 0.0 });
}

TEST_CASE("A list setting's rows gather into one setting, unset reads absent and a redaction reads its marker",
          "[node-status-view][node-status-detail]")
{
    auto runtime = NodeRuntimeFields {};
    CHECK_FALSE(NodeStatusView::SettingsOf(runtime).has_value());

    runtime.effectiveConfig = std::vector {
        NodeSettingFields { .name = "--toolchain", .kind = WireSettingValue::Value, .text = "gcc" },
        NodeSettingFields { .name = "--toolchain", .kind = WireSettingValue::Value, .text = "clang" },
        NodeSettingFields { .name = "--slots", .kind = WireSettingValue::Unset, .text = {} },
        NodeSettingFields { .name = "--requirepass", .kind = WireSettingValue::Redacted, .text = {} },
        NodeSettingFields { .name = "--listen-node", .kind = WireSettingValue::Value, .text = {} },
        NodeSettingFields { .name = "--fleet-seed", .kind = WireSettingValue::Omitted, .text = "2 more values" },
    };
    auto const read = NodeStatusView::SettingsOf(runtime);
    REQUIRE(read.has_value());
    auto const& settings = Unwrap(read);
    REQUIRE(settings.size() == 5);
    CHECK(settings[0].name == "--toolchain");
    CHECK(settings[0].values == std::optional { std::vector<std::string> { "gcc", "clang" } });
    CHECK(settings[1].name == "--slots");
    CHECK_FALSE(settings[1].values.has_value());
    CHECK(settings[2].values == std::optional { std::vector<std::string> { "<redacted>" } });
    // An empty value is a value -- `--listen-node=` serves nothing -- and must not read as unset.
    CHECK(settings[3].values == std::optional { std::vector<std::string> { "" } });
    CHECK(settings[4].values == std::optional { std::vector<std::string> { "<omitted: 2 more values>" } });
}

TEST_CASE("Component names follow the mask's bits and keep an unknown one by number", "[node-status-view]")
{
    CHECK(NodeStatusView::ComponentNames(0).empty());
    CHECK(NodeStatusView::ComponentNames(NodeComponentBit::Worker | NodeComponentBit::Consensus)
          == std::vector<std::string> { "worker", "consensus" });
    CHECK(NodeStatusView::ComponentNames(NodeComponentBit::CacheTier | 0x40U)
          == std::vector<std::string> { "cache-tier", "unknown(0x40)" });
}
