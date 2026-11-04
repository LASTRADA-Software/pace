// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/Json.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>
#include <Pace/Server/AdminHttpServer.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pace::Mcp
{

/// @file McpServer.hpp
/// The MCP envelope and the tool dispatch: one HTTP request in, one HTTP response out.
///
/// **Pure by construction.** It reads no socket, no clock and no node state: the HTTP server hands it a
/// parsed `AdminRequest`, and everything a tool knows arrives through the tool's own handler. That is
/// what lets every envelope rule below be a unit test.
///
/// **Two eras, one table** (spec "Transport and protocol"). A MODERN request (`2026-07-28`) is
/// stateless: it carries `_meta` `protocolVersion` and `clientCapabilities`, and mirrors its version,
/// method and tool name into headers that must agree with the body. A LEGACY client (`2025-11-25`)
/// opens with `initialize` and then sends `tools/list` / `tools/call` under a `2025-11-25`
/// `MCP-Protocol-Version` header; no session is minted, so no state is kept for it either. The legacy
/// era is selected ONLY by those two signals -- a request with neither is a modern request missing its
/// required fields and is refused as one, which keeps the era decision unambiguous.
///
/// Every answer is `application/json`; Server-Sent Events are never used.

/// The ceilings a request body is parsed under. The HTTP route bounds the body's length before this is
/// reached; these bound what that many bytes may cost once parsed.
inline constexpr Json::ParseLimits RequestParseLimits {
    .maxBytes = 64 * 1024,
    .maxDepth = 32,
    .maxStringBytes = 16 * 1024,
    .maxContainerElements = 1024,
};

/// The name this server reports as its `serverInfo`.
inline constexpr std::string_view ServerName { "pace-node" };

/// Serves MCP requests from a fixed table of tools.
class McpServer
{
  public:
    /// Build the server over its tool table.
    ///
    /// Every tool's schema text is parsed here, once. A schema that does not parse, or an input schema
    /// that is not an object, is a PROGRAMMER error in a table row, and throws `std::invalid_argument`
    /// naming the tool, so a test constructing the production table catches it.
    /// @param tools The tools, in the order `tools/list` reports them.
    /// @param serverVersion The version reported in `serverInfo`.
    /// @param logger Where a tool handler that throws is reported.
    McpServer(std::vector<ToolRow> tools, std::string_view serverVersion, ILogger& logger);

    /// Answer one HTTP request to the MCP endpoint.
    ///
    /// Never throws. A tool handler that throws -- or answers success without the structured content
    /// its `outputSchema` declares -- is logged and answered `200` with a tool result carrying
    /// `isError: true`, so the model reads which tool failed; only a failure in the envelope itself is a
    /// `500` with `-32603`. The server goes on serving either way.
    /// @param request The request, as the HTTP server parsed it.
    /// @return The response to write.
    [[nodiscard]] AdminResponse Handle(AdminRequest const& request) const;

    /// One tool of the table, with its schemas parsed.
    struct Tool
    {
        ToolRow row;                             ///< The row as given.
        Json::Value inputSchema;                 ///< `row.inputSchema`, parsed.
        std::optional<Json::Value> outputSchema; ///< `row.outputSchema`, parsed; absent when the row has none.
    };

  private:
    /// The request once its envelope has been read, which every method handler is given.
    struct Envelope;
    /// What a method handler answered: a result, or a JSON-RPC error.
    struct Reply;
    /// One row of the method table.
    struct MethodRow;

    [[nodiscard]] AdminResponse HandleUnguarded(AdminRequest const& request) const;
    [[nodiscard]] Reply Initialize(Envelope const& envelope) const;
    [[nodiscard]] Reply Ping(Envelope const& envelope) const;
    [[nodiscard]] Reply Discover(Envelope const& envelope) const;
    [[nodiscard]] Reply ListTools(Envelope const& envelope) const;
    [[nodiscard]] Reply CallTool(Envelope const& envelope) const;
    [[nodiscard]] Json::Value ServerInfo() const;
    [[nodiscard]] Tool const* FindTool(std::string_view name) const noexcept;

    std::vector<Tool> _tools;
    std::string _serverVersion;
    ILogger& _logger;
};

} // namespace Pace::Mcp
