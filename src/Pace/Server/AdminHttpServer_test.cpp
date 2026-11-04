// SPDX-License-Identifier: Apache-2.0
#include <Pace/Cache/IStorage.hpp>
#include <Pace/Core/Bytes.hpp>
#include <Pace/Core/HostPort.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Core/Version.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Server/AdminHttpServer.hpp>
#include <Pace/Transport/LingeringClose.hpp>
#include <Pace/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/testing/FailingListener.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/ParkingReadableSocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/HalfClose.hpp>
#include <tests/ScopedSignalHandler.hpp>
#include <tests/SmallSendBufferListener.hpp>
#include <tests/SocketDoubles.hpp>

namespace
{

/// The canceller of a listener whose connections are not native sockets: an in-memory transport has nothing a stop
/// could shut down under a write, and its cases never ask it to.
class NothingToCancel final: public Pace::IConnectionCanceller
{
  public:
    [[nodiscard]] std::size_t CancelOpenConnections() noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t CancelledTotal() const noexcept override
    {
        return 0;
    }
};

/// Shared by every in-memory case: it holds no state worth isolating.
/// @return The one instance.
[[nodiscard]] Pace::IConnectionCanceller& NothingToCancelHere()
{
    static NothingToCancel instance;
    return instance;
}

/// One read, as a task a thread can run to its end.
/// @param socket Where to read.
/// @param buffer Where the bytes go; non-empty.
/// @return What the read answered.
core::async::Task<core::net::IoResult> ReadOnce(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->read(buffer);
}

core::async::Task<bool> WriteString(core::net::ISocket* socket, std::string_view payload)
{
    auto const result = co_await socket->write(Pace::AsBytes(payload));
    co_return result.has_value();
}

/// Close a socket the surface was served on the way a served connection is closed,
/// never with a bare `Close()`.
///
/// Every refusal this surface writes over a head it did not finish reading is followed
/// by the close, and a bare close over unread bytes is a reset that destroys the
/// refusal before the client reads it (#1554) -- which is what production's own close
/// avoids. A fixture closing more bluntly than production would assert on a response
/// no real client of it receives.
///
/// Through the socket the surface was SERVED on, decorator included, as production
/// closes it: a decorator whose reads fail ends the linger at once, where the pipe
/// under it would wait for a peer that never closes.
/// @param servedOn The socket the surface was driven on.
void CloseAsServed(core::net::ISocket* servedOn)
{
    core::async::syncRun(Pace::CloseLingering(servedOn, nullptr, Pace::AdminHttpServer::Linger));
}

core::async::Task<std::string> ReadAvailable(core::net::ISocket* socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const result = co_await socket->read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value() || *result == 0)
            break;
        for (auto const i: std::views::iota(std::size_t { 0 }, *result))
            out.push_back(static_cast<char>(chunk[i]));
        if (*result < chunk.size())
            break;
    }
    co_return out;
}

/// Drive the admin surface once and return every byte it wrote.
///
/// The tail every case in this file ends with, in ONE place. It was open-coded four
/// times -- the two `Exchange` helpers and both `FailingReadSocket` cases -- and the
/// four copies each carried their own `provider` lambda, which is four spellings of
/// one snapshot that have to stay in step. That is the drift argument
/// `SocketDecorator.hpp` was written to make about fakes, one file away and not yet
/// applied here.
///
/// `serveOn` is separate from `pair` on purpose: a decorator case serves the surface
/// on the WRAPPER while the assertion still reads from `pair.client`, so the two
/// cannot be derived from each other.
///
/// @param serveOn The socket the surface is driven on -- `pair.server.get()`, or a
///        decorator wrapping it.
/// @param pair The pair `serveOn` belongs to; its client is drained here.
/// @param metrics The sink the surface renders from.
/// @param stats The storage figures the snapshot carries.
/// @return Everything the server wrote, which is empty when it wrote none.
std::string ServeAndCollect(core::net::ISocket* serveOn,
                            core::net::testing::InMemorySocketPair& pair,
                            Pace::IMetricsSink const& metrics,
                            Pace::StorageStats stats)
{
    using namespace std::chrono_literals;
    auto provider = [&stats] {
        return Pace::MetricsSnapshot { .storage = stats, .host = std::nullopt, .uptime = Pace::Uptime { 7s } };
    };
    core::platform::SteadyClock clock;
    core::async::syncRun(Pace::ServeAdminHttp(serveOn, &metrics, provider, &clock));
    CloseAsServed(serveOn);
    return core::async::syncRun(ReadAvailable(pair.client.get()));
}

std::string Exchange(std::string_view request, Pace::IMetricsSink const& metrics, Pace::StorageStats stats)
{
    auto pair = core::net::testing::InMemorySocketPair::create();
    // Guarded rather than unconditional, which is what lets `ExchangeMaybeSilent`
    // below be a wrapper instead of a second copy: an empty request is a peer that
    // said nothing, and writing zero bytes is not the same as not writing.
    if (!request.empty())
        REQUIRE(core::async::syncRun(WriteString(pair.client.get(), request)));
    REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
    return ServeAndCollect(pair.server.get(), pair, metrics, stats);
}

/// An `core::net::ISocket` that never returns more than `limit` bytes from one `Read`.
///
/// The condition this exists for cannot be created over `core::net::testing::InMemorySocketPair`,
/// which is a byte pipe: two writes coalesce, so one 1024-byte read always gets
/// the whole request. On a real socket the request routinely arrives in more than
/// one segment, and a server that stops reading at the request line then leaves
/// the headers unread in the receive queue -- which makes `close()` send RST
/// instead of FIN. Stating the segmentation directly is the same device the
/// launcher's fake `IPathResolver` uses for aliasing it cannot create on the host.
///
/// Local because the CONDITION is this file's. The forwarding is not, and lives on
/// `SocketDecorator` -- including the four defaulted `core::net::ISocket` virtuals this used
/// to answer from the base rather than from the socket it decorates.
class ShortReadSocket final: public core::net::testing::SocketDecorator
{
  public:
    ShortReadSocket(core::net::ISocket& inner, std::size_t limit) noexcept:
        SocketDecorator { inner },
        _limit { limit }
    {
    }

    /// Hand up no more than `limit` bytes, whatever the caller asked for.
    /// @param buffer Where to put them.
    /// @return What the decorated socket delivered into the narrowed span.
    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return SocketDecorator::read(buffer.subspan(0, std::min(buffer.size(), _limit)));
    }

  private:
    std::size_t _limit;
};

/// Records every `SetReceiveDeadline` the server applies, in order.
///
/// The two deadlines are the whole of #828, and a test that cannot see WHICH was armed
/// WHEN can only assert that "some deadline" exists -- which both the fixed and the
/// broken tree produce.
class DeadlineRecordingSocket final: public core::net::testing::SocketDecorator
{
  public:
    explicit DeadlineRecordingSocket(core::net::ISocket& inner) noexcept:
        SocketDecorator { inner }
    {
    }

    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
    {
        armed.push_back(deadline);
        SocketDecorator::setReceiveDeadline(deadline);
    }

    std::vector<std::chrono::milliseconds> armed;
};

/// Hands up at most `limit` bytes per read and charges the clock for each one.
///
/// Models a client dribbling under the per-read deadline: every read succeeds, so the
/// per-read bound never fires, and only a TOTAL can see it.
class ClockAdvancingSocket final: public core::net::testing::SocketDecorator
{
  public:
    ClockAdvancingSocket(core::net::ISocket& inner,
                         std::size_t limit,
                         core::platform::ManualClock& clock,
                         std::chrono::milliseconds perRead) noexcept:
        SocketDecorator { inner },
        _limit { limit },
        _clock { clock },
        _perRead { perRead }
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        _clock.advance(_perRead);
        return SocketDecorator::read(buffer.subspan(0, std::min(buffer.size(), _limit)));
    }

  private:
    std::size_t _limit;
    core::platform::ManualClock& _clock;
    std::chrono::milliseconds _perRead;
};

/// Serve one request over a fresh pair, and return what reached the client.
///
/// `Exchange` above always writes a request first, which is precisely the shape
/// that cannot reach #824: every fixture in this file sent immediately, and that is
/// why a surface answering `400` to a silent peer went unnoticed for as long as it
/// did. This one allows an EMPTY request and returns whatever the server wrote --
/// **including nothing**, which is the assertion the silent outcomes need.
/// @param request What the client sends before the server is driven; may be empty.
/// @return Every byte the server wrote, which is empty when it wrote none.
std::string ExchangeMaybeSilent(std::string_view request)
{
    Pace::AtomicMetricsSink metrics;
    return Exchange(request, metrics, {});
}

} // namespace

