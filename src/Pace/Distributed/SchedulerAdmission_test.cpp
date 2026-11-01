// SPDX-License-Identifier: Apache-2.0
//
// The admission half of `SchedulerService` (#1728) on a ManualClock: the grants it mints on a worker,
// the lease and the job each takes, and every way they are given back.
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/FleetView.hpp>
#include <Pace/Distributed/IClusterAdmin.hpp>
#include <Pace/Distributed/LeaseToken.hpp>
#include <Pace/Distributed/SchedulerService.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <latch>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
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

    /// Queue one request, from this node's own machine unless @p machine names another.
    Ticket Admit(std::string key,
                 Wire::AdmitKind kind = Wire::AdmitKind::Dispatchable,
                 bool storable = false,
                 std::function<CallerContext()> regate = {},
                 std::vector<std::string> excluded = {},
                 std::optional<MachineId> const& machine = std::nullopt)
    {
        if (!regate)
            regate = [] {
                return Insider;
            };
        auto const ticket = service.Admit(Insider,
                                          std::move(regate),
                                          machine.value_or(service.OwnMachineId()),
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

/// The joined compile rows of a leading scheduler.
[[nodiscard]] std::vector<CompileRow> CompilesOf(SchedulerService const& service)
{
    auto rows = service.AdmissionReportNow().compiles;
    REQUIRE(rows.has_value());
    return Unwrap(rows);
}

/// The row for @p ticket; the case fails when there is none.
[[nodiscard]] CompileRow RowOf(std::vector<CompileRow> const& rows, Ticket ticket)
{
    auto const found = std::ranges::find(rows, ticket, [](CompileRow const& row) { return row.ticket.ticket; });
    REQUIRE(found != rows.end());
    return *found;
}

/// The `compiles` element of a fleet document whose `ticket` is @p ticket; the case fails when there is none.
[[nodiscard]] Json::Value CompileJsonOf(Json::Value const& document, Ticket ticket)
{
    auto const* const compiles = document.Find("compiles");
    REQUIRE(compiles != nullptr);
    REQUIRE(compiles->AsArray() != nullptr);
    for (auto const& row: *compiles->AsArray())
    {
        auto const* const id = row.Find("ticket");
        if (id != nullptr && id->AsInt() == static_cast<std::int64_t>(ticket))
            return row;
    }
    FAIL("no compile row for ticket " << ticket);
    return {};
}

/// The member @p key of a JSON object; the case fails when it is missing -- which is not `null`.
[[nodiscard]] Json::Value MemberOf(Json::Value const& object, std::string_view key)
{
    auto const* const member = object.Find(key);
    REQUIRE(member != nullptr);
    return *member;
}

/// A JSON string member's text; the case fails when it is missing or not a string.
[[nodiscard]] std::string TextOf(Json::Value const& object, std::string_view key)
{
    auto const member = MemberOf(object, key);
    REQUIRE(member.AsString() != nullptr);
    return *member.AsString();
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

TEST_CASE("SchedulerAdmission.two-machines-missing-one-key-both-get-a-worker", "[distributed][admission][scheduler]")
{
    // Ruling 17. Two machines that do not share a cache missed the same key: each must compile, so
    // each is granted a worker, and neither is refused `AlreadyInFlight` -- which its launcher would
    // read as "compile here", with nothing charged anywhere.
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/2);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const otherMachine = MachineOf(Testing::TestKeyPair("another-workstation").PublicKey());
    auto const first = fleet.Admit("same", Wire::AdmitKind::Dispatchable, /*storable=*/true);
    auto const second = fleet.Admit("same", Wire::AdmitKind::Dispatchable, /*storable=*/true, {}, {}, otherMachine);

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 2);
    CHECK(verdicts[0].ticket == first);
    CHECK(verdicts[1].ticket == second);
    for (auto const& verdict: verdicts)
    {
        auto const grant = GrantOf(verdict);
        CHECK(grant.kind == Wire::GrantKind::Worker);
        // Each token still signs the OBJECT key the client missed, not the table's bookkeeping name.
        auto const claims = AuthenticateLeaseToken(Testing::FixedLeaseRoster {}, grant.leaseToken);
        REQUIRE(claims.has_value());
        CHECK(claims->key == "same");
    }
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesDuplicate) == 0);
    CHECK(fleet.Leases() == 2);

    // Each lease is its own: settling one frees one.
    CHECK(fleet.service.SettleAdmission(first, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
          == SettleResult::Settled);
    CHECK(fleet.Leases() == 1);
}

TEST_CASE("SchedulerAdmission.one-machine-missing-one-unstorable-key-twice-gets-two-workers",
          "[distributed][admission][scheduler]")
{
    // The queue deduplicates only what a waiter could FETCH: an unstorable compile is run twice, and
    // the second run is never turned away by the lease table.
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/2);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    (void) fleet.Admit("same");
    (void) fleet.Admit("same");

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 2);
    CHECK(GrantOf(verdicts[0]).kind == Wire::GrantKind::Worker);
    CHECK(GrantOf(verdicts[1]).kind == Wire::GrantKind::Worker);
    CHECK(fleet.Leases() == 2);
}

