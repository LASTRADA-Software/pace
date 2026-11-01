// SPDX-License-Identifier: Apache-2.0
#include "McpNodeTools.hpp"
#include "McpUpstream.hpp"
#include "McpUpstreamFakes.hpp"
#include "NodeStatusResponder.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/NodeStatusView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// The id this node answers to in every case.
constexpr std::string_view SelfId = "node-self";

/// Where the answering node is, as the scripted upstream reports it.
constexpr std::string_view AnsweredAt = "10.0.0.2:6674";

/// @return An upstream answering as `AnsweredAt` under term 3, refusing `Unreachable` until scripted.
[[nodiscard]] McpFakes::ScriptedUpstream NodeUpstream()
{
    return McpFakes::ScriptedUpstream { McpFakes::UpstreamParticipant { .answeredBy = std::string { AnsweredAt },
                                                                        .term = std::uint64_t { 3 } },
                                        UpstreamFailure::Unreachable };
}

using McpFakes::FixedCluster;

/// A configuration source that answers one configuration.
class FixedConfig final: public INodeConfigSource
{
  public:
    /// @param cfg The configuration in force.
    explicit FixedConfig(NodeConfig cfg):
        _cfg { std::move(cfg) }
    {
    }

    /// @copydoc INodeConfigSource::Current
    [[nodiscard]] NodeConfig Current() const override
    {
        return _cfg;
    }

  private:
    NodeConfig _cfg;
};

using McpFakes::StateOf;

/// Run `node_status` with @p arguments.
/// @param upstream What it asks.
/// @param cluster Where names resolve.
/// @param arguments Its arguments, as JSON text.
/// @return What it answered.
[[nodiscard]] Mcp::ToolResult CallNodeStatus(IMcpUpstream& upstream,
                                             IAppliedClusterView const& cluster,
                                             std::string_view arguments = "{}")
{
    auto const tools = NodeReadTools(upstream, cluster, std::string { SelfId });
    auto const row = std::ranges::find(tools, std::string_view { "node_status" }, &Mcp::ToolRow::name);
    REQUIRE(row != tools.end());
    auto const args = Json::Parse(arguments);
    REQUIRE(args.has_value());
    return row->handler(Mcp::ToolCall { .arguments = Unwrap(args), .clientName = "test" });
}

/// @param value An object.
/// @param key A member.
/// @return The member; the case fails when it is missing.
[[nodiscard]] Json::Value const& At(Json::Value const& value, std::string_view key)
{
    auto const* const member = value.Find(key);
    INFO("key " << key << " in " << Json::Serialize(value));
    REQUIRE(member != nullptr);
    return *member;
}

/// @param value An array.
/// @return Its elements; the case fails when it is not one.
[[nodiscard]] Json::Array const& Elements(Json::Value const& value)
{
    auto const* const elements = value.AsArray();
    INFO(Json::Serialize(value));
    REQUIRE(elements != nullptr);
    return *elements;
}

