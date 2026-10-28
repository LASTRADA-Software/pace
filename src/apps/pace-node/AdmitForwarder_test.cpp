// SPDX-License-Identifier: Apache-2.0
#include "AdmissionTestUtils.hpp"
#include "AdmitForwarder.hpp"
#include "EndpointDialer.hpp"
#include "FrameEndpoint.hpp"
#include "HeldCharges.hpp"
#include "MachineBudget.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeIoLoop.hpp"
#include "NodeProofClient.hpp"
#include "NodeProofResponder.hpp"
#include "NodeSurfaces.hpp"
#include "Responders.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"
#include "SchedulingRedirect.hpp"

#include <Pace/Core/ISecureRandom.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/LeaseToken.hpp>
#include <Pace/Distributed/MembershipOracle.hpp>
#include <Pace/Distributed/NodePolicy.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/LeaderRedirect.hpp>
#include <Pace/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <EndpointDial.hpp>
#include <WorkerProtocol.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/KeepAlive.hpp>
#include <core/net/NetError.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ConsensusStandingFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/RelabelledPeerListener.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using Pace::Testing::UnwrapMutable;

using namespace Pace;
using namespace Pace::Node;
using namespace Pace::Node::AdmissionFixture;
using namespace std::chrono_literals;
using Pace::Testing::ErrorOf;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// A node that leads nobody: it answers every request `NotLeader`, naming @p leader.
/// @param queue A fixture whose service was a leader.
/// @param leader Who it says leads.
void Demote(Queue& queue, std::string_view leader)
{
    queue.service.SetRole(Distributed::SchedulerRole::Follower, leader, 2);
}

/// A VOTER following another (Ruling 10): its own scheduler and queue, demoted, with the forwarder wired
/// into its scheduler surface as `main` wires it (`SchedulerTier::ForwardAdmissionWith`).
struct VoterFollower
{
    /// @param leaderEndpoint Who leads, as both its scheduler and the forwarder's source believe.
    explicit VoterFollower(std::string_view leaderEndpoint)
    {
        knownLeader.Publish(leaderEndpoint);
        Demote(node, leaderEndpoint);
        dialer.Attach(*node.io);
        forwarder.emplace(knownLeader, dialer, node.io->Reactor(), node.metrics, &node.held, &self, QuickPulse);
        node.responder.ForwardAdmissionWith(*forwarder);
    }

    KnownSchedulingLeader knownLeader;
    StatedEndpoint self;
    RecordingDialer dialer;
    std::optional<AdmitForwarder> forwarder;
    /// Declared LAST so it is destroyed FIRST: its listener and reactor go before the relay its parked
    /// frames reach.
    Queue node;
};

/// The leader as production serves it to ANOTHER machine: its scheduler behind the node-proof surface,
/// admitting by the production fold (`Testing::RosterFold`), over a listener whose every connection says
/// it comes from elsewhere -- so nothing is admitted for being loopback, and only a proven key reaches
/// the queue.
struct ProvenLeader
{
    /// The host every connection to it reports.
    static constexpr std::string_view RelabelledHost = "10.0.0.9";

    ProvenLeader()
    {
        prover.emplace(
            std::string { LeaderMachine }, identity, queue.fold.admitted, consensus, random, queue.metrics, queue.logger);
        merged.emplace(SurfaceComponents { .scheduler = &queue.responder, .nodeProof = &*prover });
    }

    /// Start serving on a fresh port.
    /// @return The port.
    [[nodiscard]] std::uint16_t Serve()
    {
        auto listened = core::net::listen(queue.io->Reactor(), core::net::ListenOptions { .host = "127.0.0.1", .port = 0 });
        REQUIRE(listened.has_value());
        auto const port = (*listened)->boundPort();
        auto listener = std::make_unique<Testing::RelabelledPeerListener>(
            std::unique_ptr<core::net::IListener> { std::move(*listened) }, std::string { RelabelledHost });
        queue.endpoint = FrameEndpoint::StartWithListener(*queue.io,
                                                          NodeSurface::Node,
                                                          std::move(listener),
                                                          std::format("127.0.0.1:{}", port),
                                                          UnwrapMutable(merged),
                                                          queue.metrics,
                                                          queue.logger);
        REQUIRE(queue.endpoint != nullptr);
        queue.io->Start();
        return port;
    }

    Ed25519KeyPair const identity = Testing::TestKeyPair(std::string { LeaderMachine });
    SystemSecureRandom random;
    Testing::ScriptedAppliedState consensus { AppliedStateReading::CaughtUp };
    std::optional<NodeProofResponder> prover;
    std::optional<MergedResponder> merged;
    /// Declared LAST so it is destroyed FIRST: its listener and reactor go before the surfaces they route to.
    Queue queue;
};

