// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/Json.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Pace::Mcp
{

/// @file McpTool.hpp
/// What an MCP tool is, as the table `McpServer` serves from describes one.
///
/// A tool is a ROW: its name, its schemas as JSON text, what it does to its environment and the
/// function that runs it. Adding a tool is adding a row; the envelope, the listing, the argument
/// check and the result shape are written once, in `McpServer`.

/// The MCP revisions this server answers, preferred first.
///
/// `2026-07-28` is the MODERN era (every request carries its version, identity and capabilities in
/// `_meta`); `2025-11-25` is the LEGACY era (an `initialize` handshake, no sessions minted here). The
/// order is what `server/discover` and `UnsupportedProtocolVersion`'s `data.supported` report.
inline constexpr std::array SupportedVersions { std::string_view { "2026-07-28" }, std::string_view { "2025-11-25" } };

/// What a tool does to its environment, which `tools/list` renders as the `readOnlyHint` and
/// `destructiveHint` annotations.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted. The wire carries the two hints by
/// NAME, through the table in `McpServer.cpp`, never this ordinal.
enum class ToolEffect : std::uint8_t
{
    ReadOnly = 0, ///< Reads only: `readOnlyHint: true`.
    Mutating,     ///< Changes something, additively: `readOnlyHint: false`, `destructiveHint: false`.
    Destructive,  ///< May remove or overwrite something: `readOnlyHint: false`, `destructiveHint: true`.
    Last
};

/// One call of a tool, as its handler sees it.
///
/// Both members borrow from the request being answered and live exactly as long as the handler runs.
struct ToolCall
{
    /// The call's `arguments` object, already checked against the input schema's `required` keys and
    /// primitive `type`s. An empty object when the client sent none.
    Json::Value const& arguments;
    /// The client's self-reported name (`_meta` `clientInfo.name`), empty when it sent none. For display
    /// and logging only: it is not verified and decides nothing.
    std::string_view clientName;
};

/// What a tool answered.
struct ToolResult
{
    /// The structured result (`structuredContent`). `null` means the tool has none, and the field is left out.
    Json::Value structured;
    /// The one text block every result carries, for a client that reads no structured content.
    std::string text;
    /// Whether this is a tool EXECUTION error -- the model sees it and may retry -- rather than a success.
    bool isError { false };
};

/// One tool of the table.
struct ToolRow
{
    std::string_view name;         ///< The name a client calls it by.
    std::string_view title;        ///< Its human-readable display name.
    std::string_view description;  ///< What it does, for the model.
    std::string_view inputSchema;  ///< JSON Schema text for `arguments`; parsed once, at construction.
    std::string_view outputSchema; ///< JSON Schema text for `structuredContent`; empty when it has none.
    ToolEffect effect;             ///< What it does to its environment.
    std::function<ToolResult(ToolCall const&)> handler; ///< Runs one call. A throw is a programmer error.
};

} // namespace Pace::Mcp