/// A status with every detail engaged, the disk tier absent and a credential redacted.
[[nodiscard]] Wire::NodeStatusFields EveryDetail()
{
    auto fields = Wire::NodeStatusFields {};
    fields.version = "9.8.7";
    fields.nodeId = std::string { SelfId };
    fields.uptimeSeconds = 42;
    fields.components = Wire::NodeComponentBit::CacheTier | Wire::NodeComponentBit::Worker;
    fields.runtime.compileSlots = 8;
    fields.runtime.compilesInFlight = 1;
    fields.runtime.cordon = Wire::WireCordonState::Serving;
    fields.runtime.slotCeilings = Wire::NodeSlotCeilingsFields {
        .ceilings = { Wire::NodeSlotCeilingFields { .name = "registered", .slots = 8 },
                      Wire::NodeSlotCeilingFields { .name = "memory", .slots = 3 },
                      Wire::NodeSlotCeilingFields { .name = "cordoned", .slots = std::nullopt } },
        .available = 3,
        .binding = "memory",
        .sampledSecondsAgo = 5,
    };
    fields.runtime.tiers = Wire::NodeTiersFields {
        Wire::NodeTierFields { .entries = 4, .bytesUsed = 400, .bytesLimit = 0, .hits = 3, .misses = 1 }, std::nullopt
    };
    fields.runtime.effectiveConfig = std::vector {
        Wire::NodeSettingFields { .name = "--toolchain", .kind = Wire::WireSettingValue::Value, .text = "gcc" },
        Wire::NodeSettingFields { .name = "--toolchain", .kind = Wire::WireSettingValue::Value, .text = "clang" },
        Wire::NodeSettingFields { .name = "--slots", .kind = Wire::WireSettingValue::Unset, .text = {} },
        Wire::NodeSettingFields { .name = "--requirepass", .kind = Wire::WireSettingValue::Redacted, .text = {} },
    };
    return fields;
}

} // namespace

