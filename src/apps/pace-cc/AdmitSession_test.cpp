// SPDX-License-Identifier: Apache-2.0
//
// The launcher's held admission, against an in-process fake node on a real loopback socket.
//
// A real socket and a real thread, because what is asserted -- the connection staying open while
// the caller does something else, and its end being read as an abandonment -- is a property of an
// open connection that a scripted socket resolving synchronously cannot have. Every assertion is
// made on the case's own thread: the fake node records what it saw and the case reads it after a
// bounded wait.
#include "AdmitSession.hpp"

#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SleepUntil.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/TcpClient.hpp>
#include <tests/BoundedWait.hpp>

using namespace Pace;
using namespace std::chrono_literals;

namespace Wire = Pace::CompileCacheWire;

namespace
{
/// What the fake node saw. Written on its thread, read on the case's after a bounded wait.
class Seen
{
  public:
    /// Record a request's verb.
    void Request(std::uint8_t op, std::vector<std::byte> payload)
    {
        std::lock_guard lock { _mutex };
        _ops.push_back(op);
        _payloads.push_back(std::move(payload));
    }

    /// @return Every verb seen, in arrival order, across all connections.
    [[nodiscard]] std::vector<std::uint8_t> Ops() const
    {
        std::lock_guard lock { _mutex };
        return _ops;
    }

    /// @param index Which request.
    /// @return Its payload.
    [[nodiscard]] std::vector<std::byte> Payload(std::size_t index) const
    {
        std::lock_guard lock { _mutex };
        return index < _payloads.size() ? _payloads[index] : std::vector<std::byte> {};
    }

    std::atomic<int> accepted { 0 };    ///< Connections accepted.
    std::atomic<bool> sawEof { false }; ///< A script's read ended in EOF.

  private:
    mutable std::mutex _mutex;
    std::vector<std::uint8_t> _ops;
    std::vector<std::vector<std::byte>> _payloads;
};

/// What a script is given: its socket, what to record into, and which connection this is.
struct Peer
{
    core::net::ISocket* socket;
    Seen* seen;
    int index; ///< Zero for the first connection accepted.
};

using Script = core::async::Task<void> (*)(Peer);

/// Read one request, record it, and say its verb.
/// @return The verb, or nullopt on EOF.
core::async::Task<std::optional<std::uint8_t>> ReadRequest(Peer peer)
{
    auto const header = co_await core::net::receiveExactly(peer.socket, Wire::RequestHeaderSize);
    if (!header.has_value())
    {
        peer.seen->sawEof = true;
        co_return std::nullopt;
    }
    auto const decoded = Wire::DecodeRequestHeader(*header);
    if (!decoded.has_value())
        co_return std::nullopt;
    std::vector<std::byte> payload;
    if (decoded->payloadLength > 0)
    {
        auto received = co_await core::net::receiveExactly(peer.socket, decoded->payloadLength);
        if (!received.has_value())
            co_return std::nullopt;
        payload = std::move(*received);
    }
    peer.seen->Request(decoded->opRaw, std::move(payload));
    co_return decoded->opRaw;
}

/// The identity key every scripted grant names: a key's width, so the client reads it as one.
inline std::vector<std::byte> const GrantWorkerKey(Wire::IdentityPublicKeyBytes, std::byte { 0x4B });

/// @return A grant frame for a worker named @p endpoint.
[[nodiscard]] std::vector<std::byte> GrantFrame(std::string_view endpoint, std::uint64_t queuedMs = 7)
{
    return Wire::EncodeReply(Wire::Status::Ok,
                             Wire::EncodeAdmitGrant(Wire::AdmitGrant { .kind = Wire::GrantKind::Worker,
                                                                       .endpoint = endpoint,
                                                                       .leaseToken = "token",
                                                                       .workerCodecs = { 1, 2 },
                                                                       .chargedBytes = 4096,
                                                                       .queuedMs = queuedMs,
                                                                       .lifetime = std::chrono::milliseconds { 90'000 },
                                                                       .workerKey = GrantWorkerKey,
                                                                       .reason = Wire::LocalGrantReason::Asked }));
}

core::async::Task<void> Send(Peer peer, std::vector<std::byte> frame)
{
    std::ignore = co_await core::net::sendAll(peer.socket, frame);
}

// --- the scripts ------------------------------------------------------------------------------

core::async::Task<void> GrantAfterProgress(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, Wire::EncodeProgressReply());
    co_await Send(peer, Wire::EncodeProgressReply());
    co_await Send(peer, GrantFrame("worker-a:1"));
    co_await ReadRequest(peer); // Held: parked until the client ends it.
}

core::async::Task<void> OlderNode(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, Wire::EncodeErrorReply(Wire::UnimplementedVerb));
}