TEST_CASE("AdminHttp: GET /healthz returns 200 OK", "[metrics][http]")
{
    Pace::AtomicMetricsSink metrics;
    auto const response = Exchange("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(response.ends_with("OK\n"));
}

TEST_CASE("AdminHttp: GET /metrics returns Prometheus body", "[metrics][http]")
{
    Pace::AtomicMetricsSink metrics;
    metrics.Increment(Pace::IMetricsSink::Counter::ConnectionsTotal, 4);
    Pace::StorageStats stats;
    stats.cmdGet = 42;
    auto const response = Exchange("GET /metrics HTTP/1.1\r\n\r\n", metrics, stats);
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(response.contains("Content-Type: text/plain; version=0.0.4\r\n"));
    REQUIRE(response.contains("pace_cmd_get_total 42\n"));
    REQUIRE(response.contains("pace_connections_total 4\n"));
    REQUIRE(response.contains("pace_uptime_seconds 7\n"));
    // The build's version, which `live-stats cache` titles its panel with (#134).
    REQUIRE(response.contains(std::format("pace_build_info{{version=\"{}\"}} 1\n", Pace::VersionString)));
}

TEST_CASE("AdminHttp: /metrics with a query string still routes", "[metrics][http]")
{
    Pace::AtomicMetricsSink metrics;
    auto const response = Exchange("GET /metrics?foo=bar HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("AdminHttp: unknown path returns 404", "[metrics][http]")
{
    Pace::AtomicMetricsSink metrics;
    auto const response = Exchange("GET /nope HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_CASE("AdminHttp: non-GET method returns 405 naming the methods it does serve", "[metrics][http]")
{
    Pace::AtomicMetricsSink metrics;
    auto const response = Exchange("POST /metrics HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    // RFC 9110 §15.5.6 makes this a MUST, and it is what separates "wrong verb"
    // from "wrong path" for whatever is probing this port.
    CHECK(response.contains("Allow: GET\r\n"));
}

TEST_CASE("AdminHttp: a request split across reads is consumed to the end", "[metrics][http]")
{
    // The defect this pins: `ReadRequestHead` used to stop at the FIRST CRLF, so a
    // request that arrived in more than one segment left `Host:` and `Connection:`
    // unread in the receive queue. Closing a socket with unread received data makes
    // the kernel send RST rather than FIN, so a monitoring probe scraping /metrics
    // saw `Connection reset by peer` -- intermittently, decided by nothing but how
    // the request happened to be segmented. An endpoint whose whole job is to report
    // that a worker is healthy must not be the thing that looks unhealthy.
    //
    // Observed here as leftover bytes, which is the same fact one layer up from the
    // reset: what is left unread is exactly what makes the kernel choose RST.
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(
        WriteString(pair.client.get(), "GET /healthz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")));
    REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());

    // One read can see no more than the request line, so the headers must be picked
    // up by a later one -- or left behind, which is the bug.
    ShortReadSocket shortReads { *pair.server, std::string_view { "GET /healthz HTTP/1.1\r\n" }.size() };

    Pace::AtomicMetricsSink metrics;
    using namespace std::chrono_literals;
    auto provider = [] {
        return Pace::MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Pace::Uptime { 7s } };
    };
    core::platform::SteadyClock clock;
    core::async::syncRun(Pace::ServeAdminHttp(&shortReads, &metrics, provider, &clock));

    auto const response = core::async::syncRun(ReadAvailable(pair.client.get()));
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));

    // Nothing of the request is left: the server read it to the end before answering.
    CHECK(core::async::syncRun(ReadAvailable(pair.server.get())).empty());
}

namespace
{

/// Drive one request against a caller-supplied route list.
///
/// Separate from `Exchange` rather than a defaulted parameter on it, so every
/// assertion that a built-in route is unchanged keeps calling the two-argument
/// form and cannot accidentally start depending on a contributed row.
/// @param request The raw request bytes.
/// @param routes What the caller registered.
/// @return Everything the server wrote back.
std::string ExchangeWithRoutes(std::string_view request, std::vector<Pace::AdminRoute> const& routes)
{
    Pace::AtomicMetricsSink metrics;
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(WriteString(pair.client.get(), request)));
    REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
    using namespace std::chrono_literals;
    auto provider = [] {
        return Pace::MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Pace::Uptime { 1s } };
    };
    core::platform::SteadyClock clock;
    core::async::syncRun(Pace::ServeAdminHttp(pair.server.get(), &metrics, provider, &clock, routes));
    CloseAsServed(pair.server.get());
    return core::async::syncRun(ReadAvailable(pair.client.get()));
}

} // namespace

TEST_CASE("AdminHttp: a contributed route answers on its own path", "[admin][http][routes]")
{
    std::vector<Pace::AdminRoute> const routes {
        { .path = "/fleet",
          .handler =
              [](Pace::AdminRequest const&) {
                  return Pace::AdminResponse { .status = "200 OK", .contentType = "text/html", .body = "<h1>fleet</h1>" };
              } },
    };

    auto const response = ExchangeWithRoutes("GET /fleet HTTP/1.1\r\nHost: x\r\n\r\n", routes);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.contains("Content-Type: text/html\r\n"));
    CHECK(response.contains("<h1>fleet</h1>"));
}