TEST_CASE("node_status equals pacectl node for the same reply", "[mcp][node-status][node-status-detail]")
{
    // **One derivation**: the tool decodes the reply `pacectl node` decodes and reads every shared quantity through
    // `NodeStatusView`, which `pacectl node`'s record reads too (`NodeVerbs_test` holds that half). So each value
    // the tool reports is compared with the view's answer over the SAME decoded payload.
    auto upstream = NodeUpstream();
    FixedCluster const cluster { StateOf({ SelfId }) };
    upstream.AnswerOk(Wire::EncodeNodeStatus(EveryDetail()));
    auto const result = CallNodeStatus(upstream, cluster);
    REQUIRE_FALSE(result.isError);

    // It asked THIS node, with the node-status request the CLI sends.
    REQUIRE(upstream.calls.size() == 1);
    CHECK(upstream.calls.front().target == UpstreamTarget::Node);
    CHECK(upstream.calls.front().nodeIdOrEmpty.empty());
    auto const request = Wire::EncodeNodeStatusRequest();
    CHECK(upstream.calls.front().frame == std::string { reinterpret_cast<char const*>(request.data()), request.size() });

    REQUIRE(upstream.answer.has_value());
    auto const payload = upstream.answer.has_value() ? upstream.answer->payload : std::string {};
    auto const decoded = Wire::DecodeNodeStatus(std::as_bytes(std::span { payload.data(), payload.size() }));
    REQUIRE(decoded.has_value());
    auto const& runtime = Unwrap(decoded).runtime;
    auto const& structured = result.structured;

    // The envelope every forwarded answer opens with.
    CHECK(At(structured, "answered_by").AsString() != nullptr);
    CHECK(*At(structured, "answered_by").AsString() == AnsweredAt);
    CHECK(At(structured, "term").AsInt() == std::optional<std::int64_t> { 3 });
    CHECK(*At(structured, "term_scope").AsString() == "this-node");
    CHECK(At(structured, "node").IsNull());

    // Components, by the view's names.
    auto const& components = Elements(At(structured, "components"));
    auto const names = NodeStatusView::ComponentNames(Unwrap(decoded).components);
    REQUIRE(components.size() == names.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, names.size()))
        CHECK(*components[index].AsString() == names[index]);

    // Tiers: the view's rows, its budget reading and its hit ratio.
    auto const tiers = NodeStatusView::TiersOf(runtime);
    REQUIRE(tiers.has_value());
    auto const& tierRows = Elements(At(structured, "tiers"));
    REQUIRE(tierRows.size() == Unwrap(tiers).size());
    for (auto const index: std::views::iota(std::size_t { 0 }, tierRows.size()))
    {
        auto const& view = Unwrap(tiers)[index];
        auto const& row = tierRows[index];
        CHECK(*At(row, "tier").AsString() == view.name);
        CHECK(At(row, "present").AsBool() == std::optional { view.stats.has_value() });
        if (!view.stats.has_value())
            continue;
        auto const ratio = NodeStatusView::HitRatio(*view.stats);
        auto const* const reported = std::get_if<double>(&At(row, "hit_ratio").v);
        REQUIRE(reported != nullptr);
        CHECK(std::optional { *reported } == ratio);
        CHECK(At(row, "budget_unbounded").AsBool()
              == std::optional { !NodeStatusView::BudgetBytes(*view.stats).has_value() });
    }

    // The configuration: the view's settings, gathered and redacted the way the CLI prints them.
    auto const settings = NodeStatusView::SettingsOf(runtime);
    REQUIRE(settings.has_value());
    auto const& settingRows = Elements(At(structured, "effective_config"));
    REQUIRE(settingRows.size() == Unwrap(settings).size());
    for (auto const index: std::views::iota(std::size_t { 0 }, settingRows.size()))
    {
        auto const& view = Unwrap(settings)[index];
        CHECK(*At(settingRows[index], "name").AsString() == view.name);
        auto const& values = At(settingRows[index], "values");
        CHECK(values.IsNull() == !view.values.has_value());
        if (!view.values.has_value())
            continue;
        auto const& listed = Elements(values);
        REQUIRE(listed.size() == view.values->size());
        for (auto const at: std::views::iota(std::size_t { 0 }, listed.size()))
            CHECK(*listed[at].AsString() == (*view.values)[at]);
    }

    // The ceilings: the view's rows, each with its binding mark -- the view `pacectl node`'s ceiling cells read.
    auto const ceilingsView = NodeStatusView::CeilingsOf(runtime);
    REQUIRE(ceilingsView.has_value());
    auto const& view = Unwrap(ceilingsView);
    auto const& ceilings = At(structured, "slot_ceilings");
    CHECK(*At(ceilings, "binding").AsString() == view.binding);
    CHECK(At(ceilings, "available").AsUint() == std::optional<std::uint64_t> { view.available });
    CHECK(At(ceilings, "sampled_age_s").AsUint() == std::optional<std::uint64_t> { view.sampledSecondsAgo });
    auto const& ceilingRows = Elements(At(ceilings, "ceilings"));
    REQUIRE(ceilingRows.size() == view.ceilings.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, ceilingRows.size()))
    {
        CHECK(*At(ceilingRows[index], "name").AsString() == view.ceilings[index].name);
        CHECK(At(ceilingRows[index], "binding").AsBool() == std::optional { view.ceilings[index].binding });
        auto const& slots = At(ceilingRows[index], "slots");
        CHECK(slots.IsNull() == !view.ceilings[index].slots.has_value());
        CHECK(slots.AsUint() == view.ceilings[index].slots.transform([](std::uint32_t n) { return std::uint64_t { n }; }));
    }

    // And a node that says nothing about overrides is ABSENT there, never an empty list.
    CHECK(At(structured, "budget_overrides").IsNull());
}