core::async::Task<void> Busy(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy));
}

core::async::Task<void> Mute(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await ReadRequest(peer); // Says nothing at all; parked until the client gives up.
}

/// Pulses every 60 ms for eight pulses -- longer than the idle bound the case sets -- then grants.
core::async::Task<void> SlowButAlive(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    for ([[maybe_unused]] auto const pulse: std::views::iota(0, 8))
    {
        co_await Send(peer, Wire::EncodeProgressReply());
        std::this_thread::sleep_for(60ms);
    }
    co_await Send(peer, GrantFrame("worker-slow:1"));
    co_await ReadRequest(peer);
}

/// Grants, then answers the next frame Ok whatever it is.
core::async::Task<void> GrantThenAckOne(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, GrantFrame("worker-a:1"));
    if (co_await ReadRequest(peer))
        co_await Send(peer, Wire::EncodeReply(Wire::Status::Ok, {}));
    co_await ReadRequest(peer);
}

/// Grants; on a SETTLE says Ok and, when an ADMIT follows on the same connection, grants again.
core::async::Task<void> GrantRequeueGrant(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, GrantFrame("worker-a:1"));
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, Wire::EncodeReply(Wire::Status::Ok, {}));
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, Wire::EncodeProgressReply());
    co_await Send(peer, GrantFrame("worker-b:2", 99));
    co_await ReadRequest(peer);
}

/// The first connection is told it is not the leader; the second is granted.
core::async::Task<void> NotLeaderOnce(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    if (peer.index == 0)
    {
        co_await Send(peer, Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader));
        co_return;
    }
    co_await Send(peer, GrantFrame("worker-a:1"));
    co_await ReadRequest(peer);
}

core::async::Task<void> NotLeaderAlways(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader));
}

/// Grants, reads the SETTLE and then keeps the connection open without answering it.
core::async::Task<void> GrantThenIgnoreSettle(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    co_await Send(peer, GrantFrame("worker-a:1"));
    if (co_await ReadRequest(peer))
        co_await ReadRequest(peer); // Parked until the client gives up.
}

/// Pulses until the client goes away; never answers.
core::async::Task<void> PulseOnly(Peer peer)
{
    if (!(co_await ReadRequest(peer)))
        co_return;
    for ([[maybe_unused]] auto const pulse: std::views::iota(0, 200))
    {
        std::this_thread::sleep_for(50ms);
        auto const sent = co_await core::net::sendAll(peer.socket, Wire::EncodeProgressReply());
        if (!sent)
            break;
    }
    co_await ReadRequest(peer); // EOF once the client is gone.
}

// --- the fake node ----------------------------------------------------------------------------

core::async::DetachedTask Serve(Script script, std::unique_ptr<core::net::ISocket> socket, Seen* seen, int index)
{
    co_await script(Peer { socket.get(), seen, index });
    socket->close();
}

core::async::DetachedTask AcceptLoop(core::net::IListener* listener, Script script, Seen* seen)
{
    while (true)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value())
            co_return;
        auto const index = seen->accepted.fetch_add(1);
        Serve(script, std::move(*accepted), seen, index);
    }
}

