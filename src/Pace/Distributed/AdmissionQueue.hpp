// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Distributed/PeakEstimates.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pace::Distributed
{

/// The machine a charge lands on: the node's identity public key, as the registry stores it.
using MachineId = std::string;
/// Names one queued request until it is settled.
using Ticket = std::uint64_t;

using CompileCacheWire::AdmitKind;
using CompileCacheWire::GrantKind;

/// One compile asking to be admitted.
struct QueueRequest
{
    AdmitKind kind {};
    MachineId caller;        ///< Machine the request came from (local leases charge it).
    std::string fingerprint; ///< Toolchain; empty for LocalOnly with an unknown toolchain.
    EstimateKey estimate;
    bool storable { true }; ///< Joins the dedup in-flight set only when true.
    std::string treeLabel;  ///< Display only.
    std::vector<std::string> excluded;
};

/// One candidate worker for a dispatchable request.
struct Placement
{
    std::string workerId;
    MachineId machine;
    std::string endpoint;
};

/// Injected: who can run a toolchain.
class IPlacementSource
{
  public:
    virtual ~IPlacementSource() = default;

    /// @param fingerprint The toolchain to run.
    /// @param excluded Worker ids that must not be returned.
    /// @return The workers that can run `fingerprint` and have a free worker slot, best first.
    [[nodiscard]] virtual std::vector<Placement> Candidates(std::string_view fingerprint,
                                                            std::span<std::string const> excluded) const = 0;
};

/// A machine's own statement of its budget and what it currently holds.
struct MachineReport
{
    std::uint64_t memoryBudgetBytes {};
    std::uint32_t slots {};
    std::uint32_t heldSlots {}; ///< Every compile running on the machine (local leases + worker jobs).
    std::uint64_t heldBytes {}; ///< Their charged bytes. The ledger uses max(charged, held), never the sum.
};

/// An admission: what to run, and where.
struct Decision
{
    Ticket ticket {};
    GrantKind kind {};
    std::optional<Placement> placement; ///< Engaged for `Worker`.
    std::uint64_t chargedBytes {};
    bool ranAlone {}; ///< Its estimate exceeded the whole budget.
};

/// The leader's FIFO of compiles waiting for memory and slots, with a per-machine ledger.
///
/// A request is admitted only onto a machine that has reported its budget. In-use memory and
/// slots on a machine are `max(charged by this ledger, held by the machine's own report)` per
/// resource, so a report that lags the ledger does not count one compile twice and, after a
/// failover (ledger empty), the report alone counts.
///
/// Admission is FIFO with backfill: a later request may be admitted past a blocked earlier one
/// only from memory and slots that are neither in use nor reserved. A dispatchable request
/// targets its reported candidates in placement order, then its caller; a local-only one its
/// caller. The oldest blocked request on a machine is that machine's head; once a request has
/// been head across a `Pump()` it reserves whatever is free (up to what it needs) on EVERY
/// reported machine of its target set, so a stream of small requests cannot starve it however
/// the candidate order changes. A request estimated above the whole budget runs alone: it
/// waits for an idle machine and reserves all free memory and slots meanwhile.
///
/// Not thread-safe: the owner locks.
class AdmissionQueue
{
  public:
    /// @param estimates Read at enqueue time; the figure is stored on the entry.
    /// @param placement Who can run a toolchain.
    AdmissionQueue(PeakEstimates& estimates, IPlacementSource const& placement) noexcept;

    /// @param request The compile to queue.
    /// @return Its ticket.
    [[nodiscard]] Ticket Enqueue(QueueRequest request);

    /// Admits whatever fits, in FIFO order with backfill.
    /// @return The admissions made, in grant order.
    [[nodiscard]] std::vector<Decision> Pump();

    /// Records a machine's own statement; replaces the previous one.
    /// @param machine The reporting machine.
    /// @param report Its budget and what it holds.
    void ReportMachine(MachineId const& machine, MachineReport const& report);

    /// Frees a granted ticket's charge.
    /// @param ticket A ticket `Pump()` granted.
    /// @return False when the ticket holds no charge.
    [[nodiscard]] bool Release(Ticket ticket);

    /// @return How many requests are waiting.
    [[nodiscard]] std::size_t Depth() const noexcept;

  private:
    struct Entry
    {
        Ticket ticket {};
        QueueRequest request;
        std::uint64_t chargedBytes {}; ///< The estimate taken at enqueue.
    };

    /// What this ledger has charged a machine, beside its own report.
    struct Ledger
    {
        std::optional<MachineReport> report;
        std::uint64_t chargedBytes {};
        std::uint32_t chargedSlots {};
    };

    /// A charge to give back on `Release`.
    struct Charge
    {
        MachineId machine;
        std::uint64_t bytes {};
    };

    /// Memory and slots held back from later requests on one machine during one pump.
    struct Reserved
    {
        std::uint64_t bytes {};
        std::uint32_t slots {};
    };

    /// What is free on a machine right now.
    struct Room
    {
        std::uint64_t usedBytes {};
        std::uint32_t usedSlots {};
        std::uint64_t freeBytes {};
        std::uint32_t freeSlots {};
    };

    /// @param machine A machine that has reported.
    /// @param hold What earlier entries of this pump reserved there.
    /// @return Its in-use and free figures.
    [[nodiscard]] Room RoomOn(MachineId const& machine, Reserved const& hold) const;

    PeakEstimates& _estimates;
    IPlacementSource const& _placement;
    Ticket _nextTicket { 1 };
    std::vector<Entry> _queue;
    std::unordered_map<MachineId, Ledger> _ledgers;
    std::unordered_map<Ticket, Charge> _charges;
    std::unordered_set<Ticket> _headTickets; ///< Entries that were a blocked head in the last pump.
};

} // namespace Pace::Distributed