/// A key as the leader files a machine's charge under it: its raw bytes.
/// @param machine The id whose test key it is.
/// @return The machine id.
[[nodiscard]] std::string MachineOf(std::string_view machine)
{
    auto id = std::string {};
    std::ranges::transform(Testing::TestKeyPair(std::string { machine }).PublicKey(),
                           std::back_inserter(id),
                           [](std::byte const byte) { return static_cast<char>(std::to_integer<unsigned char>(byte)); });
    return id;
}

/// A "leader" that answers the first request on its one connection with a reply of the case's making, then
/// waits to see the relay close the connection. On a thread of its own, every wait bounded, and asserting
/// nothing there: what it saw is read on the case's thread once `Join` returns.
class ScriptedLeader
{
  public:
    /// @param reply The whole reply frame it writes.
    explicit ScriptedLeader(std::vector<std::byte> reply):
        _reply { std::move(reply) }
    {
        REQUIRE(_listener->IsBound());
        _listener->SetTimeouts(5s, 5s);
        _thread = std::thread { [this] { Serve(); } };
    }

    ScriptedLeader(ScriptedLeader const&) = delete;
    ScriptedLeader& operator=(ScriptedLeader const&) = delete;
    ScriptedLeader(ScriptedLeader&&) = delete;
    ScriptedLeader& operator=(ScriptedLeader&&) = delete;

    ~ScriptedLeader()
    {
        Join();
    }

    /// @return The port it listens on.
    [[nodiscard]] std::uint16_t Port() const noexcept
    {
        return _listener->boundPort();
    }

    /// Wait for its one exchange to end, which the accept and read timeouts bound.
    void Join()
    {
        if (_thread.joinable())
            _thread.join();
    }

    /// @return True once the relay closed the connection after the reply; read after `Join`.
    [[nodiscard]] bool SawClose() const noexcept
    {
        return _sawClose.load();
    }

  private:
    void Serve()
    {
        auto accepted = core::async::syncRun(_listener->accept());
        if (!accepted.has_value())
            return;
        auto const socket = std::move(*accepted);
        std::vector<std::byte> request;
        if (!core::async::syncRun(ReadAtLeast(socket.get(), &request, Wire::RequestHeaderSize)))
            return;
        auto const header = Wire::DecodeRequestHeader(request);
        if (!header.has_value()
            || !core::async::syncRun(ReadAtLeast(socket.get(), &request, Wire::RequestHeaderSize + header->payloadLength)))
            return;
        if (!core::async::syncRun(SendAll(socket.get(), _reply)))
            return;
        // The relay's next move: a close reads as the end of the stream (or a reset), never as another frame
        // -- and well inside the read timeout, which would otherwise read exactly like one.
        std::vector<std::byte> after;
        auto const began = std::chrono::steady_clock::now();
        auto const more = core::async::syncRun(ReadAtLeast(socket.get(), &after, 1));
        _sawClose.store(!more && after.empty() && std::chrono::steady_clock::now() - began < 4s);
    }

    std::unique_ptr<BlockingListener> _listener = BlockingListener::Bind("127.0.0.1", 0);
    std::vector<std::byte> _reply;
    std::atomic<bool> _sawClose { false };
    std::thread _thread;
};

[[nodiscard]] std::uint64_t Count(Follower const& node, IMetricsSink::Counter counter)
{
    return node.metrics.Read(counter);
}
} // namespace

TEST_CASE("AdmitForwarder.relays-progress-and-grant", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));

    // Nothing can be admitted yet, so the request parks on the LEADER, and the launcher is told this
    // node is still there. Read BEFORE the grant, so a build that never pulses cannot pass by the
    // grant arriving first.
    constexpr std::size_t PulsesBeforeGrant = 2;
    for ([[maybe_unused]] auto const pulse: std::views::iota(std::size_t { 0 }, PulsesBeforeGrant))
        CHECK(Unwrap(client.Read()).status == Wire::Status::Progress);
    REQUIRE(WaitFor([&] { return leader.service.AdmissionSnapshotNow().depth == 1; }));

    leader.Report(1024 * MiB, 2);
    auto const grant = GrantOf(Unwrap(client.ReadAnswer()));
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(grant.chargedBytes == 64 * MiB);
    CHECK(node.dialer.Dialled() == std::vector<std::string> { EndpointOf(leaderPort) });
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwarded) == 1);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 0);

    // The leader charged it, and its settle -- relayed over the SAME connection -- gives it back.
    CHECK(leader.service.AdmissionSnapshotNow().chargedBytes.size() == 1);
    client.Send(SettleFrame(Wire::SettleOutcome::Stored, 10 * MiB));
    CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwarded) == 2);
    CHECK(node.dialer.Dialled().size() == 1);
}

