// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeRefusal.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/Mcp/McpServer.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>
#include <Pace/Protocol/ProvenIdentity.hpp>
#include <Pace/Server/AdminHttpServer.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/IListener.hpp>
#include <core/platform/Clock.hpp>

namespace Pace::Node
{

/// @file McpEndpoint.hpp
/// The node's MCP surface (`NodeSurface::Mcp`, `--mcp-listen`): one `POST /mcp` route, served on a
/// thread of its own by the same hardened HTTP code the admin surface runs.
///
/// **Loopback only, asked twice.** `--mcp-listen` on an address other machines can reach is a
/// startup refusal (`McpListenNotLoopbackRefusal`), and every request is asked again whether its
/// connection's peer is on this machine (`McpAdmits`) before the envelope is parsed -- so a future
/// bind that widened the address could not widen who is answered without also changing this.
///
/// **Its own threads, never the dashboard's and never a reactor's.** The accept loop runs on a thread of its
/// own and hands every connection to one of `MaxConcurrentRequests` serving threads
/// (`AdminConnectionServing::OnOwnThreads`), so a held connection holds one slot, never the surface; past the
/// cap the next connection is answered `503` at once. `Deadlines` bounds how long a slot can be held without a
/// request: a peer that sends nothing frees its slot in two seconds, so idle or trickling connects can make
/// MCP refuse for about that long, never wait. Hostile or slow MCP input stalls only MCP.

/// Whether a connection may be answered by the MCP surface.
///
/// The connection's kernel-reported peer host must name this machine (`IsLoopbackHost`): the
/// IPv4 loopback in any of its forms, `::1`, or the mapped `::ffff:127.x.x.x` a dual-stack listener
/// reports. An EMPTY host -- a peer the socket could not name -- is not local, and neither is any
/// name, `localhost` included: a resolver is not something this decision may depend on.
/// @param facts What the connection established about its peer.
/// @return True when the request may reach `McpServer::Handle`.
[[nodiscard]] bool McpAdmits(ConnectionFacts const& facts) noexcept;

/// The one route the MCP surface serves: `POST /mcp`, admitted by `McpAdmits` and answered by
/// @p server.
///
/// A refused connection is answered `403` with a JSON-RPC error and moves
/// `McpRequestsRefusedNotLoopback`; the envelope is never parsed for it.
/// @param server Answers an admitted request; must outlive the route.
/// @param metrics Where a refusal is counted; must outlive the route.
/// @return The route.
[[nodiscard]] AdminRoute MakeMcpRoute(Mcp::McpServer const& server, IMetricsSink& metrics);

/// The MCP surface: listener, HTTP server, MCP server and the thread that serves them, owned as one
/// thing, for the reason `AdminEndpoint` is -- the destruction order is the destructor's.
class McpEndpoint
{
  public:
    /// How many MCP requests are served at once -- each on a serving thread of its own -- before the next
    /// connection is answered `503`.
    ///
    /// Small, because the only legitimate caller is one or two sessions on this machine, each
    /// sending one request at a time over a fresh connection; anything holding more than this is
    /// either a bug or somebody trying to tie the surface up.
    static constexpr std::size_t MaxConcurrentRequests = 4;

    /// The bounds an MCP connection is read and closed under: its own row, not the dashboard's.
    ///
    /// `firstByte` is two seconds rather than the dashboard's thirty because the thirty exists for a
    /// browser's speculative preconnect (#828), which no MCP client opens: a peer that connects and says
    /// nothing is broken or hostile, and holds a slot until this frees it. `head` is two seconds in total for
    /// the same reason, so a client dribbling its head frees the slot as fast as a silent one; `request` and
    /// `body` keep the dashboard's figures, which already were two. The linger is short because a `503`
    /// written past the cap lingers on the ACCEPT thread: a quarter of 400 ms of silence ends it, so sixteen
    /// silent connects past the cap delay the next accept by under two seconds rather than eight.
    static constexpr AdminDeadlines Deadlines {
        .firstByte = std::chrono::milliseconds { 2000 },
        .request = std::chrono::milliseconds { 2000 },
        .head = std::chrono::milliseconds { 2000 },
        .body = std::chrono::milliseconds { 2000 },
        .linger =
            LingerBounds { .total = std::chrono::milliseconds { 400 }, .maxBytes = std::size_t { 64 } * 1024, .reads = 4 },
    };

