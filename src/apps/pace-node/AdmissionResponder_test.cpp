// SPDX-License-Identifier: Apache-2.0
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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <core/async/DetachedTask.hpp>
#include <core/async/SyncRun.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/net/ISocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace Pace;
using namespace Pace::Node;
using namespace std::chrono_literals;

namespace Wire = Pace::CompileCacheWire;

namespace
{
using Pace::Testing::Unwrap;

/// Membership this machine answers for, which a case can take away: what a `--cluster-forget` landing
/// while a request is parked looks like to the gate. A proven key is always live, so a machine can report.
class FlippableMembership final: public Distributed::IMembershipOracle
{
  public:
    [[nodiscard]] Distributed::MembershipDecision Explain(std::string_view peerAddress) const override
    {
        if (_revoked.load() || !IsLoopbackHost(peerAddress))
            return {};
        return Distributed::DecidedBy(Distributed::Membership::Member, Distributed::MembershipParticipant::Loopback);
    }

    [[nodiscard]] Distributed::MembershipDecision ExplainKey(ProvenIdentity const& /*identity*/,
                                                             Distributed::KeyEvidence /*evidence*/) const override
    {
        return Distributed::DecidedBy(Distributed::Membership::Member, Distributed::MembershipParticipant::ProvenIdentity);
    }

    [[nodiscard]] std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view /*id*/) const override
    {
        return std::nullopt;
    }

    /// Take this machine's membership away.
    void Revoke() noexcept
    {
        _revoked.store(true);
    }

  private:
    std::atomic<bool> _revoked { false };
};

/// A port nothing is listening on right now.
[[nodiscard]] std::uint16_t FreePort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    return probe->boundPort();
}

/// Read until @p received holds @p count bytes; false when the peer closed or the deadline ran out first.
[[nodiscard]] core::async::Task<bool> ReadAtLeast(core::net::ISocket* peer,
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

[[nodiscard]] core::async::Task<std::optional<Reply>> ReadReply(core::net::ISocket* peer)
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

[[nodiscard]] core::async::Task<bool> SendAll(core::net::ISocket* peer, std::vector<std::byte> bytes)
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

    /// Read frames until one is not a pulse.
    /// @param pulses Counts the pulses read on the way.
    /// @return That frame.
    [[nodiscard]] std::optional<Reply> ReadAnswer(std::size_t* pulses = nullptr)
    {
        for (;;)
        {
            auto reply = Read();
            if (!reply.has_value() || reply->status != Wire::Status::Progress)
                return reply;
            if (pulses != nullptr)
                ++*pulses;
        }
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
[[nodiscard]] std::vector<std::byte> AdmitFrame(Wire::AdmitKind kind, std::string_view key, bool storable = false)
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

[[nodiscard]] std::vector<std::byte> SettleFrame(Wire::SettleOutcome outcome,
                                                 std::optional<std::uint64_t> peak = std::nullopt)
{
    return Wire::EncodeSettle(Wire::SettleRequest { .outcome = outcome, .peakBytes = peak });
}

constexpr std::uint64_t MiB = 1024ULL * 1024ULL;

/// The leader, its queue, a loopback listener serving it, and the machine budget the cases report.
struct Queue
{
    explicit Queue(std::chrono::milliseconds pulse = 20ms, std::uint64_t defaultEstimate = 64 * MiB)
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        admission.emplace(service, membership, io->Reactor(), metrics, &held, pulse);
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
            PeerIdentity { .host = "10.0.0.2", .proven = ProvenIdentity { .id = "self", .key = signer.PublicKey() } }));
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
                           .proven = ProvenIdentity { .id = "worker-node", .key = workerKey.PublicKey() } }));
        REQUIRE(Pace::Testing::StatusOf(reply.bytes) == Wire::Status::Ok);
    }

    static constexpr std::string_view WorkerEndpoint = "10.0.0.2:7100";

    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Ed25519KeyPair const workerKey = Testing::TestKeyPair("admission-worker");
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {}, 64 * MiB };
    Distributed::SchedulerProtocol protocol { service, metrics };
    FlippableMembership membership;
    SchedulerResponder responder { protocol, membership, metrics };
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

