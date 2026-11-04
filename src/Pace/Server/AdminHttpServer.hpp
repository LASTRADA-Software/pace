// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/BoundedDrain.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Metrics/PrometheusFormatter.hpp>
#include <Pace/Transport/LingeringClose.hpp>
#include <Pace/Transport/OpenConnections.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/platform/Clock.hpp>

namespace core::net
{
class ITlsContext;
} // namespace core::net

namespace Pace
{

/// One request header this server reads.
///
/// A closed set rather than a header map, deliberately: a map would invite routes
/// that branch on headers this server never bounds or normalises. A table is what
/// makes the set *extensible without being open* -- the second entry here arrived
/// as a row, and the parse loop that used to stop at the first match had to be
/// fixed once and now cannot regress per header.
enum class AdminHeader : std::uint8_t
{
    /// The credential every gated route checks.
    Authorization = 0,
    /// What a client already holds, for the conditional GETs the charts serve.
    IfNoneMatch,
    /// How many body bytes follow the head. Read for framing, and the only length this server believes.
    ContentLength,
    /// What the body is.
    ContentType,
    /// Where a browser says the request came from, for a route that refuses cross-origin callers.
    Origin,
    /// What the caller accepts.
    Accept,
    /// The MCP revision the caller speaks (`MCP-Protocol-Version`).
    McpProtocolVersion,
    /// The MCP method, mirrored into a header so a route can refuse before parsing a body (`Mcp-Method`).
    McpMethod,
    /// The MCP tool or resource name, mirrored the same way (`Mcp-Name`).
    McpName,
    /// Present only so it can be REFUSED: a chunked body has no declared length, and this server
    /// bounds a body by the length it declared. Never offered to a route.
    TransferEncoding,
    Last
};

/// What one readable header is called on the wire.
struct AdminHeaderRow
{
    AdminHeader header;    ///< The header this row describes.
    std::string_view name; ///< Its field name, lower-cased for the comparison.
};

/// Every header this server reads, in enumerator order.
inline constexpr EnumTable<AdminHeader, AdminHeaderRow> AdminHeaderTable {
    AdminHeaderRow { .header = AdminHeader::Authorization, .name = "authorization" },
    AdminHeaderRow { .header = AdminHeader::IfNoneMatch, .name = "if-none-match" },
    AdminHeaderRow { .header = AdminHeader::ContentLength, .name = "content-length" },
    AdminHeaderRow { .header = AdminHeader::ContentType, .name = "content-type" },
    AdminHeaderRow { .header = AdminHeader::Origin, .name = "origin" },
    AdminHeaderRow { .header = AdminHeader::Accept, .name = "accept" },
    AdminHeaderRow { .header = AdminHeader::McpProtocolVersion, .name = "mcp-protocol-version" },
    AdminHeaderRow { .header = AdminHeader::McpMethod, .name = "mcp-method" },
    AdminHeaderRow { .header = AdminHeader::McpName, .name = "mcp-name" },
    AdminHeaderRow { .header = AdminHeader::TransferEncoding, .name = "transfer-encoding" },
};
static_assert(RowsInEnumeratorOrder(AdminHeaderTable, &AdminHeaderRow::header));

/// The HTTP methods a route may answer.
///
/// In-process only: never transmitted or persisted, so the values carry no contract.
enum class AdminMethod : std::uint8_t
{
    /// `GET`, the only method before a route could take a body.
    Get = 0,
    /// `POST`, whose body is bounded by `AdminRoute::maxBodyBytes`.
    Post,
    Last
};

/// One admin request, as far as this server understands one.
///
/// Every view borrows from storage the server keeps alive for exactly as long as the handler
/// runs; a handler that wants any of it afterwards copies it.
struct AdminRequest
{
    /// The method the route was matched on.
    AdminMethod method { AdminMethod::Get };
    /// The body, exactly `Content-Length` bytes; empty when the request carried none.
    std::string_view body {};
    /// The connection's peer as the accepted socket reports it (the host, no port), owned.
    /// Empty when the socket cannot say, which a caller deciding on it must treat as NOT local.
    std::string peerHost {};
    /// Path with the query string stripped, so `/metrics?foo=1` routes as `/metrics`.
    std::string_view path;
    /// Whatever followed the `?`, empty when there was none.
    std::string_view query;
    /// One slot per `AdminHeaderTable` row; empty where the client sent none.
    EnumTable<AdminHeader, std::string_view> headers {};