TEST_CASE("AdminHttp: the built-in routes still answer alongside a contributed one", "[admin][http][routes]")
{
    // The regression that matters most here: `/metrics` and `/healthz` are what
    // every existing scraper and probe is pointed at, and a route table that
    // shadowed or reordered them would be found in production rather than here.
    std::vector<Pace::AdminRoute> const routes {
        { .path = "/fleet", .handler = [](Pace::AdminRequest const&) { return Pace::AdminResponse {}; } },
    };

    CHECK(ExchangeWithRoutes("GET /healthz HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(ExchangeWithRoutes("GET /metrics HTTP/1.1\r\n\r\n", routes).contains("text/plain; version=0.0.4"));
    CHECK(ExchangeWithRoutes("GET /nope HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_CASE("AdminHttp: a route may refuse, and its extra headers reach the client", "[admin][http][routes]")
{
    // A 401 that does not carry `WWW-Authenticate` is one a browser cannot prompt
    // for, which turns "you need a credential" into "this page is broken".
    std::vector<Pace::AdminRoute> const routes {
        { .path = "/fleet",
          .handler =
              [](Pace::AdminRequest const&) {
                  return Pace::AdminResponse { .status = "401 Unauthorized",
                                               .contentType = "text/plain",
                                               .body = "credential required\n",
                                               .extraHeaders = { R"(WWW-Authenticate: Basic realm="fleet")" } };
              } },
    };

    auto const response = ExchangeWithRoutes("GET /fleet HTTP/1.1\r\n\r\n", routes);
    CHECK(response.starts_with("HTTP/1.1 401 Unauthorized\r\n"));
    CHECK(response.contains(R"(WWW-Authenticate: Basic realm="fleet")"));
    CHECK(response.contains("credential required\n"));
}

TEST_CASE("AdminHttp: a handler is told the Authorization header, whatever its case", "[admin][http][routes]")
{
    // Field names are case-insensitive and real clients disagree about the
    // spelling. A byte comparison would make the credential work for curl and
    // not for a proxy, which gets diagnosed as "the token is wrong".
    auto const echo = [](Pace::AdminRequest const& request) {
        return Pace::AdminResponse { .status = "200 OK",
                                     .contentType = "text/plain",
                                     .body = std::string { request.Header(Pace::AdminHeader::Authorization) } };
    };
    std::vector<Pace::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(
        ExchangeWithRoutes("GET /echo HTTP/1.1\r\nAuthorization: Bearer s3cret\r\n\r\n", routes).contains("Bearer s3cret"));
    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nauthorization:   Basic QUJD\r\n\r\n", routes).contains("Basic QUJD"));

    // No header at all is an empty view, not a missing one: a handler that reads
    // it before checking would otherwise be reading uninitialised storage.
    auto const none = ExchangeWithRoutes("GET /echo HTTP/1.1\r\nHost: x\r\n\r\n", routes);
    CHECK(none.contains("Content-Length: 0\r\n"));
}

TEST_CASE("AdminHttp: a query string reaches the handler and does not defeat routing", "[admin][http][routes]")
{
    auto const echo = [](Pace::AdminRequest const& request) {
        return Pace::AdminResponse { .status = "200 OK",
                                     .contentType = "text/plain",
                                     .body = std::string { request.query } };
    };
    std::vector<Pace::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(ExchangeWithRoutes("GET /echo?tier=disk HTTP/1.1\r\n\r\n", routes).contains("tier=disk"));
}

TEST_CASE("AdminHttp: every readable header arrives, in either order", "[admin][http][routes]")
{
    // The regression this exists for: the walk used to stop at the first header it
    // recognised. With one readable header that is correct; with two it means
    // whichever the client happened to send first is the only one that arrives, so
    // a conditional GET would silently stop revalidating whenever a browser put
    // `Authorization` above `If-None-Match`.
    auto const echo = [](Pace::AdminRequest const& request) {
        return Pace::AdminResponse { .status = "200 OK",
                                     .contentType = "text/plain",
                                     .body = std::format("[{}][{}]",
                                                         request.Header(Pace::AdminHeader::Authorization),
                                                         request.Header(Pace::AdminHeader::IfNoneMatch)) };
    };
    std::vector<Pace::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nAuthorization: Bearer t\r\nIf-None-Match: \"abc\"\r\n\r\n", routes)
              .contains("[Bearer t][\"abc\"]"));
    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nif-none-match: \"abc\"\r\nauthorization: Bearer t\r\n\r\n", routes)
              .contains("[Bearer t][\"abc\"]"));
}

TEST_CASE("AdminHttp: a 304 carries its validators and no content at all", "[admin][http][routes]")
{
    std::vector<Pace::AdminRoute> const routes {
        { .path = "/chart.svg",
          .handler =
              [](Pace::AdminRequest const& request) {
                  if (request.Header(Pace::AdminHeader::IfNoneMatch) == "\"g-7\"")
                      return Pace::AdminResponse { .status = "304 Not Modified",
                                                   .extraHeaders = { "ETag: \"g-7\"", "Cache-Control: max-age=42" } };
                  return Pace::AdminResponse { .status = "200 OK",
                                               .contentType = "image/svg+xml",
                                               .body = "<svg/>",
                                               .extraHeaders = { "ETag: \"g-7\"" } };
              } },
    };

    auto const fresh = ExchangeWithRoutes("GET /chart.svg HTTP/1.1\r\n\r\n", routes);
    CHECK(fresh.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(fresh.contains("Content-Type: image/svg+xml\r\n"));
    CHECK(fresh.ends_with("<svg/>"));

    auto const cached = ExchangeWithRoutes("GET /chart.svg HTTP/1.1\r\nIf-None-Match: \"g-7\"\r\n\r\n", routes);
    CHECK(cached.starts_with("HTTP/1.1 304 Not Modified\r\n"));
    // RFC 9110 SS15.4.5 forbids content on a 304, and SS8.6 forbids a Content-Length
    // that does not match what a 200 would have sent. A client that read one and
    // then found the connection closed would report a truncated response.
    CHECK_FALSE(cached.contains("Content-Length:"));
    CHECK_FALSE(cached.contains("Content-Type:"));
    // The validators are the whole point of the response: without them the client
    // has nothing to revalidate against next time.
    CHECK(cached.contains("ETag: \"g-7\"\r\n"));
    CHECK(cached.contains("Cache-Control: max-age=42\r\n"));
    CHECK(cached.ends_with("\r\n\r\n"));
}

TEST_CASE("AdminHttp: a prefix route answers its tail and never shadows an exact one", "[admin][http][routes]")
{
    auto const named = [](std::string_view what) {
        return [what](Pace::AdminRequest const& request) {
            return Pace::AdminResponse { .status = "200 OK",
                                         .contentType = "text/plain",
                                         .body = std::format("{}:{}", what, request.path) };
        };
    };
    // The prefix route is registered *first*, which is what makes this a test of the
    // table rather than of the caller's vector order.
    std::vector<Pace::AdminRoute> const routes {
        { .path = "/fleet/", .handler = named("prefix"), .match = Pace::AdminRouteMatch::Prefix },
        { .path = "/fleet", .handler = named("exact") },
    };

    CHECK(ExchangeWithRoutes("GET /fleet HTTP/1.1\r\n\r\n", routes).contains("exact:/fleet"));
    CHECK(ExchangeWithRoutes("GET /fleet/chart/refusals.svg HTTP/1.1\r\n\r\n", routes)
              .contains("prefix:/fleet/chart/refusals.svg"));
    // A prefix route is not a catch-all: a path that does not begin with it still
    // reaches the 404, so a typo is refused rather than quietly served.
    CHECK(ExchangeWithRoutes("GET /fleetx HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 404 Not Found\r\n"));
}

// ---------------------------------------------------------------------------
// #824: four outcomes behind one `bool ok`, all of them answered `400`.
//
// The pair that matters is the first two against the third. A test that only
// asserted "an idle connection is not answered 400" would pass just as well on a
// server that had stopped answering 400 to *anything*, which would delete the one
// case where 400 is the right answer. Silence and the refusal are asserted against
// each other, or one passing test covers both and proves neither.
// ---------------------------------------------------------------------------

TEST_CASE("AdminHttp: a peer that sent nothing before its deadline is closed, not refused", "[metrics][http][admin][idle]")
{
    // The reported bug, at the seam: Chrome opens a preconnect socket, sends
    // nothing, and the request deadline expires. Nothing was asked, so nothing is
    // owed -- and the 400 this used to answer is what reached a real browser as a
    // broken dashboard.
    auto const code = GENERATE(core::net::NetErrorCode::WouldBlock, core::net::NetErrorCode::Timeout);
    INFO("read failed with NetErrorCode " << static_cast<unsigned>(code));

    auto pair = core::net::testing::InMemorySocketPair::create();
    // Reads fail; writes still go through. So an empty response is the server
    // declining to answer, never the server failing to.
    Pace::Testing::FailingReadSocket stalled { *pair.server, code };

    Pace::AtomicMetricsSink metrics;
    REQUIRE(ServeAndCollect(&stalled, pair, metrics, {}).empty());
}

TEST_CASE("AdminHttp: a peer that closed without sending is closed, not refused", "[metrics][http][admin][idle]")
{
    // The other silence: an abandoned preconnect, a TCP liveness probe, a port
    // scan. `ExchangeMaybeSilent` half-closes with nothing written, so the first read
    // is EOF rather than an error -- the state `FailingReadSocket` cannot
    // produce and the one the case above must not be mistaken for.
    REQUIRE(ExchangeMaybeSilent({}).empty());
}

TEST_CASE("AdminHttp: a head cut off by the deadline is refused 408, not served truncated", "[metrics][http][admin][idle]")
{
    // The second route to the defect the 431 closes, and the more reachable one: a
    // prefix whose request line has arrived parses and routes, so the head is served
    // with every field that had not arrived yet silently missing. `RequestTimeout` is
    // per READ and the head has no total budget, so this needs no oversize client --
    // just a peer that stops mid-head.
    //
    // The credential is what makes it visible rather than merely wrong: served
    // truncated, this request is answered as though the client sent no
    // `Authorization` at all.
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(WriteString(pair.client.get(), "GET /healthz HTTP/1.1\r\nAuthorization: Bearer x")));
    // Deliberately NO ShutdownWrite: EOF would mean the peer had FINISHED sending,
    // which is a different fact and is still served on purpose. This peer has not
    // finished; this server gave up on it.
    Pace::Testing::FailingReadSocket stalled { *pair.server, core::net::NetErrorCode::WouldBlock, 1 };

    Pace::AtomicMetricsSink metrics;
    auto const response = ServeAndCollect(&stalled, pair, metrics, {});
    INFO("response was: " << response.substr(0, 64));
    REQUIRE(response.starts_with("HTTP/1.1 408 Request Timeout\r\n"));
}

TEST_CASE("AdminHttp: a silent peer gets the preconnect budget, and a started head gets the short one",
          "[admin][http][timeout]")
{
    // **#828: two questions were sharing one number.** `RequestTimeout` (2 s) is the
    // right answer to "how long may a peer take once it has started" and the wrong one
    // to "is this peer going to ask anything" -- a browser preconnect holds a socket far
    // longer, which is what made an ordinary navigation fail.
    //
    // Asserted on WHICH deadline is armed and WHEN, because that is the whole change.
    // A test that only checked "a request is served" passes on the broken tree too.
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(WriteString(pair.client.get(), "GET /healthz HTTP/1.1\r\n\r\n")));
    // Behind a complete head, so the server answers before it could reach it. It is here
    // for the close: a served connection listens until its peer closes, and this
    // in-process pair has no clock to end the listening the way a real deadline does.
    REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
    DeadlineRecordingSocket recorder { *pair.server };

    Pace::AtomicMetricsSink metrics;
    auto const response = ServeAndCollect(&recorder, pair, metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));

    REQUIRE(recorder.armed.size() >= 2);
    // The long budget is armed BEFORE the first read: a peer that has said nothing is a
    // preconnect, not a slow request.
    CHECK(recorder.armed.front() == Pace::AdminHttpServer::FirstByteTimeout);
    // And it is tightened the moment a byte arrives, so the slowloris bound is not
    // slackened in exchange -- which is the trade the ticket explicitly refuses.
    CHECK(recorder.armed[1] == Pace::AdminHttpServer::RequestTimeout);
    CHECK(Pace::AdminHttpServer::FirstByteTimeout > Pace::AdminHttpServer::RequestTimeout);
}

TEST_CASE("AdminHttp: a head that dribbles past its total budget is refused 408", "[admin][http][timeout]")
{
    // `RequestTimeout` is per READ, so a client sending one byte under each deadline
    // never trips it and holds a slot for `MaxRequestBytes` reads. The total is the only
    // bound that can see that, and it needs a clock -- so the clock is injected and moved
    // here rather than waited on.
    //
    // The control is the case above: an ordinary request served under the same total
    // must stay 200, or "enforces a total" and "refuses every request" are one passing
    // test.
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(WriteString(pair.client.get(), "GET /healthz HTTP/1.1\r\nX: ")));
    // The half-close sits BEHIND bytes the server never reaches before its budget runs
    // out, so it cannot inform the refusal. It is here for the close that follows: the
    // server listens to a refused peer until that peer closes, and a real socket stops
    // listening at a deadline this in-process pair has no clock for.
    REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
    core::platform::ManualClock clock;
    // Each read costs time, which is what a dribbling client actually does -- the fake
    // advances the clock rather than the test doing it, because the reads happen inside
    // one `ServeAdminHttp` call and there is no seam between them.
    ClockAdvancingSocket dribble { *pair.server, 4, clock, Pace::AdminHttpServer::RequestTimeout };

    using namespace std::chrono_literals;
    Pace::AtomicMetricsSink metrics;
    auto provider = [] {
        return Pace::MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Pace::Uptime { 1s } };
    };
    core::async::syncRun(Pace::ServeAdminHttp(&dribble, &metrics, provider, &clock));
    CloseAsServed(&dribble);
    auto const response = core::async::syncRun(ReadAvailable(pair.client.get()));
    CHECK(response.starts_with("HTTP/1.1 408 Request Timeout\r\n"));
}

