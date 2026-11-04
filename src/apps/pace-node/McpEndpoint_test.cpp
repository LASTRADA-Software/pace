// SPDX-License-Identifier: Apache-2.0
#include "McpEndpoint.hpp"
#include "NodeConfig.hpp"
#include "NodeSurfaces.hpp"

#include <Pace/Cli/Options.hpp>
#include <Pace/Core/BoundedDrain.hpp>
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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RelabelledPeerListener.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WriteWatchingListener.hpp>

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

/// One read, as a task a thread can run to its end.
/// @param socket Where to read.
/// @param buffer Where the bytes go; non-empty.
/// @return What the read answered.
core::async::Task<core::net::IoResult> ReadOnce(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->read(buffer);
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
    McpStopFlag stopping;
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
        auto& connections = bound->Connections();
        std::unique_ptr<core::net::IListener> listener = std::move(bound);
        if (!relabelAs.empty())
            listener = std::make_unique<Testing::RelabelledPeerListener>(std::move(listener), std::move(relabelAs));
        endpoint = McpEndpoint::StartWithListener(std::move(listener),
                                                  connections,
                                                  std::format("127.0.0.1:{}", port),
                                                  std::vector<Mcp::ToolRow> {},
                                                  stopping,
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
    McpStopFlag stopping;
    auto const started = McpEndpoint::Start(cfg, {}, stopping, metrics, logger, acceptLoops);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains(std::format("127.0.0.1:{}", holder->boundPort())));
}

namespace
{
/// A connection to @p port that sends nothing: what a hostile or broken local client holds.
/// @param port Where the endpoint listens.
/// @return The connected socket.
[[nodiscard]] std::unique_ptr<core::net::ISocket> IdleConnection(std::uint16_t port)
{
    core::net::BlockingConnector connector;
    auto client =
        core::async::syncRun(connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
    REQUIRE(client.has_value());
    return std::move(*client);
}
} // namespace

TEST_CASE("mcp answers a request while an idle connection holds a slot", "[node][mcp][hostile]")
{
    // The surface serves each connection on a thread of its own: a peer that connected and said nothing holds one
    // slot, never the surface. Served on the accept thread, this request would wait out the idle one's whole
    // first-byte bound before it was even read -- which is what the elapsed bound tells apart.
    RunningEndpoint const local;
    auto const idle = IdleConnection(local.port);
    auto const started = std::chrono::steady_clock::now();
    auto const response = PostMcp(local.port, DiscoverHeaders, DiscoverBody);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    CHECK(StatusLineOf(response) == "HTTP/1.1 200 OK");
    CHECK(elapsed < McpEndpoint::Deadlines.firstByte);
}

TEST_CASE("mcp answers 503 at once past its concurrency cap", "[node][mcp][hostile]")
{
    // `MaxConcurrentRequests` idle connections fill every slot, and the next is refused rather than queued. The
    // accept order is the connect order, so the slots are taken before the refused one is accepted; and well inside
    // the idle ones' first-byte bound, so none has been freed yet.
    RunningEndpoint const local;
    std::vector<std::unique_ptr<core::net::ISocket>> held;
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, McpEndpoint::MaxConcurrentRequests))
        held.push_back(IdleConnection(local.port));
    auto const started = std::chrono::steady_clock::now();
    auto const refused = PostMcp(local.port, DiscoverHeaders, DiscoverBody);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    CHECK(StatusLineOf(refused) == "HTTP/1.1 503 Service Unavailable");
    CHECK(elapsed < McpEndpoint::Deadlines.firstByte);

    SECTION("and answers again once the held connections go")
    {
        held.clear();
        // Each held connection's thread reads the close at once and frees its slot; the wait is bounded.
        auto last = std::string {};
        CHECK(Testing::WaitUntil(
            "a slot freed by the closed connections to answer server/discover 200",
            [&] {
                last = std::string { StatusLineOf(PostMcp(local.port, DiscoverHeaders, DiscoverBody)) };
                return last == "HTTP/1.1 200 OK";
            },
            [&] { return std::format("last answer: {}", last); },
            Testing::WaitOptions { .step = {}, .context = {}, .bound = McpEndpoint::Deadlines.firstByte * 2 }));
    }
}