TEST_CASE("AdmitForwarder.relays-a-workers-refusal-and-the-ask-again-over-one-connection", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    leader.Report(1024 * MiB, 4);
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);

    // Ruling 9: the leader answers the refusal `Ok` and keeps the connection, holding the SAME ticket.
    client.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
    REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    // The refused grant is no longer held on this machine, and nothing is granted to it meanwhile.
    CHECK(node.held.Held() == HeldCharges {});

    // The launcher's next ADMIT travels the same upstream connection and waits on the same ticket.
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    // The machine that refused is not tried again until it reports (R7), so the request waits -- told
    // this node is still there -- and the leader's next report decides it.
    CHECK(Unwrap(client.Read()).status == Wire::Status::Progress);
    leader.Report(1024 * MiB, 4);
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    CHECK(node.held.Held() == HeldCharges { .slots = 1, .bytes = 64 * MiB });

    CHECK(node.dialer.Dialled().size() == 1);
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionWorkerRefused) == 1);
}

TEST_CASE("AdmitForwarder.client-close-closes-upstream", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    SECTION("while the request is parked on the leader")
    {
        {
            Client client { port };
            client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
            REQUIRE(WaitFor([&] { return leader.service.AdmissionSnapshotNow().depth == 1; }));
            client.Close();
        }
        // The leader's own queue forgets it: its end of the relay connection went with the client's.
        CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().depth == 0; }));
        CHECK(WaitFor([&] { return Count(leader, IMetricsSink::Counter::AdmissionAbandoned) == 1; }));
        CHECK(Count(leader, IMetricsSink::Counter::AdmissionGrantedLocal) == 0);
    }

    SECTION("while the grant is held")
    {
        leader.Report(1024 * MiB, 2);
        {
            Client client { port };
            client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
            REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
            REQUIRE(node.held.Held().slots == 1);
            client.Close();
        }
        CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
        CHECK(WaitFor([&] { return node.held.Held() == HeldCharges {}; }));
        CHECK(Count(leader, IMetricsSink::Counter::AdmissionAbandoned) == 1);
    }
}

TEST_CASE("AdmitForwarder.forward-dial-failure-refuses-promptly", "[node][admission][forwarder]")
{
    // The leader this node believes in is nowhere: a real connect, refused by the host.
    Follower node { EndpointOf(FreePort()) };
    auto const port = node.Serve();

    Client client { port };
    auto const began = std::chrono::steady_clock::now();
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());
    auto const waited = std::chrono::steady_clock::now() - began;

    // A refusal the launcher fails open on -- `EndpointBusy` -- inside the dial's own budget, never the
    // silence a launcher would sit out for the length of a queue wait.
    CHECK(reply.status == Wire::Status::Error);
    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::EndpointBusy);
    CHECK(waited < 5s);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 1);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwarded) == 0);
    CHECK(node.held.Held() == HeldCharges {});
}

TEST_CASE("AdmitForwarder.a-silent-leader-is-noticed-behind-this-nodes-own-pulse", "[node][admission][forwarder]")
{
    // The leader pulses every ten seconds, this node every 20 ms and gives the leader 200 ms: the launcher
    // is told this node is alive the whole time, and only the node's own bound on the leader's silence can
    // tell it the leader has stopped saying anything.
    Queue leader { 10s };
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort), ForwardPolicy { .progressInterval = 20ms, .upstreamIdle = 200ms } };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());

    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::EndpointBusy);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 1);
    // And the ticket the leader queued went with the connection that was closed on it.
    CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().depth == 0; }));
}

TEST_CASE("AdmitForwarder.follows-not-leader-once", "[node][admission][forwarder]")
{
    Queue demoted;
    Queue leader;
    auto const demotedPort = demoted.Serve();
    auto const leaderPort = leader.Serve();
    Demote(demoted, EndpointOf(leaderPort));
    leader.Report(1024 * MiB, 2);

    // This node still believes the old leader leads.
    Follower node { EndpointOf(demotedPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);

    // Dialled the demoted node first, the one it named second -- one redirect, followed once, and the
    // launcher never saw it.
    CHECK(node.dialer.Dialled() == std::vector<std::string> { EndpointOf(demotedPort), EndpointOf(leaderPort) });
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(demoted, IMetricsSink::Counter::AdmissionQueued) == 0);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwarded) == 1);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 0);
}

TEST_CASE("AdmitForwarder.a-leader-that-keeps-moving-is-followed-twice-and-then-refused", "[node][admission][forwarder]")
{
    // Two nodes, each naming the other: a request chases them for as long as it is allowed to.
    Queue first;
    Queue second;
    auto const firstPort = first.Serve();
    auto const secondPort = second.Serve();
    Demote(first, EndpointOf(secondPort));
    Demote(second, EndpointOf(firstPort));

    Follower node { EndpointOf(firstPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());

    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::EndpointBusy);
    // The first dial and two followed redirects: MaxForwardHops, and not one more.
    CHECK(node.dialer.Dialled().size() == 3);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 1);
}