TEST_CASE("AdminHttp: an UNFINISHED head is refused 400 even though the peer finished sending",
          "[metrics][http][admin][idle]")
{
    // **This case used to assert the opposite, and the assertion was the bug.** It
    // read the EOF rule as "the peer finished sending, so serve what arrived" and
    // called itself a control. But `.agent/rules/wire-and-protocol.md` says a server
    // answers what is already DETERMINED, and a head with no terminating blank line
    // determines nothing -- this server does not know which headers the peer would
    // have sent, so there is no reply it can be right about.
    //
    // What it cost: `/fleet` routed with `Authorization` never read, so a valid
    // credential was answered `401`. Same wrong answer as the byte-cap case, reached
    // with a thirty-byte request and no oversize client.
    //
    // `PING` half-closed is a complete command and is answered; this is a sentence
    // cut off mid-word.
    REQUIRE(ExchangeMaybeSilent("GET /healthz HTTP/1.1\r\nHost: x\r\n").starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_CASE("AdminHttp: a COMPLETE head half-closed after is still served", "[metrics][http][admin][idle]")
{
    // The real control, and the line the pair sits either side of: what makes an
    // unfinished head refusable is that it is unfinished, NOT that the peer went
    // away. Every other case in this file half-closes too, so without stating it
    // here the distinction is only implicit -- and "refuse an unfinished head" and
    // "refuse every peer that half-closes" would be one passing test, which would
    // break every probe and scraper on this endpoint.
    REQUIRE(ExchangeMaybeSilent("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("AdminHttp: a malformed request is still refused 400", "[metrics][http][admin][idle]")
{
    // The control, and the half that was never covered at all: before #824 this
    // file had sixteen cases and not one of them asserted a 400, so the outcome
    // that legitimately deserves one was as untested as the three that did not.
    auto const response = ExchangeMaybeSilent("nonsense\r\n\r\n");
    REQUIRE(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_CASE("AdminHttp: a head that overruns the byte cap is refused 431, not served truncated",
          "[metrics][http][admin][idle]")
{
    // Not a missing refusal but a wrong answer: the read loop stopped at the cap
    // and the *prefix* was then parsed and routed, so every field past it vanished
    // and a browser whose `Authorization` fell beyond the cap was told `401`.
    //
    // The padding sits between the request line and the credential, so the request
    // line -- which is at the front and always survives truncation -- still parses.
    // That is what makes this case fail under the old code by being *served* rather
    // than by being unreadable.
    std::string request = "GET /healthz HTTP/1.1\r\nHost: x\r\n";
    request += "X-Pad: " + std::string(9000, 'p') + "\r\n";
    request += "\r\n";
    auto const response = ExchangeMaybeSilent(request);
    INFO("response was: " << response.substr(0, 64));
    REQUIRE(response.starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST_CASE("AdminHttp: a served connection's refusal over a head it did not finish reading reaches the client",
          "[metrics][http][admin][linger]")
{
    // #1554. The case above asserts the 431 through `CloseAsServed`, a fixture's copy of
    // how a served connection closes; this one asserts production's own, through
    // `AdminHttpServer::Run`. The head runs past the cap, so the surface refuses with the
    // rest unread, and a bare close over unread bytes is a reset that destroys the
    // refusal before a Windows client reads it. The served connection lingers instead,
    // until this client closes.
    core::net::testing::InMemoryListener listener;
    Pace::AtomicMetricsSink metrics;
    Pace::NullLogger logger;
    core::platform::SteadyClock clock;
    core::net::AcceptLoopHealth acceptLoops;
    Pace::AdminHttpServer server { listener,    NothingToCancelHere(),
                                   metrics,     [] { return Pace::MetricsSnapshot {}; },
                                   logger,      clock,
                                   acceptLoops, Pace::DefaultDrainWait() };

    auto client = listener.connectClient();
    std::string request = "GET /healthz HTTP/1.1\r\nHost: x\r\n";
    request += "X-Pad: " + std::string(9000, 'p') + "\r\n";
    request += "\r\n";
    REQUIRE(core::async::syncRun(WriteString(client.get(), request)));
    REQUIRE(Pace::Testing::ShutdownWrite(*client).has_value());
    // Queued connections are accepted before the closed listener ends the loop.
    listener.close();
    core::async::syncRun(server.Run());
    server.Shutdown();

    auto const response = core::async::syncRun(ReadAvailable(client.get()));
    INFO("response was: " << response.substr(0, 64));
    REQUIRE(response.starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST_CASE("AdminHttp: a large but complete head under the cap still routes", "[metrics][http][admin][idle]")
{
    // The control for the case above. Without it, "refuse an oversize head" and
    // "refuse a large head" are the same passing test, and the cap could drift down
    // to nothing with every assertion still green.
    std::string request = "GET /healthz HTTP/1.1\r\nHost: x\r\n";
    request += "X-Pad: " + std::string(4000, 'p') + "\r\n";
    request += "\r\n";
    REQUIRE(ExchangeMaybeSilent(request).starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("A tolerated admin bind failure is reported at Warn and says the daemon carries on",
          "[server][admin][bind-verdict]")
{
    // #603. `pace-kv` serves the cache on without an admin endpoint -- that is a
    // policy, not an accident -- and it reported the condition at `Error`, which is
    // what a site pages on. Two costs pointing opposite ways: a page for a working
    // daemon, and an operator trained to ignore the level.
    //
    // The message is asserted alongside the level because half of the acceptance is
    // that a reader should not have to INFER whether the process survived.
    auto const report = Pace::DescribeToleratedAdminBindFailure("0.0.0.0", 9259, "address already in use");

    CHECK(report.level == Pace::LogLevel::Warn);
    CHECK(report.level != Pace::LogLevel::Error);
    CHECK(report.message.contains("0.0.0.0:9259"));
    CHECK(report.message.contains("address already in use"));
    CHECK(report.message.contains("continuing without"));
    // Naming the surfaces, because "the metrics endpoint" is not what an operator
    // has a probe pointed at -- `/healthz` is, and it goes away too.
    CHECK(report.message.contains("/metrics"));
    CHECK(report.message.contains("/healthz"));
}

TEST_CASE("The tolerated admin verdict is not the verdict for a bind that stops the process",
          "[server][admin][bind-verdict]")
{
    // The half #603 explicitly asks for: whatever asserts the severity has to
    // DISTINGUISH the tolerated site from the fatal ones, or somebody "fixes" all
    // four to one level and the information is gone.
    //
    // The fatal side is asserted where it lives, against the real listener paths, by
    // `ReactorServerLoop_test`'s "reports a bind it cannot make at Error and refuses
    // to start" -- those return EXIT_FAILURE, so `Error` is correct there and this
    // function must never be reached for them. What is left to pin here is that the
    // two verdicts do not collapse into one level.
    auto const tolerated = Pace::DescribeToleratedAdminBindFailure("192.0.2.1", 9259, "cannot assign requested address");

    CHECK(tolerated.level == Pace::LogLevel::Warn);
    // A tolerated failure must not describe itself as the process stopping. Refusal
    // vocabulary here would read as fatal whatever the level said, which is the same
    // defect one layer up.
    CHECK_FALSE(tolerated.message.contains("refusing to start"));
    CHECK_FALSE(tolerated.message.contains("exiting"));
}

namespace
{

/// One `GET /healthz` through a whole `AdminHttpServer::Run`, over @p listener, answered from
/// @p acceptLoops.
/// @param inner Where the client connects; @p listener wraps it or is it.
/// @param listener What the server accepts from.
/// @param acceptLoops The registry `/healthz` answers from.
/// @param logger Where the server logs.
/// @return The raw response.
[[nodiscard]] std::string HealthzThroughRun(core::net::testing::InMemoryListener& inner,
                                            core::net::IListener& listener,
                                            core::net::AcceptLoopHealth& acceptLoops,
                                            Pace::ILogger& logger)
{
    Pace::AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    Pace::AdminHttpServer server { listener,    NothingToCancelHere(),
                                   metrics,     [] { return Pace::MetricsSnapshot {}; },
                                   logger,      clock,
                                   acceptLoops, Pace::DefaultDrainWait() };
    auto client = inner.connectClient();
    REQUIRE(core::async::syncRun(WriteString(client.get(), "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")));
    REQUIRE(Pace::Testing::ShutdownWrite(*client).has_value());
    // Queued connections are accepted before the closed listener ends the loop.
    inner.close();
    core::async::syncRun(server.Run());
    server.Shutdown();
    return core::async::syncRun(ReadAvailable(client.get()));
}

} // namespace

TEST_CASE("AdminHttp: /healthz answers 503 naming a surface that stopped accepting", "[metrics][http][admin][accept-loop]")
{
    // It answered `200` for nine hours over a node whose 0xFC surface listened and refused every
    // connect. A liveness probe that asks nothing is the confident wrong signal; this one asks the
    // accept-loop registry, and the control beside it keeps "always 503" from passing too.
    Pace::NullLogger logger;
    SECTION("while every surface accepts, 200")
    {
        core::net::testing::InMemoryListener listener;
        core::net::AcceptLoopHealth acceptLoops;
        CHECK(HealthzThroughRun(listener, listener, acceptLoops, logger).starts_with("HTTP/1.1 200 OK\r\n"));
    }
    SECTION("once one has stopped, 503 and which")
    {
        core::net::testing::InMemoryListener listener;
        core::net::AcceptLoopHealth acceptLoops;
        acceptLoops.record(core::net::AcceptLoopEvent { .surface = "node",
                                                        .line = "connection reset (AcceptEx) [errno 10054]",
                                                        .error = {},
                                                        .kind = core::net::AcceptLoopEventKind::GaveUp });
        auto const response = HealthzThroughRun(listener, listener, acceptLoops, logger);
        INFO("response was: " << response);
        CHECK(response.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
        CHECK(response.contains("surface node is not accepting connections: connection reset (AcceptEx)"));
    }
    SECTION("while one is degraded, 503 naming it as degraded rather than stopped")
    {
        // A loop backing off on failures nothing classifies accepts little or nothing: a probe
        // that answered 200 over it would be the confident wrong signal again, one step removed.
        core::net::testing::InMemoryListener listener;
        core::net::AcceptLoopHealth acceptLoops;
        acceptLoops.record(core::net::AcceptLoopEvent { .surface = "node",
                                                        .line = "32 accepts in a row failed",
                                                        .error = {},
                                                        .kind = core::net::AcceptLoopEventKind::Degraded });
        auto const response = HealthzThroughRun(listener, listener, acceptLoops, logger);
        INFO("response was: " << response);
        CHECK(response.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
        CHECK(response.contains("surface node is degraded, backing off on failed accepts: 32 accepts in a row failed"));
        CHECK_FALSE(response.contains("is not accepting connections"));
    }
    SECTION("once a degraded one recovers, 200 again")
    {
        core::net::testing::InMemoryListener listener;
        core::net::AcceptLoopHealth acceptLoops;
        for (auto const kind: { core::net::AcceptLoopEventKind::Degraded, core::net::AcceptLoopEventKind::Recovered })
            acceptLoops.record(core::net::AcceptLoopEvent { .surface = "node", .line = "x", .error = {}, .kind = kind });
        CHECK(HealthzThroughRun(listener, listener, acceptLoops, logger).starts_with("HTTP/1.1 200 OK\r\n"));
    }
}

TEST_CASE("AdminHttp: a connection a peer reset before it was accepted does not stop /healthz",
          "[metrics][http][admin][accept-loop]")
{
    // The admin loop used to end on anything but a poll tick, taking `/healthz` and `/metrics` with
    // it. The request queued behind the failure is the whole assertion.
    Pace::CapturingLogger logger { Pace::LogLevel::Debug };
    core::net::testing::InMemoryListener inner;
    core::net::testing::FailingListener listener {
        inner, core::net::testing::repeatedFailures(core::net::NetErrorCode::ConnReset, 1)
    };
    core::net::AcceptLoopHealth acceptLoops;

    auto const response = HealthzThroughRun(inner, listener, acceptLoops, logger);
    INFO("response was: " << response.substr(0, 64));
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(listener.failuresAnswered() == 1);
    CHECK(std::ranges::count_if(logger.Snapshot(),
                                [](Pace::CapturingLogger::Record const& record) {
                                    return record.level == Pace::LogLevel::Warn
                                           && record.message.contains("admin: an accept failed");
                                })
          == 1);
}

TEST_CASE("AdminHttp: signals landing on the accept thread do not stop /healthz",
          "[metrics][http][admin][accept-loop][signal]")
{
    // Measured on both binaries in WSL: one SIGHUP delivered to the admin thread logged "admin:
    // accept loop ended (cancelled (poll) [errno 4])" and `/healthz` went dead while the process
    // lived, because the listener read the interrupted poll as a closed socket. This drives the
    // loop production runs -- `Run()` over a real `BlockingListener` armed the way both binaries
    // arm it -- on a thread of the case's own, signals that thread, and then asks `/healthz`.
#if defined(_WIN32)
    SKIP("no POSIX signal reaches a Windows thread");
#else
    using namespace std::chrono_literals;
    Pace::Testing::ScopedSignalHandler const handler { SIGUSR2 };
    REQUIRE(handler.Installed());
    auto listener = Pace::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        SKIP("this platform would not bind a loopback listener");
    listener->SetTimeouts(Pace::AdminHttpServer::AcceptPoll, Pace::AdminHttpServer::RequestTimeout);
    auto const port = listener->boundPort();

    Pace::CapturingLogger logger { Pace::LogLevel::Debug };
    Pace::AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    core::net::AcceptLoopHealth acceptLoops;
    Pace::AdminHttpServer server { *listener,   listener->Connections(),
                                   metrics,     [] { return Pace::MetricsSnapshot {}; },
                                   logger,      clock,
                                   acceptLoops, Pace::DefaultDrainWait() };
    std::atomic<bool> ended { false };
    std::jthread acceptThread { [&server, &ended] {
        core::async::syncRun(server.Run());
        ended.store(true, std::memory_order_release);
    } };
    // Declared after the thread, so it runs first: the loop is asked to stop before the thread is
    // joined, and a failed assertion below cannot leave the join waiting on a loop nobody stopped.
    auto const requestStop = [](Pace::AdminHttpServer* admin) {
        admin->RequestStop();
    };
    std::unique_ptr<Pace::AdminHttpServer, decltype(requestStop)> const stopFirst { &server, requestStop };

    auto const native = acceptThread.native_handle();
    std::size_t signalled = 0;
    // Two hundred signals across several of the loop's 500 ms polls: far more than one lands inside
    // a parked poll, which is all the old row needed to end the loop.
    static constexpr std::size_t signalCount = 200;
    auto const sent = Pace::Testing::WaitUntil(
        "every signal to be sent to the admin accept thread",
        [&signalled] { return signalled >= signalCount; },
        [&signalled] { return std::format("{} of {} signal(s) sent", signalled, signalCount); },
        Pace::Testing::WaitOptions { .step =
                                         [&handler, native, &signalled] {
                                             handler.Interrupt(native);
                                             ++signalled;
                                         },
                                     .context = {},
                                     .bound = Pace::Testing::WaitHangGuard,
                                     .rest = 5ms });
    REQUIRE(sent);

    // The loop reported nothing -- the registry `/healthz` answers from is empty -- and is still
    // running. **At most THREE assertions can fail in this case, and that was load-bearing**: on
    // Catch2 3.6.0 ctest scored an exit of exactly 4 as Skipped (#1128), and the regression this case
    // exists for failed four when "still running" was asserted twice, reading as a skip under ctest.
    CHECK(acceptLoops.snapshot().empty());
    CHECK(std::ranges::none_of(logger.Snapshot(), [](Pace::CapturingLogger::Record const& record) {
        return record.level == Pace::LogLevel::Error;
    }));
    // Asked only of a loop still accepting: a connection to a loop that ended would sit in the
    // backlog and its read would wait for an answer nobody is left to write.
    REQUIRE_FALSE(ended.load(std::memory_order_acquire));
    core::net::BlockingConnector connector;
    auto client =
        core::async::syncRun(connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
    REQUIRE(client.has_value());
    REQUIRE(core::async::syncRun(WriteString(client->get(), "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")));
    auto const response = core::async::syncRun(ReadAvailable(client->get()));
    INFO("response was: " << response.substr(0, 64));
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
#endif
}

namespace
{

/// A route that records what its handler was shown and answers `200 OK`.
struct BodySeen
{
    int calls { 0 };
    Pace::AdminMethod method { Pace::AdminMethod::Get };
    std::string body;
    std::string peerHost;
};

/// @param seen Where the handler records the request it was given.
/// @param path The route's path.
/// @param maxBodyBytes The route's body ceiling.
/// @return A POST route that records into `seen`.
Pace::AdminRoute RecordingPostRoute(std::shared_ptr<BodySeen> seen, std::string_view path, std::size_t maxBodyBytes)
{
    return Pace::AdminRoute {
        .path = path,
        .handler =
            [seen = std::move(seen)](Pace::AdminRequest const& request) {
                ++seen->calls;
                seen->method = request.method;
                seen->body = std::string { request.body };
                seen->peerHost = request.peerHost;
                return Pace::AdminResponse { .status = "200 OK", .body = "done" };
            },
        .method = Pace::AdminMethod::Post,
        .maxBodyBytes = maxBodyBytes,
    };
}

Pace::AdminRoute GetRoute(std::string_view path)
{
    return Pace::AdminRoute { .path = path, .handler = [](Pace::AdminRequest const&) {
                                 return Pace::AdminResponse { .status = "200 OK", .body = "got" };
                             } };
}

/// A socket reporting a fixed peer, as the accepted socket of a real listener does.
class PeerSocket final: public core::net::testing::SocketDecorator
{
  public:
    PeerSocket(core::net::ISocket& inner, std::string peer) noexcept:
        SocketDecorator { inner },
        _peer { std::move(peer) }
    {
    }

    [[nodiscard]] std::string peerAddress() const override
    {
        return _peer;
    }

  private:
    std::string _peer;
};

/// Hands up the head and one body byte in the first read, then one byte per read, each costing
/// the clock `perRead`: a body that arrives, but too slowly.
class BodyDribbleSocket final: public core::net::testing::SocketDecorator
{
  public:
    BodyDribbleSocket(core::net::ISocket& inner,
                      std::size_t firstRead,
                      core::platform::ManualClock& clock,
                      std::chrono::milliseconds perRead) noexcept:
        SocketDecorator { inner },
        _firstRead { firstRead },
        _clock { clock },
        _perRead { perRead }
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        auto const limit = _first ? _firstRead : std::size_t { 1 };
        if (!_first)
            _clock.advance(_perRead);
        _first = false;
        return SocketDecorator::read(buffer.subspan(0, std::min(buffer.size(), limit)));
    }

  private:
    bool _first { true };
    std::size_t _firstRead;
    core::platform::ManualClock& _clock;
    std::chrono::milliseconds _perRead;
};

} // namespace

TEST_CASE("admin POST route receives its body", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 16) };
    auto const response =
        ExchangeWithRoutes("POST /x HTTP/1.1\r\nContent-Length: 5\r\nContent-Type: application/json\r\n\r\nhello", routes);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(seen->calls == 1);
    CHECK(seen->method == Pace::AdminMethod::Post);
    CHECK(seen->body == "hello");
}

TEST_CASE("admin POST body that straddles the head read is reassembled", "[admin][http][post]")
{
    // The head and the first body bytes arrive together; the rest arrives later.
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 4096) };
    std::string const payload(3000, 'z');
    auto const request = std::format("POST /x HTTP/1.1\r\nContent-Length: {}\r\n\r\n{}", payload.size(), payload);
    auto const response = ExchangeWithRoutes(request, routes);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(seen->body == payload);
}

TEST_CASE("admin body over the route ceiling is 413 and the server keeps serving", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 16), GetRoute("/y") };
    auto const refused = ExchangeWithRoutes("POST /x HTTP/1.1\r\nContent-Length: 17\r\n\r\n01234567890123456", routes);
    CHECK(refused.starts_with("HTTP/1.1 413 Content Too Large\r\n"));
    CHECK(seen->calls == 0);
    // The control: exactly the ceiling is served, so "refuses over" and "refuses all" differ.
    auto const atCeiling = ExchangeWithRoutes("POST /x HTTP/1.1\r\nContent-Length: 16\r\n\r\n0123456789012345", routes);
    CHECK(atCeiling.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(seen->body.size() == 16);
    CHECK(ExchangeWithRoutes("GET /y HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("admin declared length is judged before the body is read", "[admin][http][post]")
{
    // The body never arrives, yet the answer is 413 rather than a wait: the ceiling is checked on
    // the declared length. Content-Length is also saturating, not overflowing.
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(std::make_shared<BodySeen>(), "/x", 16) };
    for (std::string_view const length: { "17", "99999999999999999999999999", "18446744073709551616" })
    {
        auto const response =
            ExchangeWithRoutes(std::format("POST /x HTTP/1.1\r\nContent-Length: {}\r\n\r\n", length), routes);
        INFO(length);
        CHECK(response.starts_with("HTTP/1.1 413 Content Too Large\r\n"));
    }
}

TEST_CASE("admin a bodyless route refuses a body", "[admin][http][post]")
{
    std::vector<Pace::AdminRoute> const routes { GetRoute("/y") };
    CHECK(ExchangeWithRoutes("GET /y HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc", routes)
              .starts_with("HTTP/1.1 413 Content Too Large\r\n"));
    CHECK(ExchangeWithRoutes("GET /y HTTP/1.1\r\nContent-Length: 0\r\n\r\n", routes).starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("admin POST without Content-Length is 411", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 16) };
    CHECK(ExchangeWithRoutes("POST /x HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 411 Length Required\r\n"));
    CHECK(seen->calls == 0);
}

TEST_CASE("admin malformed Content-Length is 400", "[admin][http][post]")
{
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(std::make_shared<BodySeen>(), "/x", 16) };
    for (std::string_view const length: { "", "-1", "+4", "0x10", "4 4", "abc", "4,4" })
    {
        auto const response =
            ExchangeWithRoutes(std::format("POST /x HTTP/1.1\r\nContent-Length:{}\r\n\r\nabcd", length), routes);
        INFO("length=" << length);
        CHECK(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    }
    // Two framing headers are how one message becomes two; refused even when they agree.
    CHECK(ExchangeWithRoutes("POST /x HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\nabcd", routes)
              .starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_CASE("admin chunked transfer-encoding is refused 501", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 64), GetRoute("/y") };
    CHECK(ExchangeWithRoutes("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", routes)
              .starts_with("HTTP/1.1 501 Not Implemented\r\n"));
    // Alongside a Content-Length it is the smuggling shape; still refused, not resolved.
    CHECK(ExchangeWithRoutes("POST /x HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello", routes)
              .starts_with("HTTP/1.1 501 Not Implemented\r\n"));
    CHECK(ExchangeWithRoutes("GET /y HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", routes)
              .starts_with("HTTP/1.1 501 Not Implemented\r\n"));
    CHECK(seen->calls == 0);
}

TEST_CASE("admin GET on a POST-only path is 405 with Allow: POST", "[admin][http][post]")
{
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(std::make_shared<BodySeen>(), "/x", 16),
                                                 GetRoute("/both") };
    auto const response = ExchangeWithRoutes("GET /x HTTP/1.1\r\n\r\n", routes);
    CHECK(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(response.contains("Allow: POST\r\n"));
    // Both methods registered on one path are both advertised.
    std::vector<Pace::AdminRoute> const both { GetRoute("/b"), RecordingPostRoute(std::make_shared<BodySeen>(), "/b", 4) };
    CHECK(ExchangeWithRoutes("DELETE /b HTTP/1.1\r\n\r\n", both).contains("Allow: GET, POST\r\n"));
    // The built-ins stay GET, and an unknown path is not a method problem.
    CHECK(ExchangeWithRoutes("POST /healthz HTTP/1.1\r\nContent-Length: 0\r\n\r\n", routes).contains("Allow: GET\r\n"));
    CHECK(ExchangeWithRoutes("POST /nope HTTP/1.1\r\nContent-Length: 0\r\n\r\n", routes)
              .starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_CASE("admin body arriving slower than RequestTimeout is abandoned", "[admin][http][post][timeout]")
{
    using namespace std::chrono_literals;
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 64) };
    std::string const head = "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\n";
    std::string const request = head + "0123456789";

    auto const serve = [&](std::chrono::milliseconds perRead) {
        auto pair = core::net::testing::InMemorySocketPair::create();
        REQUIRE(core::async::syncRun(WriteString(pair.client.get(), request)));
        REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
        core::platform::ManualClock clock;
        BodyDribbleSocket dribble { *pair.server, head.size() + 1, clock, perRead };
        Pace::AtomicMetricsSink metrics;
        auto provider = [] {
            return Pace::MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Pace::Uptime { 1s } };
        };
        core::async::syncRun(Pace::ServeAdminHttp(&dribble, &metrics, provider, &clock, routes));
        // Closed on the pipe under the decorator: the decorator hands up ONE byte per read, which
        // would end the linger with the abandoned body still unread -- a reset that destroys the
        // 408 in this pipe's Windows-style model, an artefact of the dribble and not of the server.
        CloseAsServed(pair.server.get());
        return core::async::syncRun(ReadAvailable(pair.client.get()));
    };

    // Nine more bytes at 1 s each: past RequestTimeout (2 s) long before the body completes.
    auto const slow = serve(1s);
    INFO(slow);
    CHECK(slow.starts_with("HTTP/1.1 408 Request Timeout\r\n"));
    CHECK(seen->calls == 0);
    // The control: the same bytes with a free clock are served, so the 408 is the clock's.
    CHECK(serve(0ms).starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(seen->body == "0123456789");
}

TEST_CASE("admin body cut short by the peer is 400, not served", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 64) };
    CHECK(ExchangeWithRoutes("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", routes)
              .starts_with("HTTP/1.1 400 Bad Request\r\n"));
    CHECK(seen->calls == 0);
}

TEST_CASE("admin pipelined bytes after the body are ignored, not executed", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    auto const other = std::make_shared<BodySeen>();
    std::vector<Pace::AdminRoute> const routes { RecordingPostRoute(seen, "/x", 16),
                                                 RecordingPostRoute(other, "/other", 16) };
    auto const response = ExchangeWithRoutes(
        "POST /x HTTP/1.1\r\nContent-Length: 2\r\n\r\nhiPOST /other HTTP/1.1\r\nContent-Length: 1\r\n\r\nz", routes);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(seen->body == "hi");
    CHECK(other->calls == 0);
    // Exactly one response was written.
    CHECK(response.find("HTTP/1.1", 1) == std::string::npos);
}

TEST_CASE("admin handler receives the peer host and the MCP headers", "[admin][http][post]")
{
    auto const seen = std::make_shared<BodySeen>();
    std::string header;
    std::vector<Pace::AdminRoute> routes { RecordingPostRoute(seen, "/x", 16) };
    routes.front().handler = [&header, inner = routes.front().handler](Pace::AdminRequest const& request) {
        header = std::format("{}|{}|{}|{}|{}|{}",
                             request.Header(Pace::AdminHeader::ContentType),
                             request.Header(Pace::AdminHeader::Origin),
                             request.Header(Pace::AdminHeader::Accept),
                             request.Header(Pace::AdminHeader::McpProtocolVersion),
                             request.Header(Pace::AdminHeader::McpMethod),
                             request.Header(Pace::AdminHeader::McpName));
        return inner(request);
    };
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(WriteString(
        pair.client.get(),
        "POST /x HTTP/1.1\r\nContent-Length: 1\r\ncontent-type: application/json\r\nORIGIN: http://localhost\r\n"
        "Accept: text/event-stream\r\nMCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: t\r\n\r\n{")));
    REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
    PeerSocket peer { *pair.server, "203.0.113.9" };
    Pace::AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    auto provider = [] {
        return Pace::MetricsSnapshot {};
    };
    core::async::syncRun(Pace::ServeAdminHttp(&peer, &metrics, provider, &clock, routes));
    CloseAsServed(&peer);
    CHECK(seen->peerHost == "203.0.113.9");
    CHECK(header == "application/json|http://localhost|text/event-stream|2026-07-28|tools/call|t");
}

TEST_CASE("admin a handler that throws costs a 500, not the process", "[admin][http][post]")
{
    std::vector<Pace::AdminRoute> routes { GetRoute("/boom") };
    routes.front().handler = [](Pace::AdminRequest const&) -> Pace::AdminResponse {
        throw std::runtime_error { "x" };
    };
    CHECK(ExchangeWithRoutes("GET /boom HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
}

namespace
{

/// One request through a whole `AdminHttpServer::Run` shaped by @p profile, over an in-memory listener.
/// @param request The raw request.
/// @param routes The caller routes.
/// @param profile The instance's shape.
/// @param logger Where the server logs.
/// @return The raw response.
[[nodiscard]] std::string ExchangeThroughProfile(std::string_view request,
                                                 std::vector<Pace::AdminRoute> routes,
                                                 Pace::AdminServerProfile profile,
                                                 Pace::ILogger& logger)
{
    core::net::testing::InMemoryListener listener;
    Pace::AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    core::net::AcceptLoopHealth acceptLoops;
    Pace::AdminHttpServer server { listener,
                                   NothingToCancelHere(),
                                   metrics,
                                   [] { return Pace::MetricsSnapshot {}; },
                                   logger,
                                   clock,
                                   acceptLoops,
                                   Pace::DefaultDrainWait(),
                                   std::move(routes),
                                   nullptr,
                                   Pace::EverySurface,
                                   profile };
    auto client = listener.connectClient();
    REQUIRE(core::async::syncRun(WriteString(client.get(), request)));
    REQUIRE(Pace::Testing::ShutdownWrite(*client).has_value());
    listener.close();
    core::async::syncRun(server.Run());
    server.Shutdown();
    return core::async::syncRun(ReadAvailable(client.get()));
}

} // namespace

TEST_CASE("admin a profile without built-in routes serves only its own", "[admin][http][profile]")
{
    // A second surface on this HTTP code must not grow a scrape endpoint by reuse: with the built-ins
    // off, `/metrics` and `/healthz` are paths like any other, and the caller's route still answers.
    Pace::NullLogger logger;
    Pace::AdminServerProfile const bare { .surface = "probe", .builtIns = Pace::AdminBuiltInRoutes::None };
    CHECK(ExchangeThroughProfile("GET /metrics HTTP/1.1\r\n\r\n", { GetRoute("/x") }, bare, logger)
              .starts_with("HTTP/1.1 404 Not Found\r\n"));
    CHECK(ExchangeThroughProfile("GET /healthz HTTP/1.1\r\n\r\n", { GetRoute("/x") }, bare, logger)
              .starts_with("HTTP/1.1 404 Not Found\r\n"));
    CHECK(ExchangeThroughProfile("GET /x HTTP/1.1\r\n\r\n", { GetRoute("/x") }, bare, logger)
              .starts_with("HTTP/1.1 200 OK\r\n"));

    SECTION("and the default profile keeps them: the control")
    {
        CHECK(ExchangeThroughProfile("GET /healthz HTTP/1.1\r\n\r\n", { GetRoute("/x") }, {}, logger)
                  .starts_with("HTTP/1.1 200 OK\r\n"));
    }
}

TEST_CASE("admin a profile's request cap and surface name are the ones the loop uses", "[admin][http][profile]")
{
    // A cap of zero refuses every connection, which is the one arrangement that shows the loop reads
    // the profile's figure rather than the admin surface's thirty-two -- and names the profile's
    // surface while doing it.
    Pace::NullLogger logger;
    auto const refused = ExchangeThroughProfile(
        "GET /x HTTP/1.1\r\n\r\n", { GetRoute("/x") }, { .surface = "probe", .maxConcurrentRequests = 0 }, logger);
    INFO("response was: " << refused);
    CHECK(refused.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(refused.contains("probe: too many concurrent reqs"));

    SECTION("and the accept loop reports under that name")
    {
        Pace::CapturingLogger capturing { Pace::LogLevel::Debug };
        core::net::testing::InMemoryListener inner;
        core::net::testing::FailingListener listener {
            inner, core::net::testing::repeatedFailures(core::net::NetErrorCode::ConnReset, 1)
        };
        Pace::AtomicMetricsSink metrics;
        core::platform::SteadyClock clock;
        core::net::AcceptLoopHealth acceptLoops;
        Pace::AdminHttpServer server { listener,
                                       NothingToCancelHere(),
                                       metrics,
                                       [] { return Pace::MetricsSnapshot {}; },
                                       capturing,
                                       clock,
                                       acceptLoops,
                                       Pace::DefaultDrainWait(),
                                       {},
                                       nullptr,
                                       Pace::EverySurface,
                                       { .surface = "probe" } };
        auto client = inner.connectClient();
        REQUIRE(Pace::Testing::ShutdownWrite(*client).has_value());
        inner.close();
        core::async::syncRun(server.Run());
        server.Shutdown();
        auto const records = capturing.Snapshot();
        CHECK(std::ranges::any_of(records, [](Pace::CapturingLogger::Record const& record) {
            return record.message.contains("probe: an accept failed");
        }));
        CHECK_FALSE(std::ranges::any_of(
            records, [](Pace::CapturingLogger::Record const& record) { return record.message.contains("admin:"); }));
    }
}

namespace
{

/// One POST through `AdminHttpServer::Run` over a real listener bound at @p bindHost, dialled at
/// @p dialHost, returning the `peerHost` its route was shown -- or nothing when this platform will
/// not bind or dial that family.
[[nodiscard]] std::optional<std::string> PeerHostOverRealSocket(std::string_view bindHost, std::string_view dialHost)
{
    using namespace std::chrono_literals;
    auto listener = Pace::BlockingListener::Bind(std::string { bindHost }, 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        return std::nullopt;
    listener->SetTimeouts(Pace::AdminHttpServer::AcceptPoll, Pace::AdminHttpServer::RequestTimeout);
    auto const port = listener->boundPort();

    auto const seen = std::make_shared<BodySeen>();
    Pace::NullLogger logger;
    Pace::AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    core::net::AcceptLoopHealth acceptLoops;
    Pace::AdminHttpServer server { *listener,
                                   listener->Connections(),
                                   metrics,
                                   [] { return Pace::MetricsSnapshot {}; },
                                   logger,
                                   clock,
                                   acceptLoops,
                                   Pace::DefaultDrainWait(),
                                   { RecordingPostRoute(seen, "/x", 16) } };
    std::jthread acceptThread { [&server] { core::async::syncRun(server.Run()); } };
    // Declared after the thread, so it runs first: the loop is asked to stop before the join, and a
    // failed assertion below cannot leave the join waiting on a loop nobody stopped.
    auto const requestStop = [](Pace::AdminHttpServer* admin) {
        admin->RequestStop();
    };
    std::unique_ptr<Pace::AdminHttpServer, decltype(requestStop)> const stopFirst { &server, requestStop };

    core::net::BlockingConnector connector;
    auto client = core::async::syncRun(
        connector.connect(std::string { dialHost }, port, core::net::DialOptions { .connectTimeout = 5s }));
    if (!client.has_value())
        return std::nullopt;
    REQUIRE(core::async::syncRun(WriteString(client->get(), "POST /x HTTP/1.1\r\nContent-Length: 1\r\n\r\n{")));
    auto const response = core::async::syncRun(ReadAvailable(client->get()));
    INFO("response was: " << response.substr(0, 64));
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(seen->calls == 1);
    return seen->peerHost;
}

} // namespace

TEST_CASE("admin a real loopback connection's peerHost is the kernel's loopback literal", "[admin][http][peer]")
{
    // What a route deciding on WHO is asking actually reads, off a real socket rather than a fake:
    // `inet_ntop`'s host, no port and no brackets. The MCP surface admits on exactly this string.
    auto const peer = PeerHostOverRealSocket("127.0.0.1", "127.0.0.1");
    if (!peer.has_value())
        SKIP("this platform would not bind or dial an IPv4 loopback listener");
    CHECK(*peer == "127.0.0.1");
    CHECK(Pace::IsLoopbackHost(*peer));
}

TEST_CASE("admin a real IPv6 loopback connection's peerHost is ::1, unbracketed", "[admin][http][peer]")
{
    auto const peer = PeerHostOverRealSocket("::1", "::1");
    if (!peer.has_value())
        SKIP("this host has no IPv6 loopback to bind or dial");
    CHECK(*peer == "::1");
    CHECK(Pace::IsLoopbackHost(*peer));
}

namespace
{

/// The bounds a stop case runs under: a short drain, so a stop that waits for a client rather than cancelling it
/// shows as a wait far past `WorstCaseStopUnder`, and every read bound left at the admin surface's.
constexpr Pace::AdminDeadlines StopCaseDeadlines {
    .drain = std::chrono::milliseconds { 300 },
    .afterCancel = std::chrono::milliseconds { 1000 },
};
static_assert(Pace::AdminHttpServer::WorstCaseStopUnder(StopCaseDeadlines) < Pace::AdminHttpServer::RequestTimeout,
              "the bound must sit below one send timeout, or a write that merely timed out would pass for a cancel");

/// The receive buffer the slow client asks for, so what it can hold unread is this and not the host's tuning.
constexpr std::size_t SlowClientReceiveBytes = 4096;

/// Large enough that a client reading it slowly is still being written to when the drain runs out: many times what
/// the server's send buffer (`Testing::BindWithSmallSendBuffer`, refused past its grant ceiling) and the client's
/// receive buffer can hold between them, so the case does not depend on `net.ipv4.tcp_wmem`.
constexpr std::size_t SlowReaderBodyBytes = std::size_t { 4 } * 1024 * 1024;
static_assert(SlowReaderBodyBytes > 4 * (Pace::Testing::SmallSendBufferGrantCeiling + SlowClientReceiveBytes),
              "the answer must outlast every buffer under it");

/// A route answering `GET /big` with @p bytes of body, after holding for @p hold.
/// @param bytes The body's length.
/// @param hold How long the handler runs before it answers.
/// @param entered Raised as the handler starts.
/// @return The route.
[[nodiscard]] Pace::AdminRoute BigRoute(std::size_t bytes, std::chrono::milliseconds hold, std::atomic<bool>& entered)
{
    return Pace::AdminRoute { .path = "/big", .handler = [bytes, hold, &entered](Pace::AdminRequest const&) {
                                 entered.store(true);
                                 std::this_thread::sleep_for(hold);
                                 return Pace::AdminResponse { .status = "200 OK",
                                                              .contentType = "application/octet-stream",
                                                              .body = std::string(bytes, 'x') };
                             } };
}

/// A server over a real loopback listener, run on an accept thread, under the serving and bounds a case names.
struct StopCaseServer
{
    /// @param serving Where a connection is served.
    /// @param deadlines The bounds, the drain among them.
    /// @param bodyBytes How long `GET /big`'s body is.
    /// @param hold How long its handler runs first.
    StopCaseServer(Pace::AdminConnectionServing serving,
                   Pace::AdminDeadlines deadlines,
                   std::size_t bodyBytes,
                   std::chrono::milliseconds hold):
        listener { Pace::Testing::BindWithSmallSendBuffer() },
        server { *listener,
                 listener->Connections(),
                 metrics,
                 [] { return Pace::MetricsSnapshot {}; },
                 logger,
                 clock,
                 acceptLoops,
                 Pace::DefaultDrainWait(),
                 { BigRoute(bodyBytes, hold, entered) },
                 nullptr,
                 Pace::EverySurface,
                 { .surface = "stopcase",
                   .maxConcurrentRequests = 4,
                   .deadlines = deadlines,
                   .builtIns = Pace::AdminBuiltInRoutes::None,
                   .serving = serving } }
    {
        listener->SetTimeouts(Pace::AdminHttpServer::AcceptPoll, Pace::AdminHttpServer::RequestTimeout);
        acceptThread = std::jthread { [this] { core::async::syncRun(server.Run()); } };
    }

    /// The case's one connection: dialled, and the request written.
    /// @return The client's socket.
    [[nodiscard]] std::unique_ptr<core::net::ISocket> Request() const
    {
        using namespace std::chrono_literals;
        core::net::BlockingConnector connector;
        auto client = core::async::syncRun(connector.connect(
            "127.0.0.1",
            listener->boundPort(),
            core::net::DialOptions { .connectTimeout = 5s, .buffers = { .send = {}, .receive = SlowClientReceiveBytes } }));
        REQUIRE(client.has_value());
        (*client)->setReceiveDeadline(5s);
        REQUIRE(core::async::syncRun(WriteString(client->get(), "GET /big HTTP/1.1\r\n\r\n")));
        return std::move(*client);
    }

    std::atomic<bool> entered { false };
    Pace::CapturingLogger logger;
    Pace::AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    core::net::AcceptLoopHealth acceptLoops;
    std::unique_ptr<Pace::BlockingListener> listener;
    Pace::AdminHttpServer server;
    std::jthread acceptThread;

    StopCaseServer(StopCaseServer const&) = delete;
    StopCaseServer(StopCaseServer&&) = delete;
    StopCaseServer& operator=(StopCaseServer const&) = delete;
    StopCaseServer& operator=(StopCaseServer&&) = delete;

    /// Asks the loop to stop before `acceptThread` joins it, so a failed `REQUIRE` anywhere in a case -- `Request`'s
    /// included, before the case has armed anything of its own -- ends the case red rather than hanging it.
    ~StopCaseServer()
    {
        server.RequestStop();
    }
};

/// `AdminHttpServer::Stop` on a thread of its own, so the case's thread can bound the wait for it.
struct StopRun
{
    std::atomic<bool> done { false };
    std::atomic<std::int64_t> tookMs { -1 };
    std::jthread thread;

    /// Start the stop of @p fixture's server.
    /// @param fixture The server; must outlive the stop's thread.
    void Begin(StopCaseServer& fixture)
    {
        thread = std::jthread { [this, &fixture] {
            auto const began = std::chrono::steady_clock::now();
            fixture.server.Stop(fixture.acceptThread);
            tookMs.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count());
            done.store(true);
        } };
    }
};

/// Read until the server closes, however the bytes arrive: a large answer comes in short reads.
/// @param socket The client's socket.
/// @return Everything read.
core::async::Task<std::string> ReadToEnd(core::net::ISocket* socket)
{
    std::string out;
    std::vector<std::byte> chunk(std::size_t { 64 } * 1024);
    while (true)
    {
        auto const result = co_await socket->read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value() || *result == 0)
            break;
        std::ranges::transform(
            std::span { chunk }.first(*result), std::back_inserter(out), [](std::byte b) { return static_cast<char>(b); });
    }
    co_return out;
}

/// Whether @p record reports a cancel of @p count connections.
/// @param record A captured record.
/// @param count How many connections it must name.
/// @return True for the stop's cancel `Warn`.
[[nodiscard]] bool NamesCancel(Pace::CapturingLogger::Record const& record, std::size_t count)
{
    return record.level == Pace::LogLevel::Warn
           && record.message.contains(std::format("stopcase: stopping, cancelled {} connection(s)", count));
}

} // namespace

TEST_CASE("AdminHttp: a stop cancels a client that reads its answer too slowly, within its bound", "[admin][http][stop]")
{
    // The defect's own shape: a client that reads just often enough to keep every blocking write CALL inside its send
    // timeout holds the response write -- and the thread it blocks -- for as long as it likes, so a stop that waits
    // for it waits with it. Both servings: on the accept thread the join itself would wait, on threads of their own
    // the serving threads' join would.
    using namespace std::chrono_literals;
    auto const serving = GENERATE(Pace::AdminConnectionServing::OnAcceptThread, Pace::AdminConnectionServing::OnOwnThreads);
    INFO("serving " << static_cast<int>(serving));
    StopCaseServer fixture { serving, StopCaseDeadlines, SlowReaderBodyBytes, 0ms };
    auto client = fixture.Request();

    std::atomic<std::size_t> received { 0 };
    std::atomic<bool> endedByServer { false };
    std::atomic<bool> abandon { false };
    std::atomic<bool> hurry { false };
    // One KiB every 20 ms until `hurry`: far from the end of the body for the whole stop, and every write call the
    // server makes progresses well inside `RequestTimeout`. Bounded by its count, and by `abandon` on the failure path.
    std::jthread reader { [&] {
        std::vector<std::byte> chunk(std::size_t { 64 } * 1024);
        for ([[maybe_unused]] auto const step: std::views::iota(0, 20000))
        {
            if (abandon.load())
                break;
            auto const read = core::async::syncRun(ReadOnce(
                client.get(), std::span<std::byte> { chunk.data(), hurry.load() ? chunk.size() : std::size_t { 1024 } }));
            if (!read.has_value() || *read == 0)
            {
                endedByServer.store(true);
                break;
            }
            received.fetch_add(*read);
            if (!hurry.load())
                std::this_thread::sleep_for(20ms);
        }
        client->close();
    } };
    StopRun stop;
    // Destroyed FIRST: a failed assertion below must not leave the stop waiting on a client nobody releases, nor the
    // accept thread's join waiting on a loop nobody stopped.
    auto const release = [&abandon](Pace::AdminHttpServer* server) {
        abandon.store(true);
        server->RequestStop();
    };
    std::unique_ptr<Pace::AdminHttpServer, decltype(release)> const releaseFirst { &fixture.server, release };

    REQUIRE(Pace::Testing::WaitUntil(
        "the response to be on its way to the slow client",
        [&] { return received.load() > 0; },
        [&] { return std::format("received {}", received.load()); },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = 5s }));

    stop.Begin(fixture);
    constexpr auto Bound = Pace::AdminHttpServer::WorstCaseStopUnder(StopCaseDeadlines);
    auto const stopped = Pace::Testing::WaitUntil(
        "the stop to cancel the slow client and return",
        [&] { return stop.done.load(); },
        [&] { return std::format("received {}, ended by server {}", received.load(), endedByServer.load()); },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = Bound + 2s });
    // Once stopped, the client reads what the kernel still holds for it at full speed: what tells a connection the
    // SERVER ended from one the case merely stopped reading is that the stream then ENDS, short of the body.
    auto drained = false;
    if (stopped)
    {
        hurry.store(true);
        drained = Pace::Testing::WaitUntil(
            "the client to read to the end of what the cancelled connection delivered",
            [&] { return endedByServer.load(); },
            [&] { return std::format("received {}", received.load()); },
            Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = 10s });
    }
    // Release the client either way, so a stop that waited for it ends and its thread joins.
    abandon.store(true);
    if (stop.thread.joinable())
        stop.thread.join();
    reader.join();

    CHECK(stopped);
    CHECK(stop.tookMs.load() < Bound.count());
    // The SERVER ended the connection: the client neither finished the body nor gave up first.
    CHECK(drained);
    CHECK(endedByServer.load());
    CHECK(received.load() < SlowReaderBodyBytes);
    // And the cancel is what did it, named with its count.
    auto const records = fixture.logger.Snapshot();
    CHECK(std::ranges::any_of(records, [](Pace::CapturingLogger::Record const& record) { return NamesCancel(record, 1); }));
    CHECK(std::ranges::none_of(records, [](Pace::CapturingLogger::Record const& record) {
        return record.message.contains("did not finish within the stop ceiling");
    }));
}

TEST_CASE("AdminHttp: a client that reads its answer still gets all of it from a stop that began during it",
          "[admin][http][stop]")
{
    // The control: the cancel is for a client the drain cannot wait for, never for one it can. The stop lands while
    // the handler runs; the client reads at full speed and receives every byte, and nothing is cancelled.
    using namespace std::chrono_literals;
    auto const serving = GENERATE(Pace::AdminConnectionServing::OnAcceptThread, Pace::AdminConnectionServing::OnOwnThreads);
    INFO("serving " << static_cast<int>(serving));
    constexpr auto Body = std::size_t { 1024 } * 1024;
    constexpr Pace::AdminDeadlines Roomy { .drain = 3000ms, .afterCancel = 1000ms };
    StopCaseServer fixture { serving, Roomy, Body, 300ms };
    auto client = fixture.Request();
    StopRun stop;
    auto const release = [](Pace::AdminHttpServer* server) {
        server->RequestStop();
    };
    std::unique_ptr<Pace::AdminHttpServer, decltype(release)> const releaseFirst { &fixture.server, release };

    REQUIRE(Pace::Testing::WaitUntil(
        "the handler to be running",
        [&] { return fixture.entered.load(); },
        [] { return std::string { "not entered" }; },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = 5s }));
    stop.Begin(fixture);
    auto const response = core::async::syncRun(ReadToEnd(client.get()));
    auto const stopped = Pace::Testing::WaitUntil(
        "the stop to return",
        [&] { return stop.done.load(); },
        [] { return std::string { "stopping" }; },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = Pace::AdminHttpServer::WorstCaseStopUnder(Roomy) });
    if (stop.thread.joinable())
        stop.thread.join();

    CHECK(stopped);
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    auto const split = response.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    CHECK(response.size() - (split + 4) == Body);
    CHECK(stop.tookMs.load() < Roomy.drain.count());
    CHECK(std::ranges::none_of(fixture.logger.Snapshot(), [](Pace::CapturingLogger::Record const& record) {
        return record.message.contains("cancelled");
    }));
}

TEST_CASE("AdminHttp: a stop waits for a stuck handler once, and says so once", "[admin][http][stop]")
{
    // A handler that outlasts the drain and the wait after the cancel is not a client, and the cancel cannot end it.
    // The stop waits for it ONCE -- so `WorstCaseStopUnder` is the bound as written -- and reports it once, where a
    // second drain inside the listener's close doubled both.
    using namespace std::chrono_literals;
    // Past TWO stop ceilings, so a stop that waited twice would also have reported twice.
    constexpr auto Hold = 2 * (StopCaseDeadlines.drain + StopCaseDeadlines.afterCancel) + 1000ms;
    StopCaseServer fixture { Pace::AdminConnectionServing::OnOwnThreads, StopCaseDeadlines, 16, Hold };
    auto client = fixture.Request();
    REQUIRE(Pace::Testing::WaitUntil(
        "the handler to be running",
        [&] { return fixture.entered.load(); },
        [] { return std::string { "not entered" }; },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = 5s }));

    auto const began = std::chrono::steady_clock::now();
    fixture.server.Stop(fixture.acceptThread);
    auto const took = std::chrono::steady_clock::now() - began;

    CHECK(took < Pace::AdminHttpServer::WorstCaseStopUnder(StopCaseDeadlines) + 250ms);
    auto const records = fixture.logger.Snapshot();
    CHECK(std::ranges::count_if(records,
                                [](Pace::CapturingLogger::Record const& record) {
                                    return record.level == Pace::LogLevel::Error
                                           && record.message.contains("did not finish within the stop ceiling");
                                })
          == 1);
}
