// SPDX-License-Identifier: Apache-2.0
//
// The admission half of `SchedulerService` (#1728): the queue, its estimates and the grants it mints.
// Kept in its own translation unit because it is a feature beside the lease verbs and not a part of
// them -- the lease path above this stays the one it was.
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Pace::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A key as the registry stores one: the raw bytes, in a string.
    /// @param key An identity public key.
    /// @return Its bytes.
    [[nodiscard]] std::string KeyAsMachineId(Ed25519PublicKey const& key)
    {
        std::string out;
        out.reserve(key.size());
        for (auto const byte: key)
            out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        return out;
    }

    /// What a `Local` grant of a dispatchable request tells the LEADER's own operator, beside what it
    /// tells the launcher: the counter and the `unserved-toolchain` record the LEASE path moves for the
    /// same fleet (`PickErrorTable`), so the leader's signals survive admission carrying the dispatches.
    struct LocalGrantSignalRow
    {
        Wire::LocalGrantReason reason;                ///< The reason this row describes.
        std::optional<IMetricsSink::Counter> counter; ///< What rises; nothing for a decision that is no refusal.
        /// Whether the toolchain is remembered as one nobody serves: `NoWorker` alone, `PickErrorTable`'s rule.
        bool unservedToolchain;
    };

    /// One row per `LocalGrantReason`.
    constexpr EnumTable<Wire::LocalGrantReason, LocalGrantSignalRow> LocalGrantSignals { {
        // Asked to run here: nothing was refused.
        { .reason = Wire::LocalGrantReason::Asked, .counter = std::nullopt, .unservedToolchain = false },
        { .reason = Wire::LocalGrantReason::NoWorker,
          .counter = IMetricsSink::Counter::DispatchLeasesNoWorker,
          .unservedToolchain = true },
        // `PickError::Excluded`'s counter: the fleet serves the toolchain, the client could not reach it.
        { .reason = Wire::LocalGrantReason::AllExcluded,
          .counter = IMetricsSink::Counter::DispatchLeasesAllExcluded,
          .unservedToolchain = false },
        // Ruling 15: NOT counted. Workers serve the toolchain and none had room when the queue reached
        // the request, so the queue ran it on the caller's own machine -- an ordinary queue decision to
        // compile locally, which is what a queue is for, and not a refusal. Counted as `NoCapacity` it
        // would read as "buy more machines" on every build that merely filled its fleet.
        { .reason = Wire::LocalGrantReason::NoRoom, .counter = std::nullopt, .unservedToolchain = false },
    } };

    static_assert(RowsInEnumeratorOrder(LocalGrantSignals, &LocalGrantSignalRow::reason),
                  "LocalGrantSignals must hold one row per LocalGrantReason, in enumerator order");
} // namespace

std::vector<Placement> SchedulerService::RegistryPlacement::Candidates(std::string_view fingerprint,
                                                                       std::span<std::string const> excluded) const
{
    std::vector<Placement> out;
    for (auto const& worker: _workers.Candidates(fingerprint, excluded))
        // A worker that proved no identity has no machine to charge, so it cannot be placed on.
        if (!worker.identityKey.empty())
            out.push_back(Placement { .workerId = worker.id, .machine = worker.identityKey, .endpoint = worker.endpoint });
    return out;
}

Wire::LocalGrantReason LocalGrantReasonOf(std::span<WorkerInfo const> live,
                                          std::string_view fingerprint,
                                          std::span<std::string const> excluded)
{
    // Only a worker the queue could place on counts as serving: one that proved no identity has no
    // machine to charge (`RegistryPlacement::Candidates`).
    auto const serves = [&](WorkerInfo const& worker) {
        return worker.fingerprint == fingerprint && !worker.identityKey.empty();
    };
    auto const reachable = [&](WorkerInfo const& worker) {
        return serves(worker) && !std::ranges::contains(excluded, worker.endpoint);
    };
    if (std::ranges::none_of(live, serves))
        return Wire::LocalGrantReason::NoWorker;
    if (std::ranges::none_of(live, reachable))
        return Wire::LocalGrantReason::AllExcluded;
    return Wire::LocalGrantReason::NoRoom;
}

