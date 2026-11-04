// SPDX-License-Identifier: Apache-2.0
#include "McpEndpoint.hpp"
#include "NodeConfig.hpp"
#include "NodeSurfaces.hpp"

#include <Pace/Cli/Options.hpp>
#include <Pace/Core/Bytes.hpp>
#include <Pace/Core/HostPort.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/ServiceControl.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>
#include <Pace/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/ISocket.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RelabelledPeerListener.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using namespace std::chrono_literals;
using Pace::Testing::Unwrap;

namespace
{

/// Parse an argv fragment the way `main` does, shaped as a first start.
/// @param args The flags, without the program name.
/// @return The configuration; the parse must succeed.
[[nodiscard]] NodeConfig Parsed(std::vector<char const*> const& args)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg);
    INFO((flow.has_value() ? std::string {} : flow.error().ToString()));
    REQUIRE(flow.has_value());
    return cfg;
}

core::async::Task<bool> WriteAll(core::net::ISocket* socket, std::string_view payload)
{
    auto const written = co_await socket->write(AsBytes(payload));
    co_return written.has_value();
}

/// Read until the server closes, which it does after every response (`Connection: close`).
core::async::Task<std::string> ReadToClose(core::net::ISocket* socket)
{
    std::string out;
    std::vector<std::byte> chunk(4096);
    while (true)
    {
        auto const read = co_await socket->read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!read.has_value() || *read == 0)
            break;
        std::ranges::transform(
            std::span { chunk }.first(*read), std::back_inserter(out), [](std::byte b) { return static_cast<char>(b); });
    }
    co_return out;
}

/// One raw HTTP exchange with a real loopback port.
/// @param port Where the endpoint listens.
/// @param request The whole request, head and body.
/// @return Everything the server wrote before closing.
[[nodiscard]] std::string Exchange(std::uint16_t port, std::string const& request)
{
    core::net::BlockingConnector connector;
    auto client =
        core::async::syncRun(connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
    REQUIRE(client.has_value());
    (*client)->setReceiveDeadline(5s);
    REQUIRE(core::async::syncRun(WriteAll(client->get(), request)));
    return core::async::syncRun(ReadToClose(client->get()));
}

/// A `POST /mcp` with @p headers (each a whole `Name: value` line, CRLF-terminated) and @p body.
[[nodiscard]] std::string PostMcp(std::uint16_t port, std::string_view headers, std::string_view body)
{
    return Exchange(port,
                    std::format("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                "Content-Length: {}\r\n{}\r\n{}",
                                body.size(),
                                headers,
                                body));
}

/// The modern `server/discover` body, its headers left to the caller.
constexpr std::string_view DiscoverBody {
    R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{)"
    R"("io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}})"
};

/// The headers a modern `server/discover` mirrors, spelled the way the spec writes them.
constexpr std::string_view DiscoverHeaders { "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: server/discover\r\n" };

[[nodiscard]] std::string_view StatusLineOf(std::string_view response)
{
    return response.substr(0, response.find("\r\n"));
}

[[nodiscard]] std::string_view HeadOf(std::string_view response)
{
    return response.substr(0, response.find("\r\n\r\n"));
}

