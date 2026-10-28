// SPDX-License-Identifier: Apache-2.0
//
// The admission half of `SchedulerService` (#1728) on a ManualClock: the grants it mints on a worker,
// the lease and the job each takes, and every way they are given back.
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/FleetView.hpp>
#include <Pace/Distributed/LeaseToken.hpp>
#include <Pace/Distributed/SchedulerService.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Distributed;
using Pace::Testing::Unwrap;
using namespace std::chrono_literals;

namespace Wire = Pace::CompileCacheWire;

namespace
{
constexpr std::uint64_t MiB = 1024ULL * 1024ULL;
constexpr std::string_view Toolchain = "gcc-14";
constexpr std::string_view WorkerEndpoint = "10.0.0.2:7100";
constexpr std::string_view ToolchainLabel = "gcc 14.2.0";

/// A key as the registry and the queue spell a machine: the raw bytes, in a string.
[[nodiscard]] MachineId MachineOf(Ed25519PublicKey const& key)
{
    std::string out;
    for (auto const byte: key)
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    return out;
}

/// A leading scheduler, one worker machine it can place on, and the callers the cases use.
struct Fleet
{
    Fleet()
    {
        service.SetRole(SchedulerRole::Leader, {}, StandaloneSchedulerTerm);
    }

    /// What a worker that proved its key looks like to the gate.
    [[nodiscard]] CallerContext Worker() const
    {
        return CallerContext { .membership = Membership::Member,
                               .peerId = "10.0.0.2",
                               .provenNodeId = "worker-node",
                               .provenKey = workerKey.PublicKey(),
                               .identified = true };
    }

    /// Register the worker machine's toolchain; with @p proven false it registers without a key.
    /// @return Its worker id.
    std::string RegisterWorker(std::uint32_t slots = 2, bool proven = true)
    {
        auto caller = Worker();
        if (!proven)
            caller.provenKey.reset();
        auto const reply = service.Register(
            caller,
            WorkerRegistration { .fingerprint = Toolchain, .endpoint = WorkerEndpoint, .slots = slots, .codecs = {} });
        REQUIRE(reply.status == Wire::Status::Ok);
        return Unwrap(Wire::DecodeRegisterReply(reply.payload)).workerId;
    }

    /// A machine states its budget.
    void Report(MachineId const& machine, std::uint64_t budget = 1024 * MiB, std::uint32_t slots = 4)
    {
        auto const reply = service.ReportMachineBudget(
            Worker(),
            machine,
            Wire::MachineBudgetReport {
                .memoryBudgetBytes = budget, .slots = slots, .heldSlots = 0, .heldBytes = 0, .peaks = {} });
        REQUIRE(reply.status == Wire::Status::Ok);
    }

    /// Queue one request from this node's own machine.
    Ticket Admit(std::string key,
                 Wire::AdmitKind kind = Wire::AdmitKind::Dispatchable,
                 bool storable = false,
                 std::function<CallerContext()> regate = {},
                 std::vector<std::string> excluded = {})
    {
        if (!regate)
            regate = [] {
                return Insider;
            };
        auto const ticket = service.Admit(Insider,
                                          std::move(regate),
                                          service.OwnMachineId(),
                                          Wire::AdmitRequestFields { .kind = kind,
                                                                     .fingerprint = std::string { Toolchain },
                                                                     .cacheKey = std::move(key),
                                                                     .sourcePath = "src/a.cpp",
                                                                     .treeLabel = "tree",
                                                                     .storable = storable,
                                                                     .acceptedCodecs = {},
                                                                     .excluded = std::move(excluded),
                                                                     .toolchainLabel = std::string { ToolchainLabel } });
        REQUIRE(ticket.has_value());
        return *ticket;
    }

    /// @return How many leases the scheduler holds.
    [[nodiscard]] std::size_t Leases() const
    {
        return service.OutstandingLeases(16).total;
    }

    /// @return The jobs the registry counts on @p workerId.
    [[nodiscard]] std::uint32_t Jobs(std::string const& workerId) const
    {
        return Unwrap(service.Workers().Find(workerId)).inFlight;
    }

    static inline CallerContext const Insider { .membership = Membership::Member, .peerId = "127.0.0.1" };

    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    KeyPairLeaseSigner const signer = Testing::TestLeaseSigner("scheduler");
    Ed25519KeyPair const workerKey = Testing::TestKeyPair("admission-worker");
    SchedulerService service { clock, wallClock, metrics, logger, signer, "fleet-under-test", 64 * MiB };
};

[[nodiscard]] Wire::AdmitGrantFields GrantOf(AdmissionVerdict const& verdict)
{
    REQUIRE_FALSE(verdict.refusal.has_value());
    return Unwrap(Wire::DecodeAdmitGrant(verdict.grant));
}
} // namespace