MachineId SchedulerService::OwnMachineId() const
{
    return KeyAsMachineId(_signer.PublicKey());
}

std::expected<Ticket, SchedulerReply> SchedulerService::Admit(CallerContext const& caller,
                                                              std::function<CallerContext()> regate,
                                                              MachineId const& machine,
                                                              Wire::AdmitRequestFields const& request)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::unexpected { std::move(*refusal) };

    // A lease is granted against a KEY, so a request a worker may run must name one; a local-only
    // request that does not know its key still queues.
    if (request.kind == Wire::AdmitKind::Dispatchable && (request.cacheKey.empty() || request.fingerprint.empty()))
        return std::unexpected { SchedulerReply::Malformed("a dispatchable admission names its cache key and toolchain") };

    auto const guard = std::scoped_lock { _admissionMutex };
    auto const ticket = _queue.Enqueue(QueueRequest { .kind = request.kind,
                                                      .caller = machine,
                                                      .fingerprint = request.fingerprint,
                                                      .estimate = EstimateKey { .cacheKey = request.cacheKey,
                                                                                .sourcePath = request.sourcePath,
                                                                                .compilerIdentity = request.fingerprint },
                                                      .storable = request.storable,
                                                      .treeLabel = request.treeLabel,
                                                      .excluded = request.excluded });
    _admissionTickets.emplace(ticket,
                              AdmissionTicket { .regate = std::move(regate),
                                                .fingerprint = request.fingerprint,
                                                .cacheKey = request.cacheKey,
                                                .kind = request.kind,
                                                .excluded = request.excluded,
                                                .toolchainLabel = request.toolchainLabel,
                                                .enqueuedAt = _clock.now(),
                                                .lease = std::nullopt });
    return ticket;
}

void SchedulerService::ReleaseAdmissionLease(AdmissionTicket& held)
{
    if (!held.lease.has_value())
        return;
    // The same two undoings `Release` makes, for the same two reasons: the key stops being in flight,
    // and the registry speculative job count the grant added comes back.
    if (_leases.Release(held.lease->token, held.lease->key).has_value())
        _workers.JobFinished(held.lease->workerId);
    held.lease.reset();
}