[[nodiscard]] Json::Value BodyOf(std::string_view response)
{
    auto const split = response.find("\r\n\r\n");
    REQUIRE(split != std::string_view::npos);
    auto parsed = Json::Parse(response.substr(split + 4));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

/// Whether the response head carries a header named @p name, in any case.
[[nodiscard]] bool HeadNames(std::string_view response, std::string_view name)
{
    auto const lower = [](std::string_view text) {
        std::string out { text };
        std::ranges::transform(
            out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        return out;
    };
    return lower(HeadOf(response)).contains(std::format("\r\n{}:", lower(name)));
}

/// A running `McpEndpoint` over a real loopback listener, optionally relabelled as another host.
struct RunningEndpoint
{
    AtomicMetricsSink metrics;
    NullLogger logger;
    core::net::AcceptLoopHealth acceptLoops;
    std::uint16_t port { 0 };
    std::unique_ptr<McpEndpoint> endpoint;

    /// @param relabelAs What every accepted connection reports as its peer; empty for the real one.
    explicit RunningEndpoint(std::string relabelAs = {})
    {
        auto bound = BlockingListener::Bind("127.0.0.1", 0);
        REQUIRE(bound != nullptr);
        REQUIRE(bound->IsBound());
        // As `McpEndpoint::Start` arms its own listener.
        bound->SetTimeouts(AdminHttpServer::AcceptPoll, AdminHttpServer::RequestTimeout);
        port = bound->boundPort();
        std::unique_ptr<core::net::IListener> listener = std::move(bound);
        if (!relabelAs.empty())
            listener = std::make_unique<Testing::RelabelledPeerListener>(std::move(listener), std::move(relabelAs));
        endpoint = McpEndpoint::StartWithListener(std::move(listener),
                                                  std::format("127.0.0.1:{}", port),
                                                  std::vector<Mcp::ToolRow> {},
                                                  metrics,
                                                  logger,
                                                  acceptLoops);
        REQUIRE(endpoint != nullptr);
    }
};

} // namespace

TEST_CASE("mcp-listen bare port binds loopback", "[node][mcp][surfaces]")
{
    auto const cfg = Parsed({ "--mcp-listen=7123" });
    CHECK(cfg.mcpListenExplicit);
    auto const endpoint = SoleEndpointOf(NodeSurface::Mcp, cfg);
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->host == "127.0.0.1");
    CHECK(endpoint->port == 7123);
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());

    SECTION("and the surface is off unless asked for")
    {
        auto const off = Parsed({});
        CHECK(RowFor(NodeSurface::Mcp).Resolve(off).empty());
        CHECK_FALSE(off.mcpListenExplicit);
    }
}

TEST_CASE("mcp-listen on a non-loopback host refuses to start", "[node][mcp][surfaces][startup-refusal]")
{
    // The design goal is that MCP is never network-bound: there is no MCP authentication scheme,
    // so an address other machines could reach is refused before anything binds -- by the start, the
    // worksheet and the install alike, since all three ask the one table.
    for (auto const* const flag: { "--mcp-listen=0.0.0.0:7123",
                                   "--mcp-listen=10.0.0.7:7123",
                                   "--mcp-listen=[::]:7123",
                                   // A NAME is not loopback, whatever a resolver says it reaches.
                                   "--mcp-listen=localhost:7123" })
    {
        INFO(flag);
        auto const cfg = Parsed({ flag });
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal) == McpListenNotLoopbackRefusal);
        CHECK(ReportSurfaces(cfg).refusal == refusal);
        CHECK(NodeInstallRejection(cfg) == refusal);
    }

    SECTION("every loopback spelling starts: the control")
    {
        for (auto const* const flag: { "--mcp-listen=7123",
                                       "--mcp-listen=127.0.0.1:7123",
                                       "--mcp-listen=127.0.0.2:7123",
                                       "--mcp-listen=[::1]:7123" })
        {
            INFO(flag);
            CHECK_FALSE(StartupPolicyRejection(Parsed({ flag })).has_value());
        }
    }
}