TEST_CASE("secrets are redacted", "[mcp][node-status][node-status-detail]")
{
    // From the configuration a node runs by, through the node's own status source, over the wire, to the tool's
    // structured value and its text: the credential appears in none of them, and the setting says it is set.
    constexpr std::string_view Secret = "hunter2-not-for-anyone";
    auto cfg = NodeConfig {};
    cfg.requirePass = Secret;
    FixedConfig const inForce { cfg };
    core::platform::ManualClock clock;
    auto const startCfg = NodeConfig {};
    ConfiguredNodeStatus const status { startCfg,
                                        clock,
                                        clock.now(),
                                        "1.0.0",
                                        std::string { SelfId },
                                        NodeComponents {},
                                        NodeRuntimeSources { .config = &inForce } };
    auto const described = status.Describe();
    auto const bytes = Wire::EncodeNodeStatus(described);
    auto const wire = std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    CHECK(wire.find(Secret) == std::string::npos);

    auto upstream = NodeUpstream();
    FixedCluster const cluster { std::nullopt };
    upstream.AnswerOk(Wire::EncodeNodeStatus(described));
    auto const result = CallNodeStatus(upstream, cluster);
    REQUIRE_FALSE(result.isError);
    CHECK(Json::Serialize(result.structured).find(Secret) == std::string::npos);
    CHECK(result.text.find(Secret) == std::string::npos);

    auto const& rows = Elements(At(result.structured, "effective_config"));
    auto const row = std::ranges::find_if(rows, [](Json::Value const& setting) {
        auto const* const name = setting.Find("name");
        return name != nullptr && name->AsString() != nullptr && *name->AsString() == "--requirepass";
    });
    REQUIRE(row != rows.end());
    auto const& values = Elements(At(*row, "values"));
    REQUIRE(values.size() == 1);
    CHECK(*values.front().AsString() == Wire::RedactedSettingText);
}

TEST_CASE("a prefix of an id is not a match", "[mcp][node-status][node-name]")
{
    // Nothing is asked: a name that resolves nowhere is refused before any node is, never answered about another.
    auto upstream = NodeUpstream();
    FixedCluster const cluster { StateOf({ "node-alpha-1", "node-beta" }) };
    upstream.AnswerOk(Wire::EncodeNodeStatus(EveryDetail()));

    auto const prefix = CallNodeStatus(upstream, cluster, R"({"node":"node-alpha"})");
    CHECK(prefix.isError);
    CHECK(upstream.calls.empty());
    // It lists whom the name could have meant.
    CHECK(prefix.text.find("node-alpha-1") != std::string::npos);
    CHECK(prefix.text.find("node-beta") != std::string::npos);

    // The control: the whole id resolves, and is what is asked.
    auto const exact = CallNodeStatus(upstream, cluster, R"({"node":"node-alpha-1"})");
    REQUIRE_FALSE(exact.isError);
    REQUIRE(upstream.calls.size() == 1);
    CHECK(upstream.calls.front().nodeIdOrEmpty == "node-alpha-1");
    CHECK(*At(exact.structured, "node").AsString() == "node-alpha-1");

    // And a `node` that is not a string is refused rather than read as "this node".
    auto const number = CallNodeStatus(upstream, cluster, R"({"node":7})");
    CHECK(number.isError);
    CHECK(upstream.calls.size() == 1);
}

TEST_CASE("absent tier renders absent", "[mcp][node-status][node-status-detail]")
{
    auto upstream = NodeUpstream();
    FixedCluster const cluster { std::nullopt };

    // A cache that has a memory tier and no disk tier: the disk row says so, with every figure null -- never a
    // disk tier holding nothing.
    upstream.AnswerOk(Wire::EncodeNodeStatus(EveryDetail()));
    auto const result = CallNodeStatus(upstream, cluster);
    REQUIRE_FALSE(result.isError);
    auto const& tiers = Elements(At(result.structured, "tiers"));
    REQUIRE(tiers.size() == 2);
    CHECK(*At(tiers[1], "tier").AsString() == "disk");
    CHECK(At(tiers[1], "present").AsBool() == std::optional { false });
    CHECK(At(tiers[1], "entries").IsNull());
    CHECK(At(tiers[1], "bytes").IsNull());
    CHECK(At(tiers[1], "hit_ratio").IsNull());
    CHECK(At(tiers[0], "present").AsBool() == std::optional { true });
    CHECK(At(tiers[0], "entries").AsInt() == std::optional<std::int64_t> { 4 });
    // A zero budget is unbounded, and says so rather than reading as a budget of nothing.
    CHECK(At(tiers[0], "budget_bytes").IsNull());
    CHECK(At(tiers[0], "budget_unbounded").AsBool() == std::optional { true });

    // A node with no cache at all: no tiers, which is not a cache with empty ones.
    auto noCache = EveryDetail();
    noCache.runtime.tiers.reset();
    upstream.AnswerOk(Wire::EncodeNodeStatus(noCache));
    auto const without = CallNodeStatus(upstream, cluster);
    REQUIRE_FALSE(without.isError);
    CHECK(At(without.structured, "tiers").IsNull());
}