TEST_CASE("SchedulerAdmission.worker-grant-takes-a-signed-lease-and-a-job", "[distributed][admission][scheduler]")
{
    Fleet fleet;
    auto const worker = fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    CHECK(verdicts[0].ticket == ticket);
    auto const grant = GrantOf(verdicts[0]);
    CHECK(grant.kind == Wire::GrantKind::Worker);
    CHECK(grant.endpoint == WorkerEndpoint);
    CHECK(grant.chargedBytes == 64 * MiB);

    // The token is a real lease grant: signed by this scheduler, for this key, at this worker, naming
    // the key the worker proved.
    auto const claims = AuthenticateLeaseToken(Testing::FixedLeaseRoster {}, grant.leaseToken);
    REQUIRE(claims.has_value());
    CHECK(claims->key == "k1");
    CHECK(claims->endpoint == WorkerEndpoint);
    CHECK(claims->fingerprint == Toolchain);
    CHECK(claims->workerKey == MachineOf(fleet.workerKey.PublicKey()));
    // Beside the token, from the same lease and worker: the key the reply is sealed under and how long
    // the lease lives, so the client reads neither out of claims it cannot verify (Ruling 13).
    CHECK(std::ranges::equal(grant.workerKey, fleet.workerKey.PublicKey()));
    CHECK(grant.lifetime > 0ms);
    CHECK(claims->expiresAt == fleet.wallClock.now() + grant.lifetime);
    CHECK(grant.reason == Wire::LocalGrantReason::Asked);

    CHECK(fleet.Leases() == 1);
    CHECK(fleet.Jobs(worker) == 1);

    CHECK(fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = 10 * MiB })
          == SettleResult::Settled);
    CHECK(fleet.Leases() == 0);
    CHECK(fleet.Jobs(worker) == 0);
    CHECK(fleet.service.AdmissionSnapshotNow().chargedBytes.empty());
}

TEST_CASE("SchedulerAdmission.abandon-releases-the-worker-lease", "[distributed][admission][scheduler]")
{
    Fleet fleet;
    auto const worker = fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);
    REQUIRE(fleet.Leases() == 1);

    fleet.service.AbandonAdmission(ticket);
    CHECK(fleet.Leases() == 0);
    CHECK(fleet.Jobs(worker) == 0);
    CHECK(fleet.service.AdmissionSnapshotNow().chargedBytes.empty());
}

TEST_CASE("SchedulerAdmission.worker-refusal-keeps-the-ticket-and-frees-the-lease", "[distributed][admission][scheduler]")
{
    Fleet fleet;
    auto const worker = fleet.RegisterWorker();
    auto const workerMachine = MachineOf(fleet.workerKey.PublicKey());
    fleet.Report(workerMachine);
    auto const ticket = fleet.Admit("k1");
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);

    CHECK(
        fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::WorkerRefused, .peakBytes = {} })
        == SettleResult::Requeued);
    CHECK(fleet.Leases() == 0);
    CHECK(fleet.Jobs(worker) == 0);
    // The SAME ticket waits again, at the front: nothing new was queued.
    CHECK(fleet.service.AdmissionSnapshotNow().depth == 1);
    CHECK(fleet.service.PumpAdmission().empty()); // the refusing machine admits nothing until it reports

    fleet.Report(workerMachine);
    auto const again = fleet.service.PumpAdmission();
    REQUIRE(again.size() == 1);
    CHECK(again[0].ticket == ticket);
    CHECK(GrantOf(again[0]).kind == Wire::GrantKind::Worker);
    CHECK(fleet.Leases() == 1);
}

TEST_CASE("SchedulerAdmission.second-grant-of-a-leased-key-is-refused-already-in-flight",
          "[distributed][admission][scheduler]")
{
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/2);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const first = fleet.Admit("same");
    auto const second = fleet.Admit("same"); // not storable, so the queue does not deduplicate it

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 2);
    CHECK(verdicts[0].ticket == first);
    CHECK(GrantOf(verdicts[0]).kind == Wire::GrantKind::Worker);
    CHECK(verdicts[1].ticket == second);
    REQUIRE(verdicts[1].refusal.has_value());
    CHECK(Unwrap(verdicts[1].refusal).error == Wire::ErrorCode::AlreadyInFlight);
    // The refused one holds nothing: one lease, one charge.
    CHECK(fleet.Leases() == 1);
    CHECK(fleet.service.AdmissionSnapshotNow().depth == 0);
    CHECK(fleet.service.AdmissionSnapshotNow().chargedBytes.size() == 1);
}