    /// The path the endpoint answers.
    static constexpr std::string_view Path { "/mcp" };

    /// What the accept loop reports as, to `/healthz` and the log.
    static constexpr std::string_view SurfaceName { "mcp" };

    /// Bind the surface `--mcp-listen` names and start serving it.
    ///
    /// Fallible for the reasons `AdminEndpoint::Start` is, and with its diagnostic string for the
    /// same reason. A non-loopback address cannot arrive: `StartupPolicyRejection` refuses it
    /// first.
    /// @param cfg What the operator asked for; `NodeSurface::Mcp`'s row resolves the endpoint.
    /// @param tools The tool table `tools/list` reports and `tools/call` dispatches into.
    /// @param metrics Where a refused request is counted.
    /// @param logger Where the server and its tools report.
    /// @param acceptLoops The node's accept-loop registry; must outlive the endpoint.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<McpEndpoint>, std::string> Start(
        NodeConfig const& cfg,
        std::vector<Mcp::ToolRow> tools,
        IMetricsSink& metrics,
        ILogger& logger,
        core::net::AcceptLoopHealth& acceptLoops);

    /// Serve an already-bound listener: the seam a test hands a relabelled listener through.
    /// @param listener The bound listener, its accept poll already armed (owned).
    /// @param boundEndpoint What `BoundEndpoint()` reports.
    /// @param tools The tool table.
    /// @param metrics Where a refused request is counted.
    /// @param logger Where the server and its tools report.
    /// @param acceptLoops The node's accept-loop registry; must outlive the endpoint.
    /// @return The running endpoint.
    [[nodiscard]] static std::unique_ptr<McpEndpoint> StartWithListener(std::unique_ptr<core::net::IListener> listener,
                                                                        std::string boundEndpoint,
                                                                        std::vector<Mcp::ToolRow> tools,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger,
                                                                        core::net::AcceptLoopHealth& acceptLoops);

    /// Stop serving and join the thread.
    ~McpEndpoint();

    McpEndpoint(McpEndpoint const&) = delete;
    McpEndpoint& operator=(McpEndpoint const&) = delete;
    // Neither movable: the serving thread holds a pointer to `_http`, which holds references to
    // `_listener` and the route that holds `_server`.
    McpEndpoint(McpEndpoint&&) = delete;
    McpEndpoint& operator=(McpEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    /// @return Host and port, as text.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    McpEndpoint(std::unique_ptr<core::net::IListener> listener,
                std::string boundEndpoint,
                std::vector<Mcp::ToolRow> tools,
                IMetricsSink& metrics,
                ILogger& logger,
                core::net::AcceptLoopHealth& acceptLoops);

    std::unique_ptr<core::net::IListener> _listener;
    /// Owned for the reason `AdminEndpoint::_clock` is: app wiring, with the testable seam for the
    /// deadlines one level down on `ServeAdminHttp`.
    core::platform::SteadyClock _clock;
    /// Declared before `_http`, whose route borrows it.
    Mcp::McpServer _server;
    std::unique_ptr<AdminHttpServer> _http;
    std::string _boundEndpoint;
    std::jthread _thread;
};

/// Start the MCP surface the configuration asks for, or explain the refusal.
///
/// A function rather than wiring in `main`, for the reason `StartAdminSurfaceOrExplain` is one:
/// `main.cpp` is in no test target. Off unless `--mcp-listen` is set, which answers a null endpoint
/// inside a satisfied `expected`; a refusal is `NodeRefusalCause::Listener`, since the startup table
/// has already refused every address that is not loopback.
/// @param cfg What the operator asked for.
/// @param tools The tool table.
/// @param metrics Where a refused request is counted.
/// @param logger Where the bound endpoint is announced.
/// @param acceptLoops The node's accept-loop registry; must outlive the endpoint.
/// @return The running endpoint, null when the surface is off, or why it could not be served.
[[nodiscard]] std::expected<std::unique_ptr<McpEndpoint>, NodeRefusal> StartMcpSurfaceOrExplain(
    NodeConfig const& cfg,
    std::vector<Mcp::ToolRow> tools,
    IMetricsSink& metrics,
    ILogger& logger,
    core::net::AcceptLoopHealth& acceptLoops);

} // namespace Pace::Node