[[nodiscard]] Wire::AdmitGrantFields GrantOf(Reply const& reply)
{
    REQUIRE(reply.status == Wire::Status::Ok);
    return Unwrap(Wire::DecodeAdmitGrant(reply.payload));
}

[[nodiscard]] std::uint64_t Count(Queue const& queue, IMetricsSink::Counter counter)
{
    return queue.metrics.Read(counter);
}
} // namespace

TEST_CASE("AdmissionResponder.grants-immediately-with-room", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(1024 * MiB, 2);

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());
    auto const grant = GrantOf(reply);
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(grant.chargedBytes == 64 * MiB);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedLocal) == 1);
    // A loopback local lease is held in the machine's own report for as long as it lives (Ruling 4).
    CHECK(queue.held.Held() == HeldCharges { .slots = 1, .bytes = 64 * MiB });
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.size() == 1);

    // The connection is the lease: it stays open however quiet the client is -- past the bound at which
    // the endpoint gives up on a watched reply -- and the settle gives the charge back.
    std::this_thread::sleep_for(FrameServer::RefusalTimeout + 300ms);
    client.Send(SettleFrame(Wire::SettleOutcome::Stored, 10 * MiB));
    CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    CHECK(WaitFor([&] { return queue.held.Held() == HeldCharges {}; }));
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 0);
}

TEST_CASE("AdmissionResponder.pulses-while-parked", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));

    // No machine has reported, so nothing can be admitted: the request parks, and the endpoint tells
    // the client the leader is still there. Read BEFORE anything is decided, so a build that never
    // pulses cannot pass by the grant arriving first.
    constexpr std::size_t PulsesBeforeGrant = 2;
    for ([[maybe_unused]] auto const pulse: std::views::iota(std::size_t { 0 }, PulsesBeforeGrant))
        CHECK(Unwrap(client.Read()).status == Wire::Status::Progress);

    // The injected clock moves 12 s while it waits; the grant reports it.
    queue.clock.advance(12s);
    queue.Report(1024 * MiB, 2);

    std::size_t morePulses = 0;
    auto const grant = GrantOf(Unwrap(client.ReadAnswer(&morePulses)));
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(grant.queuedMs == 12'000);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionWaitMillisTotal) == 12'000);
}

TEST_CASE("AdmissionResponder.close-while-parked-releases", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    {
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        REQUIRE(WaitFor([&] { return queue.service.AdmissionSnapshotNow().depth == 1; }));
        client.Close();
    }
    CHECK(WaitFor([&] { return queue.service.AdmissionSnapshotNow().depth == 0; }));
    CHECK(WaitFor([&] { return Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1; }));
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedLocal) == 0);
}

TEST_CASE("AdmissionResponder.close-after-grant-releases-and-raises-estimate", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(1024 * MiB, 4);

    // A measured peak teaches the estimate a small figure for this key...
    {
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).chargedBytes == 64 * MiB);
        client.Send(SettleFrame(Wire::SettleOutcome::Failed, 8 * MiB));
        REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    }
    // ...which the next grant is charged...
    {
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).chargedBytes == 8 * MiB);
        // ...and a client that dies holding it (`kill -9`) gives the charge back and is taught as killed.
        client.Close();
    }
    REQUIRE(WaitFor([&] { return Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1; }));
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(queue.held.Held() == HeldCharges {});

    // A compile that was killed has no peak to teach: its figure goes back up to at least the default.
    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).chargedBytes == 64 * MiB);
}

