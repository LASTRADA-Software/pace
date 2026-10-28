// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdmissionResponder.hpp"
#include "AdmitForwarder.hpp"
#include "FrameEndpoint.hpp"
#include "HeldCharges.hpp"
#include "NodeConfig.hpp"
#include "NodeIoLoop.hpp"
#include "NodeProofClient.hpp"
#include "NodeSurfaces.hpp"
#include "Responders.hpp"
#include "SchedulingRedirect.hpp"

#include <Pace/Core/ISecureRandom.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/LeaseSigner.hpp>
#include <Pace/Distributed/MembershipOracle.hpp>
#include <Pace/Distributed/SchedulerProtocol.hpp>
#include <Pace/Distributed/SchedulerService.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/ConnectionScope.hpp>
#include <Pace/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <EndpointDial.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/net/ISocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using Pace::Testing::UnwrapMutable;

/// @file AdmissionTestUtils.hpp
/// The leader's side of the fleet compile queue as a test stands it up -- a scheduler, its queue, a real
/// loopback listener serving it, and a blocking client -- shared by the cases of the leader's responder
/// and of the forwarder that relays to it, so neither carries a copy of the other's fake leader.
namespace Pace::Node::AdmissionFixture
{

using namespace std::chrono_literals;
namespace Wire = Pace::CompileCacheWire;

using Pace::Testing::Unwrap;

/// A port nothing is listening on right now.
[[nodiscard]] inline std::uint16_t FreePort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    return probe->boundPort();
}

/// Read until @p received holds @p count bytes; false when the peer closed or the deadline ran out first.
[[nodiscard]] inline core::async::Task<bool> ReadAtLeast(core::net::ISocket* peer,
                                                         std::vector<std::byte>* received,
                                                         std::size_t count)
{
    while (received->size() < count)
    {
        std::array<std::byte, 4096> chunk {};
        auto const want = std::min(chunk.size(), count - received->size());
        auto const read = co_await peer->read(std::span<std::byte> { chunk.data(), want });
        if (!read.has_value() || *read == 0)
            co_return false;
        received->insert(received->end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));
    }
    co_return true;
}

/// One reply frame: its status and payload, or nothing when the peer closed.
struct Reply
{
    Wire::Status status {};
    std::vector<std::byte> payload;
    std::vector<std::byte> bytes; ///< The whole frame.
};

[[nodiscard]] inline core::async::Task<std::optional<Reply>> ReadReply(core::net::ISocket* peer)
{
    std::vector<std::byte> received;
    if (!co_await ReadAtLeast(peer, &received, Wire::ReplyHeaderSize))
        co_return std::nullopt;
    auto const header = Wire::DecodeReplyHeader(received);
    if (!header.has_value() || !co_await ReadAtLeast(peer, &received, Wire::ReplyHeaderSize + header->payloadLength))
        co_return std::nullopt;
    co_return Reply { .status = header->status,
                      .payload = { received.begin() + Wire::ReplyHeaderSize, received.end() },
                      .bytes = received };
}

[[nodiscard]] inline core::async::Task<bool> SendAll(core::net::ISocket* peer, std::vector<std::byte> bytes)
{
    auto const written = co_await peer->write(std::span<std::byte const> { bytes });
    co_return written.has_value() && *written == bytes.size();
}