TEST_CASE("SchedulerAdmission.one-machine-missing-one-storable-key-twice-waits-on-the-first",
          "[distributed][admission][scheduler]")
{
    // The control for the two cases above: where a waiter CAN fetch the result, the queue (and only
    // the queue) holds the second request until the first stores.
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/2);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const first = fleet.Admit("same", Wire::AdmitKind::Dispatchable, /*storable=*/true);
    auto const second = fleet.Admit("same", Wire::AdmitKind::Dispatchable, /*storable=*/true);

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    CHECK(verdicts[0].ticket == first);
    CHECK(fleet.Leases() == 1);

    REQUIRE(fleet.service.SettleAdmission(first, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
            == SettleResult::Settled);
    auto const woken = fleet.service.PumpAdmission();
    REQUIRE(woken.size() == 1);
    CHECK(woken[0].ticket == second);
    CHECK(GrantOf(woken[0]).kind == Wire::GrantKind::Stored);
}

TEST_CASE("SchedulerAdmission.worker-grant-hints-where-the-worker-was-seen", "[distributed][admission][scheduler][dialhint]")
{
    // The hint a `Lease` grant carries, minted by the same predicate: a worker that advertises a NAME
    // and was seen at an address it reports as its own is dialled there first, while the token still
    // signs the name.
    Fleet fleet;
    auto const reply = fleet.service.Register(
        fleet.Worker(),
        WorkerRegistration { .fingerprint = Toolchain, .endpoint = "build-7.corp:7100", .slots = 1, .codecs = {} });
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const id = Unwrap(Wire::DecodeRegisterReply(reply.payload)).workerId;
    std::vector<std::string> const interfaces { "10.0.0.2" };
    REQUIRE(fleet.service.Heartbeat(fleet.Worker(), id, NodeLoad {}, {}, interfaces).status == Wire::Status::Ok);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    (void) fleet.Admit("k1");

    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    auto const grant = GrantOf(verdicts[0]);
    CHECK(grant.kind == Wire::GrantKind::Worker);
    CHECK(grant.endpoint == "build-7.corp:7100");
    CHECK(grant.dialHint == "10.0.0.2:7100");
    auto const claims = AuthenticateLeaseToken(Testing::FixedLeaseRoster {}, grant.leaseToken);
    REQUIRE(claims.has_value());
    CHECK(claims->endpoint == "build-7.corp:7100");
}

TEST_CASE("SchedulerAdmission.wait-is-measured-from-the-last-park", "[distributed][admission][scheduler]")
{
    // The grant tells the client how long the whole request queued; the leader's wait metric adds,
    // per grant, only the time since the request was last parked -- or a refused request's first wait
    // is summed once per regrant.
    Fleet fleet;
    (void) fleet.RegisterWorker();
    auto const workerMachine = MachineOf(fleet.workerKey.PublicKey());
    auto const ticket = fleet.Admit("k1");
    fleet.clock.advance(5s);
    fleet.Report(workerMachine);
    auto const first = fleet.service.PumpAdmission();
    REQUIRE(first.size() == 1);
    CHECK(first[0].queued == 5s);
    CHECK(first[0].waited == 5s);

    REQUIRE(
        fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::WorkerRefused, .peakBytes = {} })
        == SettleResult::Requeued);
    fleet.clock.advance(2s);
    fleet.Report(workerMachine);
    auto const again = fleet.service.PumpAdmission();
    REQUIRE(again.size() == 1);
    CHECK(again[0].queued == 7s);
    CHECK(again[0].waited == 2s);
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

namespace
{
/// The two dispatch counters a queued worker grant moves, read together so a case asserts both.
struct DispatchCounts
{
    std::uint64_t granted = 0;  ///< `DispatchLeasesGranted`.
    std::uint64_t released = 0; ///< `DispatchLeasesReleased`.

    bool operator==(DispatchCounts const&) const = default;
};

/// @return What @p fleet's sink reads for the dispatch pair.
[[nodiscard]] DispatchCounts DispatchCountsOf(Fleet const& fleet)
{
    return DispatchCounts { .granted = fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesGranted),
                            .released = fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReleased) };
}
} // namespace

TEST_CASE("SchedulerAdmission.a-settled-worker-grant-counts-one-dispatch-and-one-release",
          "[distributed][admission][scheduler][metrics]")
{
    // #1742: the fleet page's Dispatched figure reads `DispatchLeasesGranted`, and a fleet that placed every
    // miss through the queue read zero -- only the `Lease` verb counted.
    Fleet fleet;
    (void) fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");
    REQUIRE(DispatchCountsOf(fleet) == DispatchCounts { .granted = 0, .released = 0 });

    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 0 });

    REQUIRE(fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
            == SettleResult::Settled);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 1 });
}

TEST_CASE("SchedulerAdmission.a-local-grant-counts-no-dispatch", "[distributed][admission][scheduler][metrics]")
{
    // Nothing was placed on a worker, so nothing was dispatched and there is no lease to give back.
    Fleet fleet;
    fleet.Report(fleet.service.OwnMachineId());
    auto const ticket = fleet.Admit("k1", Wire::AdmitKind::LocalOnly);
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Local);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 0, .released = 0 });

    REQUIRE(fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
            == SettleResult::Settled);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 0, .released = 0 });
}