TEST_CASE("AdmissionResponder.settle-stored-wakes-waiter", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(1024 * MiB, 4);

    Client first { port };
    first.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "same-key", /*storable=*/true));
    REQUIRE(GrantOf(Unwrap(first.ReadAnswer())).kind == Wire::GrantKind::Local);

    // The second asks for the same object: it waits for the first to STORE it, whatever room there is.
    Client second { port };
    second.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "same-key", /*storable=*/true));
    std::size_t pulses = 0;
    // A pulse proves it is parked rather than answered; the grant must not arrive before the store.
    CHECK(Unwrap(second.Read()).status == Wire::Status::Progress);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionDeduplicated) == 0);

    first.Send(SettleFrame(Wire::SettleOutcome::Stored, 20 * MiB));
    REQUIRE(Unwrap(first.ReadAnswer()).status == Wire::Status::Ok);

    auto const woken = GrantOf(Unwrap(second.ReadAnswer(&pulses)));
    CHECK(woken.kind == Wire::GrantKind::Stored);
    CHECK(woken.chargedBytes == 0);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionDeduplicated) == 1);
}

TEST_CASE("AdmissionResponder.regate-at-grant", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(Unwrap(client.Read()).status == Wire::Status::Progress);

    // A `--cluster-forget` lands during the wait; then the room appears. The grant is a refusal, and
    // it holds nothing.
    queue.membership.Revoke();
    queue.Report(1024 * MiB, 2);

    auto const reply = Unwrap(client.ReadAnswer());
    CHECK(reply.status == Wire::Status::Error);
    CHECK(Pace::Testing::ErrorOf(reply.bytes) == Wire::ErrorCode::NotAMember);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedLocal) == 0);
}

TEST_CASE("AdmissionResponder.demotion-answers-not-leader", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(Unwrap(client.Read()).status == Wire::Status::Progress);

    queue.service.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:7000", 7);

    auto const reply = Unwrap(client.ReadAnswer());
    CHECK(reply.status == Wire::Status::Error);
    CHECK(Pace::Testing::ErrorOf(reply.bytes) == Wire::ErrorCode::NotLeader);
    auto const text = std::string { reinterpret_cast<char const*>(reply.payload.data()), reply.payload.size() };
    CHECK(text.find("10.0.0.9:7000") != std::string::npos);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
}

TEST_CASE("AdmissionResponder.machine-budget-report-enables-admission", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(Unwrap(client.Read()).status == Wire::Status::Progress);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 1);

    // A machine that has not reported admits nothing; its report is what opens the door.
    queue.Report(1024 * MiB, 2);
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
}

TEST_CASE("AdmissionResponder.a-frame-other-than-settle-abandons", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(1024 * MiB, 2);

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);

    // The next frame is another admission, not the settle: the first grant is abandoned before it is answered.
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k2"));
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.size() == 1);

    // And a frame of ANY other verb does the same, before that frame is answered: the charge is not
    // held across a request that is not its settle. A lease for a toolchain nobody serves is refused, which
    // is enough -- what is checked is that the charge went first.
    client.Send(Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} }));
    CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Error);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 2);
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
}

namespace
{
/// Drive one `ADMIT` straight at the responder, on the calling thread until it first parks.
///
/// A `DetachedTask` begins eagerly, so its frame is parked on the reactor when this returns -- the
/// state a reactor torn down mid-wait finds it in.
core::async::DetachedTask DriveAdmit(AdmissionResponder* responder, std::vector<std::byte> frame, PeerIdentity peer)
{
    auto const reply = co_await responder->Answer(frame, std::move(peer));
    (void) reply;
}
} // namespace

TEST_CASE("AdmissionResponder.parked-frame-freed-on-teardown", "[node][admission][teardown]")
{
    // **The reactor frees what it parks.** A request parked in the queue is a coroutine frame the
    // reactor holds; when the reactor goes with it still there the frame must be FREED -- which runs the
    // guard that gives the ticket back -- and never resumed or leaked. Asserted on the ticket (the
    // service sees none left) and, under ASan, on the allocation (LeakSanitizer sees none left).
    auto queue = std::make_unique<Queue>();
    auto const scope = std::make_shared<ConnectionScope>();

    // Before the reactor thread exists, so the park lands on a loop nobody else is turning.
    DriveAdmit(&*queue->admission,
               AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"),
               PeerIdentity { .host = "127.0.0.1", .scope = scope });
    REQUIRE(queue->service.AdmissionSnapshotNow().depth == 1);

    // The reactor goes with the request still parked, as a node's teardown takes it.
    queue->io.reset();
    CHECK(queue->service.AdmissionSnapshotNow().depth == 0);
    CHECK(queue->service.AdmissionSnapshotNow().chargedBytes.empty());
}