    /// One header's value.
    /// @param which Which header.
    /// @return Its value, or empty when the client sent none.
    [[nodiscard]] std::string_view Header(AdminHeader which) const noexcept
    {
        return headers[static_cast<std::size_t>(which)];
    }
};

/// What a route answered.
///
/// The status is part of the *response* rather than a column of the route table,
/// because one route legitimately answers several: `/fleet` is a page when this
/// node leads and a 401 when the caller brought no credential. A table column would
/// have to be a status per route, which is the one thing that is not fixed per route.
struct AdminResponse
{
    /// Status line without the version, e.g. `200 OK`.
    ///
    /// A status that carries no content -- `304`, `204` -- suppresses `Content-Type`
    /// and `Content-Length` on its own, because whether a body is allowed is a
    /// property of the status rather than something each route must remember.
    std::string_view status { "200 OK" };
    /// What `body` is.
    std::string_view contentType { "text/plain" };
    /// The body, already rendered. Empty for a status that carries no content.
    std::string body {};
    /// Extra header lines, each without its trailing CRLF.
    ///
    /// Empty for almost every response. It exists because a 401 that does not carry
    /// `WWW-Authenticate` is one a browser cannot prompt for, which turns "you need a
    /// credential" into "this page is broken".
    std::vector<std::string> extraHeaders {};
};

/// Renders one route, or refuses it.
///
/// A `std::function` supplied by the caller rather than a member function, and that
/// is a layering decision rather than a style one: the fleet dashboard reads
/// `Distributed/` and `Cluster/`, and a handler declared here would drag both into
/// `Server/` for the benefit of one route. The server owns HTTP; what a route means
/// belongs to whoever registered it.
using AdminRouteHandler = std::function<AdminResponse(AdminRequest const&)>;

/// How a route's `path` is compared against the request's.
///
/// A column and not a special case: the charts are one resource per series, and a
/// route per series would put the series table's contents in two places. Dispatch
/// tries the kinds in enumerator order, so an exact route is never shadowed by a
/// prefix one however the caller happened to order its vector.
enum class AdminRouteMatch : std::uint8_t
{
    /// The whole path, and nothing else.
    Exact = 0,
    /// The path begins with `AdminRoute::path`; the handler reads the tail.
    Prefix,
    Last
};

/// One route: the path it answers, how that path is matched, and what answers it.
///
/// A table rather than an `if`/`else` ladder, for the reason every other table in
/// this codebase is one -- a route added by editing a chain of comparisons is a
/// route somebody adds to the chain and forgets to test, and the ladder this
/// replaced had already grown a third arm that only differed by a string.
struct AdminRoute
{
    /// The path, matched per `match`.
    std::string_view path;
    /// What renders it.
    AdminRouteHandler handler;
    /// Whole path or leading path. Exact unless a route says otherwise.
    AdminRouteMatch match { AdminRouteMatch::Exact };
    /// The method this route answers; the same path may carry one route per method.
    AdminMethod method { AdminMethod::Get };
    /// The largest body this route accepts, refused `413` BEFORE it is read. Zero means a body
    /// is refused (an empty one is not a body).
    std::size_t maxBodyBytes { 0 };
};

/// One log record: the level an operator alerts on, and the sentence they read.
///
/// Returned as a pair rather than logged in place because the level is the thing
/// under test. A severity spelled at a call site inside `main.cpp` is reachable
/// from no test in this tree, which is how `pace-kv` came to report a
/// deliberately-tolerated condition at `Error` for as long as it did.
struct AdminBindFailureReport
{
    LogLevel level;      ///< Severity that matches the verdict, not the syscall.
    std::string message; ///< What the operator reads, including what happens next.
};

/// Whether a server answers the two routes every admin surface used to carry unasked.
///
/// In-process only: never transmitted or persisted, so the values carry no contract.
enum class AdminBuiltInRoutes : std::uint8_t
{
    /// `GET /metrics` and `GET /healthz`, answered before any caller route: the admin surface.
    MetricsAndHealth = 0,
    /// Neither: only the caller's routes are served, and those two paths are a `404` like any
    /// other. For a surface that is not an admin surface at all -- MCP -- and must not grow a
    /// scrape endpoint by reusing this HTTP implementation.
    None,
    Last
};

/// Where an accepted connection is served.
///
/// In-process only: never transmitted or persisted, so the values carry no contract.
enum class AdminConnectionServing : std::uint8_t
{
    /// On the accept thread, to its end, before the next accept: the admin surface. Its listener
    /// blocks, so `maxConcurrentRequests` is never reached and a connection that sends nothing holds
    /// the surface for `AdminDeadlines::firstByte`. Right for a scrape and a probe, which are short.
    OnAcceptThread = 0,
    /// On a thread of the server's own, one per in-flight request up to `maxConcurrentRequests`: a
    /// held connection holds one slot, never the surface, and the next connection past the cap is
    /// answered `503` -- written on the accept thread, which then lingers on it for up to
    /// `AdminDeadlines::linger`, so a peer that keeps reconnecting keeps the surface refusing while it
    /// does. For a surface whose requests run long (MCP: a tool asks the leader).
    OnOwnThreads,
    Last
};

/// The bounds one instance of this server reads and lingers under, as data: a surface states its
/// own row rather than a branch choosing between two sets of constants.
///
/// The default row is the admin surface's, every figure argued at its `AdminHttpServer` constant.
struct AdminDeadlines
{
    /// How long a peer may hold the connection having sent nothing (`AdminHttpServer::FirstByteTimeout`).
    std::chrono::milliseconds firstByte { 30000 };
    /// How long one read of a request in progress may take (`AdminHttpServer::RequestTimeout`).
    std::chrono::milliseconds request { 2000 };
    /// How long the whole head may take once begun (`AdminHttpServer::HeadTimeout`).
    std::chrono::milliseconds head { 10000 };
    /// How long the whole body may take once the head is complete (`AdminHttpServer::BodyTimeout`).
    std::chrono::milliseconds body { 2000 };
    /// How a served or refused connection is closed (`AdminHttpServer::Linger`).
    LingerBounds linger { .total = std::chrono::milliseconds { 2000 }, .maxBytes = std::size_t { 64 } * 1024, .reads = 4 };
    /// How long `AdminHttpServer::Shutdown` waits for the requests still in flight before it says some did not
    /// finish. The admin surface's figure is `DrainBound`'s default; a surface whose handler waits on something
    /// longer than that (MCP: a forwarded call) states a ceiling above its own longest request, or every stop that
    /// lands during one is logged as a request abandoned.
    std::chrono::milliseconds drain { DrainBound {}.ceiling };
    /// How long a stop waits, once `drain` ran out and it CANCELLED every connection still open
    /// (`IConnectionCanceller`), for the requests those connections held to end. A cancelled write or linger
    /// fails at once, so this covers a thread being scheduled and a handler finishing, never a client: what a
    /// client does can no longer hold the stop past `drain` plus this.
    std::chrono::milliseconds afterCancel { 1000 };
};

/// How one instance of this server is shaped, beside the routes it serves.
///
/// Facts that were constants of the one admin surface, and are columns now that a second
/// surface (MCP) runs the same hardened HTTP code: one implementation, as the design requires,
/// rather than a sibling that shares its parsing and drifts on everything else.
struct AdminServerProfile
{
    /// What the accept loop reports as, to `core::net::AcceptLoopHealth` and the log. A second
    /// surface reporting as `admin` would make `/healthz` name the wrong port when it stops.
    std::string_view surface { "admin" };
    /// How many requests may be in flight before the next connection is answered `503`.
    std::size_t maxConcurrentRequests { 32 };
    /// The bounds every connection is read and closed under.
    AdminDeadlines deadlines {};
    /// Whether `/metrics` and `/healthz` are served.
    AdminBuiltInRoutes builtIns { AdminBuiltInRoutes::MetricsAndHealth };
    /// Where an accepted connection is served.
    AdminConnectionServing serving { AdminConnectionServing::OnAcceptThread };
};

/// Describe an admin endpoint (`/metrics`, `/healthz`) the DAEMON could not bind.
///
/// ## The severity follows the verdict, and the verdicts differ
///
/// `pace-kv` **tolerates** this: the admin surface is a scrape and probe
/// convenience rather than the service, so the daemon serves the cache on without
/// one, permanently and by design. `pace-node` answers the same
/// failure by refusing to start, and is right to -- an operator who asked a *worker*
/// for an endpoint is almost always wiring a probe to it, so a worker that started
/// without one looks healthy to the very thing that would have reported it was not.
///
/// The level is where that decision reaches an operator, and it was contradicting
/// it ([#603](https://github.com/LASTRADA-Software/pace/issues/603)). `Error`
/// is what a site pages on and what a log scraper counts, and it costs in both
/// directions at once: a site that alerts on `Error` gets paged for a working
/// daemon, and a site that has learned to ignore this particular `Error` has been
/// trained to ignore the level. A condition the process decided to continue past,
/// forever, on purpose, is a `Warn`.
///
/// **This is not the rule for every bind in the daemon.** `RunReactorServer`'s three
/// listener failures keep `Error`, because those paths return `EXIT_FAILURE`: the
/// process stops, so the severity and the verdict already agree. Skipped, absent and
/// failed are different states, and the log level is one of the few places an
/// operator can tell them apart.
///
/// The message says the daemon is continuing **without** the endpoint rather than
/// only that a bind failed, so a reader does not have to infer whether the process
/// survived.
/// @param address Address that was asked for.
/// @param port Port that was asked for.
/// @param error What the bind reported, or a stand-in when there is no listener.
/// @return The level and the sentence to log.
[[nodiscard]] AdminBindFailureReport DescribeToleratedAdminBindFailure(std::string_view address,
                                                                       std::uint16_t port,
                                                                       std::string_view error);

/// Tiny read-only HTTP/1.1 admin surface, served on a dedicated port so it
/// never collides with the cache wire protocols (a leading `GET` would
/// otherwise be misrouted to the memcached text autodetector). Built-in routes:
///   - `GET /metrics` — Prometheus text exposition of the counters + storage
///     snapshot (see RenderPrometheus).
///   - `GET /healthz` — `200 OK` liveness probe for containers / k8s.
///   - an unknown path — `404`; a known path with a method none of its routes answers — `405`
///     naming the ones that do; a body past a route's ceiling — `413`, a `POST` without a length
///     — `411`, a `Transfer-Encoding` — `501`; a bad request line, or a head the
///     peer stopped sending mid-way — `400`; a head this server gave up waiting
///     for at its read deadline — `408`; a request head over the byte cap — `431`.
/// A peer that sends NOTHING before the request deadline is closed without any
/// response at all: it asked nothing, so it is owed nothing, and answering it is
/// what made a browser's speculative preconnect render `400` (#824). See
/// `AdminHeadOutcome` in the implementation for the full set and its dispositions.
/// A caller may register further routes; see `AdminRoute`.
/// Each connection answers exactly one request, then closes (`Connection:
/// close`); the request head is bounded so a slow or oversized client cannot
/// tie the server up. Where a connection is served is the profile's
/// `AdminConnectionServing`: the admin surface's traffic is trivial, so it serves
/// one connection at a time on the accept thread; a surface whose requests run
/// long (MCP) serves each on a thread of the server's own, up to its cap.
class AdminHttpServer
{
  public:
    /// How long an `accept()` may park before the loop looks at its stop flag.
    ///
    /// POSIX does not unblock a parked `accept()` when another thread closes the
    /// listening socket, so this poll is the only portable way `Shutdown()` is ever
    /// observed -- the mechanism whose *absence* this repository already records as
    /// a `systemctl stop` that hung until the supervisor escalated to SIGKILL.
    static constexpr auto AcceptPoll = std::chrono::milliseconds { 500 };

