// SPDX-License-Identifier: Apache-2.0
#include "AdmissionResponder.hpp"
#include "AdmissionTestUtils.hpp"
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
#include <Pace/Protocol/AdmitKeepAlive.hpp>
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
#include <tests/MembershipFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using Pace::Testing::UnwrapMutable;

using namespace Pace;
using namespace Pace::Node;
using namespace std::chrono_literals;

namespace Wire = Pace::CompileCacheWire;

using namespace Pace::Node::AdmissionFixture;
using Pace::Testing::Unwrap;

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
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionWaitCount) == 1);
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
    DriveAdmit(&UnwrapMutable(queue->admission),
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
    auto const reply = core::async::syncRun(
        UnwrapMutable(queue.admission)
            .Answer(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"), PeerIdentity { .host = "127.0.0.1", .scope = scope }));
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

TEST_CASE("AdmissionResponder.a-parked-wait-has-no-ceiling-while-pulses-flow", "[node][admission]")
{
    // The answer window of a parked ADMIT is the SILENCE it may keep -- six pulses at this cadence --
    // and every pulse that reaches the client renews it. Parked far past several windows and several
    // sweeps, it is still there to be granted; a window that bounded the WAIT would have closed it.
    Queue queue;
    auto const port = queue.Serve();
    REQUIRE(UnwrapMutable(queue.admission).RequestTimeout(static_cast<std::uint8_t>(Wire::Op::Admit)) == 120ms);

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    auto const until = std::chrono::steady_clock::now() + 4 * FrameServer::SweepInterval;
    std::size_t pulses = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        auto const frame = client.Read();
        REQUIRE(frame.has_value());
        REQUIRE(Unwrap(frame).status == Wire::Status::Progress);
        ++pulses;
    }
    CHECK(pulses > 10);
    CHECK(Count(queue, IMetricsSink::Counter::FrameAnswerDeadlineSweeps) == 0);

    queue.Report(1024 * MiB, 2);
    CHECK(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
}

namespace
{
/// What one `ADMIT` driven straight at the responder answered, read on the case's thread.
struct DrivenReply
{
    std::atomic<bool> done { false };
    std::vector<std::byte> bytes; ///< Written before `done` is set.
};

/// Drive one `ADMIT` straight at the responder and leave its answer in @p out.
core::async::DetachedTask DriveAdmitInto(AdmissionResponder* responder,
                                         std::vector<std::byte> frame,
                                         PeerIdentity peer,
                                         std::shared_ptr<DrivenReply> out)
{
    auto reply = co_await responder->Answer(frame, std::move(peer));
    out->bytes = std::move(reply.bytes);
    out->done.store(true, std::memory_order_release);
}
} // namespace

TEST_CASE("AdmissionResponder.regate-at-grant", "[node][admission]")
{
    // R9: a request that waited is gated again when it is granted. The caller is a machine elsewhere
    // that PROVED its key on its connection, and the forget lands through the production fold -- its
    // key revoked in the roster -- while the request is parked. The grant is a refusal, and holds nothing.
    Queue queue;
    queue.RegisterWorker(2);
    auto const scope = std::make_shared<ConnectionScope>();
    auto const reply = std::make_shared<DrivenReply>();

    // Parked before the reactor turns, so it waits on a loop that is then started under it.
    DriveAdmitInto(&UnwrapMutable(queue.admission),
                   AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"),
                   PeerIdentity { .host = "10.0.0.7",
                                  .proven = Testing::IdentityOf(std::string { Queue::RemoteClient }),
                                  .scope = scope },
                   reply);
    REQUIRE(queue.service.AdmissionSnapshotNow().depth == 1);
    // Serving is what keeps the reactor turning: a loop with nothing adopted returns at once.
    (void) queue.Serve();

    // `--cluster-forget` of the client: its key is revoked, the others stay live.
    queue.fold.keys.Publish({ { "scheduler", Testing::TestKeyPair("scheduler").PublicKey() },
                              { "admission-worker", Testing::TestKeyPair("admission-worker").PublicKey() } },
                            { Testing::TestKeyPair(std::string { Queue::RemoteClient }).PublicKey() });

    // Then the room appears.
    queue.ReportWorker();

    REQUIRE(WaitFor([&] { return reply->done.load(std::memory_order_acquire); }));
    CHECK(Pace::Testing::StatusOf(reply->bytes) == Wire::Status::Error);
    CHECK(Pace::Testing::ErrorOf(reply->bytes) == Wire::ErrorCode::NotAMember);
    CHECK_FALSE(scope->Holds());
    CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(queue.service.OutstandingLeases(4).total == 0);
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionGrantedWorker) == 0);
}

TEST_CASE("AdmissionResponder.worker-refused-after-demotion-answers-not-leader", "[node][admission]")
{
    // The grant was made while leading; the node is demoted while the compile runs; the worker then
    // refuses it. Re-parking it here would queue it on a follower nobody pumps, behind pulses that keep
    // the connection alive for ever. The client is sent to the leader instead, and nothing stays.
    Queue queue;
    auto const port = queue.Serve();
    queue.RegisterWorker(1);
    queue.ReportWorker();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);

    queue.service.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:7000", 7);

    client.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
    auto const reply = Unwrap(client.ReadAnswer());
    CHECK(reply.status == Wire::Status::Error);
    CHECK(Pace::Testing::ErrorOf(reply.bytes) == Wire::ErrorCode::NotLeader);
    auto const text = std::string { reinterpret_cast<char const*>(reply.payload.data()), reply.payload.size() };
    CHECK(text.find("10.0.0.9:7000") != std::string::npos);

    CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
    CHECK(queue.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(queue.service.OutstandingLeases(4).total == 0);
    CHECK(queue.held.Held() == HeldCharges {});
    CHECK(Count(queue, IMetricsSink::Counter::AdmissionAbandoned) == 1);

    // Nothing is held on the connection either: a further ADMIT is a new request, refused by the gate.
    client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "k1"));
    CHECK(Pace::Testing::ErrorOf(Unwrap(client.ReadAnswer()).bytes) == Wire::ErrorCode::NotLeader);
    CHECK(queue.service.AdmissionSnapshotNow().depth == 0);
}

