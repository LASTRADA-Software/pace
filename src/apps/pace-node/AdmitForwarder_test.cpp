// SPDX-License-Identifier: Apache-2.0
#include "AdmissionTestUtils.hpp"
#include "AdmitForwarder.hpp"
#include "FrameEndpoint.hpp"
#include "HeldCharges.hpp"
#include "NodeConfig.hpp"
#include "NodeIoLoop.hpp"
#include "NodeSurfaces.hpp"
#include "SchedulingRedirect.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/MembershipOracle.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/LeaderRedirect.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <EndpointDial.hpp>
#include <core/async/Task.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace Pace;
using namespace Pace::Node;
using namespace Pace::Node::AdmissionFixture;
using namespace std::chrono_literals;
using Pace::Testing::ErrorOf;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

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

/// A node that runs no scheduler -- a learner -- with the forwarder wired, serving on a loopback port.
struct Follower
{
    /// @param leaderEndpoint What the node believes the leader's scheduling endpoint to be.
    /// @param pulse The cadence of its own pulse, and the unit of its silence bound on the leader.
    explicit Follower(std::string_view leaderEndpoint, std::chrono::milliseconds pulse = 20ms)
    {
        knownLeader.Publish(leaderEndpoint);
        dialer.Attach(*io);
        forwarder.emplace(knownLeader, dialer, io->Reactor(), metrics, &held, ForwardPolicy { .progressInterval = pulse });
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
    RecordingDialer dialer;
    SchedulingRedirectResponder redirect { membership, knownLeader };
    std::optional<AdmitForwarder> forwarder;

    /// Declared LAST so it is destroyed FIRST, as the leader fixture's is: the reactor goes before the
    /// responders its parked frames reach.
    std::unique_ptr<NodeIoLoop> io = std::make_unique<NodeIoLoop>();
    std::unique_ptr<FrameEndpoint> endpoint;
};

/// @param port A loopback port.
/// @return The scheduling endpoint a leader serving there answers at.
[[nodiscard]] std::string EndpointOf(std::uint16_t port)
{
    return std::format("127.0.0.1:{}", port);
}

/// A node that leads nobody: it answers every request `NotLeader`, naming @p leader.
/// @param queue A fixture whose service was a leader.
/// @param leader Who it says leads.
void Demote(Queue& queue, std::string_view leader)
{
    queue.service.SetRole(Distributed::SchedulerRole::Follower, leader, 2);
}

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
    // The leader pulses every ten seconds, this node every 20 ms: the launcher is told this node is
    // alive the whole time, and only the node's own bound on the leader's silence can tell it the leader
    // has stopped saying anything.
    Queue leader { 10s };
    auto const leaderPort = leader.Serve();
    Follower node { EndpointOf(leaderPort) };
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
        remoteFacing.ForwardAdmissionWith(*node.forwarder);
        auto const remote =
            PeerIdentity { .host = "10.0.0.7",
                           .proven = ProvenIdentity { .id = "remote", .key = Testing::TestKeyPair("remote").PublicKey() } };
        auto const refused = remoteFacing.RefusePeer(remote, static_cast<std::uint8_t>(Wire::Op::Admit));
        REQUIRE(refused.has_value());
        CHECK(ErrorOf(*refused) == Wire::ErrorCode::NotLeader);
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