TEST_CASE("node_status names who could not be asked rather than answering", "[mcp][node-status]")
{
    auto upstream = NodeUpstream();
    FixedCluster const cluster { StateOf({ SelfId, "node-far" }) };
    upstream.answer =
        std::unexpected { std::pair { UpstreamFailure::Unreachable, std::string { "node-far at 10.0.0.9:6674" } } };
    auto const result = CallNodeStatus(upstream, cluster, R"({"node":"node-far"})");
    CHECK(result.isError);
    CHECK(result.text.find("unreachable") != std::string::npos);
    CHECK(result.text.find("node-far") != std::string::npos);

    // A node with no applied state can name only itself.
    FixedCluster const none { std::nullopt };
    auto const other = CallNodeStatus(upstream, none, R"({"node":"node-far"})");
    CHECK(other.isError);
    CHECK(other.text.find(std::string { SelfId }) != std::string::npos);
}

TEST_CASE("node_status carries 64-bit figures exactly and marks a value that is not UTF-8",
          "[mcp][node-status][node-status-detail]")
{
    auto upstream = NodeUpstream();
    FixedCluster const cluster { std::nullopt };
    auto fields = EveryDetail();
    fields.runtime.tiers = Wire::NodeTiersFields { Wire::NodeTierFields {
        .entries = UINT64_MAX, .bytesUsed = (1ULL << 63) + 1, .bytesLimit = UINT64_MAX, .hits = UINT64_MAX, .misses = 1 } };
    fields.runtime.effectiveConfig = std::vector {
        Wire::NodeSettingFields {
            .name = "--cache-dir", .kind = Wire::WireSettingValue::Value, .text = std::string { "/var/cache/p\xff" } },
        Wire::NodeSettingFields { .name = "--pidfile", .kind = Wire::WireSettingValue::Value, .text = "/run/p" },
    };
    upstream.AnswerOk(Wire::EncodeNodeStatus(fields));
    auto const result = CallNodeStatus(upstream, cluster);
    REQUIRE_FALSE(result.isError);

    // Past 2^63 a figure is written as the unsigned number it is -- never a negative one.
    auto const text = Json::Serialize(result.structured);
    CHECK(text.find("\"entries\":18446744073709551615") != std::string::npos);
    CHECK(text.find("\"bytes\":9223372036854775809") != std::string::npos);
    CHECK(text.find("\"entries\":-") == std::string::npos);
    auto const& tier = Elements(At(result.structured, "tiers"))[0];
    CHECK(At(tier, "budget_bytes").AsUint() == std::optional<std::uint64_t> { UINT64_MAX });
    // And the ratio of two counters whose sum wraps is still a ratio.
    auto const* const ratio = std::get_if<double>(&At(tier, "hit_ratio").v);
    REQUIRE(ratio != nullptr);
    CHECK(*ratio <= 1.0);
    CHECK(*ratio > 0.999);

    // The path with a 0xFF byte: escaped behind the marker and flagged, never a replacement character.
    auto const& settings = Elements(At(result.structured, "effective_config"));
    REQUIRE(settings.size() == 2);
    CHECK(At(settings[0], "non_utf8").AsBool() == std::optional { true });
    auto const& values = Elements(At(settings[0], "values"));
    REQUIRE(values.size() == 1);
    CHECK(*values.front().AsString() == "<non-utf8: /var/cache/p\\xFF>");
    CHECK(text.find("\xef\xbf\xbd") == std::string::npos);
    CHECK(At(settings[1], "non_utf8").AsBool() == std::optional { false });
}
