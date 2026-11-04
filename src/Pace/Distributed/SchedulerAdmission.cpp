// SPDX-License-Identifier: Apache-2.0
//
// The admission half of `SchedulerService` (#1728): the queue, its estimates and the grants it mints.
// Kept in its own translation unit because it is a feature beside the lease verbs and not a part of
// them -- the lease path above this stays the one it was.
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Distributed/DialHint.hpp>
#include <Pace/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Pace::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// The name an admission's worker lease is filed under in the lease table (Ruling 17).
    ///
    /// One per TICKET, so two admissions never contend for one entry: deduplication is the queue's
    /// decision, taken only where a waiter can fetch the result. Spelled with spaces, which no object
    /// key holds, so it cannot collide with a `Lease` verb's entry for a real key.
    /// @param ticket The admission.
    /// @return The lease table's key for it.
    [[nodiscard]] std::string AdmissionLeaseKey(Ticket ticket)
    {
        return std::format("admission ticket {}", ticket);
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
        // Counted, unlike `NoRoom`: it is a SHAPE of the fleet -- nobody but the caller serves the toolchain --
        // and an operator who expected distribution reads it here (#1745). The toolchain is served, by the caller.
        { .reason = Wire::LocalGrantReason::OwnMachineOnly,
          .counter = IMetricsSink::Counter::DispatchLeasesOwnMachineOnly,
          .unservedToolchain = false },
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
                                          std::span<std::string const> excluded,
                                          std::string_view caller)
{
    // Only a worker the queue could place on counts as serving: one that proved no identity has no
    // machine to charge (`RegistryPlacement::Candidates`), and one on the caller's own machine is never
    // placed on (`AdmissionQueue::Pump`).
    auto const servesAnyone = [&](WorkerInfo const& worker) {
        return worker.fingerprint == fingerprint && !worker.identityKey.empty();
    };
    auto const serves = [&](WorkerInfo const& worker) {
        return servesAnyone(worker) && worker.identityKey != caller;
    };
    auto const reachable = [&](WorkerInfo const& worker) {
        return serves(worker) && !std::ranges::contains(excluded, worker.endpoint);
    };
    if (std::ranges::none_of(live, serves))
        return std::ranges::any_of(live, servesAnyone) ? Wire::LocalGrantReason::OwnMachineOnly
                                                       : Wire::LocalGrantReason::NoWorker;
    if (std::ranges::none_of(live, reachable))
        return Wire::LocalGrantReason::AllExcluded;
    return Wire::LocalGrantReason::NoRoom;
}

std::string KeyAsMachineId(Ed25519PublicKey const& key)
{
    std::string out;
    out.reserve(key.size());
    for (auto const byte: key)
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    return out;
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
                                                .parkedAt = _clock.now(),
                                                .lease = std::nullopt,
                                                .machine = machine,
                                                // A re-admission of a compile this leader already granted (the
                                                // launcher's local retry after the granted worker failed it) is the
                                                // SAME compile: reported as already counted, so it never is twice.
                                                .demandReported = request.origin == Wire::AskOrigin::AfterGrant });
    return ticket;
}

void SchedulerService::ReleaseAdmissionLease(AdmissionTicket& held, AdmissionLeaseEnd end)
{
    if (!held.lease.has_value())
        return;
    // The same two undoings `Release` makes, for the same two reasons: the key stops being in flight,
    // and the registry speculative job count the grant added comes back. And the same count, but only
    // when the CLIENT resolved it: a closed connection is the case `DispatchLeasesReleased` staying
    // flat beside a climbing grant count exists to show (#1742). The sink is an atomic, so counting
    // under `_admissionMutex` takes no second lock.
    // A release the table refuses is spent through the same row `Release` consults (an expired lease is
    // `DispatchLeasesReleasedLate`), and only for a client's SETTLE: an abandon is no report at all.
    auto const released = _leases.Release(held.lease->token, held.lease->key);
    if (released.has_value())
    {
        _workers.JobFinished(held.lease->workerId);
        if (end == AdmissionLeaseEnd::Settled)
            _metrics.Increment(IMetricsSink::Counter::DispatchLeasesReleased);
    }
    else if (end == AdmissionLeaseEnd::Settled)
        CountReleaseRefusal(released.error());
    held.lease.reset();
}