TEST_CASE("AdmissionResponder.dispatchable-grant-names-the-worker-and-its-lease", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.RegisterWorker(2);
    queue.ReportWorker();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
    auto const grant = GrantOf(Unwrap(client.ReadAnswer()));
    CHECK(grant.kind == Wire::GrantKind::Worker);
    CHECK(grant.endpoint == Queue::WorkerEndpoint);
    CHECK_FALSE(grant.leaseToken.empty());
    CHECK(queue.service.OutstandingLeases(4).total == 1);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedWorker) == 1);
    // A worker grant is no charge on this machine's own report: only a loopback LOCAL lease is.
    CHECK(queue.held.Held() == HeldCharges {});

    client.Send(SettleFrame(Wire::SettleOutcome::Stored, 10 * MiB));
    CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    CHECK(WaitFor([&] { return queue.service.OutstandingLeases(4).total == 0; }));
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
}

TEST_CASE("AdmissionResponder.worker-refused-readmits-on-the-same-ticket", "[node][admission]")
{
    // Ruling 9: a worker's refusal puts the SAME ticket back at the FRONT, and the connection's next
    // ADMIT waits on it -- no second ticket, no place at the back behind a request that came later.
    Queue queue;
    auto const port = queue.Serve();
    queue.RegisterWorker(1);
    queue.ReportWorker(1024 * MiB, 1);

    Client first { port };
    first.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "ka"));
    REQUIRE(GrantOf(Unwrap(first.ReadAnswer())).kind == Wire::GrantKind::Worker);

    // A later request waits behind the one slot the machine offers.
    Client later { port };
    later.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "kb"));
    REQUIRE(Unwrap(later.Read()).status == Wire::Status::Progress);
    REQUIRE(Count(queue, IMetricsSink::Counter::AdmissionQueued) == 2);

    first.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
    CHECK(Unwrap(first.ReadAnswer()).status == Wire::Status::Ok);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionWorkerRefused) == 1);
    // Both wait now: the refused one went back, it did not leave.
    CHECK(queue.service.AdmissionSnapshotNow().depth == 2);

    first.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "ka"));
    REQUIRE(Unwrap(first.Read()).status == Wire::Status::Progress);
    // Bound to the ticket it had: nothing new was queued.
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionQueued) == 2);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 2);

    // The machine reports again, and the refused request -- at the front -- takes the slot.
    queue.ReportWorker(1024 * MiB, 1);
    CHECK(GrantOf(Unwrap(first.ReadAnswer())).kind == Wire::GrantKind::Worker);
    CHECK(Unwrap(later.Read()).status == Wire::Status::Progress);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 1);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 0);
}

TEST_CASE("AdmissionResponder.refusal-bound-reached-over-the-wire-grants-local", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.RegisterWorker(1);
    queue.ReportWorker();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);

    for (auto const round: std::views::iota(1U, Distributed::AdmissionQueue::MaxWorkerRefusals + 1))
    {
        client.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
        REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
        client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
        REQUIRE(Unwrap(client.Read()).status == Wire::Status::Progress);
        queue.ReportWorker();
        if (round < Distributed::AdmissionQueue::MaxWorkerRefusals)
            REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);
    }

    // The bound is reached: the worker that reported is not tried again, and the request waits for its
    // own machine, which has not reported yet -- a pulse, not a grant.
    CHECK(Unwrap(client.Read()).status == Wire::Status::Progress);
    queue.Report(1024 * MiB, 2);
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);

    CHECK(Count(queue, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionWorkerRefused) == Distributed::AdmissionQueue::MaxWorkerRefusals);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedWorker) == Distributed::AdmissionQueue::MaxWorkerRefusals);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedLocal) == 1);
}

