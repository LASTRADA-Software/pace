// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Distributed/PeakEstimates.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pace::Distributed
{

/// The machine a charge lands on: the node's identity public key, as the registry stores it.
using MachineId = std::string;
/// Names one queued request until it is settled.
using Ticket = std::uint64_t;

using CompileCacheWire::AdmitKind;
using CompileCacheWire::GrantKind;
using CompileCacheWire::SettleOutcome;

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

/// How a granted compile ended.
struct Settlement
{
    SettleOutcome outcome {};
    std::optional<std::uint64_t> peakBytes; ///< The compile's measured peak, when known.
};

/// One client's load on the queue: a machine and the build tree it compiles for.
struct ClientLoad
{
    MachineId machine;        ///< The machine the compiles came from.
    std::string tree;         ///< The build tree's label, as the client stated it; possibly empty.
    std::uint32_t running {}; ///< Granted compiles not yet settled.
    std::uint32_t queued {};  ///< Requests still waiting.
};

/// One machine's ledger, as a report reads it.
struct MachineLedgerView
{
    MachineId machine;                        ///< The machine.
    std::uint64_t chargedBytes {};            ///< What this ledger has charged it.
    std::optional<std::uint64_t> budgetBytes; ///< Its own stated budget; absent until it reports one.
};

/// Everything a fleet report shows of the queue, read at one moment.
struct QueueView
{
    std::size_t depth {};                    ///< Requests waiting (`Depth()`).
    std::vector<ClientLoad> clients;         ///< One per (machine, tree) with anything running or queued.
    std::vector<MachineLedgerView> machines; ///< One per machine that reported a budget or holds a charge.
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
/// A storable request whose `estimate.cacheKey` is already in flight from a machine that
/// shares its cache waits on that compile instead of running: it is never charged and holds no
/// reservation. When the owner stores, the waiter is answered `GrantKind::Stored` on the next
/// `Pump()`; when it does not, the waiter rejoins the queue at its original FIFO position.
///
/// Not thread-safe: the owner locks.
class AdmissionQueue
{
  public:
    /// A worker refusing the same request this many times sends it to its caller.
    static constexpr std::uint32_t MaxWorkerRefusals = 3;

    /// Whether two machines read and write one compile cache.
    using SharesCache = std::function<bool(MachineId const&, MachineId const&)>;

    /// @param estimates Read at enqueue time; the figure is stored on the entry.
    /// @param placement Who can run a toolchain.
    /// @param sharesCache Whether a compile on one machine stores what the other would look up;
    ///                    empty means only the same machine does.
    AdmissionQueue(PeakEstimates& estimates, IPlacementSource const& placement, SharesCache sharesCache = {});

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

    /// A granted ticket finished: frees its charge, records its peak, and answers or releases
    /// whatever waited on it. An unknown ticket is ignored.
    /// @param ticket A ticket `Pump()` granted.
    /// @param settlement How it ended.
    void Settle(Ticket ticket, Settlement const& settlement);

    /// A grant `Pump()` made could not be handed over -- the worker it placed on left between the
    /// placement and the lease. Frees its charge and puts it back at the FRONT, ahead of every queued
    /// request, WITHOUT counting a refusal: nobody refused it, and a vanished worker must not spend the
    /// request's refusal bound or mark its machine refused. Whatever waited on it rejoins the queue at
    /// its own place. A ticket holding no charge is ignored.
    /// @param ticket A ticket the last `Pump()` granted.
    void ReturnToFront(Ticket ticket);

    /// The ticket's connection closed, at any stage. A granted ticket frees its charge and teaches
    /// the estimator it was killed; a queued or waiting one simply leaves. An unknown ticket is ignored.
    /// @param ticket Any ticket.
    void Abandon(Ticket ticket);

    /// This node is no longer the leader.
    /// @return Every queued and dedup-waiting ticket, now forgotten. Granted tickets stay tracked
    ///         so their `Settle`/`Abandon` still releases; every machine becomes unreported.
    [[nodiscard]] std::vector<Ticket> LoseLeadership();

    /// @return How many requests are waiting.
    [[nodiscard]] std::size_t Depth() const noexcept;

    /// @return What this ledger has charged each machine, in bytes, machines in no particular order.
    ///         A machine with nothing charged is omitted.
    [[nodiscard]] std::vector<std::pair<MachineId, std::uint64_t>> ChargedByMachine() const;

    /// @return The queue as a report reads it: depth, who is compiling or waiting, and each machine's
    ///         charge against its budget. Order is unspecified; the caller names and sorts.
    [[nodiscard]] QueueView Snapshot() const;

  private:
    struct Entry
    {
        Ticket ticket {};
        QueueRequest request;
        std::uint64_t chargedBytes {}; ///< The estimate taken at enqueue.
        std::uint32_t refusals {};     ///< Worker refusals so far.
        std::int64_t order {};         ///< Queue position key: the ticket, or below every key for a refused one.
    };

    /// What this ledger has charged a machine, beside its own report.
    struct Ledger
    {
        std::optional<MachineReport> report;
        std::uint64_t chargedBytes {};
        std::uint32_t chargedSlots {};
        bool refused {}; ///< A worker here declined a job; admits nothing until the next report.
    };

    /// A granted entry and what it charged, to give back on `Settle`/`Abandon`.
    struct Charge
    {
        MachineId machine;
        std::uint64_t bytes {};
        Entry entry;
    };

    /// A granted storable compile and the requests waiting for its store.
    struct Inflight
    {
        std::string cacheKey;
        MachineId machine; ///< The owner's caller.
        std::vector<Entry> waiters;
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

    /// Frees a granted ticket's charge.
    /// @return What it held, or nothing when the ticket holds no charge.
    [[nodiscard]] std::optional<Charge> Free(Ticket ticket);

    /// Puts entries back in the queue at their original FIFO place.
    void Requeue(std::vector<Entry> entries);

    /// Forgets an in-flight owner.
    /// @return The requests that were waiting on it.
    [[nodiscard]] std::vector<Entry> TakeInflight(Ticket owner);

    PeakEstimates& _estimates;
    SharesCache _sharesCache;
    IPlacementSource const& _placement;
    Ticket _nextTicket { 1 };
    std::vector<Entry> _queue;
    std::unordered_map<MachineId, Ledger> _ledgers;
    std::unordered_map<Ticket, Charge> _charges;
    std::unordered_map<Ticket, Inflight> _inflight;              ///< Keyed by the owner's ticket.
    std::unordered_multimap<std::string, Ticket> _inflightByKey; ///< cacheKey -> the owners running it.
    std::int64_t _frontOrder {};             ///< Last key handed to a refused ticket; counts down from zero.
    std::vector<Decision> _stored;           ///< Waiters woken by a store, answered by the next pump.
    std::unordered_set<Ticket> _headTickets; ///< Entries that were a blocked head in the last pump.
};

} // namespace Pace::Distributed