TEST_CASE("AdmissionResponder.a-granted-connection-is-armed-with-the-shared-keepalive", "[node][admission][keepalive]")
{
    // #1736: the connection is the lease, and a launcher whose host vanished never closes it. Once the
    // connection HOLDS a grant the leader asks the kernel to probe it -- with the intervals the dials use.
    Queue queue;
    auto const port = queue.Serve();
    queue.Report(1024 * MiB, 2);

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);

    REQUIRE(WaitFor([&] { return queue.keepAlives.Requests().size() == 1; }));
    auto const requests = queue.keepAlives.Requests();
    CHECK(requests.front().idle == AdmitKeepAliveProbe.idle);
    CHECK(requests.front().interval == AdmitKeepAliveProbe.interval);
    CHECK(requests.front().count == AdmitKeepAliveProbe.count);
}

TEST_CASE("AdmissionResponder.a-queued-connection-is-not-armed-until-it-is-granted", "[node][admission][keepalive]")
{
    // A parked request holds nothing and is already bounded by the pulse; arming it would add probes to
    // connections that may end first. No budget has been reported, so the request waits.
    Queue queue;
    auto const port = queue.Serve();

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::LocalOnly, "k1"));
    REQUIRE(Unwrap(client.Read()).status == Wire::Status::Progress);
    REQUIRE(Count(queue, IMetricsSink::Counter::AdmissionQueued) == 1);
    CHECK(queue.keepAlives.Requests().empty());

    // The control: the same connection, once the machine can take it, IS armed.
    queue.Report(1024 * MiB, 2);
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Local);
    CHECK(WaitFor([&] { return queue.keepAlives.Requests().size() == 1; }));
}

TEST_CASE("AdmissionResponder.a-requeued-ticket-does-not-arm-keepalive-twice", "[node][admission][keepalive]")
{
    // Ruling 9: a worker's refusal puts the ticket back and the SAME connection waits on it again. The
    // socket is armed once, at the first grant, and a second grant on it is not a second request.
    Queue queue;
    auto const port = queue.Serve();
    queue.RegisterWorker(1);
    queue.ReportWorker(1024 * MiB, 1);

    Client client { port };
    client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "ka"));
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);
    REQUIRE(WaitFor([&] { return queue.keepAlives.Requests().size() == 1; }));

    client.Send(SettleFrame(Wire::SettleOutcome::WorkerRefused));
    REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    client.Send(AdmitFrame(Wire::AdmitKind::Dispatchable, "ka"));
    queue.ReportWorker(1024 * MiB, 1);
    REQUIRE(GrantOf(Unwrap(client.ReadAnswer())).kind == Wire::GrantKind::Worker);

    // The connection serves one frame at a time and arms right after a grant's reply is written, so a
    // settle answered on it means any second arm would already be in the ledger.
    client.Send(SettleFrame(Wire::SettleOutcome::Stored));
    REQUIRE(Unwrap(client.ReadAnswer()).status == Wire::Status::Ok);
    CHECK(queue.keepAlives.Requests().size() == 1);
}