TEST_CASE("SchedulerAdmission.an-abandoned-worker-grant-is-not-a-release", "[distributed][admission][scheduler][metrics]")
{
    // A connection that closed holding its grant is the client dying mid-job, which granted climbing while
    // released stays flat is how the pair reports. Counting it a release would hide exactly that.
    Fleet fleet;
    auto const worker = fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);

    fleet.service.AbandonAdmission(ticket);
    // The lease is still given back -- only the count distinguishes the two endings.
    CHECK(fleet.Leases() == 0);
    CHECK(fleet.Jobs(worker) == 0);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 0 });
}

TEST_CASE("SchedulerAdmission.a-worker-refusal-is-a-release-and-its-regrant-another-dispatch",
          "[distributed][admission][scheduler][metrics]")
{
    Fleet fleet;
    (void) fleet.RegisterWorker();
    auto const workerMachine = MachineOf(fleet.workerKey.PublicKey());
    fleet.Report(workerMachine);
    auto const ticket = fleet.Admit("k1");
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);

    REQUIRE(
        fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::WorkerRefused, .peakBytes = {} })
        == SettleResult::Requeued);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 1 });

    fleet.Report(workerMachine);
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 2, .released = 1 });

    REQUIRE(fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
            == SettleResult::Settled);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 2, .released = 2 });
}

TEST_CASE("SchedulerAdmission.a-worker-refusal-settled-by-a-follower-is-a-release",
          "[distributed][admission][scheduler][metrics]")
{
    Fleet fleet;
    (void) fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Worker);

    fleet.service.SetRole(SchedulerRole::Follower, "10.0.0.9:7000", 7);
    REQUIRE(
        fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::WorkerRefused, .peakBytes = {} })
        == SettleResult::NotLeading);
    CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 1 });
}

TEST_CASE("SchedulerAdmission.a-late-queue-settle-counts-released-late-and-an-abandon-does-not",
          "[distributed][admission][scheduler][metrics]")
{
    // #1742: a SETTLE past the lease's lifetime is the same evidence the `Release` verb's late refusal is
    // (a compile outran the bound), so it spends the same `ReleaseRefusalTable` row and not `Released`.
    auto const releasedLate = [](Fleet const& fleet) {
        return fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReleasedLate);
    };
    auto const expireLease = [](Fleet& fleet, std::chrono::milliseconds lifetime) {
        fleet.clock.advance(lifetime + 1s);
        fleet.wallClock.advance(lifetime + 1s);
    };

    SECTION("settled")
    {
        Fleet fleet;
        (void) fleet.RegisterWorker();
        fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
        auto const ticket = fleet.Admit("k1");
        auto const grant = GrantOf(fleet.service.PumpAdmission().at(0));
        REQUIRE(grant.kind == Wire::GrantKind::Worker);

        expireLease(fleet, grant.lifetime);
        REQUIRE(fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
                == SettleResult::Settled);
        CHECK(releasedLate(fleet) == 1);
        CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 0 });
    }

    SECTION("abandoned")
    {
        Fleet fleet;
        (void) fleet.RegisterWorker();
        fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
        auto const ticket = fleet.Admit("k1");
        auto const grant = GrantOf(fleet.service.PumpAdmission().at(0));
        REQUIRE(grant.kind == Wire::GrantKind::Worker);

        expireLease(fleet, grant.lifetime);
        fleet.service.AbandonAdmission(ticket);
        CHECK(releasedLate(fleet) == 0);
        CHECK(DispatchCountsOf(fleet) == DispatchCounts { .granted = 1, .released = 0 });
    }
}

namespace
{
/// A cluster that has agreed one state, for the fleet page to read; it proposes nothing.
class AgreedCluster final: public IClusterAdmin
{
  public:
    /// @param state What the cluster has agreed.
    explicit AgreedCluster(Cluster::ClusterState state):
        _state { std::move(state) }
    {
    }

    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        return _state;
    }

    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& /*command*/) override
    {
        return std::unexpected { InvalidConfiguration("this cluster proposes nothing") };
    }

    void NoteAnnouncedEndpoint(Consensus::NodeId const& /*member*/, std::string /*endpoint*/) override {}

  private:
    Cluster::ClusterState _state;
};
} // namespace