TEST_CASE("AdmitForwarder.relays-a-refusal-that-names-nobody-unchanged", "[node][admission][forwarder]")
{
    // The leader is in an election: it answers `NotLeader` with no endpoint, which there is nowhere to
    // follow. The launcher is told exactly that, as the redirect would have told it.
    Queue leader;
    auto const leaderPort = leader.Serve();
    Demote(leader, "");
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());

    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::NotLeader);
    CHECK(node.dialer.Dialled().size() == 1);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwarded) == 1);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 0);
}

TEST_CASE("AdmitForwarder.local-grant-counts-as-held", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    SECTION("a local lease is held from its grant until its settle")
    {
        leader.Report(1024 * MiB, 2);
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).chargedBytes == 64 * MiB);
        CHECK(node.held.Held() == HeldCharges { .slots = 1, .bytes = 64 * MiB });

        client.Send(SettleFrame(Wire::SettleOutcome::Stored, 8 * MiB));
        REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
        CHECK(node.held.Held() == HeldCharges {});
    }

    SECTION("a grant on a worker occupies the worker's machine and not this one")
    {
        leader.RegisterWorker(2);
        leader.ReportWorker();
        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k2"));
        REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);
        CHECK(node.held.Held() == HeldCharges {});
    }
}

TEST_CASE("AdmitForwarder.other-verbs-still-redirect", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    SECTION("a LEASE from this machine is answered NotLeader naming the leader")
    {
        Client client { port };
        client.Send(Wire::Detail::EncodeRequest(Wire::CurrentVersion, Wire::Op::Lease, {}));
        auto const reply = Unwrap(client.ReadAnswer());
        auto const decoded = Unwrap(Wire::DecodeErrorPayload(reply.payload));
        CHECK(decoded.first == Wire::ErrorCode::NotLeader);
        CHECK(LeaderRedirectTarget(decoded.first, decoded.second) == std::string_view { EndpointOf(leaderPort) });
        CHECK(node.dialer.Dialled().empty());
    }

    SECTION("an ADMIT from another machine is redirected, never relayed")
    {
        // A member by its key, not by loopback: the leader would charge THIS node's machine for its
        // local lease, which is not where its compile runs.
        Testing::RosterFold fold { { "remote" } };
        SchedulingRedirectResponder remoteFacing { fold.admitted, node.knownLeader };
        remoteFacing.ForwardAdmissionWith(UnwrapMutable(node.forwarder));
        auto const remote =
            PeerIdentity { .host = "10.0.0.7",
                           .proven = ProvenIdentity { .id = "remote", .key = Testing::TestKeyPair("remote").PublicKey() } };
        auto const refused = remoteFacing.RefusePeer(remote, static_cast<std::uint8_t>(Wire::Op::Admit));
        REQUIRE(refused.has_value());
        CHECK(ErrorOf(Unwrap(refused)) == Wire::ErrorCode::NotLeader);
        CHECK_FALSE(AdmitForwarder::ForwardsFor(remote, static_cast<std::uint8_t>(Wire::Op::Admit)));
        CHECK(AdmitForwarder::ForwardsFor(PeerIdentity { .host = "127.0.0.1" }, static_cast<std::uint8_t>(Wire::Op::Admit)));
        CHECK(
            AdmitForwarder::ForwardsFor(PeerIdentity { .host = "127.0.0.1" }, static_cast<std::uint8_t>(Wire::Op::Settle)));
        CHECK_FALSE(
            AdmitForwarder::ForwardsFor(PeerIdentity { .host = "127.0.0.1" }, static_cast<std::uint8_t>(Wire::Op::Release)));
    }
}

TEST_CASE("AdmitForwarder.a-settle-with-nothing-held-is-refused-as-the-leader-refuses-it", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(SettleFrame(Wire::SettleOutcome::Stored));
    CHECK(ErrorOf(Unwrap(client.ReadAnswer()).bytes) == Wire::ErrorCode::UnknownLease);
    CHECK(node.dialer.Dialled().empty());
}

TEST_CASE("AdmitForwarder.a-lost-leader-after-the-grant-ends-the-client-connection", "[node][admission][forwarder]")
{
    auto leader = std::make_unique<Queue>();
    auto const leaderPort = leader->Serve();
    leader->Report(1024 * MiB, 2);
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    REQUIRE(node.held.Held().slots == 1);

    // The leader goes away while the compile runs.
    leader.reset();

    client.Send(SettleFrame(Wire::SettleOutcome::Stored, 8 * MiB));
    auto const reply = Unwrap(client.ReadAnswer());
    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::EndpointBusy);
    // ...and the connection ends with it: there is no lease left to hold it open for.
    CHECK_FALSE(client.Read().has_value());
    CHECK(node.held.Held() == HeldCharges {});
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 1);
}

