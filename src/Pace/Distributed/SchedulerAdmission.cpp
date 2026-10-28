// SPDX-License-Identifier: Apache-2.0
//
// The admission half of `SchedulerService` (#1728): the queue, its estimates and the grants it mints.
// Kept in its own translation unit because it is a feature beside the lease verbs and not a part of
// them -- the lease path above this stays the one it was.
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

AdmissionVerdict SchedulerService::GrantFor(Decision const& decision, AdmissionTicket& held)
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
            {
                // Dropped between the pump deciding and the grant: nothing was charged to a lease yet.
                verdict.refusal = Refuse(Wire::ErrorCode::NoWorker);
                return verdict;
            }
            auto const lease = _leases.Acquire(held.cacheKey, worker->id, AgreedLeaseLifetime());
            if (!lease.has_value())
            {
                verdict.refusal = Refuse(Wire::ErrorCode::AlreadyInFlight);
                return verdict;
            }
            _workers.JobStarted(worker->id);
            held.lease = *lease;
            auto const token = MintGrantToken(*lease, worker->endpoint, worker->fingerprint, worker->identityKey);
            verdict.grant = Wire::EncodeAdmitGrant(Wire::AdmitGrant { .kind = Wire::GrantKind::Worker,
                                                                      .endpoint = worker->endpoint,
                                                                      .leaseToken = token,
                                                                      .workerCodecs = worker->codecs,
                                                                      .chargedBytes = decision.chargedBytes,
                                                                      .queuedMs = queuedMs });
            return verdict;
        }
        case Wire::GrantKind::Local:
        case Wire::GrantKind::Stored:
            verdict.grant = Wire::EncodeAdmitGrant(Wire::AdmitGrant { .kind = decision.kind,
                                                                      .endpoint = {},
                                                                      .leaseToken = {},
                                                                      .workerCodecs = {},
                                                                      .chargedBytes = decision.chargedBytes,
                                                                      .queuedMs = queuedMs });
            return verdict;
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
    for (auto freed = true; freed;)
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
            auto verdict = GrantFor(decision, held->second);
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

void SchedulerService::SettleAdmission(Ticket ticket, Settlement const& settlement)
{
    auto const guard = std::scoped_lock { _admissionMutex };
    _queue.Settle(ticket, settlement);
    // A worker that refused the job asks for it back in the queue, and the queue would park it at the
    // head for a connection nobody holds any more: the launcher asks again with a fresh ADMIT. So the
    // re-queued entry leaves at once -- the refusal still stands on the machine's ledger, which is what
    // keeps a refusing machine from being granted again before it reports (R7).
    if (settlement.outcome == Wire::SettleOutcome::WorkerRefused)
        _queue.Abandon(ticket);
    if (auto const held = _admissionTickets.find(ticket); held != _admissionTickets.end())
    {
        ReleaseAdmissionLease(held->second);
        _admissionTickets.erase(held);
    }
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
