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
    std::vector<Decision> decisions;
    std::vector<Entry> remaining;
    std::unordered_map<MachineId, Reserved> reserved;
    std::unordered_map<MachineId, Ticket> heads;

    auto const reported = [this](MachineId const& machine) {
        auto const found = _ledgers.find(machine);
        return found != _ledgers.end() && found->second.report.has_value();
    };

    for (auto& entry: _queue)
    {
        auto const& request = entry.request;

        // The machines this entry could run on, in preference order. With nobody to dispatch to
        // (or a request that may only run where it was asked) it runs on the caller.
        std::vector<Placement> candidates;
        if (request.kind == AdmitKind::Dispatchable)
            candidates = _placement.Candidates(request.fingerprint, request.excluded);
        auto const onCaller = candidates.empty();
        if (onCaller)
            candidates.push_back(Placement { .machine = request.caller });

        std::optional<std::size_t> granted;
        std::optional<std::size_t> blockedOn;
        for (auto const index: std::views::iota(std::size_t { 0 }, candidates.size()))
        {
            auto const& machine = candidates[index].machine;
            if (!reported(machine))
                continue;
            auto const& report = *_ledgers.at(machine).report;
            auto const& hold = reserved[machine];
            auto const room = RoomOn(machine, hold);

            auto const alone = entry.chargedBytes > report.memoryBudgetBytes;
            auto const fits =
                alone
                    ? (report.slots > 0 && room.usedBytes == 0 && room.usedSlots == 0 && hold.bytes == 0 && hold.slots == 0)
                    : (room.freeBytes >= entry.chargedBytes && room.freeSlots >= 1);
            if (fits)
            {
                granted = index;
                break;
            }
            if (!blockedOn.has_value())
                blockedOn = index;
        }

        if (granted.has_value())
        {
            auto const& placement = candidates[*granted];
            auto& ledger = _ledgers.at(placement.machine);
            ledger.chargedBytes += entry.chargedBytes;
            ledger.chargedSlots += 1;
            _charges.emplace(entry.ticket, Charge { .machine = placement.machine, .bytes = entry.chargedBytes });
            auto const worker = request.kind == AdmitKind::Dispatchable && !onCaller;
            decisions.push_back(Decision {
                .ticket = entry.ticket,
                .kind = worker ? GrantKind::Worker : GrantKind::Local,
                .placement = worker ? std::optional<Placement> { placement } : std::nullopt,
                .chargedBytes = entry.chargedBytes,
                .ranAlone = entry.chargedBytes > ledger.report->memoryBudgetBytes,
            });
            continue;
        }

        if (blockedOn.has_value())
        {
            auto const& machine = candidates[*blockedOn].machine;
            // The oldest blocked entry per machine is its head. Having already waited as head across
            // a pump, it holds back everything free, up to what it needs.
            if (heads.emplace(machine, entry.ticket).second)
            {
                auto const previous = _waitingHead.find(machine);
                if (previous != _waitingHead.end() && previous->second == entry.ticket)
                {
                    auto& hold = reserved[machine];
                    auto const room = RoomOn(machine, hold);
                    auto const need = std::min(entry.chargedBytes, _ledgers.at(machine).report->memoryBudgetBytes);
                    hold.bytes += std::min(need, room.freeBytes);
                    hold.slots += std::min<std::uint32_t>(1, room.freeSlots);
                }
            }
        }
        remaining.push_back(std::move(entry));
    }

    _queue = std::move(remaining);
    _waitingHead = std::move(heads);
    return decisions;
}

} // namespace Pace::Distributed