TEST_CASE("SchedulerAdmission.dashboard-shows-the-replicated-name-and-says-where-a-name-came-from",
          "[distributed][admission][scheduler][fleetview][display-name]")
{
    // The page joins a machine to its member record by the key the machine PROVED -- for the label alone -- and
    // prefers the name an operator chose over the one the machine reports for itself, saying which it showed.
    Fleet fleet;
    auto const reply = fleet.service.Register(
        fleet.Worker(),
        WorkerRegistration {
            .fingerprint = Toolchain, .endpoint = WorkerEndpoint, .displayName = "build-7", .slots = 1, .codecs = {} });
    REQUIRE(reply.status == Wire::Status::Ok);

    auto state = Cluster::ClusterState {};
    for (auto const& [id, key]: { std::pair { std::string { "w1" }, fleet.workerKey.PublicKey() },
                                  std::pair { std::string { "w2" }, Testing::TestKeyPair("w2").PublicKey() } })
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                          .key = id,
                                          .value = {},
                                          .schedulerEndpoint = {},
                                          .publicKey = key });

    SECTION("nobody named it: the machine's own word, marked self-reported, on both rows")
    {
        AgreedCluster const cluster { state };
        auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = &cluster, .metrics = &fleet.metrics });
        auto const json = RenderFleetJson(snapshot, FleetHistoryView {});
        INFO(json);
        CHECK(json.contains(R"("name":"build-7","name-source":"self-reported")"));
        CHECK(json.contains(R"("id":"w1","name":"build-7","name-source":"self-reported")"));
        // A member no machine of the fleet speaks for, and nobody named, renders absent at the cell.
        CHECK(json.contains(R"("id":"w2","name":null,"name-source":null)"));
    }
    SECTION("an operator named it: the replicated name wins, marked replicated, on both rows and in the queue")
    {
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::SetDisplayName,
                                          .key = "w1",
                                          .value = "lab-box",
                                          .schedulerEndpoint = {},
                                          .publicKey = std::nullopt });
        AgreedCluster const cluster { state };
        fleet.Report(MachineOf(fleet.workerKey.PublicKey()), 1024 * MiB, 1);
        auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = &cluster, .metrics = &fleet.metrics });
        REQUIRE(snapshot.nodes.size() == 1);
        CHECK(snapshot.nodes.front().replicatedName == "lab-box");
        auto const json = RenderFleetJson(snapshot, FleetHistoryView {});
        INFO(json);
        CHECK(json.contains(R"("name":"lab-box","name-source":"replicated")"));
        CHECK(json.contains(R"("id":"w1","name":"lab-box","name-source":"replicated")"));
        CHECK_FALSE(json.contains("build-7"));
        // The page and the text document say it too: one column table, one spelling.
        CHECK(RenderFleetHtml(snapshot, FleetHistoryView {}, 0).contains("lab-box"));
        CHECK(RenderFleetText(snapshot, FleetHistoryView {}, FleetSection::Members).contains("w1\tlab-box\treplicated"));
        // And a machine the admission queue names is named by it.
        REQUIRE(snapshot.admission.has_value());
        CHECK(std::ranges::contains(Unwrap(snapshot.admission).machines, "lab-box", &AdmissionMachineRow::machine));
    }
}

TEST_CASE("SchedulerAdmission.a-machine-is-named-as-a-member-by-the-key-it-proved-never-by-the-endpoint-it-claimed",
          "[distributed][admission][scheduler][fleetview][display-name]")
{
    // A registration's endpoint is the machine's own unverified claim, so the `node-id` a machine and worker row carry
    // is the member holding the key the connection PROVED -- the join the name already makes -- and nothing else.
    Fleet fleet;
    constexpr std::string_view OwnEndpoint = "127.0.0.1:7100";
    constexpr std::string_view ClaimedEndpoint = "10.0.0.7:6674";
    // This fleet's own machine, over loopback: a member records no loopback 0xFC endpoint (`Validate` refuses one), so
    // only the proven key can place this row -- the one-node fleet's every row.
    auto own = fleet.Worker();
    own.peerId = "127.0.0.1";
    REQUIRE(fleet.service
                .Register(own,
                          WorkerRegistration { .fingerprint = Toolchain, .endpoint = OwnEndpoint, .slots = 1, .codecs = {} })
                .status
            == Wire::Status::Ok);
    // A machine proving a key NO member holds, registering under the endpoint member w2 RECORDS as its 0xFC endpoint.
    auto const strangerKey = Testing::TestKeyPair("stranger");
    auto stranger = fleet.Worker();
    stranger.peerId = "10.0.0.9";
    stranger.provenNodeId = "stranger";
    stranger.provenKey = strangerKey.PublicKey();
    REQUIRE(
        fleet.service
            .Register(stranger,
                      WorkerRegistration { .fingerprint = Toolchain, .endpoint = ClaimedEndpoint, .slots = 1, .codecs = {} })
            .status
        == Wire::Status::Ok);

    auto state = Cluster::ClusterState {};
    for (auto const& [id, endpoint, key]:
         { std::tuple { std::string { "w1" }, std::string {}, fleet.workerKey.PublicKey() },
           std::tuple { std::string { "w2" }, std::string { ClaimedEndpoint }, Testing::TestKeyPair("w2").PublicKey() } })
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                          .key = id,
                                          .value = {},
                                          .schedulerEndpoint = endpoint,
                                          .publicKey = key });
    REQUIRE(state.members.size() == 2);
    // The control: w2 really does RECORD the endpoint the stranger claims, so an endpoint join would name it w2.
    auto const* const claimed = core::findOrNull(state.members, std::string { "w2" }, &Cluster::ClusterMember::id);
    REQUIRE(claimed != nullptr);
    REQUIRE(claimed->schedulerEndpoint == ClaimedEndpoint);

    AgreedCluster const cluster { state };
    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = &cluster, .metrics = &fleet.metrics });
    auto const nodeAt = [&snapshot](std::string_view endpoint) {
        auto const* const node = core::findOrNull(snapshot.nodes, endpoint, &NodeReport::endpoint);
        REQUIRE(node != nullptr);
        return *node;
    };
    auto const workerAt = [&snapshot](std::string_view endpoint) {
        auto const* const worker = core::findIfOrNull(
            snapshot.workers, [endpoint](WorkerReport const& candidate) { return candidate.info.endpoint == endpoint; });
        REQUIRE(worker != nullptr);
        return *worker;
    };
    // Its own row, named by its key over loopback.
    CHECK(nodeAt(OwnEndpoint).memberId == "w1");
    CHECK(workerAt(OwnEndpoint).memberId == "w1");
    // The claimant is NOT w2, whose endpoint it claimed; its key is no member's, so it is nobody.
    CHECK(nodeAt(ClaimedEndpoint).memberId.empty());
    CHECK(workerAt(ClaimedEndpoint).memberId.empty());

    // And the documents say it in the one `node-id` column, absent at the cell where nobody is placed.
    auto const json = RenderFleetJson(snapshot, FleetHistoryView {});
    INFO(json);
    CHECK(json.contains(R"("endpoint":"127.0.0.1:7100","node-id":"w1")"));
    CHECK(json.contains(R"("endpoint":"10.0.0.7:6674","node-id":null)"));
    CHECK(json.contains(R"("name-source":null,"node-id":"w1")"));
    CHECK(json.contains(R"("name-source":null,"node-id":null)"));
    CHECK_FALSE(json.contains(R"("node-id":"w2")"));
    CHECK(RenderFleetText(snapshot, FleetHistoryView {}, FleetSection::Workers).contains("127.0.0.1:7100\tw1"));

    // Without cluster state nobody is placed: absent, not guessed.
    auto const alone = CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    CHECK(std::ranges::all_of(alone.nodes, [](NodeReport const& node) { return node.memberId.empty(); }));
    CHECK(std::ranges::all_of(alone.workers, [](WorkerReport const& worker) { return worker.memberId.empty(); }));
}

