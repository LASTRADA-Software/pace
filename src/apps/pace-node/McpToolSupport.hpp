// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"

#include <Pace/Core/Json.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

/// Which node a call asks: this one, or the member the `node` argument names exactly.
struct NodeTarget
{
    std::string nodeId;               ///< The member's id; empty means this node.
    std::optional<std::string> asked; ///< The name as given, or nothing when none was.
};

/// Resolve a call's `node` argument, through `ResolveNodeName`: exactly, never by prefix.
/// @param call The call.
/// @param cluster Where names resolve.
/// @param selfId This node's id: the one name that resolves on a node that has applied no cluster state.
/// @return Whom to ask, or the tool error naming why the argument names nobody.
[[nodiscard]] inline std::expected<NodeTarget, Mcp::ToolResult> NodeTargetOf(Mcp::ToolCall const& call,
                                                                             IAppliedClusterView const& cluster,
                                                                             std::string const& selfId)
{
    auto const* const node = call.arguments.Find("node");
    if (node == nullptr || node->IsNull())
        return NodeTarget { .nodeId = {}, .asked = std::nullopt };
    // Refused rather than ignored: a `node` this tool cannot read answered about THIS node would be an answer about
    // a machine nobody named.
    auto const* const name = node->AsString();
    if (name == nullptr)
        return std::unexpected { Error("`node` must be a string: a member's id, matched exactly") };

    auto const state = cluster.AppliedState();
    if (!state.has_value())
    {
        if (*name == selfId && !selfId.empty())
            return NodeTarget { .nodeId = {}, .asked = *name };
        return std::unexpected { Error(std::format(
            "this node has applied no cluster state, so it can name no member but itself ({}); omit `node` to ask it",
            selfId.empty() ? std::string { "it has no id" } : selfId)) };
    }
    auto resolved = ResolveNodeName(*state, *name);
    if (!resolved.has_value())
        return std::unexpected { Error(std::move(resolved.error())) };
    return NodeTarget { .nodeId = std::move(*resolved), .asked = *name };
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