std::optional<AdmissionVerdict> SchedulerService::GrantFor(Decision const& decision, AdmissionTicket& held)
{
    auto verdict =
        AdmissionVerdict { .ticket = decision.ticket,
                           .refusal = std::nullopt,
                           .kind = decision.kind,
                           .grant = {},
                           .chargedBytes = decision.chargedBytes,
                           .ranAlone = decision.ranAlone,
                           .queued = std::chrono::duration_cast<std::chrono::milliseconds>(_clock.now() - held.enqueuedAt) };

    // R9: a request that waited is asked again. A forget, or a lost leadership, during a long wait ends
    // in a refusal and never in a grant.
    if (auto refusal = Gate(held.regate()); refusal.has_value())
    {
        verdict.refusal = std::move(*refusal);
        return verdict;
    }

    auto const queuedMs = static_cast<std::uint64_t>(verdict.queued.count());
    switch (decision.kind)
    {
        case Wire::GrantKind::Worker: {
            auto const worker = decision.placement.has_value() ? _workers.Find(decision.placement->workerId) : std::nullopt;
            if (!worker.has_value())
                // Dropped between the pump placing it and this grant: nothing was charged to a lease
                // yet, and nobody refused it. The caller puts it back at the front; a `NoWorker` here
                // would send a request the queue still had room for to a client told to give up.
                return std::nullopt;
            auto const lease = _leases.Acquire(held.cacheKey, worker->id, AgreedLeaseLifetime());
            if (!lease.has_value())
            {
                verdict.refusal = Refuse(Wire::ErrorCode::AlreadyInFlight);
                return verdict;
            }
            _workers.JobStarted(worker->id);
            held.lease = *lease;
            auto const token = MintGrantToken(*lease, worker->endpoint, worker->fingerprint, worker->identityKey);
            // The lifetime and the key beside the token exactly as `Lease` relays them, from the lease and
            // the worker the token was just minted from: the client bounds its compile by the one and
            // authenticates the reply against the other, and verifies no claims to learn either.
            verdict.grant = Wire::EncodeAdmitGrant(Wire::AdmitGrant { .kind = Wire::GrantKind::Worker,
                                                                      .endpoint = worker->endpoint,
                                                                      .leaseToken = token,
                                                                      .workerCodecs = worker->codecs,
                                                                      .chargedBytes = decision.chargedBytes,
                                                                      .queuedMs = queuedMs,
                                                                      .lifetime = lease->lifetime,
                                                                      .workerKey = Wire::AsBytes(worker->identityKey),
                                                                      .reason = Wire::LocalGrantReason::Asked });
            return verdict;
        }
        case Wire::GrantKind::Local:
        case Wire::GrantKind::Stored: {
            // A dispatchable request kept here says why, so the launcher records a fleet that placed it
            // nowhere as it records a lease the scheduler refused -- a fleet that distributes nothing must
            // not go green in silence.
            auto const reason = decision.kind == Wire::GrantKind::Local && held.kind == Wire::AdmitKind::Dispatchable
                                    ? LocalGrantReasonOf(_workers.LiveWorkers(), held.fingerprint, held.excluded)
                                    : Wire::LocalGrantReason::Asked;
            // Under `_admissionMutex`: the counter is an atomic, and `_unserved` takes only its own lock
            // and calls nothing back, so nothing can take these two in the other order.
            auto const& signal = LocalGrantSignals[static_cast<std::size_t>(reason)];
            if (signal.counter.has_value())
                _metrics.Increment(*signal.counter);
            if (signal.unservedToolchain)
                _unserved.Refused(held.fingerprint, held.toolchainLabel);
            verdict.grant = Wire::EncodeAdmitGrant(Wire::AdmitGrant { .kind = decision.kind,
                                                                      .endpoint = {},
                                                                      .leaseToken = {},
                                                                      .workerCodecs = {},
                                                                      .chargedBytes = decision.chargedBytes,
                                                                      .queuedMs = queuedMs,
                                                                      .lifetime = {},
                                                                      .workerKey = {},
                                                                      .reason = reason });
            return verdict;
        }
        case Wire::GrantKind::Last:
            break;
    }
    verdict.refusal = SchedulerReply::Malformed("the queue decided a grant this build cannot encode");
    return verdict;
}

std::vector<AdmissionVerdict> SchedulerService::PumpAdmission()
{
    // First, for `Lease`'s reason: a worker that vanished mid-job left its leases behind, and a
    // placement over a registry still holding it would grant onto a machine that is gone.
    ReapExpiredWorkers();

    std::vector<AdmissionVerdict> verdicts;
    auto const guard = std::scoped_lock { _admissionMutex };
    auto freed = true;
    while (freed)
    {
        freed = false;
        for (auto const& decision: _queue.Pump())
        {
            auto const held = _admissionTickets.find(decision.ticket);
            if (held == _admissionTickets.end())
            {
                // Nobody is left to answer (forgotten at demotion): give the charge straight back.
                _queue.Abandon(decision.ticket);
                freed = true;
                continue;
            }
            auto granted = GrantFor(decision, held->second);
            if (!granted.has_value())
            {
                // Its worker left between placement and grant: back at the front, uncounted. The freed
                // charge may fit it elsewhere, so the pump asks again -- and cannot place it on the
                // same worker, which the registry no longer offers.
                _queue.ReturnToFront(decision.ticket);
                freed = true;
                continue;
            }
            auto verdict = std::move(*granted);
            // A refusal holds nothing, so the charge the queue took goes back at once -- which frees
            // room, so the pump asks again.
            if (verdict.refusal.has_value())
            {
                _queue.Abandon(decision.ticket);
                _admissionTickets.erase(held);
                freed = true;
            }
            // A `Stored` answer charged nothing and the connection holds nothing: the ticket is done.
            else if (decision.kind == Wire::GrantKind::Stored)
                _admissionTickets.erase(held);
            verdicts.push_back(std::move(verdict));
        }
    }
    return verdicts;
}