TEST_CASE("AdmitForwarder.a-leader-pulsing-slower-than-this-node-is-not-dropped", "[node][admission][forwarder]")
{
    // The leader pulses at 300 ms, this node at 20 ms: the leader's ordinary gap is fifteen of this node's
    // pulses, which a bound derived from THIS node's cadence would read as silence. The wire's own bound
    // on the leader is what the relay waits by.
    Queue leader { 300ms };
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(WaitFor([&] { return leader.service.AdmissionSnapshotNow().depth == 1; }));

    // Three of the leader's gaps pass with the request parked on it, each read as this node's own pulses.
    auto const until = std::chrono::steady_clock::now() + 900ms;
    auto pulses = std::size_t { 0 };
    while (std::chrono::steady_clock::now() < until)
    {
        auto const reply = client.Read();
        REQUIRE(reply.has_value());
        REQUIRE(Unwrap(reply).status == Wire::Status::Progress);
        ++pulses;
    }
    CHECK(pulses > 3);

    leader.Report(1024 * MiB, 2);
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 0);
    CHECK(node.dialer.Dialled().size() == 1);
}

TEST_CASE("AdmitForwarder.an-undecodable-grant-is-refused-and-its-connection-closed", "[node][admission][forwarder]")
{
    // The leader answers `Ok` with a payload that is no grant: this node cannot tell what the leader now
    // holds for it, so it relays nothing it would have to hold, closes, and lets the launcher fail open.
    ScriptedLeader leader { Wire::EncodeReply(Wire::Status::Ok,
                                              std::vector<std::byte> { std::byte { 0xFF }, std::byte { 0x01 } }) };
    Follower node { EndpointOf(leader.Port()) };
    auto const port = node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());
    leader.Join();

    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::EndpointBusy);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 1);
    CHECK(Count(node, IMetricsSink::Counter::AdmissionForwarded) == 0);
    CHECK(node.held.Held() == HeldCharges {});
    // Closed, which is what gives back whatever the leader charged for it.
    CHECK(leader.SawClose());
}

