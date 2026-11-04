// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"
#include "NodeConfig.hpp"
#include "NodeRefusal.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/Mcp/McpServer.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>
#include <Pace/Protocol/ProvenIdentity.hpp>
#include <Pace/Server/AdminHttpServer.hpp>

#include <atomic>
#include <chrono>
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
/// The node's MCP surface (`NodeSurface::Mcp`, `--mcp-listen`): one `POST /mcp` route, served on
/// threads of its own by the same hardened HTTP code the admin surface runs.
///
/// **Loopback only, asked twice.** `--mcp-listen` on an address other machines can reach is a
/// startup refusal (`McpListenNotLoopbackRefusal`), and every request is asked again whether its
/// connection's peer is on this machine (`McpAdmits`) before the envelope is parsed -- so a future
/// bind that widened the address could not widen who is answered without also changing this.
///
/// **Its own threads, never the dashboard's and never a reactor's.** The accept loop runs on a thread of its
/// own and hands every connection to one of `MaxConcurrentRequests` serving threads
/// (`AdminConnectionServing::OnOwnThreads`), so a held connection holds one slot, never the surface; past the
/// cap the next connection is answered `503`, and each `503` lingers on the accept thread for up to
/// `Deadlines.linger`. `Deadlines` bounds how long a slot can be held without a request: a peer that sends
/// nothing frees its slot in two seconds. So idle or trickling connects make MCP refuse while they keep them
/// open -- a local process that keeps reconnecting keeps MCP refusing for as long as it keeps going -- and
/// never make it wait. Hostile or slow MCP input stalls only MCP.
///
/// **A stop is bounded whatever a client does, and the bound is derived** (`McpEndpoint::WorstCaseStop`): the stop
/// flag is raised before the accept loop is asked to stop, so a tool asks nothing more (`LoopGatedUpstream`), the drain
/// waits out the one forwarded call a request can have in flight, and once it runs out every connection still open is
/// CANCELLED (`IConnectionCanceller`) -- shut down under the write a slow reader was holding, so the serving thread
/// ends and is joined.

/// Whether the MCP surface is stopping: raised by `~McpEndpoint` BEFORE it asks its accept loop to stop, read by
/// `LoopGatedUpstream` on every forwarded call.
///
/// Owned by whoever owns the upstream (`McpForwarding`), which outlives the endpoint, so a request still running
/// on a serving thread after the stop began reads a flag that is still there. Never lowered: a stopped endpoint
/// is not restarted.
class McpStopFlag
{
  public:
    /// Say the surface is stopping.
    void Raise() noexcept
    {
        _raised.store(true, std::memory_order_release);
    }

    /// @return Whether `Raise` was called.
    [[nodiscard]] bool Raised() const noexcept
    {
        return _raised.load(std::memory_order_acquire);
    }