    /// How long one request read may take before the connection is dropped.
    ///
    /// The admin surface serves one connection at a time on its accept thread, so
    /// an idle client that never finishes its request head would otherwise wedge it
    /// for everybody (slowloris); a surface serving on threads of its own would
    /// lose a slot to it for as long.
    static constexpr auto RequestTimeout = AdminDeadlines {}.request;

    /// How long a peer may hold the connection having sent **nothing**.
    ///
    /// **Two questions were sharing one number and the shared answer was wrong for one
    /// of them** ([#828](https://github.com/LASTRADA-Software/pace/issues/828)).
    /// A browser opens speculative *preconnect* sockets ahead of a navigation and may
    /// send on one long afterwards, so 2 s is simply the wrong answer to "is this peer
    /// going to ask anything" -- and it is the answer that makes an ordinary navigation
    /// fail. #824 stopped that being a `400`; it is still a close, so whether the page
    /// loads depends on the client retrying.
    ///
    /// Raising `RequestTimeout` instead would slacken the mid-head bound in exchange,
    /// which `.agent/rules/distributed-compilation.md` refuses in the same words: *"a
    /// cache exchange is bounded by a round trip, a dispatched compile by how long a
    /// COMPILER runs; sharing one deadline abandoned every TU worth distributing while
    /// the worker finished the job anyway."* Same defect, different wire.
    ///
    /// Armed on the socket at the top of the head read and **tightened to
    /// `RequestTimeout` the moment a byte arrives**, so the two bounds never overlap
    /// and neither is enforced by a retry loop -- a loop would have to assume the read
    /// consumed real time, which a non-blocking or reactor-backed socket does not.
    static constexpr auto FirstByteTimeout = AdminDeadlines {}.firstByte;

