// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/AdmissionQueue.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace Pace::Distributed
{

namespace
{

    /// @return a - b, or zero when b exceeds a.
    template <typename T>
    constexpr T Saturating(T a, T b) noexcept
    {
        return a > b ? static_cast<T>(a - b) : T {};
    }

} // namespace

AdmissionQueue::AdmissionQueue(PeakEstimates& estimates, IPlacementSource const& placement, SharesCache sharesCache):
    _estimates { estimates },
    _sharesCache { sharesCache ? std::move(sharesCache) : SharesCache { std::equal_to<> {} } },
    _placement { placement }
{
}

Ticket AdmissionQueue::Enqueue(QueueRequest request)
{
    auto const ticket = _nextTicket++;
    auto const charged = _estimates.Estimate(request.estimate);
    _queue.push_back(Entry { .ticket = ticket,
                             .request = std::move(request),
                             .chargedBytes = charged,
                             .refusals = 0,
                             .order = static_cast<std::int64_t>(ticket) });
    return ticket;
}

void AdmissionQueue::ReportMachine(MachineId const& machine, MachineReport const& report)
{
    auto& ledger = _ledgers[machine];
    ledger.report = report;
    ledger.refused = false;
}

std::size_t AdmissionQueue::Depth() const noexcept
{
    return _queue.size();
}

std::optional<AdmissionQueue::Charge> AdmissionQueue::Free(Ticket ticket)
{
    auto const found = _charges.find(ticket);
    if (found == _charges.end())
        return std::nullopt;
    auto charge = std::move(found->second);
    _charges.erase(found);
    auto& ledger = _ledgers[charge.machine];
    ledger.chargedBytes = Saturating(ledger.chargedBytes, charge.bytes);
    ledger.chargedSlots = Saturating<std::uint32_t>(ledger.chargedSlots, 1);
    return charge;
}

std::vector<AdmissionQueue::Entry> AdmissionQueue::TakeInflight(Ticket owner)
{
    auto const found = _inflight.find(owner);
    if (found == _inflight.end())
        return {};
    auto waiters = std::move(found->second.waiters);
    auto const range = _inflightByKey.equal_range(found->second.cacheKey);
    _inflightByKey.erase(std::ranges::find(range.first, range.second, owner, &decltype(_inflightByKey)::value_type::second));
    _inflight.erase(found);
    return waiters;
}

void AdmissionQueue::Requeue(std::vector<Entry> entries)
{
    for (auto& entry: entries)
    {
        auto const place = std::ranges::find_if(_queue, [&](Entry const& queued) { return queued.order > entry.order; });
        _queue.insert(place, std::move(entry));
    }
}

void AdmissionQueue::Settle(Ticket ticket, Settlement const& settlement)
{
    auto charge = Free(ticket);
    if (!charge)
        return;
    auto waiters = TakeInflight(ticket);

    if (settlement.outcome == SettleOutcome::WorkerRefused)
    {
        auto& entry = charge->entry;
        ++entry.refusals;
        _ledgers[charge->machine].refused = true;
        entry.order = --_frontOrder; // below every key in the queue, the older refusals' included
        _queue.insert(_queue.begin(), std::move(entry));
        Requeue(std::move(waiters));
        return;
    }

    if (settlement.peakBytes)
        _estimates.Record(charge->entry.request.estimate, *settlement.peakBytes);

    if (settlement.outcome == SettleOutcome::Stored)
        for (auto& waiter: waiters)
            _stored.push_back(Decision { .ticket = waiter.ticket, .kind = GrantKind::Stored });
    else
        Requeue(std::move(waiters));
}

void AdmissionQueue::Abandon(Ticket ticket)
{
    if (auto const queued = std::ranges::find(_queue, ticket, &Entry::ticket); queued != _queue.end())
    {
        _queue.erase(queued);
        _headTickets.erase(ticket);
        return;
    }
    if (auto const woken = std::ranges::find(_stored, ticket, &Decision::ticket); woken != _stored.end())
    {
        _stored.erase(woken);
        return;
    }
    for (auto& [owner, inflight]: _inflight)
        if (std::erase_if(inflight.waiters, [&](Entry const& waiter) { return waiter.ticket == ticket; }) > 0)
            return;

    auto charge = Free(ticket);
    if (!charge)
        return;
    _estimates.RecordKilled(charge->entry.request.estimate);
    Requeue(TakeInflight(ticket));
}

std::vector<Ticket> AdmissionQueue::LoseLeadership()
{
    std::vector<Ticket> parked;
    for (auto const& entry: _queue)
        parked.push_back(entry.ticket);
    for (auto const& decision: _stored)
        parked.push_back(decision.ticket);
    for (auto const& [owner, inflight]: _inflight)
        for (auto const& waiter: inflight.waiters)
            parked.push_back(waiter.ticket);
    std::ranges::sort(parked);

    _queue.clear();
    _stored.clear();
    _inflight.clear();
    _inflightByKey.clear();
    _headTickets.clear();
    for (auto& [machine, ledger]: _ledgers)
        ledger.report.reset();
    return parked;
}

AdmissionQueue::Room AdmissionQueue::RoomOn(MachineId const& machine, Reserved const& hold) const
{
    auto const& ledger = _ledgers.at(machine);
    auto const& report = *ledger.report;
    Room room;
    room.usedBytes = std::max(ledger.chargedBytes, report.heldBytes);
    room.usedSlots = std::max(ledger.chargedSlots, report.heldSlots);
    room.freeBytes = Saturating(Saturating(report.memoryBudgetBytes, room.usedBytes), hold.bytes);
    // A machine running an over-budget compile has nothing left for anybody.
    room.freeSlots =
        room.usedBytes > report.memoryBudgetBytes ? 0U : Saturating(Saturating(report.slots, room.usedSlots), hold.slots);
    return room;
}

std::vector<Decision> AdmissionQueue::Pump()
{
    /// A machine an entry may run on; `placement` is engaged for a worker.
    struct Target
    {
        MachineId machine;
        std::optional<Placement> placement;
    };

    std::vector<Decision> decisions = std::exchange(_stored, {});
    std::vector<Entry> remaining;
    std::unordered_map<MachineId, Reserved> reserved;
    std::unordered_set<MachineId> claimed; // machines whose head was already found this pump
    std::unordered_set<Ticket> heads;

    auto const reported = [this](MachineId const& machine) {
        auto const found = _ledgers.find(machine);
        return found != _ledgers.end() && found->second.report.has_value() && !found->second.refused;
    };

    for (auto& entry: _queue)
    {
        auto const& request = entry.request;

        // Another compile of this key is running where this one's cache is: wait for its store.
        if (request.storable && !request.estimate.cacheKey.empty())
        {
            auto const [first, last] = _inflightByKey.equal_range(request.estimate.cacheKey);
            auto const owner = std::ranges::find_if(first, last, [&](auto const& item) {
                return _sharesCache(_inflight.at(item.second).machine, request.caller);
            });
            if (owner != last)
            {
                _inflight.at(owner->second).waiters.push_back(std::move(entry));
                continue;
            }
        }

        // Reported candidates in placement order, then the caller: the CPU never idles while the
        // caller has room.
        std::vector<Target> targets;
        if (request.kind == AdmitKind::Dispatchable && entry.refusals < MaxWorkerRefusals)
            for (auto& placement: _placement.Candidates(request.fingerprint, request.excluded))
                if (reported(placement.machine))
                    targets.push_back(Target { .machine = placement.machine, .placement = std::move(placement) });
        if (reported(request.caller))
            targets.push_back(Target { .machine = request.caller, .placement = std::nullopt });

        auto const granted = std::ranges::find_if(targets, [&](Target const& target) {
            auto const& report = *_ledgers.at(target.machine).report;
            auto const& hold = reserved[target.machine];
            auto const room = RoomOn(target.machine, hold);
            if (entry.chargedBytes > report.memoryBudgetBytes)
                return report.slots > 0 && room.usedBytes == 0 && room.usedSlots == 0 && hold.bytes == 0 && hold.slots == 0;
            return room.freeBytes >= entry.chargedBytes && room.freeSlots >= 1;
        });

        if (granted != targets.end())
        {
            auto& ledger = _ledgers.at(granted->machine);
            ledger.chargedBytes += entry.chargedBytes;
            ledger.chargedSlots += 1;
            auto const ticket = entry.ticket;
            auto const chargedBytes = entry.chargedBytes;
            if (request.storable && !request.estimate.cacheKey.empty())
            {
                _inflight.emplace(
                    ticket, Inflight { .cacheKey = request.estimate.cacheKey, .machine = request.caller, .waiters = {} });
                _inflightByKey.emplace(request.estimate.cacheKey, ticket);
            }
            _charges.emplace(ticket,
                             Charge { .machine = granted->machine, .bytes = chargedBytes, .entry = std::move(entry) });
            decisions.push_back(Decision {
                .ticket = ticket,
                .kind = granted->placement.has_value() ? GrantKind::Worker : GrantKind::Local,
                .placement = granted->placement,
                .chargedBytes = chargedBytes,
                .ranAlone = chargedBytes > ledger.report->memoryBudgetBytes,
            });
            continue;
        }

        // Blocked everywhere. On each machine of its target set where nothing older is blocked it
        // is the head; having been head across a pump already, it holds back everything free there.
        auto const wasHead = _headTickets.contains(entry.ticket);
        for (auto const& target: targets)
        {
            if (!claimed.insert(target.machine).second)
                continue;
            heads.insert(entry.ticket);
            if (!wasHead)
                continue;
            auto& hold = reserved[target.machine];
            auto const room = RoomOn(target.machine, hold);
            auto const budget = _ledgers.at(target.machine).report->memoryBudgetBytes;
            auto const alone = entry.chargedBytes > budget;
            hold.bytes += std::min(std::min(entry.chargedBytes, budget), room.freeBytes);
            hold.slots += alone ? room.freeSlots : std::min<std::uint32_t>(1, room.freeSlots);
        }
        remaining.push_back(std::move(entry));
    }

    _queue = std::move(remaining);
    _headTickets = std::move(heads);
    return decisions;
}

} // namespace Pace::Distributed
