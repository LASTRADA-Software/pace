// SPDX-License-Identifier: Apache-2.0
#include "McpEndpoint.hpp"
#include "NodeSurfaces.hpp"

#include <Pace/Core/BoundedDrain.hpp>
#include <Pace/Core/HostPort.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Core/Version.hpp>
#include <Pace/Transport/NativeListen.hpp>

#include <cstdint>
#include <format>
#include <utility>

#include <core/async/SyncRun.hpp>

namespace Pace::Node
{

namespace
{
    /// The JSON-RPC code a refused connection is answered with: `-32600`, the one `McpServer` answers a
    /// forbidden browser `Origin` with, since both are the transport refusing the caller rather than the
    /// method failing.
    constexpr std::int64_t NotLoopbackCode = -32600;

    /// The `403` a caller that is not on this machine is answered with. No `id`: the body was never
    /// parsed, so there is none to echo.
    [[nodiscard]] AdminResponse NotLoopbackResponse()
    {
        Json::Value const body { Json::Object {
            { "jsonrpc", Json::Value { std::string { "2.0" } } },
            { "error",
              Json::Value { Json::Object {
                  { "code", Json::Value { NotLoopbackCode } },
                  { "message",
                    Json::Value { std::string { "the MCP endpoint answers callers on this machine only" } } } } } } } };
        return AdminResponse { .status = "403 Forbidden", .contentType = "application/json", .body = Json::Serialize(body) };
    }
} // namespace

bool McpAdmits(ConnectionFacts const& facts) noexcept
{
    return IsLoopbackHost(facts.host);
}

AdminRoute MakeMcpRoute(Mcp::McpServer const& server, IMetricsSink& metrics)
{
    return AdminRoute {
        .path = McpEndpoint::Path,
        .handler =
            [&server, &metrics](AdminRequest const& request) {
                // Asked per request, before the envelope is parsed, through the facts every admission
                // gate folds: the bind refusal is what keeps the address loopback, and this is what
                // keeps a future wider bind from silently widening who is answered.
                if (!McpAdmits(ConnectionFacts { .host = request.peerHost }))
                {
                    metrics.Increment(IMetricsSink::Counter::McpRequestsRefusedNotLoopback);
                    return NotLoopbackResponse();
                }
                return server.Handle(request);
            },
        .match = AdminRouteMatch::Exact,
        .method = AdminMethod::Post,
        // The ceiling the envelope is parsed under, refused `413` before one byte is buffered: a body
        // longer than the parser would accept is never worth reading.
        .maxBodyBytes = Mcp::RequestParseLimits.maxBytes,
    };
}

McpEndpoint::McpEndpoint(std::unique_ptr<core::net::IListener> listener,
                         IConnectionCanceller& connections,
                         std::string boundEndpoint,
                         std::vector<Mcp::ToolRow> tools,
                         McpStopFlag& stopping,
                         IMetricsSink& metrics,
                         ILogger& logger,
                         core::net::AcceptLoopHealth& acceptLoops):
    _stopping { stopping },
    _listener { std::move(listener) },
    _server { std::move(tools), VersionString, logger },
    _http { std::make_unique<AdminHttpServer>(
        *_listener,
        connections,
        metrics,
        // Never called: the profile serves no `/metrics`, so there is no snapshot to take.
        AdminHttpServer::SnapshotProvider {},
        logger,
        _clock,
        acceptLoops,
        // The loop owns its thread and its listener blocks, so a backoff blocks too.
        DefaultDrainWait(),
        std::vector<AdminRoute> { MakeMcpRoute(_server, metrics) },
        nullptr,
        EverySurface,
        AdminServerProfile { .surface = SurfaceName,
                             .maxConcurrentRequests = MaxConcurrentRequests,
                             .deadlines = Deadlines,
                             .builtIns = AdminBuiltInRoutes::None,
                             .serving = AdminConnectionServing::OnOwnThreads }) },
    _boundEndpoint { std::move(boundEndpoint) },
    _thread { [http = _http.get()] { core::async::syncRun(http->Run()); } }
{
}

McpEndpoint::~McpEndpoint()
{
    // The flag FIRST: from here every forwarded call a running request makes is refused at once, so the drain
    // below waits for the one call each request has in flight rather than for every call a tool would go on making.
    _stopping.Raise();
    // Ask, drain, join, then close: `AdminHttpServer::Stop`'s one order (#260) -- closing the listener while the
    // accept thread reads it is a data race, and closing is not what wakes it. Past the drain every connection still
    // open is cancelled, so a client reading its answer slowly cannot hold the serving threads' join.
    _http->Stop(_thread);
}

std::unique_ptr<McpEndpoint> McpEndpoint::StartWithListener(std::unique_ptr<core::net::IListener> listener,
                                                            IConnectionCanceller& connections,
                                                            std::string boundEndpoint,
                                                            std::vector<Mcp::ToolRow> tools,
                                                            McpStopFlag& stopping,
                                                            IMetricsSink& metrics,
                                                            ILogger& logger,
                                                            core::net::AcceptLoopHealth& acceptLoops)
{
    // `new` because the constructor is private: this and `Start` are the only ways to reach it.
    return std::unique_ptr<McpEndpoint> { new McpEndpoint { std::move(listener),
                                                            connections,
                                                            std::move(boundEndpoint),
                                                            std::move(tools),
                                                            stopping,
                                                            metrics,
                                                            logger,
                                                            acceptLoops } };
}

std::expected<std::unique_ptr<McpEndpoint>, std::string> McpEndpoint::Start(NodeConfig const& cfg,
                                                                            std::vector<Mcp::ToolRow> tools,
                                                                            McpStopFlag& stopping,
                                                                            IMetricsSink& metrics,
                                                                            ILogger& logger,
                                                                            core::net::AcceptLoopHealth& acceptLoops)
{
    // Resolved by the row, so the address the startup rule judged loopback is the address bound.
    auto const resolved = SoleEndpointOf(NodeSurface::Mcp, cfg);
    if (!resolved.has_value())
        return std::unexpected { resolved.error() };
    auto const& endpoint = *resolved;

    auto listener = BlockingListener::Bind(endpoint.host, endpoint.port);
    if (!listener || !listener->IsBound())
    {
        auto judged = JudgeBindFailure(RowFor(NodeSurface::Mcp),
                                       std::format("cannot bind {} ({})",
                                                   FormatHostPort(endpoint.host, endpoint.port),
                                                   listener ? listener->BindError() : std::string_view { "null listener" }),
                                       logger);
        if (!judged.has_value())
            return std::unexpected { std::move(judged).error() };
        // Refused rather than tolerated, because this opener cannot carry a tolerated verdict
        // (#352): its caller logs `BoundEndpoint()` of what it returns.
        return std::unexpected { BindToleranceUnsupported(
            RowFor(NodeSurface::Mcp), "the caller logs the endpoint's BoundEndpoint() unconditionally") };
    }

    // The admin surface's own figures: the same HTTP code, on the same terms.
    listener->SetTimeouts(AdminHttpServer::AcceptPoll, AdminHttpServer::RequestTimeout);
    // Taken before the listener moves: the registry is the listener's, and lives exactly as long.
    auto& connections = listener->Connections();
    return StartWithListener(std::move(listener),
                             connections,
                             FormatHostPort(endpoint.host, endpoint.port),
                             std::move(tools),
                             stopping,
                             metrics,
                             logger,
                             acceptLoops);
}

std::expected<std::unique_ptr<McpEndpoint>, NodeRefusal> StartMcpSurfaceOrExplain(NodeConfig const& cfg,
                                                                                  std::vector<Mcp::ToolRow> tools,
                                                                                  McpStopFlag& stopping,
                                                                                  IMetricsSink& metrics,
                                                                                  ILogger& logger,
                                                                                  core::net::AcceptLoopHealth& acceptLoops)
{
    // Off unless asked for, like every other surface this program serves.
    if (cfg.mcpListen.empty())
        return std::unique_ptr<McpEndpoint> {};
    auto started = McpEndpoint::Start(cfg, std::move(tools), stopping, metrics, logger, acceptLoops);
    if (!started.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::Listener, std::format("--mcp-listen {}", started.error())) };
    logger.Logf(LogLevel::Info,
                "MCP endpoint on http://{}{} (callers on this machine only)",
                (*started)->BoundEndpoint(),
                McpEndpoint::Path);
    return std::move(*started);
}

} // namespace Pace::Node