TEST_CASE("SchedulerAdmission.a-worker-that-proved-no-key-is-not-placed-on", "[distributed][admission][scheduler]")
{
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/2, /*proven=*/false);
    // The registry offers it, so what keeps it out is the placement source, not the registry.
    REQUIRE(fleet.service.Workers().Candidates(Toolchain).size() == 1);
    fleet.Report(fleet.service.OwnMachineId());
    (void) fleet.Admit("k1");

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    CHECK(GrantOf(verdicts[0]).kind == Wire::GrantKind::Local);
    // Nothing the queue could place on serves the toolchain, which is what the launcher is told.
    CHECK(GrantOf(verdicts[0]).reason == Wire::LocalGrantReason::NoWorker);
    CHECK(fleet.Leases() == 0);
}

TEST_CASE("SchedulerAdmission.a-local-grant-of-a-dispatchable-request-says-why", "[distributed][admission][scheduler]")
{
    // Ruling 11: "no worker serves this" and "every worker was busy" are opposite fixes, and the
    // launcher records them as the LEASE path's `NoWorker` and `NoCapacity` rows -- so the grant says which.
    Fleet fleet;
    fleet.Report(fleet.service.OwnMachineId());
    auto expected = Wire::LocalGrantReason::Asked;
    auto kind = Wire::AdmitKind::Dispatchable;
    std::vector<std::string> excluded;
    // What the LEADER's own operator sees, beside what the launcher is told: the lease path's counters
    // and its `unserved-toolchain` record, through `LocalGrantSignals`.
    std::uint64_t noWorker = 0;
    std::uint64_t allExcluded = 0;
    bool unserved = false;

    SECTION("no worker serves the toolchain")
    {
        expected = Wire::LocalGrantReason::NoWorker;
        noWorker = 1;
        unserved = true;
    }
    SECTION("the only worker serving it is excluded")
    {
        (void) fleet.RegisterWorker();
        fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
        excluded.emplace_back(WorkerEndpoint);
        expected = Wire::LocalGrantReason::AllExcluded;
        allExcluded = 1;
    }
    SECTION("a worker serves it and had no room")
    {
        // Its machine reported nothing, so the queue may not charge it. Ruling 15: an ordinary queue
        // decision, counted as no refusal at all.
        (void) fleet.RegisterWorker();
        expected = Wire::LocalGrantReason::NoRoom;
    }
    SECTION("a request that asked to run here has nothing to explain")
    {
        kind = Wire::AdmitKind::LocalOnly;
        expected = Wire::LocalGrantReason::Asked;
    }

    (void) fleet.Admit("k1", kind, false, {}, excluded);
    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    auto const grant = GrantOf(verdicts[0]);
    CHECK(grant.kind == Wire::GrantKind::Local);
    CHECK(grant.reason == expected);
    CHECK(grant.workerKey.empty());
    CHECK(grant.lifetime == 0ms);

    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoWorker) == noWorker);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesAllExcluded) == allExcluded);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 0);
    auto const named = fleet.service.UnservedToolchainsNow();
    CHECK(named.empty() == !unserved);
    if (unserved)
    {
        REQUIRE(named.size() == 1);
        CHECK(named[0].fingerprint == Toolchain);
        CHECK(named[0].label == ToolchainLabel);
    }
}

TEST_CASE("SchedulerAdmission.local-grant-reason-reads-the-registry", "[distributed][admission][scheduler]")
{
    auto const worker = [](std::string fingerprint, std::string endpoint, std::string key) {
        WorkerInfo info;
        info.fingerprint = std::move(fingerprint);
        info.endpoint = std::move(endpoint);
        info.identityKey = std::move(key);
        return info;
    };
    std::vector<WorkerInfo> const live { worker("gcc-14", "a:1", "ka"),
                                         worker("gcc-14", "b:1", "kb"),
                                         worker("clang-19", "c:1", "kc"),
                                         worker("msvc", "d:1", "") };
    std::vector<std::string> const none;
    std::vector<std::string> const oneOfTwo { "a:1" };
    std::vector<std::string> const both { "a:1", "b:1" };

    CHECK(LocalGrantReasonOf(live, "gcc-14", none) == Wire::LocalGrantReason::NoRoom);
    CHECK(LocalGrantReasonOf(live, "gcc-14", oneOfTwo) == Wire::LocalGrantReason::NoRoom);
    CHECK(LocalGrantReasonOf(live, "gcc-14", both) == Wire::LocalGrantReason::AllExcluded);
    CHECK(LocalGrantReasonOf(live, "icc", none) == Wire::LocalGrantReason::NoWorker);
    // A worker that proved no identity is one the queue never places on, so it serves nobody.
    CHECK(LocalGrantReasonOf(live, "msvc", none) == Wire::LocalGrantReason::NoWorker);
    CHECK(LocalGrantReasonOf({}, "gcc-14", none) == Wire::LocalGrantReason::NoWorker);
}