TEST_CASE("AdmitForwarder.a-voter-that-follows-relays-to-the-leader", "[node][admission][forwarder]")
{
    Queue leader;
    auto const leaderPort = leader.Serve();
    leader.Report(1024 * MiB, 2);
    VoterFollower voter { EndpointOf(leaderPort) };
    auto const port = voter.node.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const grant = GrantOf(Unwrap(client.ReadAnswer()));

    // Granted by the LEADER's queue, reached through the voter's scheduler surface -- never refused
    // `NotLeader` there, and never queued on the voter's own queue, which leads nobody.
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(voter.dialer.Dialled() == std::vector<std::string> { EndpointOf(leaderPort) });
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(voter.node, IMetricsSink::Counter::AdmissionQueued) == 0);
    CHECK(Count(voter.node, IMetricsSink::Counter::AdmissionForwarded) == 1);
    CHECK(voter.node.held.Held() == HeldCharges { .slots = 1, .bytes = 64 * MiB });

    SECTION("and its settle travels the same link")
    {
        client.Send(SettleFrame(Wire::SettleOutcome::Stored, 8 * MiB));
        CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    }

    SECTION("and its settle goes to the leader that granted it, even once this node leads")
    {
        // The link is the leader's half of the lease: its settle is the forwarder's to relay, whoever
        // leads here by the time the compile ends.
        voter.node.service.SetRole(Distributed::SchedulerRole::Leader, {}, 3);
        client.Send(SettleFrame(Wire::SettleOutcome::Stored, 8 * MiB));
        CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    }

    CHECK(WaitFor([&] { return leader.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
    CHECK(voter.node.held.Held() == HeldCharges {});
    CHECK(Count(voter.node, IMetricsSink::Counter::AdmissionForwarded) == 2);
    CHECK(voter.dialer.Dialled().size() == 1);
}

TEST_CASE("AdmitForwarder.a-voter-that-knows-no-leader-refuses-promptly", "[node][admission][forwarder]")
{
    // An election: nobody is named, so there is nothing to relay to and nothing is dialled. The launcher is
    // told `NotLeader` naming nobody, which it reads as no leader and compiles locally at once.
    VoterFollower voter { "" };
    auto const port = voter.node.Serve();

    Client client { port };
    auto const began = std::chrono::steady_clock::now();
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const reply = Unwrap(client.ReadAnswer());
    auto const waited = std::chrono::steady_clock::now() - began;

    auto const decoded = Unwrap(Wire::DecodeErrorPayload(reply.payload));
    CHECK(decoded.first == Wire::ErrorCode::NotLeader);
    CHECK_FALSE(LeaderRedirectTarget(decoded.first, decoded.second).has_value());
    CHECK(waited < 2s);
    CHECK(voter.dialer.Dialled().empty());
    CHECK(Count(voter.node, IMetricsSink::Counter::AdmissionForwardFailed) == 0);
    CHECK(Count(voter.node, IMetricsSink::Counter::AdmissionQueued) == 0);
}

TEST_CASE("AdmitForwarder.a-leader-answers-its-own-launchers-and-relays-nothing", "[node][admission][forwarder]")
{
    // The forwarder is wired on every node that answers the scheduling verbs; while this one leads its
    // own queue decides, exactly as before. Declared before the leader, so the leader's reactor and
    // listener go first.
    KnownSchedulingLeader knownLeader;
    StatedEndpoint self;
    RecordingDialer dialer;
    std::optional<AdmitForwarder> forwarder;
    Queue leader;
    dialer.Attach(*leader.io);
    forwarder.emplace(knownLeader, dialer, leader.io->Reactor(), leader.metrics, &leader.held, &self, QuickPulse);
    leader.responder.ForwardAdmissionWith(*forwarder);
    leader.Report(1024 * MiB, 2);
    auto const port = leader.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(Count(leader, IMetricsSink::Counter::AdmissionForwarded) == 0);
    CHECK(dialer.Dialled().empty());
}

TEST_CASE("AdmitForwarder.a-node-never-relays-to-itself", "[node][admission][forwarder]")
{
    SECTION("a leader named at this node's own endpoint is not dialled")
    {
        // A node that stopped leading a moment ago may still be named: relaying to itself would come back.
        VoterFollower voter { "127.0.0.1:1" };
        auto const port = voter.node.Serve();
        voter.self.Set(EndpointOf(port));
        voter.knownLeader.Publish(EndpointOf(port));

        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        CHECK(ErrorOf(Unwrap(client.ReadAnswer()).bytes) == Wire::ErrorCode::NotLeader);
        CHECK(voter.dialer.Dialled().empty());
    }

    SECTION("a redirect naming this node is not followed")
    {
        Queue demoted;
        auto const demotedPort = demoted.Serve();
        VoterFollower voter { EndpointOf(demotedPort) };
        auto const port = voter.node.Serve();
        voter.self.Set(EndpointOf(port));
        Demote(demoted, EndpointOf(port));

        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        CHECK(ErrorOf(Unwrap(client.ReadAnswer()).bytes) == Wire::ErrorCode::NotLeader);
        CHECK(voter.dialer.Dialled() == std::vector<std::string> { EndpointOf(demotedPort) });
    }

    SECTION("a connection proving this machine's own key is answered here, never relayed again")
    {
        // What arrives when this node's forwarder dialled itself under a spelling its endpoint check did not
        // recognise: the connection proves THIS machine's key, and relaying it would chase itself.
        Queue leader;
        auto const leaderPort = leader.Serve();
        leader.Report(1024 * MiB, 2);
        VoterFollower voter { EndpointOf(leaderPort) };
        static_cast<void>(voter.node.Serve());

        auto const self = PeerIdentity { .host = "127.0.0.1",
                                         .proven = ProvenIdentity { .id = std::string { LeaderMachine },
                                                                    .key = voter.node.signer.PublicKey() },
                                         .scope = std::make_shared<ConnectionScope>() };
        auto const launcher = PeerIdentity { .host = "127.0.0.1", .scope = std::make_shared<ConnectionScope>() };
        CHECK(UnwrapMutable(voter.node.admission).DecidesHere(self));
        CHECK_FALSE(UnwrapMutable(voter.node.admission).DecidesHere(launcher));

        auto const reply =
            core::async::syncRun(voter.node.responder.Answer(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"), self));
        REQUIRE(reply.bytes.size() > Wire::ReplyHeaderSize);
        auto const decoded =
            Unwrap(Wire::DecodeErrorPayload(std::span<std::byte const> { reply.bytes }.subspan(Wire::ReplyHeaderSize)));
        CHECK(decoded.first == Wire::ErrorCode::NotLeader);
        CHECK(LeaderRedirectTarget(decoded.first, decoded.second) == std::string_view { EndpointOf(leaderPort) });
        CHECK(voter.dialer.Dialled().empty());
        CHECK(Count(leader, IMetricsSink::Counter::AdmissionQueued) == 0);
    }
}

TEST_CASE("AdmitForwarder.a-proven-relay-charges-the-forwarding-machine", "[node][admission][forwarder]")
{
    // The production dialer against the production fold: the leader admits the relay by the key it PROVED
    // -- every connection to it reports another host, so loopback admits nothing -- and files the local
    // lease under that machine, never under its own.
    ProvenLeader leader;
    auto const leaderPort = leader.Serve();

    SECTION("a machine the roster holds is granted, and charged")
    {
        leader.queue.ReportFor(Queue::ForwardingMachine);
        Follower node { EndpointOf(leaderPort), QuickPulse, Queue::ForwardingMachine };
        auto const port = node.Serve();

        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        auto const grant = GrantOf(Unwrap(client.ReadAnswer()));
        CHECK(grant.kind == Wire::GrantKind::Local);

        auto const charged = leader.queue.service.AdmissionSnapshotNow().chargedBytes;
        REQUIRE(charged.size() == 1);
        CHECK(charged.front().first == MachineOf(Queue::ForwardingMachine));
        CHECK(charged.front().first != leader.queue.service.OwnMachineId());
        CHECK(charged.front().second == grant.chargedBytes);
        CHECK(node.held.Held() == HeldCharges { .slots = 1, .bytes = grant.chargedBytes });

        // And the settle, sealed like everything after the proof, gives it back.
        client.Send(SettleFrame(Wire::SettleOutcome::Stored, 8 * MiB));
        CHECK(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
        CHECK(WaitFor([&] { return leader.queue.service.AdmissionSnapshotNow().chargedBytes.empty(); }));
        CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 0);
    }

    SECTION("a machine the roster does not hold proves nothing, and the launcher fails open")
    {
        Follower node { EndpointOf(leaderPort), QuickPulse, std::string_view { "stranger" } };
        auto const port = node.Serve();

        Client client { port };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        CHECK(ErrorOf(Unwrap(client.ReadAnswer()).bytes) == Wire::ErrorCode::EndpointBusy);
        CHECK(Count(node, IMetricsSink::Counter::AdmissionForwardFailed) == 1);
        CHECK(Count(leader.queue, IMetricsSink::Counter::AdmissionQueued) == 0);
    }

    SECTION("control: the relabelling is live, so an unproven caller is no member there")
    {
        Client client { leaderPort };
        client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
        CHECK(ErrorOf(Unwrap(client.ReadAnswer()).bytes) == Wire::ErrorCode::NotAMember);
        CHECK(Count(leader.queue, IMetricsSink::Counter::AdmissionQueued) == 0);
    }
}

TEST_CASE("AdmitForwarder.the-largest-grant-fits-the-relay-ceiling", "[node][admission][forwarder]")
{
    // Every field a grant carries at its wire ceiling, and the leader's own ids -- which no wire ceiling
    // holds, being the fleet's configuration rather than anything a peer sends -- far longer than any a
    // node mints. A grant past `MaxForwardReplyBytes` would read as a lost leader and fail open.
    constexpr std::size_t GenerousIdBytes = 1024;
    auto const signer = Testing::TestLeaseSigner(std::string(GenerousIdBytes, 'n'));
    auto const endpoint = std::string(Wire::MaxEndpointBytes, 'e');
    auto const token = Distributed::MintLeaseToken(
        signer,
        Distributed::LeaseClaims { .serial = std::string(GenerousIdBytes, 's'),
                                   .endpoint = endpoint,
                                   .fingerprint = std::string(Wire::MaxToolchainFingerprintBytes, 'f'),
                                   .key = std::string(Wire::MaxLeaseKeyBytes, 'k'),
                                   .expiresAt = {},
                                   .clusterId = std::string(GenerousIdBytes, 'c'),
                                   .epoch = ~std::uint64_t { 0 },
                                   .signer = {},
                                   .workerKey = std::string(Ed25519PublicKeyBytes, 'w') });
    auto const workerKeyBytes = std::string(Wire::IdentityPublicKeyBytes, 'w');
    auto const grant =
        Wire::EncodeAdmitGrant(Wire::AdmitGrant { .kind = Wire::GrantKind::Worker,
                                                  .endpoint = endpoint,
                                                  .leaseToken = token,
                                                  .workerCodecs = Wire::CodecList(Wire::MaxCodecListIds, 1),
                                                  .chargedBytes = ~std::uint64_t { 0 },
                                                  .queuedMs = ~std::uint64_t { 0 },
                                                  .lifetime = std::chrono::milliseconds { ~std::uint32_t { 0 } },
                                                  .workerKey = Wire::AsBytes(std::string_view { workerKeyBytes }),
                                                  .reason = Wire::LocalGrantReason::Asked,
                                                  .dialHint = {} });
    CAPTURE(grant.size());
    // An eighth of the ceiling: room for every id to grow eightfold before the relay could refuse one.
    CHECK(grant.size() < MaxForwardReplyBytes / 8);
    CHECK(Wire::DecodeAdmitGrant(grant).has_value());
}

namespace
{
/// A connector that refuses every dial and records the options each was asked with.
class OptionsRecordingConnector final: public core::net::IConnector
{
  public:
    /// @copydoc IConnector::connect
    [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                     std::uint16_t port,
                                                                     core::net::DialOptions options) override
    {
        std::ignore = port;
        asked.push_back(options);
        co_return std::unexpected { core::net::NetError {
            .code = core::net::NetErrorCode::ConnRefused, .systemCode = 0, .context = std::move(host) } };
    }

    std::vector<core::net::DialOptions> asked; ///< Every dial's options, in order.
};
} // namespace

TEST_CASE("AdmitForwarder.the-relay-to-the-leader-is-dialled-with-keepalive", "[node][admission][forwarder]")
{
    // Ruling 18. The relay holds its connection to the leader for as long as the launcher's compile runs;
    // a leader whose host vanished sends nothing to say so, so the dial must ask the kernel to probe. The
    // option is per DIAL (the rulebook's rule), so it is asserted on the dial the production dialer makes.
    OptionsRecordingConnector connector;
    auto const key = Testing::TestKeyPair("forwarding-node");
    NamedMachineTrust trust { std::string { LeaderMachine },
                              Testing::TestKeyPair(std::string { LeaderMachine }).PublicKey() };
    SystemSecureRandom random;
    NodeProofClient proof { "forwarding-node", key, trust, nullptr, nullptr, random };
    auto io = std::make_unique<NodeIoLoop>();
    ProvenSchedulerDialer dialer { proof, connector, io->Reactor() };

    auto const opened = core::async::syncRun(dialer.Open("10.0.0.9:6674"));
    CHECK(opened.socket == nullptr);
    REQUIRE(connector.asked.size() == 1);
    CHECK(connector.asked[0].keepAlive == core::net::KeepAlive::Yes);
    CHECK(connector.asked[0].connectTimeout == ForwardDialBudget {}.connectTimeout);
}

TEST_CASE("AdmitForwarder.the-leaders-own-budget-round-charges-its-own-machine",
          "[node][admission][forwarder][machinebudget]")
{
    // The leader's launchers reach its queue over loopback and are charged to THIS machine
    // (`SchedulerService::OwnMachineId`). The queue admits nothing on a machine that has not reported a
    // budget, so the leader's own presence round must reach ITSELF -- dialled, proven under its own
    // identity key, sealed -- and file that budget under the same machine its loopback launchers are
    // charged to. The production round, the production dialer and the production proof, end to end.
    ProvenLeader leader;
    auto const port = leader.Serve();

    NodeConfig cfg;
    cfg.machineMemoryBudgetBytes = 8 * 1024 * MiB;
    cfg.machineSlots = 8;
    Distributed::NodeCapacity const capacity {};
    HeldChargeRegistry held;
    MeasuredPeaks peaks;
    NullLogger logger;
    NodeConditions conditions;
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock, &conditions };
    NamedMachineTrust trust { std::string { LeaderMachine }, leader.identity.PublicKey() };
    SystemSecureRandom random;
    NodeProofClient proof { std::string { LeaderMachine }, leader.identity, trust, nullptr, nullptr, random };
    BlockingEndpointDialer dialer { HeartbeatIoTimeout };
    auto link = Unwrap(SchedulerLink::For(std::vector<std::string> { EndpointOf(port) }));

    REQUIRE(SendMachineBudgetOnce(MachineBudgetRound { .cfg = cfg,
                                                       .capacity = capacity,
                                                       .held = held,
                                                       .peaks = peaks,
                                                       .logger = logger,
                                                       .prover = &proof,
                                                       .reachability = reachability },
                                  link,
                                  dialer));

    auto const budgets = leader.queue.service.AdmissionSnapshotNow().budgets;
    REQUIRE(budgets.size() == 1);
    CHECK(budgets.front().machine == leader.queue.service.OwnMachineId());
    CHECK(budgets.front().budgetBytes == std::optional<std::uint64_t> { 8 * 1024 * MiB });

    // And so a launcher on the leader's own machine -- loopback, charged to `OwnMachineId` -- is granted.
    auto const insider = Distributed::CallerContext { .membership = Distributed::Membership::Member, .peerId = "127.0.0.1" };
    auto const ticket = leader.queue.service.Admit(
        insider,
        [insider] { return Distributed::CallerContext { insider }; },
        leader.queue.service.OwnMachineId(),
        Wire::AdmitRequestFields { .kind = Wire::AdmitKind::LocalOnly,
                                   .fingerprint = {},
                                   .cacheKey = "k1",
                                   .sourcePath = {},
                                   .treeLabel = "tree",
                                   .storable = false,
                                   .acceptedCodecs = {},
                                   .excluded = {},
                                   .toolchainLabel = {} });
    REQUIRE(ticket.has_value());
    auto const verdicts = leader.queue.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    CHECK(verdicts.front().ticket == *ticket);
    CHECK_FALSE(verdicts.front().refusal.has_value());
    CHECK(verdicts.front().kind == Wire::GrantKind::Local);
    leader.queue.service.AbandonAdmission(*ticket);
}