TEST_CASE("SchedulerAdmission.two-members-registering-under-one-endpoint-name-nobody-on-the-machine-row",
          "[distributed][admission][scheduler][fleetview][display-name]")
{
    // One machine row folds every registration at an endpoint. When two MEMBERS' proven keys meet there, the row is
    // no one member's account: `node-id` is null on it -- the answer `fleet_overview` reads -- just as the MCP
    // `toolchains` tool names nobody for registrations that disagree. Each worker row keeps its own key's member.
    Fleet fleet;
    constexpr std::string_view Shared = "10.0.0.7:6674";
    auto const otherKey = Testing::TestKeyPair("w2");
    auto other = fleet.Worker();
    other.provenNodeId = "w2";
    other.provenKey = otherKey.PublicKey();
    for (auto const& [caller, fingerprint]:
         { std::pair { fleet.Worker(), std::string { "gcc-14" } }, std::pair { other, std::string { "clang-18" } } })
        REQUIRE(
            fleet.service
                .Register(caller,
                          WorkerRegistration { .fingerprint = fingerprint, .endpoint = Shared, .slots = 1, .codecs = {} })
                .status
            == Wire::Status::Ok);

    auto state = Cluster::ClusterState {};
    for (auto const& [id, key]: { std::pair { std::string { "w1" }, fleet.workerKey.PublicKey() },
                                  std::pair { std::string { "w2" }, otherKey.PublicKey() } })
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                          .key = id,
                                          .value = {},
                                          .schedulerEndpoint = {},
                                          .publicKey = key });
    AgreedCluster const cluster { state };
    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = &cluster, .metrics = &fleet.metrics });
    REQUIRE(snapshot.nodes.size() == 1);
    CHECK(snapshot.nodes.front().fingerprints.size() == 2);
    CHECK(snapshot.nodes.front().memberId.empty());
    // Each registration still names its own member: the disagreement is the ROW's, never invented per worker.
    auto workerIds = std::vector<std::string> {};
    for (auto const& worker: snapshot.workers)
        workerIds.push_back(worker.memberId);
    std::ranges::sort(workerIds);
    CHECK(workerIds == std::vector<std::string> { "w1", "w2" });
    auto const json = RenderFleetJson(snapshot, FleetHistoryView {});
    INFO(json);
    CHECK(json.contains(R"("name-source":null,"node-id":null)"));
}

