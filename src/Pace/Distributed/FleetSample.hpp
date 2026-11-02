// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/EnumTable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Pace::Distributed
{

/// One number a sample records.
///
/// Slots rather than members, so a new one is a row and every walk over
/// them -- sampling, encoding, decoding, rendering -- picks it up without being
/// edited. The dispatch counters are `LeaseOutcomeTable`'s, read through the same
/// `IMetricsSink::Counter` values so the page and the history cannot drift into two
/// vocabularies for one fact.
/// **ORDINALS ARE A PERSISTED CONTRACT. Append only; never insert or reorder.** (#308)
///
/// A history file's bucket is `[start][sample][present][coverage][N values][N folds]` through
/// version 3 and `[start][sample][present][coverage][absent][N values][N folds]` from version 4,
/// where `N` is the width the format VERSION that wrote it used -- see `FleetHistory.cpp`'s
/// `LegacyValueWidth` (nine, versions 1 and 2) and `VersionThreeValueWidth` (ten, version 3),
/// each frozen forever, and this build's count from version 4 onward. The readings are stored POSITIONALLY, indexed by
/// `static_cast<std::size_t>(metric)`, and read back the same way. Inserting an
/// enumerator mid-enum therefore does not fail to load: every bucket already on disk
/// comes back with each reading attributed to the NEXT series, silently, for as long
/// as the file exists. A cache-hit count rendered as a gauge of jobs in flight, with
/// nothing anywhere reporting a fault.
///
/// The same ordinal is also `CompileCacheWire::HistorySlotCount`'s width, positionally,
/// over the wire -- see the `static_assert` tying the two in `SchedulerProtocol.hpp`.
/// That record is variable-arity (a peer carrying fewer slots is tolerated, extra ones
/// from a newer peer are ignored), so growing this needs no `CompileCacheWire` version
/// move; the two on-disk history formats do, because EACH of their layouts is
/// fixed-width per version -- `FleetHistory.cpp`'s `RingFile` for a node's own series
/// and `NodeStoreFile` for what it received from others, independently, since a
/// nested body inside the latter carries no envelope of its own to move on its own
/// schedule.
///
/// The explicit `= N` is the enforcement rather than decoration. Without it a mid-enum
/// insertion is one added line with no other visible change; with it, the same edit
/// either collides or shows up in review as a renumbered literal on every row below it.
/// `Last` carries one too, because `readability-enum-initial-value` accepts all, none or
/// only-the-first and this tree does not silence clang-tidy. It also earns it: forgetting
/// to bump it while adding a slot makes the new enumerator COLLIDE with `Last`, which
/// shortens `EnumeratorCount` and fails `RowsInEnumeratorOrder`'s static_assert against
/// `FleetMetricTable`. One line of friction, caught at compile time rather than in a
/// history file nobody re-reads.
enum class FleetMetric : std::uint8_t
{
    DispatchGranted = 0,    ///< Cumulative: compiles handed to a worker.
    DispatchNoWorker = 1,   ///< Cumulative: nothing registered for that toolchain.
    DispatchNoCapacity = 2, ///< Cumulative: every matching worker full of our own work.
    DispatchWithdrawn = 3,  ///< Cumulative: a ceiling withdrew the slots.
    DispatchDuplicate = 4,  ///< Cumulative: already being built somewhere.
    CacheHits = 5,          ///< Cumulative, summed over machines.
    CacheMisses = 6,        ///< Cumulative, summed over machines.
    OfferableSlots = 7,     ///< Gauge: slots a compile could start on.
    JobsInFlight = 8,       ///< Gauge: this fleet's compiles running.
    /// Cumulative: every matching worker was on the asking client's exclusion list.
    ///
    /// Appended rather than placed beside its `LeaseOutcomeTable` neighbours, per the
    /// append-only contract above -- a history file older than this build has no
    /// column for it, and reading one back at zero is the correct answer for that
    /// period: no exclusion list existed yet for it to count.
    DispatchAllExcluded = 9,
    /// Gauge: host-wide CPU busy over the interval since the previous sample, in parts per thousand
    /// (`HostLoad::cpuBusyPermille`). Includes this node's own compiles: no platform separates them.
    ///
    /// The first slot that can be ABSENT within a present bucket -- the first sample after a start has
    /// no earlier counters to difference against, and a platform may not answer at all -- which is why
    /// `FleetBucket::absent` exists. Absent is never written as zero: an idle machine and one that could
    /// not say are opposite facts.
    CpuBusyPermille = 10,
    Last = 11
};

static_assert(static_cast<std::uint8_t>(FleetMetric::DispatchAllExcluded) == 9
                  && static_cast<std::uint8_t>(FleetMetric::CpuBusyPermille) == 10,
              "FleetMetric's ordinals are a persisted, wire-carried contract: append only, never renumber");

/// Which of a bucket's slots carry no reading, one bit per `FleetMetric` ordinal.
///
/// A mask rather than a second `EnumTable` of flags because it is persisted and carried over the wire
/// beside the readings, and one word is the whole of it. **Persisted and transmitted**: bit `n` is
/// ordinal `n`, so the bit positions are the ordinals' contract, append-only with them.
using FleetSlotMask = std::uint64_t;

static_assert(static_cast<std::size_t>(FleetMetric::Last) <= sizeof(FleetSlotMask) * 8,
              "one bit per slot: a 65th slot needs a wider absence mask, and a format version to carry it");

/// @param metric A slot.
/// @return Its bit in a `FleetSlotMask`.
[[nodiscard]] constexpr FleetSlotMask SlotBit(FleetMetric metric) noexcept
{
    return FleetSlotMask { 1 } << static_cast<std::uint8_t>(metric);
}

/// Every slot this build has, as a mask: what a mask read off a file or a wire is clipped to, so a bit no slot
/// owns can never make a later slot read absent.
inline constexpr FleetSlotMask EveryFleetSlot = SlotBit(FleetMetric::Last) - 1;

/// Whether a slot accumulates or is read afresh each time.
///
/// The distinction is load-bearing at render: a counter's *rate* is the
/// difference between two buckets, while a gauge's value is the bucket itself.
/// Getting it wrong draws a monotonic ramp for everything.
enum class FleetMetricKind : std::uint8_t
{
    Counter = 0, ///< Monotonic; the interesting quantity is the delta.
    Gauge,       ///< A level; the interesting quantity is the value.
};

/// Whether a MACHINE can answer for a slot about itself.
///
/// Not "which series carries it" -- the fleet series carries every slot, because a
/// leader can answer for every one of them, its readings fleet-wide sums (save a slot
/// with no fleet total, `FleetTotal::None`, which every fleet view carries absent). This
/// is the narrower question the node series asks: is there a version of this number that is
/// true of one machine, whoever happens to be leading?
///
/// For a cache figure or a slot count there is, and it is what that machine did. For
/// a dispatch outcome there is not: only a scheduler produces one, a follower has
/// none, and a follower reporting zero refusals is indistinguishable from a leader
/// that genuinely refused none.
enum class FleetMetricScope : std::uint8_t
{
    /// A machine can answer this about itself: its cache, its slots, its compiles.
    Node = 0,
    /// Only a scheduler can, and only a leader has one that means anything.
    Fleet,
    Last
};

/// Whether a fleet-wide figure exists for a slot.
///
/// The question `FleetMetricScope` does not ask. A node-scoped slot is true of one machine, and for a
/// cache count or a slot count the sum over machines is also a true figure about the fleet, which is what
/// the leader's series and a backfilled window carry. For a load ratio it is not: three machines at 400
/// permille are not a fleet at 1200, and an average would weight a laptop like a build server. Such a slot
/// is ABSENT from every fleet view and answered only per machine.
enum class FleetTotal : std::uint8_t
{
    Summed = 0, ///< The fleet figure is the sum over machines (or the scheduler's own count).
    None,       ///< No fleet figure means anything; asked per machine only.
};

/// What a record that never carried a slot says about it.
///
/// A slot appended after a file format or a peer was built is missing from what that format or peer wrote,
/// and the right reading of the hole is a property of the SLOT. `DispatchAllExcluded` reads zero there --
/// no exclusion list existed yet for it to count; `CpuBusyPermille` reads absent -- the machine was busy or
/// idle all along, and nothing recorded which.
enum class FleetUncarried : std::uint8_t
{
    Zero = 0, ///< Nothing happened that this slot counts: zero is the truth.
    Absent,   ///< Something happened and nobody recorded it: a gap.
};

/// What one slot is, for the walks that need to know.
struct FleetMetricRow
{
    FleetMetric metric;     ///< The slot this row describes.
    FleetMetricKind kind;   ///< Delta or level.
    FleetMetricScope scope; ///< Who can answer for it.
    std::string_view key;   ///< Stable name, for JSON and for the on-disk header.
    /// Whether a fleet-wide figure exists; `Summed` for every slot that predates the CPU one.
    FleetTotal fleetTotal { FleetTotal::Summed };
    /// What a record that never carried this slot means by its absence.
    FleetUncarried whenUncarried { FleetUncarried::Zero };
};

/// Every slot, in enumerator order.
inline constexpr EnumTable<FleetMetric, FleetMetricRow> FleetMetricTable {
    FleetMetricRow { .metric = FleetMetric::DispatchGranted,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "granted" },
    FleetMetricRow { .metric = FleetMetric::DispatchNoWorker,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "no-worker" },
    FleetMetricRow { .metric = FleetMetric::DispatchNoCapacity,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "no-capacity" },
    FleetMetricRow { .metric = FleetMetric::DispatchWithdrawn,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "withdrawn" },
    FleetMetricRow { .metric = FleetMetric::DispatchDuplicate,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "duplicate" },
    FleetMetricRow { .metric = FleetMetric::CacheHits,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Node,
                     .key = "cache-hits" },
    FleetMetricRow { .metric = FleetMetric::CacheMisses,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Node,
                     .key = "cache-misses" },
    FleetMetricRow { .metric = FleetMetric::OfferableSlots,
                     .kind = FleetMetricKind::Gauge,
                     .scope = FleetMetricScope::Node,
                     .key = "offerable" },
    FleetMetricRow { .metric = FleetMetric::JobsInFlight,
                     .kind = FleetMetricKind::Gauge,
                     .scope = FleetMetricScope::Node,
                     .key = "in-flight" },
    FleetMetricRow { .metric = FleetMetric::DispatchAllExcluded,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "all-excluded" },
    FleetMetricRow { .metric = FleetMetric::CpuBusyPermille,
                     .kind = FleetMetricKind::Gauge,
                     .scope = FleetMetricScope::Node,
                     .key = "cpu-busy",
                     .fleetTotal = FleetTotal::None,
                     .whenUncarried = FleetUncarried::Absent },
};
static_assert(RowsInEnumeratorOrder(FleetMetricTable, &FleetMetricRow::metric));

/// Both scopes are populated, or the split this table exists to express does nothing.
///
/// A table where every row said `Fleet` would make a node record nothing about itself
/// and read exactly like the leadership guard this replaced; one where every row said
/// `Node` would have a follower reporting dispatch counters it never produced. Either
/// is silent, and both survive every other check here.
static_assert(std::ranges::any_of(FleetMetricTable,
                                  [](FleetMetricRow const& row) { return row.scope == FleetMetricScope::Node; })
                  && std::ranges::any_of(FleetMetricTable,
                                         [](FleetMetricRow const& row) { return row.scope == FleetMetricScope::Fleet; }),
              "a scope no row carries is a split that quietly does nothing");

/// A slot with no fleet figure is one a MACHINE answers for: a fleet-scoped slot is the scheduler's own count,
/// so it always has one, and a slot nobody could answer for at all would be a slot nothing renders.
static_assert(std::ranges::all_of(FleetMetricTable,
                                  [](FleetMetricRow const& row) {
                                      return row.fleetTotal == FleetTotal::Summed || row.scope == FleetMetricScope::Node;
                                  }),
              "a slot without a fleet total must be node-scoped, or no view answers for it");

/// The slots no fleet view carries, as a mask: what the leader's series and a backfilled window record absent.
/// @return Every slot whose `fleetTotal` is `None`.
[[nodiscard]] constexpr FleetSlotMask SlotsWithoutFleetTotal() noexcept
{
    FleetSlotMask mask = 0;
    for (auto const& row: FleetMetricTable)
        if (row.fleetTotal == FleetTotal::None)
            mask |= SlotBit(row.metric);
    return mask;
}

/// The slots only a scheduler answers for, as a mask: what one machine's own view records absent.
/// @return Every slot whose `scope` is `Fleet`.
[[nodiscard]] constexpr FleetSlotMask FleetScopedSlots() noexcept
{
    FleetSlotMask mask = 0;
    for (auto const& row: FleetMetricTable)
        if (row.scope == FleetMetricScope::Fleet)
            mask |= SlotBit(row.metric);
    return mask;
}

/// What a record carrying only its first @p carried slots says about the rest.
/// @param carried How many slots, from ordinal zero, the record held.
/// @return Every slot at or past @p carried whose `whenUncarried` is `Absent`.
[[nodiscard]] constexpr FleetSlotMask UncarriedAbsentSlots(std::size_t carried) noexcept
{
    FleetSlotMask mask = 0;
    for (auto const& row: FleetMetricTable)
        if (static_cast<std::size_t>(row.metric) >= carried && row.whenUncarried == FleetUncarried::Absent)
            mask |= SlotBit(row.metric);
    return mask;
}

/// What folding several samples into one bucket kept, per slot.
///
/// A bucket wider than the sample interval is a fold, and a fold that kept only its
/// last reading has thrown away the part an operator reads: **a refusal spike
/// averaged over a day is invisible**, and a gauge's floor -- the moment the fleet
/// had nothing left -- is the end that matters. Neither is recoverable afterwards,
/// so both are computed while folding.
///
/// What each field means depends on the slot's `FleetMetricKind`, and that is the
/// same split `values` already lives with: a counter's interesting quantity is its
/// delta and a gauge's is its level.
struct FleetFold
{
    /// Gauge: the smallest reading in the bucket. Unused by a counter, whose floor
    /// is a rate rather than a level and is not a fact anybody acts on.
    std::uint64_t low { 0 };

    /// Gauge: the largest reading. Counter: the largest per-SAMPLE delta folded in
    /// -- the peak rate inside the window, which is exactly what an average over a
    /// day destroys.
    std::uint64_t high { 0 };

    /// Gauge: the readings summed, so a mean survives alongside `coverage`. Unused
    /// by a counter, whose mean over a window is the delta across it and needs no
    /// second number.
    std::uint64_t total { 0 };
};

/// One bucket of the history.
///
/// `present` rather than a sentinel value, because zero is a legitimate reading
/// for every slot here. A bucket nobody sampled is a **gap** -- a fleet that did
/// nothing and a fleet nobody was watching are different facts, and the page draws
/// them differently.
struct FleetBucket
{
    /// Wall-clock start of the bucket, in milliseconds since the epoch.
    ///
    /// Wall clock and not `core::platform::SteadyTimePoint`: this outlives the process, and a steady
    /// clock's origin is meaningless across a restart.
    std::int64_t startMillis { 0 };

    /// Wall-clock instant the reading in `values` was actually taken.
    ///
    /// Separate from `startMillis` because the two answer different questions and
    /// only one of them can be the bucket's x-position. `Buckets()` folds a window
    /// down to its newest sample and then stamps the WINDOW's start on it, so the
    /// chart's points are evenly spaced -- which is right for plotting and wrong
    /// for dividing by. The newest bucket is always still open, so a rate that
    /// divided the delta by the nominal width reported a fraction of the truth on
    /// exactly the point an operator reads first.
    ///
    /// Persisted, and it has to be. That was once derived from the stored bucket's
    /// own `startMillis`, on the argument that a ring's bucket IS its sample instant
    /// at sub-bucket granularity -- true when the coarsest ring was the hour a Week
    /// view read one-for-one, and false the moment a ring folds sixty samples into a
    /// bucket. Restoring a partly-sampled day bucket with its window's start would
    /// divide its delta by a nominal twenty-four hours, which is precisely the error
    /// this field exists to prevent.
    std::int64_t sampleMillis { 0 };
    EnumTable<FleetMetric, std::uint64_t> values {}; ///< Raw readings, counters cumulative.

    /// What was lost between the first reading in this bucket and `values`.
    ///
    /// Meaningful only where `coverage` exceeds one; in the base ring a bucket is
    /// one sample and every field here equals the reading itself.
    EnumTable<FleetMetric, FleetFold> fold {};

    /// How many samples folded into this bucket.
    ///
    /// The third state. `present` distinguishes *nobody sampled* from *somebody
    /// did*; this distinguishes a day a node contributed three hours to from one it
    /// contributed twenty-four to, which are both `present` and must not be
    /// compared like for like. A node absent for most of a year is the ORDINARY
    /// case for a workstation on a VPN, not an edge one.
    std::uint64_t coverage { 0 };

    /// The slots that carry no reading although the bucket is present (`SlotBit`).
    ///
    /// The per-slot third state beside `present`: a bucket somebody sampled can still hold a slot nobody
    /// could read -- the CPU on the first sample after a start, or on a platform that will not say -- and
    /// zero is a reading for every slot. For a folded bucket a slot is absent only when EVERY sample in it
    /// lacked a reading; the others make up its value and fold. Persisted and handed over, because nothing
    /// can recompute it.
    FleetSlotMask absent { 0 };

    bool present { false }; ///< False means nobody sampled this bucket.

    /// Summed from what the nodes reported rather than sampled by this leader.
    ///
    /// Computed at RENDER and never stored -- which is why it costs the file format
    /// nothing. It marks a window this leader was not scheduling for, so the
    /// FLEET-scoped slots in it are unknowable rather than zero: a backfilled bucket
    /// claiming `granted = 0` beside a real one at 5000 would draw a rate spike off a
    /// number nobody measured, which is the same defect a gap folded in as a peak
    /// already was.
    bool backfilled { false };
};

/// @param bucket A window.
/// @param metric A slot.
/// @return Whether the slot carries no reading in it.
[[nodiscard]] constexpr bool IsAbsent(FleetBucket const& bucket, FleetMetric metric) noexcept
{
    return (bucket.absent & SlotBit(metric)) != 0;
}

/// Where a node's handed-over history goes.
///
/// An interface so `SchedulerService` can route what arrives without holding a
/// history itself: it stays pure with respect to I/O and knows nothing about files,
/// rings or ranges, exactly as it knows nothing about sockets.
///
/// Declared HERE rather than beside `FleetHistory`, and that is the whole point of
/// this header: the scheduler decides a fleet's capacity, and a header of its own
/// that dragged in a ring buffer, a `std::map` and `<filesystem>` would make those
/// decisions compile against a file format.
class IFleetHistorySink
{
  public:
    IFleetHistorySink() = default;
    virtual ~IFleetHistorySink() = default;
    IFleetHistorySink(IFleetHistorySink const&) = delete;
    IFleetHistorySink& operator=(IFleetHistorySink const&) = delete;
    IFleetHistorySink(IFleetHistorySink&&) = delete;
    IFleetHistorySink& operator=(IFleetHistorySink&&) = delete;

    /// Take what a node handed over.
    /// @param endpoint Which machine reported; the machine, never the worker id.
    /// @param provenKey The identity key the reporting connection PROVED, raw; empty when it proved none. What a
    ///        machine's series is later attributed BY -- an endpoint is the machine's own claim.
    /// @param buckets Its closed buckets, oldest first.
    /// @return How many were new -- a redelivered batch answers zero.
    virtual std::size_t AcceptHistory(std::string_view endpoint,
                                      std::string_view provenKey,
                                      std::span<FleetBucket const> buckets) = 0;
};

/// Where the leader's admission queue reports each compile it GRANTS, for the keyed history (who asked, which
/// toolchain).
///
/// Called once per admission ticket, at the first decision that runs a compile -- on a worker or on the requesting
/// machine itself -- and never for a request refused, abandoned before its grant, or answered `Stored` (another
/// compile of the same key stored it). A worker refusal that puts the ticket back is not a second grant.
class ICompileDemandSink
{
  public:
    ICompileDemandSink() = default;
    virtual ~ICompileDemandSink() = default;
    ICompileDemandSink(ICompileDemandSink const&) = delete;
    ICompileDemandSink& operator=(ICompileDemandSink const&) = delete;
    ICompileDemandSink(ICompileDemandSink&&) = delete;
    ICompileDemandSink& operator=(ICompileDemandSink&&) = delete;

    /// One compile granted.
    /// @param requester The machine the queue charged: the identity key its connection PROVED, raw, or this leader's
    ///        own for a caller on this machine -- never an endpoint, a worker id or a hostname.
    /// @param fingerprint The toolchain the request named; empty for a local-only request that did not know it.
    virtual void CompileGranted(std::string_view requester, std::string_view fingerprint) = 0;
};

} // namespace Pace::Distributed