/// A node on a thread of its own, running one script per accepted connection.
class FakeNode
{
  public:
    explicit FakeNode(Script script):
        _script { script }
    {
        _thread = std::thread { [this] { Body(); } };
        std::unique_lock lock { _mutex };
        _cv.wait(lock, [this] { return _port != 0; });
    }

    ~FakeNode()
    {
        {
            std::lock_guard lock { _mutex };
            if (_loop != nullptr)
                _loop->post([this] {
                    _listener->close();
                    _loop->stop();
                });
        }
        _thread.join();
    }

    FakeNode(FakeNode const&) = delete;
    FakeNode(FakeNode&&) = delete;
    FakeNode& operator=(FakeNode const&) = delete;
    FakeNode& operator=(FakeNode&&) = delete;

    /// @return `127.0.0.1:<port>`.
    [[nodiscard]] std::string HostPort() const
    {
        return "127.0.0.1:" + std::to_string(_port);
    }

    Seen seen; ///< What it observed.

  private:
    void Body()
    {
        core::platform::SteadyClock clock;
        core::net::PlatformLoop loop { clock };
        auto listened = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1", .port = 0 });
        std::unique_ptr<core::net::IListener> listener = listened.has_value() ? std::move(*listened) : nullptr;
        {
            std::lock_guard lock { _mutex };
            // Published only with a listener: a node that could not listen has no loop to post to.
            _loop = listener != nullptr ? &loop : nullptr;
            _listener = listener.get();
            _port = listener != nullptr ? listener->boundPort() : 1;
        }
        _cv.notify_all();
        if (listener == nullptr)
            return;
        AcceptLoop(listener.get(), _script, &seen);
        loop.run();
        std::lock_guard lock { _mutex };
        _loop = nullptr;
    }

    Script _script;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    core::net::EventLoop* _loop { nullptr };
    core::net::IListener* _listener { nullptr };
    std::uint16_t _port { 0 };
};

constexpr auto Op = [](Wire::Op op) {
    return static_cast<std::uint8_t>(op);
};

/// The budget the cases use: a real connect bound and an idle bound short enough to test.
[[nodiscard]] Cc::ExchangeBudget BudgetWithIdle(std::chrono::milliseconds idle)
{
    auto budget = Cc::DefaultAdmitBudget();
    budget.idle = idle;
    return budget;
}

[[nodiscard]] std::unique_ptr<Cc::IAdmitSession> OpenAt(std::string const& hostPort, std::chrono::milliseconds idle = 5s)
{
    return Cc::OpenAdmitSession(hostPort,
                                Cc::Credential {},
                                Wire::AdmitRequest { .kind = Wire::AdmitKind::Dispatchable,
                                                     .fingerprint = "fp",
                                                     .cacheKey = "key",
                                                     .sourcePath = "a.cpp",
                                                     .treeLabel = "tree",
                                                     .storable = true,
                                                     .acceptedCodecs = { 1 },
                                                     .excluded = {} },
                                BudgetWithIdle(idle));
}

[[nodiscard]] bool SeenOps(FakeNode& node, std::size_t count)
{
    return Testing::WaitUntil(
        "the fake node to see the requests", [&] { return node.seen.Ops().size() >= count; }, [] { return std::string {}; });
}
} // namespace

TEST_CASE("AdmitSession.granted-after-progress")
{
    FakeNode node { &GrantAfterProgress };
    auto const before = core::platform::SteadyClock {}.now();
    auto session = OpenAt(node.HostPort());
    auto const ticket = session->Await();
    auto const after = core::platform::SteadyClock {}.now();

    REQUIRE(ticket.has_value());
    CHECK(ticket->kind == Wire::GrantKind::Worker);
    CHECK(ticket->endpoint == "worker-a:1");
    CHECK(ticket->leaseToken == "token");
    CHECK(ticket->workerCodecs == Wire::CodecList { 1, 2 });
    CHECK(ticket->chargedBytes == 4096);
    CHECK(ticket->queuedMs == 7);
    // What the dispatch bounds the compile by and authenticates its reply against, carried beside the
    // token rather than read out of it.
    CHECK(ticket->lifetime == std::chrono::milliseconds { 90'000 });
    CHECK(ticket->workerKey == GrantWorkerKey);
    // R2: the grant's age starts at its receipt on this side, between the two readings.
    CHECK(ticket->receivedAt >= before);
    CHECK(ticket->receivedAt <= after);
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit) });
}

