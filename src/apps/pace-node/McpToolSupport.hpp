// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"
#include "NodeNameResolver.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <core/Ranges.hpp>

/// @file McpToolSupport.hpp
/// What every MCP tool of this node spells the same way: the JSON value builders, the provenance envelope every
/// forwarded answer opens with (`answered_by`, `term`, `term_scope`), the upstream-failure-to-`isError` mapping,
/// how a `node` argument resolves, and the frame and payload byte views. Shared rather than copied per tool file,
/// so the fleet, node and action tools cannot drift on how a failure, a term or a node name is read.

namespace Pace::Node::McpSupport
{
// ------------------------------------------------------------ values

/// @param text A string.
/// @return It as a JSON value.
[[nodiscard]] inline Json::Value Str(std::string text)
{
    return Json::Value { .v = std::move(text) };
}

/// @param number An integer.
/// @return It as a JSON value.
[[nodiscard]] inline Json::Value Int(std::int64_t number)
{
    return Json::Value { .v = number };
}

/// @param number A count; a `std::size_t` past `int64` cannot occur for anything counted here.
/// @return It as a JSON value.
[[nodiscard]] inline Json::Value Count(std::size_t number)
{
    return Int(static_cast<std::int64_t>(number));
}

/// The earliest instant `Rfc3339` renders: 1970-01-01T00:00:00Z, the epoch itself.
inline constexpr std::int64_t Rfc3339FirstMillis = 0;

/// The latest instant `Rfc3339` renders: 9999-12-31T23:59:59.999Z, the last millisecond a four-digit year holds.
inline constexpr std::int64_t Rfc3339LastMillis = 253'402'300'799'999;

/// @param millis An instant, in milliseconds since the epoch, as a foreign document stated it.
/// @return It as RFC 3339 UTC, to the second, or nothing when it lies outside years 1970 to 9999 -- a figure that
///         cannot be a real instant is absent, never wrapped text and never a throw.
[[nodiscard]] inline std::optional<std::string> Rfc3339(std::int64_t millis)
{
    if (millis < Rfc3339FirstMillis || millis > Rfc3339LastMillis)
        return std::nullopt;
    auto const instant = std::chrono::floor<std::chrono::seconds>(
        std::chrono::sys_time<std::chrono::milliseconds> { std::chrono::milliseconds { millis } });
    return std::format("{:%FT%TZ}", instant);
}

/// @param number A real.
/// @return It as a JSON value.
[[nodiscard]] inline Json::Value Real(double number)
{
    return Json::Value { .v = number };
}

/// @param flag A boolean.
/// @return It as a JSON value.
[[nodiscard]] inline Json::Value Bool(bool flag)
{
    return Json::Value { .v = flag };
}

/// @return JSON `null`: absent, never zero.
[[nodiscard]] inline Json::Value Null()
{
    return Json::Value {};
}

/// @param members An object's members.
/// @return The object as a JSON value.
[[nodiscard]] inline Json::Value Obj(Json::Object members)
{
    return Json::Value { .v = std::move(members) };
}

/// @param elements An array's elements.
/// @return The array as a JSON value.
[[nodiscard]] inline Json::Value Arr(Json::Array elements)
{
    return Json::Value { .v = std::move(elements) };
}

/// @param text Text, or nothing.
/// @return The text, or `null` for nothing.
[[nodiscard]] inline Json::Value StrOrNull(std::optional<std::string> text)
{
    return text.has_value() ? Str(std::move(*text)) : Null();
}

/// @param value An object.
/// @param key A member's key.
/// @return The member's string, or nothing when it is absent, `null` or not a string.
[[nodiscard]] inline std::optional<std::string> StringAt(Json::Value const& value, std::string_view key)
{
    auto const* const member = value.Find(key);
    if (member == nullptr || member->AsString() == nullptr)
        return std::nullopt;
    return *member->AsString();
}

// ------------------------------------------------------------ the envelope

/// What `term_scope` says, on every result: whose view of the term `term` is.
inline constexpr std::string_view TermScope = "this-node";

/// @param reply What the upstream answered.
/// @return `answered_by`, `term` and `term_scope`, the members every fleet result opens with.
[[nodiscard]] inline Json::Object Envelope(UpstreamReply const& reply)
{
    return Json::Object {
        { "answered_by", Str(reply.answeredBy) },
        { "term", reply.term.has_value() ? Int(static_cast<std::int64_t>(*reply.term)) : Null() },
        { "term_scope", Str(std::string { TermScope }) },
    };
}

/// @param reply What the upstream answered.
/// @return The line every text block ends with: who answered, and the term as this node sees it.
[[nodiscard]] inline std::string Provenance(UpstreamReply const& reply)
{
    auto const term = reply.term.has_value() ? std::format("term {} as this node sees it", *reply.term)
                                             : std::string { "no term known to this node" };
    return std::format("answered by {} ({})", reply.answeredBy, term);
}

/// @param sentence Why there is no answer.
/// @return A tool execution error carrying it.
[[nodiscard]] inline Mcp::ToolResult Error(std::string sentence)
{
    return Mcp::ToolResult { .structured = Null(), .text = std::move(sentence), .isError = true };
}

// ------------------------------------------------------------ the node argument

// ------------------------------------------------------------ naming a machine

/// A machine, as every answer that names one names it: its member id AND the display name the applied state records.
///
/// **The one way an MCP answer names a machine** (`AppendNaming`), whatever it was keyed by where the answer came from
/// -- an id, the `node-id` the leader joined a fleet row to by the key it PROVED, a requester's key -- so a model
/// joining one tool's rows to another's joins on the same two fields. **Never by an endpoint**: a registration's
/// endpoint is the machine's own unverified claim, and naming by it lets one machine be named as another. Absent is
/// not empty: a machine nobody can place has a null id and says so, and a member nobody named has a null display name;
/// neither is ever guessed.
struct MachineNaming
{
    std::optional<std::string> nodeId;      ///< The member's id; nothing when the state places no member here.
    std::optional<std::string> displayName; ///< The member's display name; nothing when it has none or is not placed.