/// One connection a case holds open.
class Client
{
  public:
    explicit Client(std::uint16_t port)
    {
        auto socket =
            core::async::syncRun(_connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
        REQUIRE(socket.has_value());
        _socket = std::move(*socket);
        // Bounded: a build that never answers fails the case, never hangs it.
        _socket->setReceiveDeadline(10s);
    }

    Client(Client const&) = delete;
    Client& operator=(Client const&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    ~Client()
    {
        Close();
    }

    /// Send a frame without waiting for its reply.
    void Send(std::vector<std::byte> frame)
    {
        auto const sent = core::async::syncRun(SendAll(_socket.get(), std::move(frame)));
        REQUIRE(sent);
    }

    /// @return The next frame, or nothing when the peer closed or stayed silent past the deadline.
    [[nodiscard]] std::optional<Reply> Read()
    {
        return core::async::syncRun(ReadReply(_socket.get()));
    }

    /// Read frames until one is not a pulse, for at most ten seconds.
    ///
    /// Bounded as a whole and not only per read: a request parked forever is pulsed forever, so a
    /// per-read deadline never fires and a broken build would hang the case instead of failing it.
    /// @param pulses Counts the pulses read on the way.
    /// @return That frame, or nothing when the peer closed or only pulsed until the bound.
    [[nodiscard]] std::optional<Reply> ReadAnswer(std::size_t* pulses = nullptr)
    {
        auto const until = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < until)
        {
            auto reply = Read();
            if (!reply.has_value() || reply->status != Wire::Status::Progress)
                return reply;
            if (pulses != nullptr)
                ++*pulses;
        }
        return std::nullopt;
    }

    /// Hang up now.
    void Close()
    {
        if (_socket)
            _socket->close();
    }

  private:
    core::net::BlockingConnector _connector;
    std::unique_ptr<core::net::ISocket> _socket;
};

/// A wire frame asking to be admitted.
[[nodiscard]] inline std::vector<std::byte> AdmitFrame(Wire::AdmitKind kind, std::string_view key, bool storable = false)
{
    auto const excluded = std::array<std::string_view, 0> {};
    return Wire::EncodeAdmit(Wire::AdmitRequest { .kind = kind,
                                                  .fingerprint = kind == Wire::AdmitKind::Dispatchable ? "gcc-14" : "",
                                                  .cacheKey = key,
                                                  .sourcePath = "src/a.cpp",
                                                  .treeLabel = "tree",
                                                  .storable = storable,
                                                  .acceptedCodecs = {},
                                                  .excluded = excluded,
                                                  .toolchainLabel = {} });
}

[[nodiscard]] inline std::vector<std::byte> SettleFrame(Wire::SettleOutcome outcome,
                                                        std::optional<std::uint64_t> peak = std::nullopt)
{
    return Wire::EncodeSettle(Wire::SettleRequest { .outcome = outcome, .peakBytes = peak });
}

inline constexpr std::uint64_t MiB = 1024ULL * 1024ULL;

/// The leader, its queue, a loopback listener serving it, and the machine budget the cases report.
struct Queue
{
    explicit Queue(std::chrono::milliseconds pulse = 20ms, std::uint64_t defaultEstimate = 64 * MiB)
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        admission.emplace(service, fold.admitted, io->Reactor(), metrics, &held, pulse);
        responder.ServeAdmissionWith(*admission);
        (void) defaultEstimate;
    }

    /// Start serving on a fresh port.
    [[nodiscard]] std::uint16_t Serve()
    {
        auto const port = FreePort();
        NodeConfig cfg;
        cfg.*RowFor(NodeSurface::Node).spec = std::format("127.0.0.1:{}", port);
        auto started = FrameEndpoint::Start(*io, NodeSurface::Node, cfg, responder, metrics, logger);
        REQUIRE(started.has_value());
        endpoint = std::move(*started);
        io->Start();
        return port;
    }

    /// This machine states its budget, as a proven peer would send it. Pumps, so whatever waited is decided.
    void Report(std::uint64_t budgetBytes, std::uint32_t slots)
    {
        auto const frame = Wire::EncodeMachineBudget(Wire::MachineBudgetReport {
            .memoryBudgetBytes = budgetBytes, .slots = slots, .heldSlots = 0, .heldBytes = 0, .peaks = {} });
        auto reply = core::async::syncRun(UnwrapMutable(admission).Answer(
            frame,
            PeerIdentity { .host = "10.0.0.2", .proven = ProvenIdentity { .id = "scheduler", .key = signer.PublicKey() } }));
        REQUIRE(Pace::Testing::StatusOf(reply.bytes) == Wire::Status::Ok);
    }

    /// Register one worker on another machine, which proved its key, for the `gcc-14` toolchain.
    void RegisterWorker(std::uint32_t slots)
    {
        auto const reply =
            service.Register(Distributed::CallerContext { .membership = Distributed::Membership::Member,
                                                          .peerId = "10.0.0.2",
                                                          .provenNodeId = "worker-node",
                                                          .provenKey = workerKey.PublicKey(),
                                                          .identified = true },
                             Distributed::WorkerRegistration {
                                 .fingerprint = "gcc-14", .endpoint = WorkerEndpoint, .slots = slots, .codecs = {} });
        REQUIRE(reply.status == Wire::Status::Ok);
    }

    /// The worker's machine states its budget over the wire, as the proven peer it is. Pumps.
    void ReportWorker(std::uint64_t budgetBytes = 1024 * MiB, std::uint32_t slots = 4)
    {
        ReportFor("admission-worker", budgetBytes, slots);
    }