TEST_CASE("mcp-listen appears in --print-surfaces and the service spec only when explicit", "[node][mcp][surfaces]")
{
    auto const named = Parsed({ "--mcp-listen=7123" });
    auto const sheet = RenderSurfaces(named);
    INFO(sheet);
    CHECK(sheet.contains("mcp"));
    CHECK(sheet.contains("127.0.0.1:7123"));

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "pace-node" }, named, Testing::InstallerPathProbe());
    CHECK(std::ranges::count(spec.arguments, std::string { "--mcp-listen=7123" }) == 1);

    SECTION("unnamed, the worksheet says it is off and the registration carries nothing")
    {
        auto const unnamed = Parsed({});
        auto const offSheet = RenderSurfaces(unnamed);
        INFO(offSheet);
        CHECK(offSheet.contains("--mcp-listen"));
        CHECK_FALSE(offSheet.contains("127.0.0.1:7123"));
        auto const offSpec =
            MakeNodeServiceSpec(std::filesystem::path { "pace-node" }, unnamed, Testing::InstallerPathProbe());
        CHECK(std::ranges::none_of(offSpec.arguments,
                                   [](std::string const& arg) { return FlagMatches(arg, "--mcp-listen"); }));
    }
    SECTION("not typed, it is not emitted whatever its value: provenance, not value")
    {
        auto copied = named;
        copied.mcpListenExplicit = false;
        auto const quiet = MakeNodeServiceSpec(std::filesystem::path { "pace-node" }, copied, Testing::InstallerPathProbe());
        CHECK(
            std::ranges::none_of(quiet.arguments, [](std::string const& arg) { return FlagMatches(arg, "--mcp-listen"); }));
    }
}

TEST_CASE("mcp admission takes the kernel's loopback spellings and nothing else", "[node][mcp][admission]")
{
    // What `AdminRequest::peerHost` can be: `inet_ntop`'s host, unbracketed and portless -- the IPv4
    // loopback, `::1`, the mapped form a dual-stack listener reports, or EMPTY when the socket could not
    // say, which must not count as local.
    for (auto const* const host: { "127.0.0.1", "127.0.0.2", "::1", "::ffff:127.0.0.1" })
    {
        INFO(host);
        CHECK(McpAdmits(ConnectionFacts { .host = host }));
    }
    for (auto const* const host:
         { "", "10.0.0.7", "203.0.113.9", "::ffff:10.0.0.7", "localhost", "[::1]", "127.0.0.1:7123" })
    {
        INFO(host);
        CHECK_FALSE(McpAdmits(ConnectionFacts { .host = host }));
    }
}

TEST_CASE("a relabelled non-loopback peer is refused 403 and the counter moves", "[node][mcp][admission]")
{
    // Loopback is the only caller a fixture on this machine can be, so the refusal leg is reached by a
    // real socket that REPORTS another host -- with the endpoint's real HTTP code in between.
    RunningEndpoint relabelled { "203.0.113.9" };
    auto const refused = PostMcp(relabelled.port, DiscoverHeaders, DiscoverBody);
    INFO(refused);
    CHECK(StatusLineOf(refused) == "HTTP/1.1 403 Forbidden");
    auto const body = BodyOf(refused);
    REQUIRE(body.Find("error") != nullptr);
    CHECK(body.Find("error")->Find("code")->AsInt() == -32600);
    CHECK(body.Find("result") == nullptr);
    CHECK(relabelled.metrics.Read(IMetricsSink::Counter::McpRequestsRefusedNotLoopback) == 1);

    SECTION("the same endpoint unrelabelled answers, and counts nothing: the control")
    {
        RunningEndpoint local;
        auto const answered = PostMcp(local.port, DiscoverHeaders, DiscoverBody);
        INFO(answered);
        CHECK(StatusLineOf(answered) == "HTTP/1.1 200 OK");
        CHECK(local.metrics.Read(IMetricsSink::Counter::McpRequestsRefusedNotLoopback) == 0);
    }
}

