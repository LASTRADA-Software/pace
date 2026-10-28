// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdmissionResponder.hpp"
#include "FrameEndpoint.hpp"
#include "HeldCharges.hpp"
#include "NodeConfig.hpp"
#include "NodeIoLoop.hpp"
#include "NodeSurfaces.hpp"
#include "Responders.hpp"

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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/async/SyncRun.hpp>
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
        REQUIRE(core::async::syncRun(SendAll(_socket.get(), std::move(frame))));
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
                                                  .excluded = excluded });
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
        auto reply = core::async::syncRun(admission->Answer(
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
        auto const frame = Wire::EncodeMachineBudget(Wire::MachineBudgetReport {
            .memoryBudgetBytes = budgetBytes, .slots = slots, .heldSlots = 0, .heldBytes = 0, .peaks = {} });
        auto reply = core::async::syncRun(admission->Answer(
            frame,
            PeerIdentity { .host = "10.0.0.2",
                           .proven = ProvenIdentity { .id = "admission-worker", .key = workerKey.PublicKey() } }));
        REQUIRE(Pace::Testing::StatusOf(reply.bytes) == Wire::Status::Ok);
    }

    static constexpr std::string_view WorkerEndpoint = "10.0.0.2:7100";

    /// A machine elsewhere that proves its key on its connection.
    static constexpr std::string_view RemoteClient = "remote-client";

    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Ed25519KeyPair const workerKey = Testing::TestKeyPair("admission-worker");
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {}, 64 * MiB };
    Distributed::SchedulerProtocol protocol { service, metrics };
    /// The production fold -- loopback, then the cluster's key roster -- over the machines these cases
    /// name: this node (`scheduler`), the worker's machine, and a remote client that proves its key.
    Testing::RosterFold fold { { "scheduler", "admission-worker", std::string { RemoteClient } } };
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
} // namespace Pace::Node::AdmissionFixture