TEST_CASE("SchedulerAdmission.a-granted-ticket-lease-carries-its-ticket-id",
          "[distributed][admission][scheduler][fleetview]")
{
    Fleet fleet;
    auto const worker = fleet.RegisterWorker();
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()));
    auto const ticket = fleet.Admit("k1");
    auto const verdicts = fleet.service.PumpAdmission();
    REQUIRE(verdicts.size() == 1);
    auto const grant = GrantOf(verdicts[0]);
    REQUIRE(grant.kind == Wire::GrantKind::Worker);

    fleet.clock.advance(3s);
    auto const rows = CompilesOf(fleet.service);
    REQUIRE(rows.size() == 1);
    auto const& row = rows.front();
    CHECK(row.ticket.ticket == ticket);
    CHECK(row.ticket.state == TicketView::State::Running);
    CHECK(row.ticket.sourcePath == "src/a.cpp");
    CHECK(row.ticket.fingerprint == Toolchain);
    CHECK(row.ticket.treeLabel == "tree");
    CHECK(row.ticket.caller == fleet.service.OwnMachineId());
    CHECK(row.ticket.age == 3s);
    CHECK(row.toolchainLabel == ToolchainLabel);
    REQUIRE(row.ticket.placement.has_value());
    CHECK(Unwrap(row.ticket.placement).workerId == worker);
    // Joined through the lease the grant took: its worker, and what is left of the lifetime the grant told
    // the client, measured on the same clock.
    CHECK(row.worker == std::optional<std::string> { worker });
    CHECK(row.leaseExpiresIn == std::optional<std::chrono::milliseconds> { grant.lifetime - 3s });

    // Settled, the ticket and its lease are gone together.
    REQUIRE(fleet.service.SettleAdmission(ticket, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
            == SettleResult::Settled);
    CHECK(CompilesOf(fleet.service).empty());

    // A node that does not lead has no compiles to show, which is not a fleet compiling nothing.
    fleet.service.SetRole(SchedulerRole::Follower, "10.9.9.9:7100", 1);
    CHECK_FALSE(fleet.service.AdmissionReportNow().compiles.has_value());
    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    CHECK_FALSE(snapshot.compiles.has_value());
    CHECK(RenderFleetJson(snapshot, FleetHistoryView {})
              .contains(R"("compiles":null,"compiles-truncated":null,"compile-totals":null)"));
}

TEST_CASE("SchedulerAdmission.compiles-join-exact-ticket-ids-never-by-cache-key",
          "[distributed][admission][scheduler][fleetview]")
{
    // Two tickets that missed the SAME cache key, unstorable so the queue runs both, each on a worker of
    // its own. A join by cache key would hand both rows one lease; the join by ticket id hands each its own.
    Fleet fleet;
    auto const first = fleet.RegisterWorker(/*slots=*/1);
    constexpr std::string_view SecondEndpoint = "10.0.0.3:7100";
    auto const registered = fleet.service.Register(
        fleet.Worker(),
        WorkerRegistration { .fingerprint = Toolchain, .endpoint = SecondEndpoint, .slots = 1, .codecs = {} });
    REQUIRE(registered.status == Wire::Status::Ok);
    auto const second = Unwrap(Wire::DecodeRegisterReply(registered.payload)).workerId;
    REQUIRE(first != second);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()), 1024 * MiB, 2);
    auto const workerAt = [&](std::string_view endpoint) {
        return endpoint == WorkerEndpoint ? first : second;
    };

    auto const older = fleet.Admit("same");
    auto const olderGrant = GrantOf(fleet.service.PumpAdmission().at(0));
    fleet.clock.advance(5s);
    auto const newer = fleet.Admit("same");
    auto const newerGrant = GrantOf(fleet.service.PumpAdmission().at(0));
    REQUIRE(olderGrant.endpoint != newerGrant.endpoint);

    auto const rows = CompilesOf(fleet.service);
    REQUIRE(rows.size() == 2);
    auto const olderRow = RowOf(rows, older);
    auto const newerRow = RowOf(rows, newer);
    CHECK(olderRow.worker == std::optional<std::string> { workerAt(olderGrant.endpoint) });
    CHECK(newerRow.worker == std::optional<std::string> { workerAt(newerGrant.endpoint) });
    CHECK(olderRow.leaseExpiresIn == std::optional<std::chrono::milliseconds> { olderGrant.lifetime - 5s });
    CHECK(newerRow.leaseExpiresIn == std::optional<std::chrono::milliseconds> { newerGrant.lifetime });

    // Settling one leaves the other joined to ITS lease, not to whatever lease remains for the key.
    REQUIRE(fleet.service.SettleAdmission(older, Settlement { .outcome = Wire::SettleOutcome::NotStored, .peakBytes = {} })
            == SettleResult::Settled);
    auto const after = CompilesOf(fleet.service);
    REQUIRE(after.size() == 1);
    CHECK(after.front().ticket.ticket == newer);
    CHECK(after.front().worker == std::optional<std::string> { workerAt(newerGrant.endpoint) });
}

TEST_CASE("SchedulerAdmission.a-local-placement-reports-worker-local-and-no-expiry",
          "[distributed][admission][scheduler][fleetview]")
{
    Fleet fleet;
    fleet.Report(fleet.service.OwnMachineId());
    auto const ticket = fleet.Admit("k1", Wire::AdmitKind::LocalOnly);
    REQUIRE(GrantOf(fleet.service.PumpAdmission().at(0)).kind == Wire::GrantKind::Local);

    auto const row = RowOf(CompilesOf(fleet.service), ticket);
    CHECK(row.ticket.state == TicketView::State::Running);
    CHECK_FALSE(row.ticket.placement.has_value());
    CHECK(row.worker == std::optional<std::string> { std::string { LocalCompileWorker } });
    CHECK_FALSE(row.leaseExpiresIn.has_value());

    auto const document = Unwrap(Json::Parse(RenderFleetJson(
        CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics }), FleetHistoryView {})));
    auto const json = CompileJsonOf(document, ticket);
    CHECK(TextOf(json, "worker") == LocalCompileWorker);
    CHECK(MemberOf(json, "lease-expires-in").IsNull());
    // The requester exactly, by its key, beside the name a person reads.
    CHECK(TextOf(json, "requester-key") == FormatEd25519PublicKey(fleet.signer.PublicKey()));
    CHECK_FALSE(TextOf(json, "requester").empty());
    CHECK(TextOf(json, "fingerprint") == Toolchain);
}