    /// How long the whole request head may take once the peer has started.
    ///
    /// `RequestTimeout` is per **read**, so without a total a client dribbling one byte
    /// under each deadline holds a slot for `MaxRequestBytes` reads. A per-read bound
    /// cannot express a total and a total cannot express a stall, so both exist and
    /// neither is derived from the other.
    static constexpr auto HeadTimeout = AdminDeadlines {}.head;

    /// How long the whole body may take once the head is complete.
    ///
    /// A named bound of its own rather than `RequestTimeout` reused by accident: `RequestTimeout`
    /// is per READ, and a body of `maxBodyBytes` dribbled one byte per read would hold the slot
    /// for that many reads. Equal to it today, which is a choice and not a derivation.
    static constexpr auto BodyTimeout = AdminDeadlines {}.body;

    /// How long a served connection listens to its client before closing, and how much.
    ///
    /// Every refusal this surface writes over a head it did not finish reading -- `431`
    /// past the byte cap, `408` past the total, `503` past the concurrency cap -- is
    /// followed by a close, and a close over unread bytes is a reset that destroys the
    /// refusal before a Windows client reads it (#1554; `CloseLingering` has the
    /// measurement). Short, because these sockets BLOCK the thread they are served on
    /// while it listens -- for a `503` that is the ACCEPT thread, on every profile: no
    /// longer than one `RequestTimeout`, spread over the reads -- a read that meets a
    /// quarter of it in silence ends the linger.
    static constexpr LingerBounds Linger = AdminDeadlines {}.linger;
    static_assert(Linger.total == RequestTimeout, "the admin linger is one RequestTimeout, as argued above");