    [[nodiscard]] friend bool operator==(MachineNaming const&, MachineNaming const&) = default;
};

/// What naming a member by an id the caller already holds says.
/// @param state The applied state, or nothing on a node that applied none.
/// @param id A member's id; empty is no member.
/// @return The id, and the display name the state records for it.
[[nodiscard]] inline MachineNaming NamingOfMember(std::optional<Cluster::ClusterState> const& state, std::string_view id)
{
    if (id.empty())
        return MachineNaming {};
    return MachineNaming { .nodeId = std::string { id },
                           .displayName = state.has_value() ? DisplayNameIn(*state, id) : std::nullopt };
}

/// The member a predicate places, when exactly one does: several would be a guess, so they place nobody.
/// @param state The applied state, or nothing.
/// @param places Whether a member is the one meant.
/// @return Its naming, or the empty one.
template <typename Predicate>
[[nodiscard]] MachineNaming NamingWhere(std::optional<Cluster::ClusterState> const& state, Predicate places)
{
    if (!state.has_value())
        return MachineNaming {};
    auto const matches = std::ranges::count_if(state->members, places);
    if (matches != 1)
        return MachineNaming {};
    auto const* const member = core::findIfOrNull(state->members, places);
    return NamingOfMember(state, member->id);
}

/// Name the machine an identity key's text belongs to: the member holding that key, exactly.
/// @param state The applied state, or nothing.
/// @param keyText A key as `FormatEd25519PublicKey` spells it.
/// @return The member's naming, or the empty one.
[[nodiscard]] inline MachineNaming NamingOfKeyText(std::optional<Cluster::ClusterState> const& state,
                                                   std::string_view keyText)
{
    return NamingWhere(state, [keyText](Cluster::ClusterMember const& member) {
        return !keyText.empty() && FormatEd25519PublicKey(member.publicKey) == keyText;
    });
}

/// Add `node_id` and `display_name` to @p row: the two members every machine-naming row of every tool carries.
/// @param row A result row.
/// @param naming How the machine is named.
/// @param prefix Prepended to both keys, for a row naming a machine other than its subject (`requester_`); empty
///        for a row ABOUT the machine.
inline void AppendNaming(Json::Object& row, MachineNaming const& naming, std::string_view prefix = {})
{
    row.emplace_back(std::format("{}node_id", prefix), StrOrNull(naming.nodeId));
    row.emplace_back(std::format("{}display_name", prefix), StrOrNull(naming.displayName));
}

/// @param naming How a machine is named.
/// @return Whether the applied state could not place it, which an answer says rather than leaving to inference.
[[nodiscard]] inline bool Unplaced(MachineNaming const& naming) noexcept
{
    return !naming.nodeId.has_value();
}

/// @param unplaced How many machine rows the applied state could not place.
/// @return The clause a tool's text adds when there were any: what a null `node_id` means, said rather than inferred.
[[nodiscard]] inline std::string UnplacedNote(std::size_t unplaced)
{
    if (unplaced == 0)
        return {};
    return std::format("; {} machine(s) proved no identity key a member holds, so their node_id is null", unplaced);
}

/// Which node a call asks: this one, or the member the `node` argument names exactly.
struct NodeTarget
{
    std::string nodeId;               ///< The member's id; empty means this node.
    std::optional<std::string> asked; ///< The name as given, or nothing when none was.