TEST_CASE("AdmitSession.unreachable-fails-open")
{
    // Port 1 (tcpmux): below any ephemeral range, so no outbound connection of this host can hold it, and
    // nothing here serves it. A port released a moment ago would be free for another process to take.
    auto session = OpenAt("127.0.0.1:1");
    auto const ticket = session->Await();
    REQUIRE_FALSE(ticket.has_value());
    CHECK_FALSE(ticket.error().empty());
}

TEST_CASE("AdmitSession.older-node-fails-open")
{
    FakeNode node { &OlderNode };
    auto session = OpenAt(node.HostPort());
    auto const ticket = session->Await();
    REQUIRE_FALSE(ticket.has_value());
    CHECK(node.seen.accepted == 1); // Not a NotLeader: never retried.
}

TEST_CASE("AdmitSession.other-refusals-fail-open-without-retry")
{
    FakeNode node { &Busy };
    auto session = OpenAt(node.HostPort());
    REQUIRE_FALSE(session->Await().has_value());
    CHECK(node.seen.accepted == 1);
}

TEST_CASE("AdmitSession.silence-fails-open")
{
    FakeNode node { &Mute };
    auto session = OpenAt(node.HostPort(), 200ms);
    auto const started = std::chrono::steady_clock::now();
    auto const ticket = session->Await();
    auto const waited = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(ticket.has_value());
    CHECK(ticket.error().find("stopped reporting progress") != std::string::npos);
    CHECK(waited >= 150ms); // Not instant: the bound was waited out.
}

TEST_CASE("AdmitSession.progress-keeps-a-slow-queue-alive")
{
    // Eight pulses 60 ms apart is well over the 200 ms bound, which a fixed bound would end.
    FakeNode node { &SlowButAlive };
    auto session = OpenAt(node.HostPort(), 200ms);
    auto const ticket = session->Await();
    REQUIRE(ticket.has_value());
    CHECK(ticket->endpoint == "worker-slow:1");
}

TEST_CASE("AdmitSession.held-connection-has-no-idle-bound")
{
    // Past the idle bound with the grant held, the session is still usable: the bound ends with the
    // wait for the grant, not with the compile.
    FakeNode node { &GrantThenAckOne };
    auto session = OpenAt(node.HostPort(), 150ms);
    REQUIRE(session->Await().has_value());
    std::this_thread::sleep_for(400ms);
    session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::Stored, .peakBytes = std::nullopt });
    REQUIRE(SeenOps(node, 2));
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit), Op(Wire::Op::Settle) });
}

TEST_CASE("AdmitSession.destroy-without-settle-closes")
{
    FakeNode node { &GrantAfterProgress };
    {
        auto session = OpenAt(node.HostPort());
        REQUIRE(session->Await().has_value());
        CHECK_FALSE(node.seen.sawEof);
    }
    // The leader reads the end of the connection as an abandonment, so the node must see EOF.
    CHECK(Testing::WaitUntil(
        "the node to see the connection end", [&] { return node.seen.sawEof.load(); }, [] { return std::string {}; }));
}

TEST_CASE("AdmitSession.settle-is-sent-on-same-connection")
{
    FakeNode node { &GrantThenAckOne };
    auto session = OpenAt(node.HostPort());
    REQUIRE(session->Await().has_value());
    session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::Stored, .peakBytes = 123 });

    REQUIRE(SeenOps(node, 2));
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit), Op(Wire::Op::Settle) });
    CHECK(node.seen.accepted == 1);
    auto const settle = Wire::DecodeSettlePayload(node.seen.Payload(1));
    REQUIRE(settle.has_value());
    CHECK(settle->outcome == Wire::SettleOutcome::Stored);
    CHECK(settle->peakBytes == std::optional<std::uint64_t> { 123 });
}