TEST_CASE("AdmissionResponder.requeued-ticket-abandoned-by-close-or-another-frame", "[node][admission]")
{
    Queue queue;
    auto const port = queue.Serve();
    queue.RegisterWorker(1);
    queue.ReportWorker();

    SECTION("a close")
    {
        {
            Client client { port };
            client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
            REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);
            client.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
            REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
            REQUIRE(queue.service.AdmissionSnapshotNow().depth == 1);
            client.Close();
        }
        CHECK(WaitFor([&] { return queue.service.AdmissionSnapshotNow().depth == 0; }));
        CHECK(WaitFor([&] { return Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1; }));
    }
    SECTION("a frame that is not an admission")
    {
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
        REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);
        client.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
        REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
        REQUIRE(queue.service.AdmissionSnapshotNow().depth == 1);

        // A second settle: there is nothing granted to settle, and the put-back ticket went first.
        client.Send(SettleFrame(Wire::SettleOutcome::Stored));
        auto const reply = Unwrap(client.ReadAnswer());
        CHECK(Pace::Testing::ErrorOf(reply.bytes) == Wire::ErrorCode::UnknownLease);
        CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
        CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);
    }
}

TEST_CASE("AdmissionResponder.a-departure-the-endpoint-counted-is-not-counted-again", "[node][admission]")
{
    // A grant ready for a peer that had already gone: the endpoint files it under `AdmissionAbandoned`
    // (the verb's watch counter), and the hold that departure releases must not file it a second time.
    Queue queue;
    queue.Report(1024 * MiB, 2);
    auto const scope = std::make_shared<ConnectionScope>();
    auto const reply = core::async::syncRun(queue.admission->Answer(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"),
                                                                    PeerIdentity { .host = "127.0.0.1", .scope = scope }));
    REQUIRE(Pace::Testing::StatusOf(reply.bytes) == Wire::Status::Ok);
    REQUIRE(scope->Holds());

    SECTION("filed by the endpoint")
    {
        scope->NoteDepartureCounted();
        scope->End();
        CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 0);
    }
    SECTION("filed by nobody else")
    {
        scope->End();
        CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);
    }
    // Given back either way.
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(queue.held.Held() == HeldCharges {});
}

TEST_CASE("AdmissionResponder.peer-gone-before-the-grant-is-written-counts-once", "[node][admission]")
{
    // Over the wire: the request is granted at once, and the client has already hung up. Whether the
    // endpoint sees the departure before its write or only after, it is one abandoned admission.
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(1024 * MiB, 2);
    {
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        client.Close();
    }
    REQUIRE(WaitFor([&] {
        return Count(queue, IMetricsSink::Counter::AdmissionAbandoned) >= 1
               && queue.service.AdmissionSnapshotNow().chargedBytes.empty() && queue.held.Held() == HeldCharges {};
    }));
    // Past where a second count would have landed: it lands in the same call that frees the charge, and
    // the endpoint's own count comes before that call.
    std::this_thread::sleep_for(200ms);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);
}

TEST_CASE("AdmissionResponder.a-parked-wait-has-no-ceiling-while-pulses-flow", "[node][admission]")
{
    // The answer window of a parked ADMIT is the SILENCE it may keep -- six pulses at this cadence --
    // and every pulse that reaches the client renews it. Parked far past several windows and several
    // sweeps, it is still there to be granted; a window that bounded the WAIT would have closed it.
    Queue queue;
    auto const port = queue.Serve();
    REQUIRE(queue.admission->RequestTimeout(static_cast<std::uint8_t>(Wire::Op::Admit)) == 120ms);

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const until = std::chrono::steady_clock::now() + 4 * FrameServer::SweepInterval;
    std::size_t pulses = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        auto const frame = client.Read();
        REQUIRE(frame.has_value());
        REQUIRE(frame->status == Wire::Status::Progress);
        ++pulses;
    }
    CHECK(pulses > 10);
    CHECK(Count(queue, IMetricsSink::Counter::FrameAnswerDeadlineSweeps) == 0);

    queue.Report(1024 * MiB, 2);
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
}