TEST_CASE("SchedulerAdmission.a-worker-gone-before-its-grant-sends-the-ticket-to-the-front",
          "[distributed][admission][scheduler]")
{
    // The worker is placed on, then stops being live before the grant mints its lease -- here by the
    // clock passing the heartbeat window during the grant's own re-gate. The ticket is not refused
    // `NoWorker`: it goes back to the front, uncounted, and the next worker takes it.
    Fleet fleet;
    (void) fleet.RegisterWorker();
    auto const workerMachine = MachineOf(fleet.workerKey.PublicKey());
    fleet.Report(workerMachine);
    auto advanced = false;
    auto const ticket = fleet.Admit("k1", Wire::AdmitKind::Dispatchable, false, [&] {
        if (!advanced)
            fleet.clock.advance(WorkerRegistry::DefaultHeartbeatTimeout + 1s);
        advanced = true;
        return Fleet::Insider;
    });

    CHECK(fleet.service.PumpAdmission().empty()); // no refusal reached anybody
    CHECK(advanced);
    CHECK(fleet.service.AdmissionSnapshotNow().depth == 1);
    CHECK(fleet.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(fleet.Leases() == 0);

    // The machine comes back; no new report is needed, because nobody refused anything.
    (void) fleet.RegisterWorker();
    auto const again = fleet.service.PumpAdmission();
    REQUIRE(again.size() == 1);
    CHECK(again[0].ticket == ticket);
    CHECK(GrantOf(again[0]).kind == Wire::GrantKind::Worker);
}

TEST_CASE("SchedulerAdmission.worker-refusal-after-demotion-abandons-the-ticket", "[distributed][admission][scheduler]")
{
    // A grant outlives a demotion; a refusal settled after it must not re-park the ticket in a
    // follower's queue, which nothing pumps.
    Fleet fleet;
    auto const worker = fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);

    fleet.service.SetRole(SchedulerRole::Follower, "10.0.0.9:7000", 7);
    CHECK(
        fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::WorkerRefused, .peakBytes = {} })
        == SettleResult::NotLeading);
    CHECK(fleet.service.AdmissionSnapshotNow().depth == 0);
    CHECK(fleet.service.AdmissionSnapshotNow().chargedBytes.empty());
    CHECK(fleet.Leases() == 0);
    CHECK(fleet.Jobs(worker) == 0);
}

TEST_CASE("SchedulerAdmission.fleet-collects-the-queue-with-machines-named",
          "[distributed][admission][scheduler][fleetview]")
{
    Fleet fleet;
    // Nothing has reported a budget or queued a request yet: no admission data, which is not a queue of zero.
    CHECK_FALSE(CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics }).admission);

    auto const reply = fleet.service.Register(
        fleet.Worker(),
        WorkerRegistration {
            .fingerprint = Toolchain, .endpoint = WorkerEndpoint, .displayName = "build-7", .slots = 1, .codecs = {} });
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const workerMachine = MachineOf(fleet.workerKey.PublicKey());
    fleet.Report(workerMachine, 1024 * MiB, 1);
    // Only the worker machine has reported, so the one worker slot is all there is.
    (void) fleet.Admit("k1");
    (void) fleet.Admit("k2");
    REQUIRE(fleet.service.PumpAdmission().size() == 1); // one slot: the second waits

    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    REQUIRE(snapshot.admission.has_value());
    CHECK(Unwrap(snapshot.admission).depth == 1);
    // The queued request's caller is this node's own machine, which no worker named: its row carries
    // the short form of its key, never an empty cell.
    REQUIRE(Unwrap(snapshot.admission).clients.size() == 1);
    auto const& client = Unwrap(snapshot.admission).clients.front();
    CHECK_FALSE(client.machine.empty());
    CHECK(client.tree == "tree");
    CHECK(client.running + client.queued == 2);
    CHECK(client.queued == 1);
    // The worker's machine is named as it registered.
    auto const named = std::ranges::find(Unwrap(snapshot.admission).machines, "build-7", &AdmissionMachineRow::machine);
    REQUIRE(named != Unwrap(snapshot.admission).machines.end());
    CHECK(named->budgetBytes == std::optional<std::uint64_t> { 1024 * MiB });
    CHECK(named->chargedBytes == 64 * MiB);

    // A node that stops leading has no queue to show.
    fleet.service.SetRole(SchedulerRole::Follower, "10.9.9.9:7100", 1);
    CHECK_FALSE(CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics }).admission);
}