void SchedulerService::ReportDemandOnce(AdmissionTicket& held)
{
    if (_demand == nullptr || std::exchange(held.demandReported, true))
        return;
    // Under `_admissionMutex`: the sink takes only its own stores' locks and calls nothing back.
    _demand->CompileGranted(held.machine, held.fingerprint);
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
                           .queued = std::chrono::duration_cast<std::chrono::milliseconds>(_clock.now() - held.enqueuedAt),
                           .waited = std::chrono::duration_cast<std::chrono::milliseconds>(_clock.now() - held.parkedAt) };

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
            // Keyed by the TICKET, never by the cache key (Ruling 17). Deduplication is the QUEUE's, and
            // only where a waiter can fetch the result (`SharesCache`); two machines that missed the same
            // key without sharing a cache must BOTH compile, and a lease table keyed by the cache key
            // refused the second `AlreadyInFlight` -- which its launcher read as "compile here", with
            // nothing charged. A ticket is unique on this leader, so this `Acquire` cannot be refused.
            //
            // The ticket rides on the lease too, so a report joins a running compile to ITS lease by the
            // ticket rather than by parsing the bookkeeping name back, or by guessing from the cache key.
            auto const lease =
                _leases.Acquire(AdmissionLeaseKey(decision.ticket), worker->id, AgreedLeaseLifetime(), decision.ticket);
            if (!lease.has_value())
            {
                verdict.refusal = SchedulerReply::Malformed("an admission ticket was granted twice");
                return verdict;
            }
            _workers.JobStarted(worker->id);
            // A queued worker grant IS a dispatch, as a `Lease` grant is: the fleet page's Dispatched
            // figure reads this counter, and read zero for every compile the queue placed (#1742).
            // Under `_admissionMutex`, for `LocalGrantSignals`' reason below.
            _metrics.Increment(IMetricsSink::Counter::DispatchLeasesGranted);
            held.lease = *lease;
            // The token signs the OBJECT key, as a `Lease` grant's does: the table's key is a bookkeeping
            // name the worker never sees, and what a grant covers is the key the client missed.
            auto signedLease = *lease;
            signedLease.key = held.cacheKey;
            auto const token = MintGrantToken(signedLease, worker->endpoint, worker->fingerprint, worker->identityKey);
            // A hint BESIDE the name, from the predicate the `Lease` grant asks: the token signs the name,
            // so a stale hint landing on another machine is refused there and the client falls back.
            auto const hint = DialHintFor(DialHintInputs { .advertised = worker->endpoint,
                                                           .observedHost = worker->observedHost,
                                                           .interfaceAddresses = worker->interfaceAddresses });
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
                                                                      .reason = Wire::LocalGrantReason::Asked,
                                                                      .dialHint = hint });
            ReportDemandOnce(held);
            return verdict;
        }
        case Wire::GrantKind::Local:
        case Wire::GrantKind::Stored: {
            // A dispatchable request kept here says why, so the launcher records a fleet that placed it
            // nowhere as it records a lease the scheduler refused -- a fleet that distributes nothing must
            // not go green in silence.
            auto const reason =
                decision.kind == Wire::GrantKind::Local && held.kind == Wire::AdmitKind::Dispatchable
                    ? LocalGrantReasonOf(_workers.LiveWorkers(), held.fingerprint, held.excluded, held.machine)
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
                                                                      .reason = reason,
                                                                      .dialHint = {} });
            // A `Stored` answer ran no compile: another of the same key stored the object, so nothing was asked of the
            // fleet that this request alone caused.
            if (decision.kind == Wire::GrantKind::Local)
                ReportDemandOnce(held);
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
            ReleaseAdmissionLease(held->second, AdmissionLeaseEnd::Settled);
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
        ReleaseAdmissionLease(held->second, AdmissionLeaseEnd::Settled);
        if (!refused)
            _admissionTickets.erase(held);
        else
            // Parked again: its next grant's wait is measured from here, so the wait before the refusal
            // is not summed a second time.
            held->second.parkedAt = _clock.now();
    }
    return refused ? SettleResult::Requeued : SettleResult::Settled;
}

void SchedulerService::AbandonAdmission(Ticket ticket)
{
    auto const guard = std::scoped_lock { _admissionMutex };
    _queue.Abandon(ticket);
    if (auto const held = _admissionTickets.find(ticket); held != _admissionTickets.end())
    {
        ReleaseAdmissionLease(held->second, AdmissionLeaseEnd::Abandoned);
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
    auto snapshot = AdmissionSnapshot { .depth = _queue.Depth(), .chargedBytes = _queue.ChargedByMachine(), .budgets = {} };
    for (auto& machine: _queue.Snapshot().machines)
        if (machine.budgetBytes.has_value())
            snapshot.budgets.push_back(std::move(machine));
    return snapshot;
}

std::optional<QueueView> SchedulerService::AdmissionDetailLocked() const
{
    if (!_admissionLeading)
        return std::nullopt;
    auto view = _queue.Snapshot();
    if (view.depth == 0 && view.clients.empty() && view.machines.empty())
        return std::nullopt;
    return view;
}

std::optional<std::vector<CompileRow>> SchedulerService::CompileTicketsLocked() const
{
    if (!_admissionLeading)
        return std::nullopt;

    // Read under the admission lock, after the queue: every admission lease is acquired and released
    // under this same lock, so the two readings are one sample. A lease the registry's drop of its
    // worker released (`ReleaseWorker`, outside this lock) is simply not there -- a running compile
    // whose worker the join cannot establish, and the row says so by leaving it absent.
    auto const leases = _leases.LiveTicketLeases();
    std::unordered_map<Ticket, TicketLease const*> byTicket;
    byTicket.reserve(leases.size());
    for (auto const& lease: leases)
        byTicket.emplace(lease.ticket, &lease);

    std::vector<CompileRow> rows;
    for (auto& ticket: _queue.Tickets())
    {
        auto row = CompileRow { .ticket = std::move(ticket), .toolchainLabel = {}, .worker = {}, .leaseExpiresIn = {} };
        if (auto const held = _admissionTickets.find(row.ticket.ticket); held != _admissionTickets.end())
            row.toolchainLabel = held->second.toolchainLabel;
        if (row.ticket.state == TicketView::State::Running)
        {
            if (!row.ticket.placement.has_value())
                row.worker = std::string { LocalCompileWorker };
            else if (auto const lease = byTicket.find(row.ticket.ticket); lease != byTicket.end())
            {
                row.worker = lease->second->workerId;
                row.leaseExpiresIn = lease->second->expiresIn;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

AdmissionReport SchedulerService::AdmissionReportNow() const
{
    // ONE acquisition for both halves: a pump between two reads would publish a queue depth beside
    // ticket totals from a different moment, in one document.
    auto const guard = std::scoped_lock { _admissionMutex };
    return AdmissionReport { .queue = AdmissionDetailLocked(), .compiles = CompileTicketsLocked() };
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