    /// Provider for a fresh metrics snapshot (storage stats + uptime), so
    /// `/metrics` reflects live state on each scrape rather than a stale copy.
    /// Computing uptime here keeps the server itself clock-agnostic.
    using SnapshotProvider = std::function<MetricsSnapshot()>;

    /// Construct over its collaborators; all must outlive the server.
    /// @param listener Bound listener for the admin port.
    /// @param connections Cancels the connections @p listener accepted that are still open, which a stop does once
    ///        its drain runs out (`Stop`). A `BlockingListener`'s own `Connections()`. Required, because without it
    ///        a client that reads its answer slowly enough holds the stop for as long as it likes.
    /// @param metrics Connection-level counter sink to expose.
    /// @param snapshotProvider Returns a fresh storage snapshot + uptime per scrape.
    /// @param logger Shared logger.
    /// @param acceptLoops The process's accept-loop registry: this server's own loop reports to
    ///        it, and `/healthz` answers from it -- `503` naming every surface that has stopped
    ///        accepting or is degraded, where it used to answer `200` over a dead one. Required,
    ///        because a probe that asks nothing is the confident wrong signal this exists to end.
    /// @param backoffWait How the accept loop waits out a backoff: it owns its thread and its
    ///        listener blocks, so there is no reactor timer to suspend on.
    /// @param routes Routes beyond `/metrics` and `/healthz`; copied.
    /// @param tls Server TLS context, or nullptr to serve plaintext.
    /// @param surfaces The metrics surfaces this process serves, for `/metrics`. Defaults to
    ///                 `EverySurface`, so a process that has not been narrowed is unchanged.
    /// @param profile What this instance reports as, how many requests it serves at once and
    ///                whether it answers the built-in routes. Defaults to the admin surface's.
    AdminHttpServer(core::net::IListener& listener,
                    IConnectionCanceller& connections,
                    IMetricsSink const& metrics,
                    SnapshotProvider snapshotProvider,
                    ILogger& logger,
                    core::platform::IClock& clock,
                    core::net::AcceptLoopHealth& acceptLoops,
                    IDrainWait& backoffWait,
                    std::vector<AdminRoute> routes = {},
                    core::net::ITlsContext* tls = nullptr,
                    std::span<MetricsSurface const> surfaces = EverySurface,
                    AdminServerProfile profile = {});