TEST_CASE("SchedulerAdmission.an-unknown-fingerprint-renders-absent", "[distributed][admission][scheduler][fleetview]")
{
    // A local-only request may not know its toolchain. That is a fingerprint nobody stated -- `null` at
    // the cell -- never an empty string presented as one.
    Fleet fleet;
    auto const ticket = fleet.service.Admit(
        Fleet::Insider,
        [] { return Fleet::Insider; },
        fleet.service.OwnMachineId(),
        Wire::AdmitRequestFields { .kind = Wire::AdmitKind::LocalOnly,
                                   .fingerprint = {},
                                   .cacheKey = "k1",
                                   .sourcePath = "src/a.cpp",
                                   .treeLabel = "tree",
                                   .storable = false,
                                   .acceptedCodecs = {},
                                   .excluded = {},
                                   .toolchainLabel = {} });
    REQUIRE(ticket.has_value());

    auto const document = Unwrap(Json::Parse(RenderFleetJson(
        CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics }), FleetHistoryView {})));
    auto const json = CompileJsonOf(document, Unwrap(ticket));
    CHECK(MemberOf(json, "fingerprint").IsNull());
    CHECK(MemberOf(json, "toolchain").IsNull());
}

TEST_CASE("SchedulerAdmission.queue-depth-and-compile-totals-are-one-sample",
          "[distributed][admission][scheduler][fleetview]")
{
    // The queue's depth and the compile totals are published side by side in one document, so they come
    // from ONE read: `AdmissionReportNow` is the only door to either half, which is the structural half of
    // the guarantee. This is the behavioural half -- a helper thread admits and abandons as fast as it can
    // while this thread reads, and every read's `depth` equals the queued tickets it lists.
    constexpr int Churns = 50'000;
    constexpr int Reads = 50'000;
    Fleet fleet;
    // A reported machine keeps the queue's view present whatever the depth; nothing is placed on it.
    fleet.Report(MachineOf(Testing::TestKeyPair("bystander").PublicKey()), 1024 * MiB, 0);
    auto const request = Wire::AdmitRequestFields { .kind = Wire::AdmitKind::LocalOnly,
                                                    .fingerprint = {},
                                                    .cacheKey = "k",
                                                    .sourcePath = "src/a.cpp",
                                                    .treeLabel = "tree",
                                                    .storable = false,
                                                    .acceptedCodecs = {},
                                                    .excluded = {},
                                                    .toolchainLabel = {} };

    std::latch start { 2 };
    auto churn = std::thread { [&] {
        start.arrive_and_wait();
        // Every ticket is abandoned again, so the queue stays small and each read stays cheap; the depth
        // still moves on every admit and every abandon.
        for ([[maybe_unused]] auto const round: std::views::iota(0, Churns))
        {
            auto const ticket =
                fleet.service.Admit(Fleet::Insider, [] { return Fleet::Insider; }, fleet.service.OwnMachineId(), request);
            if (ticket.has_value())
                fleet.service.AbandonAdmission(*ticket);
        }
    } };

    start.arrive_and_wait();
    auto disagreements = 0;
    auto sampled = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, Reads))
    {
        auto const report = fleet.service.AdmissionReportNow();
        if (!report.queue.has_value() || !report.compiles.has_value())
            continue;
        ++sampled;
        auto const queued = std::ranges::count(
            Unwrap(report.compiles), TicketView::State::Queued, [](CompileRow const& row) { return row.ticket.state; });
        if (std::cmp_not_equal(queued, Unwrap(report.queue).depth))
            ++disagreements;
    }
    churn.join();

    CHECK(sampled == Reads);
    CHECK(disagreements == 0);
}

TEST_CASE("SchedulerAdmission.queued-tickets-report-no-worker", "[distributed][admission][scheduler][fleetview]")
{
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/1);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()), 1024 * MiB, 1);
    (void) fleet.Admit("k1");
    auto const queued = fleet.Admit("k2");
    REQUIRE(fleet.service.PumpAdmission().size() == 1);

    auto const row = RowOf(CompilesOf(fleet.service), queued);
    CHECK(row.ticket.state == TicketView::State::Queued);
    CHECK_FALSE(row.ticket.placement.has_value());
    CHECK_FALSE(row.worker.has_value());
    CHECK_FALSE(row.leaseExpiresIn.has_value());

    // In the document the cells are ABSENT -- `null`, the fleet document's spelling of it -- never an
    // empty string, `local`, or a zero.
    auto const document = Unwrap(Json::Parse(RenderFleetJson(
        CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics }), FleetHistoryView {})));
    auto const json = CompileJsonOf(document, queued);
    CHECK(TextOf(json, "state") == "queued");
    CHECK(MemberOf(json, "worker").IsNull());
    CHECK(MemberOf(json, "lease-expires-in").IsNull());
}