    /// The node asked, named as every answer names a machine: its id (this node's own when `nodeId` is empty) and the
    /// display name the applied state records. Resolved here, once, for every tool.
    MachineNaming naming {};
};

/// Resolve a `node` member, through `ResolveNodeName`: exactly, never by prefix.
///
/// The member rather than the call, so a tool asking two nodes (`toolchain_diff`) resolves each side's `node` the way
/// a one-node tool resolves its own -- one rule for what a name means, wherever it is written.
/// @param node The member, or null when it is absent.
/// @param cluster Where names resolve.
/// @param selfId This node's id: the one name that resolves on a node that has applied no cluster state.
/// @return Whom to ask, or the tool error naming why the member names nobody.
[[nodiscard]] inline std::expected<NodeTarget, Mcp::ToolResult> NodeTargetFrom(Json::Value const* node,
                                                                               IAppliedClusterView const& cluster,
                                                                               std::string const& selfId)
{
    auto const state = cluster.AppliedState();
    if (node == nullptr || node->IsNull())
        return NodeTarget { .nodeId = {}, .asked = std::nullopt, .naming = NamingOfMember(state, selfId) };
    // Refused rather than ignored: a `node` this tool cannot read answered about THIS node would be an answer about
    // a machine nobody named.
    auto const* const name = node->AsString();
    if (name == nullptr)
        return std::unexpected { Error("`node` must be a string: a member's id or display name, matched exactly") };

    if (!state.has_value())
    {
        if (*name == selfId && !selfId.empty())
            return NodeTarget { .nodeId = {}, .asked = *name, .naming = NamingOfMember(state, selfId) };
        return std::unexpected { Error(std::format(
            "this node has applied no cluster state, so it can name no member but itself ({}); omit `node` to ask it",
            selfId.empty() ? std::string { "it has no id" } : selfId)) };
    }
    auto resolved = ResolveNodeName(*state, *name);
    if (!resolved.has_value())
        return std::unexpected { Error(std::move(resolved.error())) };
    auto naming = NamingOfMember(state, *resolved);
    return NodeTarget { .nodeId = std::move(*resolved), .asked = *name, .naming = std::move(naming) };
}

/// Resolve a call's `node` argument, through `ResolveNodeName`: exactly, never by prefix.
/// @param call The call.
/// @param cluster Where names resolve.
/// @param selfId This node's id: the one name that resolves on a node that has applied no cluster state.
/// @return Whom to ask, or the tool error naming why the argument names nobody.
[[nodiscard]] inline std::expected<NodeTarget, Mcp::ToolResult> NodeTargetOf(Mcp::ToolCall const& call,
                                                                             IAppliedClusterView const& cluster,
                                                                             std::string const& selfId)
{
    return NodeTargetFrom(call.arguments.Find("node"), cluster, selfId);
}

/// @param failure Why the upstream could not answer, and who and what happened.
/// @return The tool execution error naming it.
[[nodiscard]] inline Mcp::ToolResult UpstreamError(std::pair<UpstreamFailure, std::string> const& failure)
{
    return Error(std::format("{}: {}", UpstreamFailureRowOf(failure.first).name, failure.second));
}

/// @param bytes A frame.
/// @return The same bytes, as the upstream takes them.
[[nodiscard]] inline std::string FrameText(std::span<std::byte const> bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(
        bytes, text.begin(), [](std::byte const byte) { return static_cast<char>(std::to_integer<unsigned char>(byte)); });
    return text;
}

/// @param text A payload, as the upstream carried it.
/// @return The same bytes, as the wire decoders read them.
[[nodiscard]] inline std::span<std::byte const> PayloadBytes(std::string const& text)
{
    return std::as_bytes(std::span { text.data(), text.size() });
}

/// @param result The structured value.
/// @param text Its text rendering.
/// @return The successful result.
[[nodiscard]] inline Mcp::ToolResult Answer(Json::Object result, std::string text)
{
    return Mcp::ToolResult { .structured = Obj(std::move(result)), .text = std::move(text), .isError = false };
}

} // namespace Pace::Node::McpSupport