    /// The machine named @p machine states its budget over the wire, as the proven peer it is. Pumps.
    /// @param machine The id whose test key the report is proven under; live in `fold`.
    /// @param budgetBytes Its memory budget.
    /// @param slots Its compile slots.
    void ReportFor(std::string_view machine, std::uint64_t budgetBytes = 1024 * MiB, std::uint32_t slots = 4)
    {
        auto const frame = Wire::EncodeMachineBudget(Wire::MachineBudgetReport {
            .memoryBudgetBytes = budgetBytes, .slots = slots, .heldSlots = 0, .heldBytes = 0, .peaks = {} });
        auto reply = core::async::syncRun(UnwrapMutable(admission).Answer(
            frame,
            PeerIdentity { .host = "10.0.0.9",
                           .proven = ProvenIdentity { .id = std::string { machine },
                                                      .key = Testing::TestKeyPair(std::string { machine }).PublicKey() } }));
        REQUIRE(Pace::Testing::StatusOf(reply.bytes) == Wire::Status::Ok);
    }

    static constexpr std::string_view WorkerEndpoint = "10.0.0.2:7100";

    /// A machine elsewhere that proves its key on its connection.
    static constexpr std::string_view RemoteClient = "remote-client";

    /// A machine elsewhere whose node relays its launchers' admissions here, proving ITS key.
    static constexpr std::string_view ForwardingMachine = "admission-follower";

    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Ed25519KeyPair const workerKey = Testing::TestKeyPair("admission-worker");
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {}, 64 * MiB };
    Distributed::SchedulerProtocol protocol { service, metrics };
    /// The production fold -- loopback, then the cluster's key roster -- over the machines these cases
    /// name: this node (`scheduler`), the worker's machine, a remote client that proves its key, and a
    /// node that relays its launchers' admissions.
    Testing::RosterFold fold {
        { "scheduler", "admission-worker", std::string { RemoteClient }, std::string { ForwardingMachine } }
    };
    SchedulerResponder responder { protocol, fold.admitted, metrics };
    HeldChargeRegistry held;
    std::optional<AdmissionResponder> admission;

    /// Declared LAST so it is destroyed FIRST: the reactor goes before the responders its parked frames
    /// reach, which is the order a node tears down in. A pointer so a case can end it on purpose.
    std::unique_ptr<NodeIoLoop> io = std::make_unique<NodeIoLoop>();

    /// The listener a case serves, declared AFTER the reactor so it is destroyed BEFORE it: its
    /// shutdown posts closes onto a reactor that must still be turning.
    std::unique_ptr<FrameEndpoint> endpoint;
};

/// Wait, on the case's own thread and bounded, for the queue to reach the state a case expects.
/// @param reached True once it has.
/// @return Whether it did within the bound.
template <std::predicate Predicate>
[[nodiscard]] bool WaitFor(Predicate reached)
{
    return Pace::Testing::WaitUntil(
        "the admission queue to reach the state the case expects", std::move(reached), [] { return std::string {}; });
}

[[nodiscard]] inline Wire::AdmitGrantFields GrantOf(Reply const& reply)
{
    REQUIRE(reply.status == Wire::Status::Ok);
    return Unwrap(Wire::DecodeAdmitGrant(reply.payload));
}

[[nodiscard]] inline std::uint64_t Count(Queue const& queue, IMetricsSink::Counter counter)
{
    return queue.metrics.Read(counter);
}

/// The follower-side pieces -- a node that relays its launchers' admissions to the leader -- shared by the
/// forwarder's cases and the fleet's end-to-end ones.
/// Dials the way production does -- from the loop, over a real socket -- minus the proof, which a
/// loopback leader does not ask for, and records every endpoint it was asked to dial.
class RecordingDialer final: public IUpstreamDialer
{
  public:
    /// Where connections are made; set once the node's loop exists.
    /// @param io The follower's loop.
    void Attach(NodeIoLoop& io) noexcept
    {
        _io = &io;
    }

    /// @copydoc IUpstreamDialer::Open
    [[nodiscard]] core::async::Task<UpstreamOpening> Open(std::string endpoint) override
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _dialled.push_back(endpoint);
        }
        auto socket =
            co_await Cc::DialEndpoint(&_io->Connector(), endpoint, core::net::DialOptions { .connectTimeout = 1s });
        if (socket == nullptr)
            co_return UpstreamOpening { .socket = nullptr, .reason = "nothing answered" };
        co_return UpstreamOpening { .socket = std::move(socket), .reason = {} };
    }

    /// @return Every endpoint dialled so far, in order.
    [[nodiscard]] std::vector<std::string> Dialled() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _dialled;
    }

  private:
    NodeIoLoop* _io { nullptr };
    mutable std::mutex _mutex;
    std::vector<std::string> _dialled;
};