    /// Accept loop; returns when the listener is closed via Shutdown().
    /// @return Task that resolves when the accept loop exits.
    [[nodiscard]] core::async::Task<void> Run();

    /// Ask the accept loop to stop, without touching the listener.
    ///
    /// **Call this, join the accept thread, and only then call `Shutdown()`.**
    /// `Shutdown()` closes the listener, which is a write to a member the accept
    /// thread reads on every pass -- a data race ThreadSanitizer reports at
    /// `BlockingListener::Close`, and one the whole suite passes straight through
    /// (#260). Closing was never the wake mechanism in the first place: POSIX does
    /// not unblock a parked `accept()` on close, which is exactly why the listener
    /// arms `AcceptPoll` via `BlockingListener::SetTimeouts`. So the loop leaves on
    /// its own within one poll interval, and waiting for it costs at most that.
    ///
    /// Split out rather than folded into `Shutdown()`, because a single call
    /// cannot both precede and follow the join.
    void RequestStop() noexcept;

    /// Stop accepting and close the listener, then drain in-flight requests (`DrainOrCancel`).
    ///
    /// Safe to call after the accept thread has been joined; see `RequestStop()`
    /// for why that order matters. Calling it alone still works and still stops
    /// the loop -- it simply races the accept thread while doing so.
    void Shutdown() noexcept;

    /// The whole stop, in its one order: ask the loop to stop, drain the requests in flight -- CANCELLING the
    /// connections still open once `AdminDeadlines::drain` runs out -- then join @p acceptThread, and only then
    /// close the listener.
    ///
    /// **The drain comes BEFORE the join**, because on an `OnAcceptThread` profile the request is served on the
    /// accept thread itself: a join first would wait for a client that reads its answer slowly, for as long as it
    /// reads, and the drain after it would never get to cancel anything. So the stop is bounded whatever a client
    /// does: one `AcceptPoll`, `drain`, then `afterCancel`.
    /// @param acceptThread The thread running `Run()`; joined here.
    void Stop(std::jthread& acceptThread) noexcept;

    /// The longest `Stop` takes under @p deadlines, whatever a client does: one accept poll, the drain, and the wait
    /// after the cancel. A handler that does not return is not a client and is not covered.
    /// @param deadlines The profile's bounds.
    /// @return The bound.
    [[nodiscard]] static constexpr std::chrono::milliseconds WorstCaseStopUnder(AdminDeadlines const& deadlines) noexcept
    {
        return AcceptPoll + deadlines.drain + deadlines.afterCancel;
    }

    /// Maximum number of concurrently-in-flight admin requests. The accept
    /// loop spawns each request as a detached coroutine; once this cap is
    /// reached the loop replies 503 and closes the connection rather than
    /// queueing it. Default sized for "more than every typical scraper plus a
    /// handful of healthcheck probes" but small enough that a buggy or hostile
    /// client cannot exhaust memory. The admin surface's figure; another surface
    /// states its own through `AdminServerProfile`.
    static constexpr std::size_t MaxConcurrentRequests = 32;
    static_assert(AdminServerProfile {}.maxConcurrentRequests == MaxConcurrentRequests,
                  "the default profile is the admin surface's, so its cap is this one");

