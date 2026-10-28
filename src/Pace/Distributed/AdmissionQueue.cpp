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

AdmissionQueue::AdmissionQueue(PeakEstimates& estimates, IPlacementSource const& placement) noexcept:
    _estimates { estimates },
    _placement { placement }
{
}

Ticket AdmissionQueue::Enqueue(QueueRequest request)
{
    auto const ticket = _nextTicket++;
    auto const charged = _estimates.Estimate(request.estimate);
    _queue.push_back(Entry { .ticket = ticket, .request = std::move(request), .chargedBytes = charged });
    return ticket;
}

void AdmissionQueue::ReportMachine(MachineId const& machine, MachineReport const& report)
{
    _ledgers[machine].report = report;
}

std::size_t AdmissionQueue::Depth() const noexcept
{
    return _queue.size();
}

bool AdmissionQueue::Release(Ticket ticket)
{
    auto const found = _charges.find(ticket);
    if (found == _charges.end())
        return false;
    auto& ledger = _ledgers[found->second.machine];
    ledger.chargedBytes = Saturating(ledger.chargedBytes, found->second.bytes);
    ledger.chargedSlots = Saturating<std::uint32_t>(ledger.chargedSlots, 1);
    _charges.erase(found);
    return true;
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

    std::vector<Decision> decisions;
    std::vector<Entry> remaining;
    std::unordered_map<MachineId, Reserved> reserved;
    std::unordered_set<MachineId> claimed; // machines whose head was already found this pump
    std::unordered_set<Ticket> heads;

    auto const reported = [this](MachineId const& machine) {
        auto const found = _ledgers.find(machine);
        return found != _ledgers.end() && found->second.report.has_value();
    };

    for (auto& entry: _queue)
    {
        auto const& request = entry.request;

        // Reported candidates in placement order, then the caller: the CPU never idles while the
        // caller has room.
        std::vector<Target> targets;
        if (request.kind == AdmitKind::Dispatchable)
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
            _charges.emplace(entry.ticket, Charge { .machine = granted->machine, .bytes = entry.chargedBytes });
            decisions.push_back(Decision {
                .ticket = entry.ticket,
                .kind = granted->placement.has_value() ? GrantKind::Worker : GrantKind::Local,
                .placement = granted->placement,
                .chargedBytes = entry.chargedBytes,
                .ranAlone = entry.chargedBytes > ledger.report->memoryBudgetBytes,
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