  private:
    std::atomic<bool> _raised { false };
};

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
    /// connection is answered `503` (on the accept thread, lingering there up to `Deadlines.linger`).
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
    ///
    /// `drain` is how long a stop waits for the requests in flight, and it is sized from the longest a request
    /// can still run once the stop flag is raised: either the forwarded call it is waiting on
    /// (`McpUpstream::CallDeadline`; every later call is refused `Stopping` at once), or what is left of reading
    /// its head and body -- then its response write and its linger (`DrainMargin`). The dashboard's five seconds
    /// would log every stop that lands during a call as a request abandoned. Past it, every connection still open is
    /// cancelled, and `afterCancel` is how long the stop then waits for the serving threads to end: a cancelled write
    /// fails at once, so the dashboard's one second is plenty.
    static constexpr AdminDeadlines Deadlines {
        .firstByte = std::chrono::milliseconds { 2000 },
        .request = std::chrono::milliseconds { 2000 },
        .head = std::chrono::milliseconds { 2000 },
        .body = std::chrono::milliseconds { 2000 },
        .linger =
            LingerBounds { .total = std::chrono::milliseconds { 400 }, .maxBytes = std::size_t { 64 } * 1024, .reads = 4 },
        .drain = McpUpstream::CallDeadline + std::chrono::seconds { 5 },
        .afterCancel = std::chrono::milliseconds { 1000 },
    };

    /// What `Deadlines.drain` allows beyond the forwarded call: one response write to a client that reads it (each
    /// blocking write CALL is bounded by `AdminHttpServer::RequestTimeout`, which `Start` arms on the listener) and one
    /// linger. A client that does not read is not waited for: past the drain its connection is cancelled.
    static constexpr auto DrainMargin = Deadlines.drain - McpUpstream::CallDeadline;
    static_assert(DrainMargin >= AdminHttpServer::RequestTimeout + Deadlines.linger.total,
                  "the drain must outlast a forwarded call, the response write after it and the linger");
    static_assert(Deadlines.drain >= Deadlines.firstByte + Deadlines.head + Deadlines.body + AdminHttpServer::RequestTimeout
                                         + Deadlines.linger.total,
                  "the drain must outlast a request still being read when the stop began, its refused calls answered "
                  "at once");

    /// The longest `~McpEndpoint` takes, whatever a client does: one accept poll for the accept thread to see the
    /// stop, the drain, and `Deadlines.afterCancel` for the connections the drain left open to end once they are
    /// cancelled (`AdminHttpServer::WorstCaseStopUnder`). The serving threads are joined after it.
    ///
    /// **A client that will not read is covered.** Each blocking write is bounded only per CALL by `RequestTimeout`,
    /// so a loopback peer that reads just often enough to keep every write moving would hold a response write -- and
    /// its serving thread -- for as long as it liked; past the drain its connection is shut down under the write,
    /// which fails at once. So is a `503` the accept thread is lingering on: that connection is tracked too.
    static constexpr auto WorstCaseStop = AdminHttpServer::WorstCaseStopUnder(Deadlines);
    /// A quarter of the packaged units' `TimeoutStopSec=120` (`packaging/linux/pace-node.service`): the node's
    /// other surfaces and tiers stop inside the same supervisor budget, so MCP may take no more than its share.
    static_assert(WorstCaseStop <= std::chrono::seconds { 30 },
                  "an MCP stop must leave the supervisor's stop budget to the rest of the node");

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
    /// @param stopping Raised when the endpoint begins to stop; must outlive the endpoint.
    /// @param metrics Where a refused request is counted.
    /// @param logger Where the server and its tools report.
    /// @param acceptLoops The node's accept-loop registry; must outlive the endpoint.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<McpEndpoint>, std::string> Start(
        NodeConfig const& cfg,
        std::vector<Mcp::ToolRow> tools,
        McpStopFlag& stopping,
        IMetricsSink& metrics,
        ILogger& logger,
        core::net::AcceptLoopHealth& acceptLoops);

    /// Serve an already-bound listener: the seam a test hands a relabelled listener through.
    /// @param listener The bound listener, its accept poll already armed (owned).
    /// @param connections Cancels what @p listener accepted, at a stop whose drain ran out: the `BlockingListener`'s
    ///        own `Connections()`, taken before it is wrapped or moved; must outlive the endpoint.
    /// @param boundEndpoint What `BoundEndpoint()` reports.
    /// @param tools The tool table.
    /// @param stopping Raised when the endpoint begins to stop; must outlive the endpoint.
    /// @param metrics Where a refused request is counted.
    /// @param logger Where the server and its tools report.
    /// @param acceptLoops The node's accept-loop registry; must outlive the endpoint.
    /// @return The running endpoint.
    [[nodiscard]] static std::unique_ptr<McpEndpoint> StartWithListener(std::unique_ptr<core::net::IListener> listener,
                                                                        IConnectionCanceller& connections,
                                                                        std::string boundEndpoint,
                                                                        std::vector<Mcp::ToolRow> tools,
                                                                        McpStopFlag& stopping,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger,
                                                                        core::net::AcceptLoopHealth& acceptLoops);

    /// Raise the stop flag, stop accepting, drain the requests in flight -- cancelling every connection still open
    /// once the drain runs out -- and join every thread; within `WorstCaseStop` whatever a client does.
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
                IConnectionCanceller& connections,
                std::string boundEndpoint,
                std::vector<Mcp::ToolRow> tools,
                McpStopFlag& stopping,
                IMetricsSink& metrics,
                ILogger& logger,
                core::net::AcceptLoopHealth& acceptLoops);

    McpStopFlag& _stopping;
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
/// @param stopping Raised when the endpoint begins to stop; must outlive the endpoint.
/// @param metrics Where a refused request is counted.
/// @param logger Where the bound endpoint is announced.
/// @param acceptLoops The node's accept-loop registry; must outlive the endpoint.
/// @return The running endpoint, null when the surface is off, or why it could not be served.
[[nodiscard]] std::expected<std::unique_ptr<McpEndpoint>, NodeRefusal> StartMcpSurfaceOrExplain(
    NodeConfig const& cfg,
    std::vector<Mcp::ToolRow> tools,
    McpStopFlag& stopping,
    IMetricsSink& metrics,
    ILogger& logger,
    core::net::AcceptLoopHealth& acceptLoops);

} // namespace Pace::Node