  private:
    /// Wait for the requests in flight up to `AdminDeadlines::drain`, then cancel every connection still open
    /// and wait `afterCancel` for what they held to end. Run ONCE per stop, so `WorstCaseStopUnder` is literal.
    void DrainOrCancel() noexcept;

    /// Close the listener: after the accept thread is joined, since closing writes what it reads (#260).
    void CloseListener() noexcept;

    /// Say, as a `Warn`, how many connections the stop's cancel shut down, once no accept can add to the count.
    void ReportCancelled() noexcept;

    core::net::IListener& _listener;
    IConnectionCanceller& _openConnections;
    IMetricsSink const& _metrics;
    SnapshotProvider _snapshotProvider;
    ILogger& _logger;
    core::platform::IClock& _clock;
    core::net::AcceptLoopHealth& _acceptLoops;
    IDrainWait& _backoffWait;
    std::vector<AdminRoute> _routes;
    /// Server TLS context, or null for plaintext. Not owned.
    core::net::ITlsContext* _tls { nullptr };
    /// Which metrics surfaces this process serves, for `/metrics`. A span over storage that
    /// outlives this server -- `EverySurface` and a binary's own set are both constexpr.
    std::span<MetricsSurface const> _surfaces { EverySurface };
    /// What this instance reports as, its request cap and its built-in routes.
    AdminServerProfile _profile;
    std::atomic<bool> _shuttingDown { false };
    /// Number of admin requests currently being served as detached tasks.
    /// Bumped in the accept loop, decremented at the end of each request -- on a `_connections` thread for an
    /// `OnOwnThreads` profile, which is why it is declared before that member.
    std::atomic<std::size_t> _inFlight { 0 };
    /// The threads an `AdminConnectionServing::OnOwnThreads` profile serves its connections on, one per
    /// in-flight request; null for `OnAcceptThread`. Declared LAST, after everything a connection borrows
    /// (`_inFlight`, which each connection decrements as it ends, included), so it is destroyed -- its threads
    /// joined -- first: a request still running when `Shutdown`'s drain gave up finishes against members that
    /// are all still alive.
    std::unique_ptr<core::async::ThreadPoolExecutor> _connections;

  public:
    /// Test hook: live count of concurrently-in-flight admin requests.
    /// @return Detached requests currently being serviced.
    [[nodiscard]] std::size_t InFlight() const noexcept
    {
        return _inFlight.load(std::memory_order_acquire);
    }
};

/// Serve a single admin HTTP request on `socket`: read one request, route it,
/// write the response. Exposed separately from the accept loop so it can be
/// driven directly over an in-memory socket in tests.
/// Pointer/by-value parameters (not references) because this is a coroutine —
/// a reference parameter would dangle across a suspension point.
/// @param socket Connected client socket.
/// @param metrics Counter sink for `/metrics`.
/// @param snapshotProvider Storage-stats + uptime provider for `/metrics`.
/// @param routes Caller-registered routes, consulted before the 404.
/// @return Task that resolves when the response has been written.
/// @param surfaces The metrics surfaces this process serves, for `/metrics`. Defaults to
///                 `EverySurface`, so the daemon is unchanged; a binary serving only some of
///                 them renders those rows absent rather than as a plausible zero (#1484).
/// @param liveness What `/healthz` answers from: `503` naming every surface that stopped
///                 accepting or is degraded, `200` while none is. Null only for a caller
///                 exercising another route; `AdminHttpServer` always passes its registry.
/// @param builtIns Whether `/metrics` and `/healthz` are answered; with `None` they are a `404`
///                 and @p metrics, @p snapshotProvider and @p liveness are never read.
/// @param deadlines The bounds the head and body are read under; the admin surface's by default.
[[nodiscard]] core::async::Task<void> ServeAdminHttp(core::net::ISocket* socket,
                                                     IMetricsSink const* metrics,
                                                     AdminHttpServer::SnapshotProvider snapshotProvider,
                                                     core::platform::IClock* clock,
                                                     std::span<AdminRoute const> routes = {},
                                                     std::span<MetricsSurface const> surfaces = EverySurface,
                                                     core::net::AcceptLoopHealth const* liveness = nullptr,
                                                     AdminBuiltInRoutes builtIns = AdminBuiltInRoutes::MetricsAndHealth,
                                                     AdminDeadlines deadlines = {});

} // namespace Pace
