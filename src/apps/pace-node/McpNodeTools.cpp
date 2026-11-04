// SPDX-License-Identifier: Apache-2.0
#include "McpNodeTools.hpp"
#include "McpToolSupport.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Distributed/ShownName.hpp>
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

#include <core/Ranges.hpp>

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
        return value.has_value() ? Json::Unsigned(*value) : Null();
    }

    /// @param text A name the view spells.
    /// @return It as JSON.
    [[nodiscard]] Json::Value Name(std::string_view text)
    {
        return Str(std::string { text });
    }

    /// @param runtime The decoded record.
    /// @return The `slot_ceilings` member, read through `NodeStatusView::CeilingsOf` -- the view `pacectl node`
    ///         renders its ceiling cells from.
    [[nodiscard]] Json::Value SlotCeilings(Wire::NodeRuntimeFields const& runtime)
    {
        auto const ceilings = NodeStatusView::CeilingsOf(runtime);
        if (!ceilings.has_value())
            return Null();
        auto rows = Json::Array {};
        rows.reserve(ceilings->ceilings.size());
        for (auto const& ceiling: ceilings->ceilings)
            rows.push_back(Obj({ { "name", Str(NodeStatusView::ReadableText(ceiling.name)) },
                                 { "slots", NumberOrNull(ceiling.slots) },
                                 { "binding", Bool(ceiling.binding) } }));
        return Obj({ { "available", Json::Unsigned(ceilings->available) },
                     { "binding", Str(NodeStatusView::ReadableText(ceilings->binding)) },
                     { "sampled_age_s", Json::Unsigned(ceilings->sampledSecondsAgo) },
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
                                 { "entries", Json::Unsigned(stats.entries) },
                                 { "bytes", Json::Unsigned(stats.bytesUsed) },
                                 { "budget_bytes", NumberOrNull(budget) },
                                 { "budget_unbounded", Bool(!budget.has_value()) },
                                 { "hits", Json::Unsigned(stats.hits) },
                                 { "misses", Json::Unsigned(stats.misses) },
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
            rows.push_back(Obj(
                { { "name", Str(setting.name) }, { "values", std::move(values) }, { "non_utf8", Bool(setting.escaped) } }));
        }
        return Arr(std::move(rows));
    }

    /// @param runtime The decoded record.
    /// @return The `budget_overrides` member: `null` when the node did not say, else one row per tier through the view
    ///         `pacectl node` renders from, every absent quantity `null` rather than zero.
    [[nodiscard]] Json::Value Overrides(Wire::NodeRuntimeFields const& runtime)
    {
        auto const views = NodeStatusView::BudgetOverridesOf(runtime);
        if (!views.has_value())
            return Null();
        auto rows = Json::Array {};
        for (auto const& view: *views)
            rows.push_back(Obj({ { "tier", Str(view.tier) },
                                 { "bytes", NumberOrNull(view.effectiveBytes) },
                                 { "source", Str(view.source) },
                                 { "configured_bytes", Json::Unsigned(view.configuredBytes) },
                                 { "override_bytes", NumberOrNull(view.overrideBytes) },
                                 { "baseline_bytes", NumberOrNull(view.baselineBytes) },
                                 { "set_at", view.setAt.has_value() ? Str(*view.setAt) : Null() },
                                 { "set_by", view.setBy.empty() ? Null() : Str(view.setBy) },
                                 { "set_by_name", view.setByName.empty() ? Null() : Str(view.setByName) },
                                 { "excess_bytes", NumberOrNull(view.excessBytes) },
                                 { "note", view.note.empty() ? Null() : Str(view.note) },
                                 { "text", Str(NodeStatusView::DescribeBudget(view)) } }));
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
        if (auto const ceilings = NodeStatusView::CeilingsOf(runtime); ceilings.has_value())
            slots = std::format(
                "{} slot(s) offerable, bound by {}", ceilings->available, NodeStatusView::ReadableText(ceilings->binding));
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
                           fields.nodeId.empty() ? std::string { "(no id)" } : NodeStatusView::ReadableText(fields.nodeId),
                           NodeStatusView::ReadableText(fields.version),
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
        auto target = NodeTargetOf(call, cluster, selfId);
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
        result.emplace_back("version", Str(NodeStatusView::ReadableText(fields.version)));
        // Named as every answer names a machine (`AppendNaming`): by the id the node REPORTED when it reported one -- it
        // is the node that answered -- and the display name the applied state records for that id. Only where this
        // node's state does not record that member at all (no state, or not yet applied here) does the answering
        // node's own word stand in (field 30); a member this state records UNNAMED stays unnamed, so `display_name`
        // and `name` below are one value from one state, never a null beside a name another state supplied.
        auto const state = cluster.AppliedState();
        auto naming =
            fields.nodeId.empty() ? target->naming : NamingOfMember(state, NodeStatusView::ReadableText(fields.nodeId));
        auto const recorded = state.has_value() && naming.nodeId.has_value()
                              && core::findIfOrNull(state->members, [&naming](Cluster::ClusterMember const& member) {
                                     return member.id == *naming.nodeId;
                                 }) != nullptr;
        if (!recorded)
            naming.displayName = runtime.displayName.has_value()
                                     ? std::optional { NodeStatusView::ReadableText(*runtime.displayName) }
                                     : std::nullopt;
        AppendNaming(result, naming);
        // And the name a surface SHOWS, by the dashboard's own rule (`Distributed::NameShown`): that replicated name,
        // else what the machine calls itself, with which of the two it is.
        auto const& replicated = naming.displayName;
        auto const shown = Distributed::NameShown(
            replicated.has_value() ? std::string_view { *replicated } : std::string_view {},
            runtime.hostName.has_value() ? std::string_view { *runtime.hostName } : std::string_view {});
        result.emplace_back("name", shown.text.empty() ? Null() : Str(NodeStatusView::ReadableText(shown.text)));
        result.emplace_back("name_source",
                            shown.source.has_value() ? Str(std::string { Distributed::NameSourceWord(*shown.source) })
                                                     : Null());
        result.emplace_back("public_key",
                            runtime.identityPublicKey.has_value()
                                ? Str(FormatEd25519PublicKey(Ed25519PublicKey { *runtime.identityPublicKey }))
                                : Null());
        result.emplace_back("uptime_s", Json::Unsigned(fields.uptimeSeconds));
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
        result.emplace_back("slot_ceilings", SlotCeilings(runtime));
        result.emplace_back("tiers", Tiers(runtime));
        result.emplace_back("surfaces", Surfaces(fields));
        result.emplace_back("effective_config", Settings(runtime));
        result.emplace_back("budget_overrides", Overrides(runtime));
        return Answer(std::move(result), std::format("{}; {}", Summary(fields), Provenance(*answer)));
    }

    constexpr std::string_view NodeStatusInput =
        R"({"type":"object","properties":{)"
        R"("node":{"type":"string","description":"A member's id or replicated display name, matched exactly -- never by prefix. Omit it to ask this node."}},"additionalProperties":false})";

    constexpr std::string_view NodeStatusOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","node","version","node_id","display_name","name","name_source","public_key","uptime_s","components","consensus_standing","scheduler_role","toolchains","compile_slots","compiles_in_flight","compile_inflight_max_bytes","compile_request_max_bytes","cordon","slot_ceilings","tiers","surfaces","effective_config","budget_overrides"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("node":{"type":["string","null"],"description":"The name asked for; null when this node was asked."},)"
        R"("version":{"type":"string"},"node_id":{"type":["string","null"]},"display_name":{"type":["string","null"],"description":"The display name the cluster records for the node; null when nobody named it."},"name":{"type":["string","null"],"description":"The name the dashboard shows: display_name, else the machine's own hostname; null when neither says."},"name_source":{"type":["string","null"],"description":"replicated or self-reported; null when there is no name."},"public_key":{"type":["string","null"]},)"
        R"("uptime_s":{"type":"integer"},"components":{"type":"array","items":{"type":"string"}},)"
        R"("consensus_standing":{"type":["string","null"]},"scheduler_role":{"type":["string","null"]},)"
        R"("toolchains":{"type":["object","null"]},)"
        R"("compile_slots":{"type":["integer","null"]},"compiles_in_flight":{"type":["integer","null"]},)"
        R"("compile_inflight_max_bytes":{"type":["integer","null"]},"compile_request_max_bytes":{"type":["integer","null"]},)"
        R"("cordon":{"type":["string","null"]},)"
        R"("slot_ceilings":{"type":["object","null"],"description":"Each ceiling on the worker's slots (slots null where a limit did not apply) and the binding one, as of the last heartbeat; null with no worker or before the first heartbeat."},)"
        R"("tiers":{"type":["array","null"],"description":"One row per cache tier; present:false is a tier this cache lacks (every figure null); budget_unbounded when the budget is zero; hit_ratio null with no lookups. null: no cache."},)"
        R"("surfaces":{"type":"array"},)"
        R"("effective_config":{"type":["array","null"],"description":"The configuration in force after reloads: {name, values}; values null when unset; a credential reads <redacted> and never leaves its node; non_utf8 true when a value was not UTF-8 and is shown escaped as <non-utf8: ...> with each such byte as \\xHH."},)"
        R"("budget_overrides":{"type":["array","null"],"description":"Each cache tier's budget: one row per tier the node has, or that the cluster records an override for. null: the node does not say.",)"
        R"("items":{"type":"object","required":["tier","bytes","source","configured_bytes","override_bytes","baseline_bytes","set_at","set_by","set_by_name","excess_bytes","note","text"],"properties":{)"
        R"("tier":{"type":"string"},"bytes":{"type":["integer","null"],"description":"The budget in force; 0 is unbounded; null when the node has no such tier."},)"
        R"("source":{"type":"string","description":"cluster (an override) or configured (the machine's own flag)."},)"
        R"("configured_bytes":{"type":"integer","description":"The machine's own budget in the configuration in force; 0 is unbounded."},)"
        R"("override_bytes":{"type":["integer","null"],"description":"The override the cluster records; null when none is."},)"
        R"("baseline_bytes":{"type":["integer","null"],"description":"The configured value the override was set against, reported by the node; null until reported, or with no override."},)"
        R"("set_at":{"type":["string","null"],"description":"When the leader recorded the override, UTC, by the leader's clock; display only."},)"
        R"("set_by":{"type":["string","null"],"description":"The member the override is attributed to: the machine the operator acted from."},)"
        R"("set_by_name":{"type":["string","null"]},"excess_bytes":{"type":["integer","null"],"description":"Bytes still over the budget in force while a shrink converges; null with no tier."},)"
        R"("note":{"type":["string","null"],"description":"Why the recorded override is not in force, or a budget was refused; null when all is."},)"
        R"("text":{"type":"string","description":"The row as pacectl node prints it."}}}}}})";

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
                           "another member by its id or display name, matched exactly. The same reply `pacectl node` "
                           "reads. Durations are in SECONDS (fields ending _s); a null field is absent, never zero. "
                           "`term` is the term THIS node holds.",
            .inputSchema = NodeStatusInput,
            .outputSchema = NodeStatusOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler =
                [to, in, self = std::move(selfId)](Mcp::ToolCall const& call) { return NodeStatus(*to, *in, self, call); },
        },
    };
}

} // namespace Pace::Node