TEST_CASE("AdmitSession.settle-without-a-grant-sends-nothing")
{
    FakeNode node { &OlderNode };
    auto session = OpenAt(node.HostPort());
    REQUIRE_FALSE(session->Await().has_value());
    session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::Failed, .peakBytes = std::nullopt });
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit) });
}

TEST_CASE("AdmitSession.not-leader-retried-once")
{
    FakeNode node { &NotLeaderOnce };
    auto session = OpenAt(node.HostPort());
    auto const ticket = session->Await();
    REQUIRE(ticket.has_value());
    CHECK(node.seen.accepted == 2);
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit), Op(Wire::Op::Admit) });
}

TEST_CASE("AdmitSession.not-leader-twice-fails-open")
{
    FakeNode node { &NotLeaderAlways };
    auto session = OpenAt(node.HostPort());
    REQUIRE_FALSE(session->Await().has_value());
    CHECK(node.seen.accepted == 2); // Once, never a third.
}

TEST_CASE("AdmitSession.requeue-asks-again-on-the-same-connection")
{
    FakeNode node { &GrantRequeueGrant };
    auto session = OpenAt(node.HostPort());
    auto const first = session->Await();
    REQUIRE(first.has_value());
    CHECK(first->endpoint == "worker-a:1");

    auto const second = session->Requeue();
    REQUIRE(second.has_value());
    CHECK(second->endpoint == "worker-b:2");
    CHECK(second->queuedMs == 99);
    CHECK(second->receivedAt >= first->receivedAt);

    CHECK(node.seen.accepted == 1);
    REQUIRE(SeenOps(node, 3));
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit), Op(Wire::Op::Settle), Op(Wire::Op::Admit) });
    auto const refused = Wire::DecodeSettlePayload(node.seen.Payload(1));
    REQUIRE(refused.has_value());
    CHECK(refused->outcome == Wire::SettleOutcome::WorkerRefused);
}

TEST_CASE("AdmitSession.requeue-without-a-grant-fails-open")
{
    FakeNode node { &OlderNode };
    auto session = OpenAt(node.HostPort());
    REQUIRE_FALSE(session->Await().has_value());
    CHECK_FALSE(session->Requeue().has_value());
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit) });
}

TEST_CASE("AdmitSession.settle-has-its-own-short-bound")
{
    // The node reads the SETTLE and never answers. The idle bound here is 5 s, which is what a SETTLE used to
    // wait under; it must now give up at `SettleBound`.
    FakeNode node { &GrantThenIgnoreSettle };
    auto session = OpenAt(node.HostPort(), 5s);
    REQUIRE(session->Await().has_value());

    auto const started = std::chrono::steady_clock::now();
    session->Settle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::Stored, .peakBytes = std::nullopt });
    auto const waited = std::chrono::steady_clock::now() - started;

    CHECK(waited < Cc::SettleBound + 1s);
    CHECK(waited >= Cc::SettleBound - 500ms); // It waited the bound out rather than failing at once.
    CHECK(node.seen.Ops() == std::vector<std::uint8_t> { Op(Wire::Op::Admit), Op(Wire::Op::Settle) });
}

TEST_CASE("AdmitSession.destroy-while-queued-is-prompt")
{
    FakeNode node { &PulseOnly };
    std::chrono::steady_clock::duration took {};
    {
        auto session = OpenAt(node.HostPort(), 5s);
        REQUIRE(SeenOps(node, 1)); // The ADMIT is in flight, and the node only pulses.
        auto const started = std::chrono::steady_clock::now();
        session.reset();
        took = std::chrono::steady_clock::now() - started;
    }
    CHECK(took < 2s); // Far under the 5 s idle bound.
    CHECK(Testing::WaitUntil(
        "the node to see the connection end", [&] { return node.seen.sawEof.load(); }, [] { return std::string {}; }));
}