/// The `0xFC` endpoint a node announces, as a case states it. Guarded: a case states it while the node's
/// reactor may already read it.
class StatedEndpoint final: public Cc::IAdvertisedEndpointSource
{
  public:
    /// @param endpoint What this node announces from now on.
    void Set(std::string endpoint)
    {
        auto const guard = std::scoped_lock { _mutex };
        _endpoint = std::move(endpoint);
    }

    /// @copydoc Cc::IAdvertisedEndpointSource::Current
    [[nodiscard]] std::string Current() const override
    {
        auto const guard = std::scoped_lock { _mutex };
        return _endpoint;
    }

  private:
    mutable std::mutex _mutex;
    std::string _endpoint;
};

/// The cadence these cases run a node at: a pulse every 20 ms, and the wire's own bound on the leader.
inline constexpr ForwardPolicy QuickPulse { .progressInterval = 20ms };

/// The leader as every case's fixture names it, and the identity it proves.
inline constexpr std::string_view LeaderMachine = "scheduler";

/// A node that runs no scheduler -- a learner -- with the forwarder wired, serving on a loopback port.
struct Follower
{
    /// @param leaderEndpoint What the node believes the leader's scheduling endpoint to be.
    /// @param policy Its pulse and its bound on the leader's silence.
    /// @param provingAs The machine it proves itself as, through the production dialer
    ///        (`ProvenSchedulerDialer`); nothing to dial plainly and record what it dials.
    explicit Follower(std::string_view leaderEndpoint,
                      ForwardPolicy policy = QuickPulse,
                      std::optional<std::string_view> provingAs = std::nullopt)
    {
        knownLeader.Publish(leaderEndpoint);
        dialer.Attach(*io);
        IUpstreamDialer* upstream = &dialer;
        if (provingAs.has_value())
        {
            key.emplace(Testing::TestKeyPair(std::string { *provingAs }));
            proof.emplace(std::string { *provingAs }, *key, leaderTrust, nullptr, nullptr, random);
            provenDialer.emplace(*proof, io->Connector(), io->Reactor());
            upstream = &*provenDialer;
        }
        forwarder.emplace(knownLeader, *upstream, io->Reactor(), metrics, &held, &self, policy);
        redirect.ForwardAdmissionWith(*forwarder);
    }

    /// Start serving on a fresh port.
    /// @return The port.
    [[nodiscard]] std::uint16_t Serve()
    {
        auto const port = FreePort();
        NodeConfig cfg;
        cfg.*RowFor(NodeSurface::Node).spec = std::format("127.0.0.1:{}", port);
        auto started = FrameEndpoint::Start(*io, NodeSurface::Node, cfg, redirect, metrics, logger);
        REQUIRE(started.has_value());
        endpoint = std::move(*started);
        io->Start();
        return port;
    }

    AtomicMetricsSink metrics;
    NullLogger logger;
    Distributed::LoopbackMembership membership;
    KnownSchedulingLeader knownLeader;
    HeldChargeRegistry held;
    StatedEndpoint self;
    RecordingDialer dialer;
    /// Whom the proving dialer proves to: exactly the leader, under its key -- production's own trust type.
    NamedMachineTrust leaderTrust { std::string { LeaderMachine },
                                    Testing::TestKeyPair(std::string { LeaderMachine }).PublicKey() };
    SystemSecureRandom random;
    std::optional<Ed25519KeyPair> key;
    std::optional<NodeProofClient> proof;
    std::optional<ProvenSchedulerDialer> provenDialer;
    SchedulingRedirectResponder redirect { membership, knownLeader };
    std::optional<AdmitForwarder> forwarder;

    /// Declared LAST so it is destroyed FIRST, as the leader fixture's is: the reactor goes before the
    /// responders its parked frames reach.
    std::unique_ptr<NodeIoLoop> io = std::make_unique<NodeIoLoop>();
    std::unique_ptr<FrameEndpoint> endpoint;
};

/// @param port A loopback port.
/// @return The scheduling endpoint a leader serving there answers at.
[[nodiscard]] inline std::string EndpointOf(std::uint16_t port)
{
    return std::format("127.0.0.1:{}", port);
}

} // namespace Pace::Node::AdmissionFixture