TEST_CASE("mcp server/discover over a real loopback socket names 2026-07-28 and no tools yet", "[node][mcp]")
{
    RunningEndpoint local;
    auto const discovered = PostMcp(local.port, DiscoverHeaders, DiscoverBody);
    INFO(discovered);
    REQUIRE(StatusLineOf(discovered) == "HTTP/1.1 200 OK");
    auto const body = BodyOf(discovered);
    auto const* versions = body.Find("result")->Find("supportedVersions");
    REQUIRE(versions != nullptr);
    REQUIRE(versions->AsArray() != nullptr);
    CHECK(std::ranges::any_of(Unwrap(versions->AsArray()), [](Json::Value const& v) {
        return v.AsString() != nullptr && *v.AsString() == "2026-07-28";
    }));

    auto const listed = PostMcp(local.port,
                                "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/list\r\n",
                                R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{)"
                                R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                                R"("io.modelcontextprotocol/clientCapabilities":{}}}})");
    INFO(listed);
    REQUIRE(StatusLineOf(listed) == "HTTP/1.1 200 OK");
    auto const listedBody = BodyOf(listed);
    auto const* const tools = listedBody.Find("result")->Find("tools");
    REQUIRE(tools != nullptr);
    REQUIRE(tools->AsArray() != nullptr);
    CHECK(tools->AsArray()->empty());
}

TEST_CASE("mcp ignores Mcp-Session-Id and Last-Event-ID and echoes neither", "[node][mcp]")
{
    // The transport says a server that mints no session ignores both. Through the real HTTP code, so
    // a header the parser did not bound or a response echoing one is caught where it would happen.
    RunningEndpoint local;
    auto const response = PostMcp(
        local.port, std::format("{}Mcp-Session-Id: abc-123\r\nLast-Event-ID: 42\r\n", DiscoverHeaders), DiscoverBody);
    INFO(response);
    CHECK(StatusLineOf(response) == "HTTP/1.1 200 OK");
    CHECK_FALSE(HeadNames(response, "Mcp-Session-Id"));
    CHECK_FALSE(HeadNames(response, "Last-Event-ID"));
    CHECK_FALSE(response.contains("abc-123"));
}

TEST_CASE("mcp matches its headers without regard to case", "[node][mcp]")
{
    // A field name is case-insensitive (RFC 9110), and the MCP transport says a server MUST treat it so.
    // The mismatch control proves the headers were READ: with them spelled oddly and wrong, the request
    // is refused as a header mismatch rather than passing because nothing looked.
    RunningEndpoint local;
    auto const mixed =
        PostMcp(local.port, "mcp-protocol-version: 2026-07-28\r\nMCP-METHOD: server/discover\r\n", DiscoverBody);
    INFO(mixed);
    CHECK(StatusLineOf(mixed) == "HTTP/1.1 200 OK");

    auto const wrong = PostMcp(local.port, "mcp-protocol-version: 2026-07-28\r\nMCP-METHOD: tools/list\r\n", DiscoverBody);
    INFO(wrong);
    CHECK(StatusLineOf(wrong) == "HTTP/1.1 400 Bad Request");
    CHECK(BodyOf(wrong).Find("error")->Find("code")->AsInt() == -32020);
}

TEST_CASE("mcp serves no scrape endpoint and only POST on its path", "[node][mcp]")
{
    // The surface reuses the admin surface's HTTP code; it must not grow `/metrics` or `/healthz` by it.
    RunningEndpoint local;
    CHECK(StatusLineOf(Exchange(local.port, "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n")) == "HTTP/1.1 404 Not Found");
    CHECK(StatusLineOf(Exchange(local.port, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")) == "HTTP/1.1 404 Not Found");
    auto const get = Exchange(local.port, "GET /mcp HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(StatusLineOf(get) == "HTTP/1.1 405 Method Not Allowed");
    CHECK(HeadNames(get, "Allow"));
}

TEST_CASE("an MCP port another listener holds refuses to start, naming the address", "[node][mcp][surfaces]")
{
    auto holder = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(holder != nullptr);
    REQUIRE(holder->IsBound());
    auto const flag = std::format("--mcp-listen={}", holder->boundPort());
    auto const cfg = Parsed({ flag.c_str() });

    AtomicMetricsSink metrics;
    CapturingLogger logger;
    core::net::AcceptLoopHealth acceptLoops;
    auto const started = McpEndpoint::Start(cfg, {}, metrics, logger, acceptLoops);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains(std::format("127.0.0.1:{}", holder->boundPort())));
}
