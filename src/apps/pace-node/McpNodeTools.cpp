// SPDX-License-Identifier: Apache-2.0
#include "McpNodeTools.hpp"
#include "McpToolSupport.hpp"

#include <Pace/Core/Ed25519.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/NodeStatusView.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;
    using namespace McpSupport;

    /// @param value A count the node sent.
    /// @return It as JSON, or `null` when the node did not say.
    template <typename T>
    [[nodiscard]] Json::Value NumberOrNull(std::optional<T> const& value)
    {
        return value.has_value() ? Int(static_cast<std::int64_t>(*value)) : Null();
    }

    /// @param text A name the view spells.
    /// @return It as JSON.
    [[nodiscard]] Json::Value Name(std::string_view text)
    {
        return Str(std::string { text });
    }

    /// Which node a call asks: this one, or the member the `node` argument names exactly.
    struct Target
    {
        std::string nodeId;               ///< The member's id; empty means this node.
        std::optional<std::string> asked; ///< The name as given, or nothing when none was.
    };

    /// @param call The call.
    /// @param cluster Where names resolve.
    /// @param selfId This node's id.
    /// @return Whom to ask, or the tool error naming why the argument names nobody.
    [[nodiscard]] std::expected<Target, Mcp::ToolResult> TargetOf(Mcp::ToolCall const& call,
                                                                  IAppliedClusterView const& cluster,
                                                                  std::string const& selfId)
    {
        auto const* const node = call.arguments.Find("node");
        if (node == nullptr || node->IsNull())
            return Target { .nodeId = {}, .asked = std::nullopt };
        // Refused rather than ignored: a `node` this tool cannot read answered about THIS node would be an answer
        // about a machine nobody named.
        auto const* const name = node->AsString();
        if (name == nullptr)
            return std::unexpected { Error("`node` must be a string: a member's id, matched exactly") };

        auto const state = cluster.AppliedState();
        if (!state.has_value())
        {
            if (*name == selfId && !selfId.empty())
                return Target { .nodeId = {}, .asked = *name };
            return std::unexpected { Error(std::format(
                "this node has applied no cluster state, so it can name no member but itself ({}); omit `node` to ask "
                "it",
                selfId.empty() ? std::string { "it has no id" } : selfId)) };
        }
        auto resolved = ResolveNodeName(*state, *name);
        if (!resolved.has_value())
            return std::unexpected { Error(std::move(resolved.error())) };
        return Target { .nodeId = std::move(*resolved), .asked = *name };
    }

    /// @param ceilings The record the node sent, or nothing.
    /// @return The `slot_ceilings` member.
    [[nodiscard]] Json::Value SlotCeilings(std::optional<Wire::NodeSlotCeilingsFields> const& ceilings)
    {
        if (!ceilings.has_value())
            return Null();
        auto rows = Json::Array {};
        rows.reserve(ceilings->ceilings.size());
        for (auto const& ceiling: ceilings->ceilings)
            rows.push_back(Obj({ { "name", Str(ceiling.name) },
                                 { "slots", NumberOrNull(ceiling.slots) },
                                 { "binding", Bool(ceiling.name == ceilings->binding) } }));
        return Obj({ { "available", Int(ceilings->available) },
                     { "binding", Str(ceilings->binding) },
                     { "sampled_age_s", Int(static_cast<std::int64_t>(ceilings->sampledSecondsAgo)) },
                     { "ceilings", Arr(std::move(rows)) } });
    }

    /// @param runtime The decoded record.
    /// @return The `tiers` member: `null` with no cache, else one row per tier this build names.
    [[nodiscard]] Json::Value Tiers(Wire::NodeRuntimeFields const& runtime)
    {
        auto const tiers = NodeStatusView::TiersOf(runtime);
        if (!tiers.has_value())
            return Null();
        auto rows = Json::Array {};
        for (auto const& tier: *tiers)
        {
            if (!tier.stats.has_value())
            {
                // Present false and every figure null: a tier this cache lacks, never one holding nothing.
                rows.push_back(Obj({ { "tier", Name(tier.name) },
                                     { "present", Bool(false) },
                                     { "entries", Null() },
                                     { "bytes", Null() },
                                     { "budget_bytes", Null() },
                                     { "budget_unbounded", Null() },
                                     { "hits", Null() },
                                     { "misses", Null() },
                                     { "hit_ratio", Null() } }));
                continue;
            }
            auto const& stats = *tier.stats;
            auto const budget = NodeStatusView::BudgetBytes(stats);
            auto const ratio = NodeStatusView::HitRatio(stats);
            rows.push_back(Obj({ { "tier", Name(tier.name) },
                                 { "present", Bool(true) },
                                 { "entries", Int(static_cast<std::int64_t>(stats.entries)) },
                                 { "bytes", Int(static_cast<std::int64_t>(stats.bytesUsed)) },
                                 { "budget_bytes", NumberOrNull(budget) },
                                 { "budget_unbounded", Bool(!budget.has_value()) },
                                 { "hits", Int(static_cast<std::int64_t>(stats.hits)) },
                                 { "misses", Int(static_cast<std::int64_t>(stats.misses)) },
                                 { "hit_ratio", ratio.has_value() ? Real(*ratio) : Null() } }));
        }
        return Arr(std::move(rows));
    }

    /// @param runtime The decoded record.
    /// @return The `effective_config` member: `null` from a node that did not say, else one row per setting.
    [[nodiscard]] Json::Value Settings(Wire::NodeRuntimeFields const& runtime)
    {
        auto const settings = NodeStatusView::SettingsOf(runtime);
        if (!settings.has_value())
            return Null();
        auto rows = Json::Array {};
        rows.reserve(settings->size());
        for (auto const& setting: *settings)
        {
            auto values = Null();
            if (setting.values.has_value())
            {
                auto array = Json::Array {};
                for (auto const& value: *setting.values)
                    array.push_back(Str(value));
                values = Arr(std::move(array));
            }
            rows.push_back(Obj({ { "name", Str(setting.name) }, { "values", std::move(values) } }));
        }
        return Arr(std::move(rows));
    }

    /// @param overrides The record the node sent, or nothing.
    /// @return The `budget_overrides` member: `null` when the node did not say.
    [[nodiscard]] Json::Value Overrides(std::optional<std::vector<Wire::NodeBudgetOverrideFields>> const& overrides)
    {
        if (!overrides.has_value())
            return Null();
        auto rows = Json::Array {};
        for (auto const& entry: *overrides)
            rows.push_back(Obj({ { "tier",
                                   entry.tier < StorageTierTable.size() ? Name(StorageTierTable[entry.tier].name)
                                                                        : Str(std::format("tier-{}", entry.tier)) },
                                 { "bytes", Int(static_cast<std::int64_t>(entry.bytes)) },
                                 { "baseline_bytes", NumberOrNull(entry.baselineBytes) } }));
        return Arr(std::move(rows));
    }

    /// @param fields The decoded reply.
    /// @return The `surfaces` member.
    [[nodiscard]] Json::Value Surfaces(Wire::NodeStatusFields const& fields)
    {
        auto rows = Json::Array {};
        for (auto const& surface: fields.surfaces)
            rows.push_back(Obj({ { "surface", Name(NodeStatusView::SurfaceName(surface.surface)) },
                                 { "port", Int(surface.port) },
                                 { "tls", Bool(surface.tls) } }));
        return Arr(std::move(rows));
    }

    /// @param fields The decoded reply.
    /// @return The one-line summary the text block leads with.
    [[nodiscard]] std::string Summary(Wire::NodeStatusFields const& fields)
    {
        auto const& runtime = fields.runtime;
        auto components = std::string {};
        for (auto const& name: NodeStatusView::ComponentNames(fields.components))
            components += (components.empty() ? "" : ", ") + name;
        auto slots = std::string { "no worker" };
        if (runtime.slotCeilings.has_value())
            slots = std::format(
                "{} slot(s) offerable, bound by {}", runtime.slotCeilings->available, runtime.slotCeilings->binding);
        else if (runtime.compileSlots.has_value())
            slots = std::format("{} slot(s), ceilings not sampled yet", *runtime.compileSlots);
        auto cache = std::string { "no cache" };
        if (auto const tiers = NodeStatusView::TiersOf(runtime); tiers.has_value())
        {
            cache.clear();
            for (auto const& tier: *tiers)
                cache += std::format("{}{} {}",
                                     cache.empty() ? "" : ", ",
                                     tier.name,
                                     tier.stats.has_value() ? std::format("{} entries", tier.stats->entries)
                                                            : std::string { "absent" });
        }
        return std::format("node {} version {}, up {} s; runs {}; {}; cache: {}{}",
                           fields.nodeId.empty() ? std::string { "(no id)" } : fields.nodeId,
                           fields.version,
                           fields.uptimeSeconds,
                           components.empty() ? std::string { "nothing" } : components,
                           slots,
                           cache,
                           runtime.cordon.has_value()
                               ? std::format("; cordon {}", NodeStatusView::CordonStateName(*runtime.cordon))
                               : std::string {});
    }

    [[nodiscard]] Mcp::ToolResult NodeStatus(IMcpUpstream& upstream,
                                             IAppliedClusterView const& cluster,
                                             std::string const& selfId,
                                             Mcp::ToolCall const& call)
    {
        auto target = TargetOf(call, cluster, selfId);
        if (!target.has_value())
            return std::move(target.error());

        auto answer = upstream.Ask(UpstreamTarget::Node, target->nodeId, FrameText(Wire::EncodeNodeStatusRequest()));
        if (!answer.has_value())
            return UpstreamError(answer.error());
        if (answer->status != Wire::Status::Ok)
            return Error(std::format(
                "{} answered node-status with a miss, which that verb never answers; nothing it said is reported",
                answer->answeredBy));
        auto const decoded = Wire::DecodeNodeStatus(PayloadBytes(answer->payload));
        if (!decoded.has_value())
            return Error(std::format(
                "{} answered node-status with a body this node cannot read; nothing it said is reported -- run the "
                "same pace-node build on both",
                answer->answeredBy));
        auto const& fields = *decoded;
        auto const& runtime = fields.runtime;

        auto components = Json::Array {};
        for (auto const& name: NodeStatusView::ComponentNames(fields.components))
            components.push_back(Str(name));

        auto result = Envelope(*answer);
        result.emplace_back("node", StrOrNull(target->asked));
        result.emplace_back("version", Str(fields.version));
        result.emplace_back("node_id", fields.nodeId.empty() ? Null() : Str(fields.nodeId));
        result.emplace_back("public_key",
                            runtime.identityPublicKey.has_value()
                                ? Str(FormatEd25519PublicKey(Ed25519PublicKey { *runtime.identityPublicKey }))
                                : Null());
        result.emplace_back("uptime_s", Int(static_cast<std::int64_t>(fields.uptimeSeconds)));
        result.emplace_back("components", Arr(std::move(components)));
        result.emplace_back("consensus_standing",
                            runtime.consensusStanding.has_value()
                                ? Name(NodeStatusView::ConsensusStandingName(*runtime.consensusStanding))
                                : Null());
        result.emplace_back(
            "scheduler_role",
            runtime.schedulerRole.has_value() ? Name(NodeStatusView::SchedulerRoleName(*runtime.schedulerRole)) : Null());
        result.emplace_back("toolchains",
                            runtime.toolchains.has_value()
                                ? Obj({ { "state", Name(NodeStatusView::ToolchainStateName(*runtime.toolchains)) },
                                        { "served", Int(runtime.toolchainsServed) },
                                        { "discovered", Int(runtime.toolchainsDiscovered) } })
                                : Null());
        result.emplace_back("compile_slots", NumberOrNull(runtime.compileSlots));
        result.emplace_back("compiles_in_flight", NumberOrNull(runtime.compilesInFlight));
        result.emplace_back("compile_inflight_max_bytes", NumberOrNull(runtime.compileInflightMaxBytes));
        result.emplace_back("compile_request_max_bytes", NumberOrNull(runtime.compileRequestMaxBytes));
        result.emplace_back("cordon",
                            runtime.cordon.has_value() ? Name(NodeStatusView::CordonStateName(*runtime.cordon)) : Null());
        result.emplace_back("slot_ceilings", SlotCeilings(runtime.slotCeilings));
        result.emplace_back("tiers", Tiers(runtime));
        result.emplace_back("surfaces", Surfaces(fields));
        result.emplace_back("effective_config", Settings(runtime));
        result.emplace_back("budget_overrides", Overrides(runtime.budgetOverrides));
        return Answer(std::move(result), std::format("{}; {}", Summary(fields), Provenance(*answer)));
    }

    constexpr std::string_view NodeStatusInput =
        R"({"type":"object","properties":{)"
        R"("node":{"type":"string","description":"A member's id, matched exactly -- never by prefix. Omit it to ask this node."}},"additionalProperties":false})";

    constexpr std::string_view NodeStatusOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","node","version","node_id","public_key","uptime_s","components","consensus_standing","scheduler_role","toolchains","compile_slots","compiles_in_flight","compile_inflight_max_bytes","compile_request_max_bytes","cordon","slot_ceilings","tiers","surfaces","effective_config","budget_overrides"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("node":{"type":["string","null"],"description":"The name asked for; null when this node was asked."},)"
        R"("version":{"type":"string"},"node_id":{"type":["string","null"]},"public_key":{"type":["string","null"]},)"
        R"("uptime_s":{"type":"integer"},"components":{"type":"array","items":{"type":"string"}},)"
        R"("consensus_standing":{"type":["string","null"]},"scheduler_role":{"type":["string","null"]},)"
        R"("toolchains":{"type":["object","null"]},)"
        R"("compile_slots":{"type":["integer","null"]},"compiles_in_flight":{"type":["integer","null"]},)"
        R"("compile_inflight_max_bytes":{"type":["integer","null"]},"compile_request_max_bytes":{"type":["integer","null"]},)"
        R"("cordon":{"type":["string","null"]},)"
        R"("slot_ceilings":{"type":["object","null"],"description":"Each ceiling on the worker's slots (slots null where a limit did not apply) and the binding one, as of the last heartbeat; null with no worker or before the first heartbeat."},)"
        R"("tiers":{"type":["array","null"],"description":"One row per cache tier; present:false is a tier this cache lacks (every figure null); budget_unbounded when the budget is zero; hit_ratio null with no lookups. null: no cache."},)"
        R"("surfaces":{"type":"array"},)"
        R"("effective_config":{"type":["array","null"],"description":"The configuration in force after reloads: {name, values}; values null when unset; a credential reads <redacted> and never leaves its node."},)"
        R"("budget_overrides":{"type":["array","null"],"description":"null: the node does not say."}}})";

} // namespace

std::vector<Mcp::ToolRow> NodeReadTools(IMcpUpstream& upstream, IAppliedClusterView const& cluster, std::string selfId)
{
    auto* const to = &upstream;
    auto const* const in = &cluster;
    return {
        Mcp::ToolRow {
            .name = "node_status",
            .title = "Node status",
            .description = "One node's full status: version, uptime, components and consensus standing, the worker's "
                           "slots with every ceiling on them and the binding one named, cordon, each cache tier with "
                           "entries, bytes, budget and hit ratio, the surfaces it opened, and the configuration in "
                           "force after reloads with every credential redacted. This node by default; `node` names "
                           "another member by its id, matched exactly. The same reply `pacectl node` reads. Durations "
                           "are in SECONDS (fields ending _s); a null field is absent, never zero. `term` is the term "
                           "THIS node holds.",
            .inputSchema = NodeStatusInput,
            .outputSchema = NodeStatusOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler =
                [to, in, self = std::move(selfId)](Mcp::ToolCall const& call) { return NodeStatus(*to, *in, self, call); },
        },
    };
}

} // namespace Pace::Node