TEST_CASE("mcp closes a connection that sends nothing after its own first-byte bound", "[node][mcp][hostile]")
{
    // A silent peer frees its slot in `Deadlines.firstByte`, not the admin surface's thirty seconds: the bound that
    // decides how long four idle connects can keep MCP refusing. Closed without an answer -- it asked nothing.
    static_assert(McpEndpoint::Deadlines.firstByte < AdminHttpServer::FirstByteTimeout,
                  "MCP's first-byte bound is its own, and shorter than the dashboard's preconnect allowance");
    RunningEndpoint const local;
    auto const idle = IdleConnection(local.port);
    idle->setReceiveDeadline(McpEndpoint::Deadlines.firstByte * 5);
    auto const started = std::chrono::steady_clock::now();
    auto const said = core::async::syncRun(ReadToClose(idle.get()));
    auto const elapsed = std::chrono::steady_clock::now() - started;
    CHECK(said.empty());
    // Not sooner than the bound, less the time between the connect and this read starting...
    CHECK(elapsed >= McpEndpoint::Deadlines.firstByte - 500ms);
    // ...and closed by the server, well before this read's own deadline would have ended it.
    CHECK(elapsed < McpEndpoint::Deadlines.firstByte * 3);
}

namespace
{
/// An upstream whose first question takes `Hold` to answer and whose later ones answer at once, counting each:
/// a forwarded call that is still running when the surface is told to stop.
class SlowFirstUpstream final: public IMcpUpstream
{
  public:
    /// Longer than the dashboard's five-second drain, so a stop that kept that figure would log an abandoned request;
    /// well inside `McpUpstream::CallDeadline`, as a forwarded call that answers is.
    static constexpr auto Hold = std::chrono::milliseconds { 6000 };
    static_assert(Hold > DrainBound {}.ceiling && Hold < McpUpstream::CallDeadline);

    /// @copydoc IMcpUpstream::Ask
    [[nodiscard]] UpstreamResult Ask(UpstreamTarget /*target*/, std::string_view /*node*/, std::string /*frame*/) override
    {
        if (asked.fetch_add(1) == 0)
        {
            entered.store(true);
            std::this_thread::sleep_for(Hold);
        }
        return UpstreamReply { .status = CompileCacheWire::Status::Ok, .payload = {}, .answeredBy = "leader", .term = {} };
    }

    std::atomic<std::size_t> asked { 0 };
    std::atomic<bool> entered { false };
};

/// The `tools/call` of the one tool the stop case serves.
constexpr std::string_view SlowCallBody {
    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"_meta":{)"
    R"("io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},)"
    R"("name":"two_questions","arguments":{}}})"
};
} // namespace