SettleResult SchedulerService::SettleAdmission(Ticket ticket, Settlement const& settlement)
{
    auto const guard = std::scoped_lock { _admissionMutex };
    auto const refused = settlement.outcome == Wire::SettleOutcome::WorkerRefused;
    if (refused && !_admissionLeading)
    {
        // A follower's queue is pumped by nobody: abandoned, never re-parked there.
        _queue.Abandon(ticket);
        if (auto const held = _admissionTickets.find(ticket); held != _admissionTickets.end())
        {
            ReleaseAdmissionLease(held->second);
            _admissionTickets.erase(held);
        }
        return SettleResult::NotLeading;
    }
    // A worker that refused the job puts the SAME ticket back at the front of the queue, its refusal
    // counted against the request's bound and its machine's ledger marked refused until it reports
    // (R7) -- the queue does both. The record stays, so the connection's next `ADMIT` waits on this
    // ticket rather than starting a new one at the back.
    _queue.Settle(ticket, settlement);
    if (auto const held = _admissionTickets.find(ticket); held != _admissionTickets.end())
    {
        // The worker the grant named is not running it, whatever the outcome: its lease goes back.
        ReleaseAdmissionLease(held->second);
        if (!refused)
            _admissionTickets.erase(held);
    }
    return refused ? SettleResult::Requeued : SettleResult::Settled;
}

void SchedulerService::AbandonAdmission(Ticket ticket)
{
    auto const guard = std::scoped_lock { _admissionMutex };
    _queue.Abandon(ticket);
    if (auto const held = _admissionTickets.find(ticket); held != _admissionTickets.end())
    {
        ReleaseAdmissionLease(held->second);
        _admissionTickets.erase(held);
    }
}

SchedulerReply SchedulerService::ReportMachineBudget(CallerContext const& caller,
                                                     MachineId const& machine,
                                                     Wire::MachineBudgetReport const& report)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    auto const guard = std::scoped_lock { _admissionMutex };
    _queue.ReportMachine(machine,
                         MachineReport { .memoryBudgetBytes = report.memoryBudgetBytes,
                                         .slots = report.slots,
                                         .heldSlots = report.heldSlots,
                                         .heldBytes = report.heldBytes });
    for (auto const& [key, peak]: report.peaks)
        _estimates.Record(
            EstimateKey { .cacheKey = key.cacheKey, .sourcePath = key.sourcePath, .compilerIdentity = key.compilerIdentity },
            peak);
    return SchedulerReply::Success();
}

AdmissionSnapshot SchedulerService::AdmissionSnapshotNow() const
{
    auto const guard = std::scoped_lock { _admissionMutex };
    return AdmissionSnapshot { .depth = _queue.Depth(), .chargedBytes = _queue.ChargedByMachine() };
}

std::optional<QueueView> SchedulerService::AdmissionDetailNow() const
{
    auto const guard = std::scoped_lock { _admissionMutex };
    if (!_admissionLeading)
        return std::nullopt;
    auto view = _queue.Snapshot();
    if (view.depth == 0 && view.clients.empty() && view.machines.empty())
        return std::nullopt;
    return view;
}

void SchedulerService::ObserveAdmission(AdmissionObserver observer)
{
    auto const guard = std::scoped_lock { _leaderMutex };
    _admissionObserver = std::move(observer);
}

void SchedulerService::NotifyAdmissionChanged()
{
    auto changed = std::function<void()> {};
    {
        auto const guard = std::scoped_lock { _leaderMutex };
        changed = _admissionObserver.changed;
    }
    if (changed)
        changed();
}

} // namespace Pace::Distributed