TEST_CASE("SchedulerAdmission.compile-totals-count-tickets-as-the-queue-does",
          "[distributed][admission][scheduler][fleetview]")
{
    // Running equals what was granted and not settled, queued equals the queue's own depth, and a request
    // waiting on an identical storable compile is a third state -- in neither of those, and still listed.
    Fleet fleet;
    (void) fleet.RegisterWorker(/*slots=*/2);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()), 1024 * MiB, 2);
    auto const owner = fleet.Admit("shared", Wire::AdmitKind::Dispatchable, /*storable=*/true);
    auto const waiter = fleet.Admit("shared", Wire::AdmitKind::Dispatchable, /*storable=*/true);
    (void) fleet.Admit("k2");
    auto const queued = fleet.Admit("k3");
    auto const granted = fleet.service.PumpAdmission();

    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    REQUIRE(snapshot.compiles.has_value());
    REQUIRE(snapshot.admission.has_value());
    auto const& compiles = Unwrap(snapshot.compiles);
    auto const total = [&](TicketView::State state) {
        return compiles.totals[static_cast<std::size_t>(state)];
    };
    CHECK(total(TicketView::State::Running) == granted.size());
    CHECK(total(TicketView::State::Queued) == Unwrap(snapshot.admission).depth);
    CHECK(total(TicketView::State::Waiting) == 1);
    CHECK(compiles.rows.size() == 4);
    CHECK(compiles.truncated == 0);

    auto const stateOf = [&](Ticket ticket) {
        return std::ranges::find(compiles.rows, ticket, [](FleetCompile const& c) { return c.compile.ticket.ticket; })
            ->compile.ticket.state;
    };
    CHECK(stateOf(owner) == TicketView::State::Running);
    CHECK(stateOf(waiter) == TicketView::State::Waiting);
    CHECK(stateOf(queued) == TicketView::State::Queued);

    CHECK(RenderFleetJson(snapshot, FleetHistoryView {})
              .contains(std::format(R"("compile-totals":{{"queued":{},"waiting":1,"running":{}}})",
                                    Unwrap(snapshot.admission).depth,
                                    granted.size())));

    // Woken by the owner's store and not yet answered, the waiter is still in progress: still waiting.
    REQUIRE(fleet.service.SettleAdmission(owner, Settlement { .outcome = Wire::SettleOutcome::Stored, .peakBytes = {} })
            == SettleResult::Settled);
    CHECK(RowOf(CompilesOf(fleet.service), waiter).ticket.state == TicketView::State::Waiting);
    (void) fleet.service.PumpAdmission();
    auto const answered = CompilesOf(fleet.service);
    CHECK(std::ranges::find(answered, waiter, [](CompileRow const& row) { return row.ticket.ticket; }) == answered.end());
}

TEST_CASE("SchedulerAdmission.compiles-over-the-ceiling-report-truncated", "[distributed][admission][scheduler][fleetview]")
{
    // Nothing reports a budget, so every ticket stays queued. One past the ceiling is left out of the
    // list -- the NEWEST, since the list is oldest first -- and counted, and the totals still count it.
    Fleet fleet;
    std::vector<Ticket> tickets;
    tickets.reserve(FleetJsonCompileCeiling + 1);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, FleetJsonCompileCeiling + 1))
        tickets.push_back(fleet.Admit(std::format("k{}", index)));

    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    REQUIRE(snapshot.compiles.has_value());
    auto const& compiles = Unwrap(snapshot.compiles);
    CHECK(compiles.rows.size() == FleetJsonCompileCeiling);
    CHECK(compiles.truncated == 1);
    CHECK(compiles.totals[static_cast<std::size_t>(TicketView::State::Queued)] == FleetJsonCompileCeiling + 1);
    CHECK(compiles.rows.front().compile.ticket.ticket == tickets.front());
    CHECK(compiles.rows.back().compile.ticket.ticket == tickets[FleetJsonCompileCeiling - 1]);
    CHECK(RenderFleetJson(snapshot, FleetHistoryView {}).contains(R"("compiles-truncated":1,)"));
}

TEST_CASE("SchedulerAdmission.fleet-collects-every-lease-past-the-page-window",
          "[distributed][admission][scheduler][fleetview]")
{
    // The page shows `OutstandingLeaseRows`; the snapshot -- and so the JSON -- holds every live lease
    // up to `FleetJsonLeaseCeiling`, so a program reading it is not handed the page's window as the whole.
    constexpr auto Granted = static_cast<std::uint32_t>(OutstandingLeaseRows + 5);
    Fleet fleet;
    (void) fleet.RegisterWorker(Granted);
    fleet.Report(MachineOf(fleet.workerKey.PublicKey()), Granted * 128 * MiB, Granted);
    for (auto const index: std::views::iota(std::uint32_t { 0 }, Granted))
        (void) fleet.Admit(std::format("k{}", index));
    REQUIRE(fleet.service.PumpAdmission().size() == Granted);

    auto const snapshot = CollectFleet({ .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    CHECK(snapshot.liveLeases == Granted);
    CHECK(snapshot.outstandingLeases.size() == Granted);
    CHECK(RenderFleetJson(snapshot, FleetHistoryView {}).contains(R"("leases-outstanding-truncated":0)"));
}