TEST_CASE("an MCP stop during a forwarded call is bounded and refuses later calls at once and abandons nothing",
          "[node][mcp][hostile]")
{
    // A tool asks twice through the production gate. The stop lands while the first question is in flight on a serving
    // thread: the stop flag must refuse the second at once rather than forward it, the drain must outlast the call in
    // flight rather than log it abandoned, and the whole stop must fit `WorstCaseStop`. The counter the connection
    // decrements is a member of the server, so the serving thread must be joined before it goes.
    SlowFirstUpstream inner;
    McpStopFlag stopping;
    LoopGatedUpstream gated { inner, [] { return true; }, [&stopping] { return stopping.Raised(); } };
    std::mutex secondMutex;
    std::optional<UpstreamResult> second;
    auto secondTook = std::chrono::steady_clock::duration {};
    auto tools = std::vector<Mcp::ToolRow> { Mcp::ToolRow {
        .name = "two_questions",
        .title = "Two questions",
        .description = "Asks the upstream twice.",
        .inputSchema = R"({"type":"object"})",
        .outputSchema = {},
        .effect = Mcp::ToolEffect::ReadOnly,
        .handler =
            [&](Mcp::ToolCall const& /*call*/) {
                std::ignore = gated.Ask(UpstreamTarget::Leader, {}, "first");
                auto const started = std::chrono::steady_clock::now();
                auto answer = gated.Ask(UpstreamTarget::Leader, {}, "second");
                auto const guard = std::scoped_lock { secondMutex };
                secondTook = std::chrono::steady_clock::now() - started;
                second = std::move(answer);
                return Mcp::ToolResult { .structured = {}, .text = "asked twice", .isError = false };
            },
    } };

    AtomicMetricsSink metrics;
    CapturingLogger logger;
    core::net::AcceptLoopHealth acceptLoops;
    auto bound = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(bound != nullptr);
    REQUIRE(bound->IsBound());
    bound->SetTimeouts(AdminHttpServer::AcceptPoll, AdminHttpServer::RequestTimeout);
    auto const port = bound->boundPort();
    auto& connections = bound->Connections();
    auto endpoint = McpEndpoint::StartWithListener(std::move(bound),
                                                   connections,
                                                   std::format("127.0.0.1:{}", port),
                                                   std::move(tools),
                                                   stopping,
                                                   metrics,
                                                   logger,
                                                   acceptLoops);
    REQUIRE(endpoint != nullptr);

    // Sent and left open: the response is never read, as a client that went quiet would leave it.
    core::net::BlockingConnector connector;
    auto client =
        core::async::syncRun(connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
    REQUIRE(client.has_value());
    REQUIRE(core::async::syncRun(
        WriteAll(client->get(),
                 std::format("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                             "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: two_questions\r\n"
                             "Content-Length: {}\r\n\r\n{}",
                             SlowCallBody.size(),
                             SlowCallBody))));
    REQUIRE(Testing::WaitUntil(
        "the tool's first question to be in flight on a serving thread",
        [&] { return inner.entered.load(); },
        [&] { return std::format("asked {}", inner.asked.load()); },
        Testing::WaitOptions { .step = {}, .context = {}, .bound = 5s }));

    auto const stopBegan = std::chrono::steady_clock::now();
    endpoint.reset();
    auto const stopTook = std::chrono::steady_clock::now() - stopBegan;

    // The stop waited for the call in flight and no longer than its stated bound.
    CHECK(stopTook < McpEndpoint::WorstCaseStop);
    // The second question was refused by name, at once, and never reached the upstream.
    {
        auto const guard = std::scoped_lock { secondMutex };
        REQUIRE(second.has_value());
        auto const& answered = Unwrap(second);
        REQUIRE_FALSE(answered.has_value());
        CHECK(answered.error().first == UpstreamFailure::Stopping);
        CHECK(secondTook < 1s);
    }
    CHECK(inner.asked.load() == 1);
    // And the request finished inside the drain, so the stop abandoned nothing.
    auto const records = logger.Snapshot();
    CHECK(std::ranges::none_of(records, [](CapturingLogger::Record const& record) {
        return record.message.contains("did not finish within the stop ceiling");
    }));
    // Nor did it cancel anything: the client was waiting for its answer, which the drain let finish.
    CHECK(std::ranges::none_of(records,
                               [](CapturingLogger::Record const& record) { return record.message.contains("cancelled"); }));
}

TEST_CASE("an MCP stop cancels a client that does not read its answer, within WorstCaseStop", "[node][mcp][hostile]")
{
    // A local client that reads its answer just often enough to keep every blocking write call inside its send
    // timeout holds the serving thread -- and the stop that joins it -- for as long as it likes. The drain cannot wait
    // for it, so past the drain the connection is cancelled and the stop ends inside `WorstCaseStop` regardless.
    //
    // **The case takes the send timeout OUT of the race, so the cancel is the only thing that can end the write.** The
    // shape it stands for -- a reader slow but moving -- defeats the send timeout by construction; reproducing it with
    // a trickle means tuning against every platform's timers, and a macOS send timed out under each trickle tried,
    // ending the connection before the drain did. So the listener is armed with a send timeout longer than the whole
    // case (`NoSendTimeoutInTheCase`) and the client reads NOTHING until the stop has returned: the server's write
    // parks once the buffers under the connection are full, and stays parked until something ends it from outside.
    // That is the listener's own column (`BlockingListener::SetTimeouts`), never the surface's deadlines row, so
    // drain, afterCancel and linger -- and `WorstCaseStop`, which is what is asserted -- are production's.
    //
    // Size does not stand in for a parked write -- Winsock took a 4 MiB send whole -- so the case SEES one
    // (`Testing::WaitUntilParkedInWrite`). The answer has only to outlast what the buffers under the connection hold:
    // the accepted socket's send buffer (`ClientSocketBufferBytes`, which every accept sets and Linux doubles) and the
    // client's receive buffer. No larger: the server serializes it, and a sanitized build pays for every byte.
    constexpr auto NoSendTimeoutInTheCase = std::chrono::milliseconds { std::chrono::minutes { 10 } };
    static_assert(NoSendTimeoutInTheCase > 4 * McpEndpoint::WorstCaseStop, "the send timeout must outlast the case");
    constexpr auto ClientReceiveBytes = std::size_t { 64 } * 1024;
    constexpr auto AnswerBytes = std::size_t { 6 } * 1024 * 1024;
    static_assert(AnswerBytes > 2 * ((2 * ClientSocketBufferBytes) + (2 * ClientReceiveBytes)),
                  "the answer must outlast every buffer under it");
    auto tools = std::vector<Mcp::ToolRow> { Mcp::ToolRow {
        .name = "huge_answer",
        .title = "Huge answer",
        .description = "Answers more than a socket buffers.",
        .inputSchema = R"({"type":"object"})",
        .outputSchema = {},
        .effect = Mcp::ToolEffect::ReadOnly,
        .handler =
            [](Mcp::ToolCall const& /*call*/) {
                return Mcp::ToolResult { .structured = {}, .text = std::string(AnswerBytes, 'x'), .isError = false };
            },
    } };

    AtomicMetricsSink metrics;
    CapturingLogger logger;
    core::net::AcceptLoopHealth acceptLoops;
    McpStopFlag stopping;
    auto bound = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(bound != nullptr);
    REQUIRE(bound->IsBound());
    bound->SetTimeouts(AdminHttpServer::AcceptPoll, NoSendTimeoutInTheCase);
    auto const port = bound->boundPort();
    auto& connections = bound->Connections();
    auto watched = std::make_unique<Testing::WriteWatchingListener>(std::move(bound));
    auto const watch = watched->Watch();
    auto endpoint = McpEndpoint::StartWithListener(std::move(watched),
                                                   connections,
                                                   std::format("127.0.0.1:{}", port),
                                                   std::move(tools),
                                                   stopping,
                                                   metrics,
                                                   logger,
                                                   acceptLoops);
    REQUIRE(endpoint != nullptr);

    constexpr std::string_view HugeCallBody {
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"_meta":{)"
        R"("io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},)"
        R"("name":"huge_answer","arguments":{}}})"
    };
    core::net::BlockingConnector connector;
    auto client = core::async::syncRun(connector.connect(
        "127.0.0.1",
        port,
        core::net::DialOptions { .connectTimeout = 5s, .buffers = { .send = {}, .receive = ClientReceiveBytes } }));
    REQUIRE(client.has_value());
    REQUIRE(core::async::syncRun(
        WriteAll(client->get(),
                 std::format("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                             "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: huge_answer\r\n"
                             "Content-Length: {}\r\n\r\n{}",
                             HugeCallBody.size(),
                             HugeCallBody))));

    // Nothing read: the server serializes the answer, fills what is under the connection and parks in a write. A
    // failure from here on leaves the endpoint to stop at the end of the scope, which its own cancel bounds.
    REQUIRE(Testing::WaitUntilParkedInWrite(*watch, 250ms, 60s));

    auto const stopBegan = std::chrono::steady_clock::now();
    std::atomic<bool> stopped { false };
    std::atomic<std::int64_t> stopTookMs { -1 };
    std::jthread stopper { [&] {
        endpoint.reset();
        stopTookMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopBegan).count());
        stopped.store(true);
    } };
    auto const ended = Testing::WaitUntil(
        "the MCP stop to cancel the parked write and return",
        [&] { return stopped.load(); },
        [&] { return watch->Describe(stopBegan); },
        Testing::WaitOptions { .step = {}, .context = {}, .bound = McpEndpoint::WorstCaseStop + 3s });
    // What the client was left to read, at full speed: what tells a connection the SERVER ended from one the case
    // merely stopped reading is that the stream then ENDS -- with end-of-stream or a reset, never a read timing out --
    // short of the answer. A stop that never returned is released by closing the client, so its thread joins.
    auto received = std::size_t { 0 };
    auto endedByServer = false;
    if (stopped)
    {
        (*client)->setReceiveDeadline(5s);
        std::vector<std::byte> chunk(std::size_t { 64 } * 1024);
        while (true)
        {
            auto const read = core::async::syncRun(ReadOnce(client->get(), std::span<std::byte> { chunk }));
            if (read.has_value() && *read > 0)
            {
                received += *read;
                continue;
            }
            endedByServer = read.has_value() || !core::net::isDeadlineExpiry(read.error().code);
            break;
        }
    }
    (*client)->close();
    stopper.join();

    auto const records = logger.Snapshot();
    // Every Warn and Error the stop logged, and what ended the server's write, so a red names what ended the
    // connection rather than only that the cancel did not.
    auto const said = [&records] {
        auto text = std::string {};
        for (auto const& record: records)
            if (record.level == LogLevel::Warn || record.level == LogLevel::Error)
                text += std::format("\n  {}", record.message);
        return text.empty() ? std::string { " (none)" } : text;
    }();
    INFO("the stop logged:" << said);
    INFO("the server's write, from the stop's start: " << watch->Describe(stopBegan));
    CHECK(ended);
    CHECK(stopTookMs.load() < std::chrono::duration_cast<std::chrono::milliseconds>(McpEndpoint::WorstCaseStop).count());
    // The SERVER ended the connection: the client was cut short of the answer, and its read did not merely time out.
    CHECK(endedByServer);
    CHECK(received < AnswerBytes);
    CHECK(std::ranges::any_of(records, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Warn && record.message.contains("mcp: stopping, cancelled 1 connection(s)");
    }));
}
