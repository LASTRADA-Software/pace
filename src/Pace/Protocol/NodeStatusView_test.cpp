// SPDX-License-Identifier: Apache-2.0
#include <Pace/Protocol/NodeStatusView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

    // Near UINT64_MAX the two counters sum past the width: the ratio stays in [0, 1] rather than dividing by a
    // wrapped total -- which, for these two, would be zero lookups, or a ratio far above one.
    stats.hits = UINT64_MAX;
    stats.misses = 1;
    auto const nearlyAll = NodeStatusView::HitRatio(stats);
    REQUIRE(nearlyAll.has_value());
    CHECK(Unwrap(nearlyAll) <= 1.0);
    CHECK(Unwrap(nearlyAll) > 0.999);
    stats.hits = UINT64_MAX;
    stats.misses = UINT64_MAX;
    CHECK(NodeStatusView::HitRatio(stats) == std::optional { 0.5 });
}

TEST_CASE("A value that is not UTF-8 is marked and escaped, never replaced", "[node-status-view][node-status-detail]")
{
    CHECK(NodeStatusView::ReadableText("/var/cache/plain") == "/var/cache/plain");
    // A 0xFF byte starts no UTF-8 sequence: spelled, marked, and the valid text around it kept.
    CHECK(NodeStatusView::ReadableText(std::string { "/var/cache/a\xff" }) == "<non-utf8: /var/cache/a\\xFF>");
    // A backslash already in such a value is doubled, so it cannot pass for an escape.
    CHECK(NodeStatusView::ReadableText(std::string { "c:\\x\xfe" }) == "<non-utf8: c:\\\\x\\xFE>");
    // Valid multi-byte UTF-8 beside the bad byte is kept as it is.
    CHECK(NodeStatusView::ReadableText(std::string { "\xc3\xa9\xff" }) == "<non-utf8: \xc3\xa9\\xFF>");

    auto runtime = NodeRuntimeFields {};
    runtime.effectiveConfig = std::vector {
        NodeSettingFields { .name = "--cache-dir", .kind = WireSettingValue::Value, .text = std::string { "/d\xff" } },
        NodeSettingFields { .name = "--pidfile", .kind = WireSettingValue::Value, .text = "/run/p" },
    };
    auto const read = NodeStatusView::SettingsOf(runtime);
    REQUIRE(read.has_value());
    auto const& settings = Unwrap(read);
    REQUIRE(settings.size() == 2);
    CHECK(settings[0].escaped);
    CHECK(settings[0].values == std::optional { std::vector<std::string> { "<non-utf8: /d\\xFF>" } });
    CHECK_FALSE(settings[1].escaped);
}

TEST_CASE("The slot ceilings read with the binding one marked", "[node-status-view][node-status-detail]")
{
    auto runtime = NodeRuntimeFields {};
    CHECK_FALSE(NodeStatusView::CeilingsOf(runtime).has_value());
    runtime.slotCeilings = NodeSlotCeilingsFields {
        .ceilings = { NodeSlotCeilingFields { .name = "registered", .slots = 8 },
                      NodeSlotCeilingFields { .name = "memory", .slots = 3 } },
        .available = 3,
        .binding = "memory",
        .sampledSecondsAgo = 9,
    };
    auto const read = NodeStatusView::CeilingsOf(runtime);
    REQUIRE(read.has_value());
    auto const& view = Unwrap(read);
    CHECK(view.available == 3);
    CHECK(view.binding == "memory");
    CHECK(view.sampledSecondsAgo == 9);
    REQUIRE(view.ceilings.size() == 2);
    CHECK_FALSE(view.ceilings[0].binding);
    CHECK(view.ceilings[1].binding);
    CHECK(view.ceilings[1].slots == std::optional<std::uint32_t> { 3 });
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

TEST_CASE("A budget row reads through one view: in force, from where, the override, its baseline and why",
          "[node-status-view][node-status-detail][budget]")
{
    auto runtime = NodeRuntimeFields {};
    CHECK_FALSE(NodeStatusView::BudgetOverridesOf(runtime).has_value());

    constexpr auto MiB = std::uint64_t { 1024 } * 1024;
    runtime.budgetOverrides = std::vector {
        NodeBudgetOverrideFields { .tier = 0,
                                   .bytes = 64 * MiB,
                                   .baselineBytes = 512 * MiB,
                                   .source = "cluster",
                                   .configuredBytes = 512 * MiB,
                                   .overrideBytes = 64 * MiB,
                                   .setAtUnixSeconds = std::int64_t { 1'767'225'600 },
                                   .setBy = "voter-1",
                                   .setByName = "build box",
                                   .excessBytes = 8 * MiB,
                                   .note = {} },
        NodeBudgetOverrideFields { .tier = 1,
                                   .bytes = std::nullopt,
                                   .baselineBytes = std::nullopt,
                                   .source = "configured",
                                   .configuredBytes = 0,
                                   .overrideBytes = 2048 * MiB,
                                   .setAtUnixSeconds = std::nullopt,
                                   .setBy = {},
                                   .setByName = {},
                                   .excessBytes = std::nullopt,
                                   .note = "not applied: this node has no disk tier" },
    };
    auto const views = Unwrap(NodeStatusView::BudgetOverridesOf(runtime));
    REQUIRE(views.size() == 2);
    CHECK(views[0].tier == "memory");
    CHECK(views[0].setAt == std::optional<std::string> { "2026-01-01T00:00:00Z" });
    CHECK(
        NodeStatusView::DescribeBudget(views[0])
        == R"(64M from cluster (configured 512M, override 64M, baseline 512M, set 2026-01-01T00:00:00Z by voter-1 "build box", converging: 8M over))");
    // A tier the node lacks has no budget in force -- never a zero -- and says why; an unreported baseline is `unset`.
    CHECK(views[1].tier == "disk");
    CHECK_FALSE(views[1].effectiveBytes.has_value());
    CHECK(NodeStatusView::DescribeBudget(views[1])
          == "no such tier (configured unbounded, override 2G, baseline unset); not applied: this node has no disk tier");
}
