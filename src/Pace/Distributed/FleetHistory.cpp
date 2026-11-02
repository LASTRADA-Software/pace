// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/WireFields.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/HistoryEnvelope.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <utility>

namespace Pace::Distributed
{

namespace
{
    using namespace HistoryEnvelope;

    /// One ring's bucket width, in milliseconds.
    /// @param ring Which ring.
    /// @return Its bucket width.
    [[nodiscard]] constexpr std::int64_t RingWidthMillis(FleetRing ring) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(FleetRingTable[static_cast<std::size_t>(ring)].bucket)
            .count();
    }

    /// A node's own series: three rings in one file.
    ///
    /// Version 4 added each bucket's absence mask and the CPU slot, together: the slot is the first that can be
    /// absent inside a present bucket, so the two are one change to the shape.
    constexpr FileEnvelope RingFile { .magic = { 'F', 'C', 'F', 'H' }, .newest = 4 };

    /// The received-histories store: one nested `FleetHistory` body per machine.
    ///
    /// Its own magic, so a path typed into the wrong flag is refused by name rather
    /// than read as a truncated history.
    ///
    /// Its version moves with the nested body's shape, not with `RingFile`'s: a
    /// nested body carries no envelope of its own, so THIS is the only place a
    /// reader can learn whether it was written at `ReadVersion2`'s width or
    /// `ReadVersion3`'s. Version 1 wrote the former, version 2 the latter -- moved
    /// here because `DispatchAllExcluded` widened `FleetMetric`, the same reason
    /// `RingFile` moved to 3. Version 3 writes `ReadVersion4`'s shape -- the CPU slot and
    /// the absence mask -- and files each series under the identity key it was proven under
    /// (`[endpoint][high-water][key][body]`), which is what a machine's own series is looked up BY.
    constexpr FileEnvelope NodeStoreFile { .magic = { 'F', 'C', 'N', 'H' }, .newest = 3 };

    /// What one bucket costs in a body: five scalars, the readings, and the fold.
    ///
    /// Derived from the metric table rather than written down, so a tenth metric
    /// moves it. Only a reservation hint -- being wrong costs a reallocation, not a
    /// wrong file -- but a hint nothing keeps current is a hint that stops helping.
    constexpr std::size_t BucketBytes =
        (5 + EnumeratorCount<FleetMetric> + (EnumeratorCount<FleetMetric> * 3)) * sizeof(std::uint64_t);

    /// The largest ring this build keeps, as a ceiling on a declared bucket count.
    ///
    /// Derived from the table rather than written down: a count a body could not
    /// possibly hold is refused before anything is allocated against it, and the
    /// bound has to move when a ring grows or it becomes the thing that rejects a
    /// legitimate file.
    constexpr std::size_t MaxRingSlots = []() consteval {
        std::size_t most = 0;
        for (auto const& row: FleetRingTable)
            most = std::max(most, row.slots);
        return most;
    }();

    /// Which ring holds a bucket of this width, and how wide its slots are.
    [[nodiscard]] std::int64_t WidthMillisFor(FleetRange range) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(FleetRangeTable[static_cast<std::size_t>(range)].bucket)
            .count();
    }

    /// Fold one reading into a ring, keyed by absolute bucket number.
    ///
    /// The slot carries its own start, so a slot left over from a previous lap is
    /// detectable rather than being mistaken for this lap's data -- which is what
    /// makes a gap after downtime render as a gap rather than as week-old numbers.
    ///
    /// Accumulates rather than overwrites, because a bucket wider than the sample
    /// interval is a FOLD and a fold that kept only its last reading has thrown the
    /// interesting part away: a refusal spike averaged over a day is invisible, and
    /// neither the peak nor the floor is recoverable afterwards.
    ///
    /// @param ring The ring to fold into.
    /// @param widthMillis Width of one of its buckets.
    /// @param nowMillis When this reading was taken.
    /// @param values The reading.
    /// @param absent The slots this reading could not answer for; their values are ignored.
    /// @param previous The reading before this one, or null when there is none or
    ///                 when it is too old to subtract from.
    /// @param previousAbsent The slots that earlier reading could not answer for.
    /// @return True when this opened a bucket that was not already there.
    bool AccumulateInto(std::vector<FleetBucket>& ring,
                        std::int64_t widthMillis,
                        std::int64_t nowMillis,
                        EnumTable<FleetMetric, std::uint64_t> const& values,
                        FleetSlotMask absent,
                        EnumTable<FleetMetric, std::uint64_t> const* previous,
                        FleetSlotMask previousAbsent)
    {
        auto const number = nowMillis / widthMillis;
        auto const start = number * widthMillis;
        auto& slot = ring[static_cast<std::size_t>(number) % ring.size()];
        auto const opened = !slot.present || slot.startMillis != start;

        // What the bucket held before this reading: on opening, nothing at all -- every slot absent -- so the
        // first reading of each slot is its first wherever in the bucket it lands.
        auto const heldBefore = opened ? EveryFleetSlot : slot.absent;
        if (opened)
            slot = FleetBucket { .startMillis = start, .sampleMillis = nowMillis, .values = {}, .present = true };
        // `nowMillis` for the sample instant, NOT the aligned `start`. Stamping
        // the alignment throws the reading's real time away at the moment it is
        // recorded, and no later fold can recover it -- which made the divisor a
        // no-op for a view whose bucket IS its sub-bucket, since those are always
        // exactly one nominal width apart.
        slot.sampleMillis = nowMillis;
        // A slot stays absent only while EVERY reading in the bucket lacked it.
        slot.absent = heldBefore & absent;

        for (auto const& row: FleetMetricTable)
        {
            auto const index = static_cast<std::size_t>(row.metric);
            auto const bit = SlotBit(row.metric);
            auto& fold = slot.fold[index];
            if (opened)
                fold = FleetFold {};
            // A slot this reading could not answer for keeps what the bucket already had: the newest reading
            // that existed, and a fold over readings alone -- an absence is neither a floor nor a peak.
            if ((absent & bit) != 0)
                continue;

            auto const reading = values[index];
            auto const first = (heldBefore & bit) != 0;
            slot.values[index] = reading;

            if (row.kind == FleetMetricKind::Gauge)
            {
                fold.low = first ? reading : std::min(fold.low, reading);
                fold.high = first ? reading : std::max(fold.high, reading);
                fold.total = (first ? 0 : fold.total) + reading;
                continue;
            }

            // A counter's peak is a RATE, so it needs the reading before this one --
            // which for a bucket's first sample sits in the bucket before it. A
            // counter has no meaningful floor or sum: its mean over a window is the
            // delta across it, and its minimum is not a fact anybody acts on.
            // Backwards is a restart, whose step is unknowable rather than enormous;
            // and a reading the previous sample lacked has no step at all.
            if (previous != nullptr && (previousAbsent & bit) == 0 && reading >= (*previous)[index])
                fold.high = std::max(fold.high, reading - (*previous)[index]);
        }

        slot.coverage = opened ? 1 : slot.coverage + 1;
        return opened;
    }

    /// Fold every present sub-bucket of one window into the bucket a view draws.
    ///
    /// `values` is the window's NEWEST reading rather than a sum, because counters
    /// are stored raw and cumulative: the value at a window's end is what makes the
    /// difference between two windows the work done between them.
    ///
    /// The `fold` is merged across the whole window instead, which is the entire
    /// reason it is carried -- taking the newest sub-bucket's copy would discard
    /// every spike that did not happen to land in the last one.
    ///
    /// @param ring The ring to read.
    /// @param subWidthMillis Width of one of its buckets.
    /// @param windowStart First instant of the window.
    /// @param windowEnd One past its last.
    /// @return The folded bucket, or nullopt when nothing in the window was sampled.
    [[nodiscard]] std::optional<FleetBucket> FoldWithin(std::vector<FleetBucket> const& ring,
                                                        std::int64_t subWidthMillis,
                                                        std::int64_t windowStart,
                                                        std::int64_t windowEnd)
    {
        // Walked by sub-bucket: `windowStart`, then every `subWidthMillis` after it while still
        // inside the window -- the window's span rounded up by the sub-width, and none at all
        // for an empty or inverted window. The sub-width is a ring's bucket from
        // `FleetRingTable`, a positive duration, and the span is one view bucket, so the
        // rounding can neither divide by zero nor overflow.
        auto const span = windowEnd - windowStart;
        auto const steps = span <= 0 ? std::int64_t { 0 } : (span + subWidthMillis - 1) / subWidthMillis;
        std::optional<FleetBucket> out;
        for (auto const step: std::views::iota(std::int64_t { 0 }, steps))
        {
            auto const start = windowStart + (step * subWidthMillis);
            auto const number = start / subWidthMillis;
            auto const& slot = ring[static_cast<std::size_t>(number) % ring.size()];
            if (!slot.present || slot.startMillis != start)
                continue;

            if (!out.has_value())
            {
                out = slot;
                continue;
            }

            // Walked oldest first, so every later sub-bucket is the newer one: its instant wins, and so does each
            // reading it HAS -- a slot it lacks keeps the newest reading an earlier sub-bucket held.
            auto const newer = slot.startMillis > out->startMillis;
            if (newer)
            {
                out->startMillis = slot.startMillis;
                out->sampleMillis = slot.sampleMillis;
            }

            for (auto const& row: FleetMetricTable)
            {
                auto const index = static_cast<std::size_t>(row.metric);
                auto const bit = SlotBit(row.metric);
                if ((slot.absent & bit) != 0)
                    continue;
                auto& into = out->fold[index];
                auto const& from = slot.fold[index];
                if ((out->absent & bit) != 0)
                {
                    // The first sub-bucket with a reading for this slot: its value and fold are the window's so far.
                    out->values[index] = slot.values[index];
                    into = from;
                    continue;
                }
                if (newer)
                    out->values[index] = slot.values[index];
                // A counter's `low` and `total` are unused and stay zero, so the same
                // three lines serve both kinds rather than branching on one to do
                // nothing.
                into.low = std::min(into.low, from.low);
                into.high = std::max(into.high, from.high);
                into.total += from.total;
            }
            out->absent &= slot.absent;
            out->coverage += slot.coverage;
        }
        return out;
    }

    /// Stamp a big-endian word over one already appended.
    /// @param out The buffer.
    /// @param offset Where the word sits.
    /// @param value What it should say.
    void WriteU64(std::string& out, std::size_t offset, std::uint64_t value)
    {
        WireFields::PutBigEndian<std::uint64_t>(std::as_writable_bytes(std::span { out }), offset, value);
    }

    /// The rings a reader recovered, before they are installed.
    struct LoadedHistory
    {
        EnumTable<FleetRing, std::vector<FleetBucket>> rings; ///< Sized to THIS build's geometry.
        std::uint64_t generation { 0 };                       ///< Restored, never reset.
    };

    /// Put a bucket where THIS build's geometry says it belongs.
    ///
    /// A ring's slot is `bucketNumber % slots`, so a ring whose size changed between
    /// builds has every one of its buckets at the wrong index -- copying the vector
    /// across would scatter a year of readings into positions that then read as a
    /// previous lap and are silently dropped. Re-placing by the bucket's own start is
    /// what makes a geometry change an inflation rather than a loss, and it costs
    /// nothing on a version whose geometry did not move.
    ///
    /// @param ring The destination, already sized by this build.
    /// @param widthMillis That ring's bucket width.
    /// @param bucket What was read.
    void ReplaceInto(std::vector<FleetBucket>& ring, std::int64_t widthMillis, FleetBucket const& bucket)
    {
        if (!bucket.present || ring.empty())
            return;
        auto const number = bucket.startMillis / widthMillis;
        auto& slot = ring[static_cast<std::size_t>(number) % ring.size()];
        // A ring that SHRANK maps several old buckets onto one slot, so the newest
        // wins -- the same rule a lap already uses.
        if (!slot.present || slot.startMillis < bucket.startMillis)
            slot = bucket;
    }

    /// Read one ring's buckets and re-place each of them.
    ///
    /// @param reader The cursor.
    /// @param ring Which ring these buckets belong to.
    /// @param into The destination.
    /// @param readBucket Reads one bucket in the format being served.
    /// @return False on a truncated or implausible body.
    bool ReadRing(BodyReader& reader, FleetRing ring, std::vector<FleetBucket>& into, auto const& readBucket)
    {
        std::uint64_t count = 0;
        if (!reader.Take(count) || count > MaxRingSlots)
            return false;
        for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, count))
        {
            FleetBucket bucket;
            if (!readBucket(reader, bucket))
                return false;
            ReplaceInto(into, RingWidthMillis(ring), bucket);
        }
        return true;
    }

    /// Read a whole body: the generation, then one ring per entry of @p rings.
    ///
    /// Every version so far has that shape and differs only in which rings it holds
    /// and how one bucket is spelled, so those are the two parameters and the rest is
    /// written once. A version that departs from the shape writes its own reader --
    /// which is what the format table is for.
    ///
    /// @param reader The cursor.
    /// @param out Where the rings land; sized to this build before anything is read.
    /// @param rings Which rings the body carries, in the order it carries them.
    /// @param readBucket Reads one bucket in the format being served.
    /// @return False on a truncated or implausible body.
    bool ReadBody(BodyReader& reader, LoadedHistory& out, std::span<FleetRing const> rings, auto const& readBucket)
    {
        // Sized to THIS build's geometry, not the file's: a ring the file is shorter
        // or longer than is the ordinary case across an upgrade, and `ReplaceInto`
        // puts each bucket where this build keeps it.
        for (auto const& row: FleetRingTable)
            out.rings[static_cast<std::size_t>(row.ring)].assign(row.slots, FleetBucket {});

        if (!reader.Take(out.generation))
            return false;
        for (auto const ring: rings)
            if (!ReadRing(reader, ring, out.rings[static_cast<std::size_t>(ring)], readBucket))
                return false;
        return true;
    }

    /// The rings version 1 carried: minute, then hour.
    constexpr std::array<FleetRing, 2> Version1Rings { FleetRing::Minute, FleetRing::Hour };

    /// The rings version 2 carries: every one this build has, in enumerator order.
    ///
    /// Derived from the table rather than listed, so a ring added there is written and
    /// read without this constant being edited -- and a build whose ring set differs
    /// from the file's is a different version by definition, which is why neither the
    /// writer nor the reader stores a count.
    ///
    /// Version 3 carries the same rings; only the value width moved between the two, so
    /// this is shared rather than duplicated under a second name.
    constexpr auto Version2Rings = []() consteval {
        std::array<FleetRing, EnumeratorCount<FleetRing>> rings {};
        for (auto const& row: FleetRingTable)
            rings[static_cast<std::size_t>(row.ring)] = row.ring;
        return rings;
    }();

    /// The value and fold width versions 1 and 2 wrote: nine, for as long as
    /// `FleetMetric` had nine enumerators.
    ///
    /// Frozen here rather than derived from `EnumeratorCount<FleetMetric>`, which is
    /// THIS build's width and not what a v1 or v2 file actually holds. `FleetMetric`'s
    /// own doc comment states the ordinals are a persisted, append-only contract for
    /// exactly this reason: a metric added after this constant was fixed lands past
    /// index 8, and a v1 or v2 bucket simply never wrote it -- reading one back with
    /// that slot at zero is correct, not a gap.
    constexpr std::size_t LegacyValueWidth = 9;

    /// The value and fold width version 3 wrote: ten, for as long as `DispatchAllExcluded` was the newest slot.
    ///
    /// Frozen for `LegacyValueWidth`'s reason. `CpuBusyPermille` lands past index 9, and a v3 bucket never
    /// wrote it -- which, unlike an exclusion count, reads ABSENT (`FleetUncarried`), not zero.
    constexpr std::size_t VersionThreeValueWidth = 10;
    /// Version 1: a generation, then two rings -- minute and hour -- of
    /// `[start][present][9 values]`, with no fold and no coverage.
    ///
    /// Inflated rather than discarded. `coverage` becomes 1, which is *literally*
    /// what a v1 bucket held: the writer overwrote, so however many samples fell in
    /// an hour, exactly one reading was kept. A gauge's fold collapses to that
    /// reading, and a counter's peak rate stays zero -- unknowable, and absent rather
    /// than invented.
    ///
    /// v1's hour ring held 168 slots against this build's 720, which is precisely the
    /// case `ReplaceInto` exists for.
    /// @param reader The cursor.
    /// @param out Where the rings land.
    /// @return False on a truncated or implausible body.
    bool ReadVersion1(BodyReader& reader, LoadedHistory& out)
    {
        return ReadBody(reader, out, Version1Rings, [](BodyReader& from, FleetBucket& bucket) {
            std::uint64_t start = 0;
            std::uint64_t present = 0;
            if (!from.Take(start) || !from.Take(present))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            // Derived, and only for v1: that format kept one reading per bucket, so
            // the bucket's own start is as close to the sample instant as anything
            // recoverable from it. v2 stores the instant because its buckets fold.
            bucket.sampleMillis = bucket.startMillis;
            bucket.present = present != 0;
            // Exactly the width v1 wrote, never `bucket.values`' own extent: a metric
            // appended since lands past index 8 and this format never carried it.
            for (auto const index: std::views::iota(std::size_t { 0 }, LegacyValueWidth))
                if (!from.Take(bucket.values[index]))
                    return false;
            bucket.coverage = bucket.present ? 1 : 0;
            bucket.absent = UncarriedAbsentSlots(LegacyValueWidth);
            for (auto const& row: FleetMetricTable)
            {
                if (row.kind != FleetMetricKind::Gauge)
                    continue;
                auto const index = static_cast<std::size_t>(row.metric);
                auto const reading = bucket.values[index];
                bucket.fold[index] = FleetFold { .low = reading, .high = reading, .total = reading };
            }
            return true;
        });
    }

    /// Version 2: a generation, then one ring per `FleetRingTable` row of
    /// `[start][sample][present][coverage][9 values][9 x low,high,total]`.
    ///
    /// `sample` is stored rather than derived from `start`, because a ring that folds
    /// sixty readings into a bucket has no way to recover when the last of them
    /// landed -- and a rate divided by a nominal width instead of the span actually
    /// observed understates by however much of the window was never sampled.
    ///
    /// Reads exactly `LegacyValueWidth` values and folds, never `bucket.values`' own
    /// extent: this format is frozen at the width it was written with, the same reason
    /// `ReadVersion1` is. A metric appended since -- `DispatchAllExcluded` -- lands past
    /// index 8 of `bucket.values` and stays at its default zero for every v2 bucket,
    /// which is correct: no v2 file ever counted an exclusion, because the wire carried
    /// none yet.
    /// @param reader The cursor.
    /// @param out Where the rings land.
    /// @return False on a truncated or implausible body.
    bool ReadVersion2(BodyReader& reader, LoadedHistory& out)
    {
        return ReadBody(reader, out, Version2Rings, [](BodyReader& from, FleetBucket& bucket) {
            std::uint64_t start = 0;
            std::uint64_t sample = 0;
            std::uint64_t present = 0;
            if (!from.Take(start) || !from.Take(sample) || !from.Take(present) || !from.Take(bucket.coverage))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            bucket.sampleMillis = static_cast<std::int64_t>(sample);
            bucket.present = present != 0;
            for (auto const index: std::views::iota(std::size_t { 0 }, LegacyValueWidth))
                if (!from.Take(bucket.values[index]))
                    return false;
            for (auto const index: std::views::iota(std::size_t { 0 }, LegacyValueWidth))
                if (!from.Take(bucket.fold[index].low) || !from.Take(bucket.fold[index].high)
                    || !from.Take(bucket.fold[index].total))
                    return false;
            bucket.absent = UncarriedAbsentSlots(LegacyValueWidth);
            return true;
        });
    }

    /// Version 3: the same shape version 2 wrote, at ten values and ten folds --
    /// `[start][sample][present][coverage][10 values][10 x low,high,total]`.
    ///
    /// Minted because `DispatchAllExcluded` widened `FleetMetric` past what versions 1 and
    /// 2 ever wrote (#308's ordinals are a persisted contract, so it landed at the END
    /// rather than reordering either), and reading a v2 body against a wider layout
    /// would misalign every field after the first missing one. Frozen at
    /// `VersionThreeValueWidth` since version 4 moved forward with the live table; the CPU
    /// slot it never carried reads absent.
    /// @param reader The cursor.
    /// @param out Where the rings land.
    /// @return False on a truncated or implausible body.
    bool ReadVersion3(BodyReader& reader, LoadedHistory& out)
    {
        return ReadBody(reader, out, Version2Rings, [](BodyReader& from, FleetBucket& bucket) {
            std::uint64_t start = 0;
            std::uint64_t sample = 0;
            std::uint64_t present = 0;
            if (!from.Take(start) || !from.Take(sample) || !from.Take(present) || !from.Take(bucket.coverage))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            bucket.sampleMillis = static_cast<std::int64_t>(sample);
            bucket.present = present != 0;
            for (auto const index: std::views::iota(std::size_t { 0 }, VersionThreeValueWidth))
                if (!from.Take(bucket.values[index]))
                    return false;
            for (auto const index: std::views::iota(std::size_t { 0 }, VersionThreeValueWidth))
                if (!from.Take(bucket.fold[index].low) || !from.Take(bucket.fold[index].high)
                    || !from.Take(bucket.fold[index].total))
                    return false;
            bucket.absent = UncarriedAbsentSlots(VersionThreeValueWidth);
            return true;
        });
    }

    /// Version 4: version 3's shape with each bucket's absence mask after its coverage, over THIS build's full
    /// value and fold width -- `[start][sample][present][coverage][absent][N values][N x low,high,total]`, where
    /// `N` is `EnumeratorCount<FleetMetric>`.
    ///
    /// Minted by `CpuBusyPermille`, the first slot that can be absent inside a present bucket: without the mask
    /// a restart would read every CPU gap back as an idle zero. This reader moves forward with the live table
    /// until it grows again and earns a version 5 of its own.
    /// @param reader The cursor.
    /// @param out Where the rings land.
    /// @return False on a truncated or implausible body.
    bool ReadVersion4(BodyReader& reader, LoadedHistory& out)
    {
        return ReadBody(reader, out, Version2Rings, [](BodyReader& from, FleetBucket& bucket) {
            std::uint64_t start = 0;
            std::uint64_t sample = 0;
            std::uint64_t present = 0;
            std::uint64_t absent = 0;
            if (!from.Take(start) || !from.Take(sample) || !from.Take(present) || !from.Take(bucket.coverage)
                || !from.Take(absent))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            bucket.sampleMillis = static_cast<std::int64_t>(sample);
            bucket.present = present != 0;
            bucket.absent = absent & EveryFleetSlot;
            for (auto& value: bucket.values)
                if (!from.Take(value))
                    return false;
            for (auto& fold: bucket.fold)
                if (!from.Take(fold.low) || !from.Take(fold.high) || !from.Take(fold.total))
                    return false;
            return true;
        });
    }

    /// One readable on-disk format.
    struct HistoryFormat
    {
        std::uint8_t version;                      ///< What the header says.
        bool (*read)(BodyReader&, LoadedHistory&); ///< Reads a body of that version.
    };

    /// Every format this build can read, oldest first.
    ///
    /// A TABLE and not an equality test, which is the idiom `.agent/rules/storage.md`
    /// already fixes for the store: each row reads its own layout and inflates it
    /// forward, so adding a resolution is a row and the year already on disk survives
    /// the upgrade. Bumping `FileVersion` without adding a row here remains the
    /// decision to discard every history -- and is then a visible one, made by
    /// deleting a row rather than by not noticing a constant.
    constexpr std::array<HistoryFormat, 4> HistoryFormats {
        HistoryFormat { .version = 1, .read = &ReadVersion1 },
        HistoryFormat { .version = 2, .read = &ReadVersion2 },
        HistoryFormat { .version = 3, .read = &ReadVersion3 },
        HistoryFormat { .version = 4, .read = &ReadVersion4 },
    };

    static_assert(HistoryFormats.back().version == RingFile.newest,
                  "the newest ring format this build writes must be one it can read back");

    /// The reader for a version, if this build has one.
    /// @param version What the header said.
    /// @return The row, or nullptr.
    [[nodiscard]] HistoryFormat const* FormatFor(std::uint8_t version) noexcept
    {
        for (auto const& row: HistoryFormats)
            if (row.version == version)
                return &row;
        return nullptr;
    }

    /// Which shape `NodeStoreFile` wrote its nested bodies in, per its OWN version.
    ///
    /// A nested body carries no envelope of its own -- `AppendBody` stamps nothing
    /// inside it -- so the outer `NodeStoreFile` version is the only place this fact
    /// travels, and it is a DIFFERENT axis from `RingFile`'s `HistoryFormats`: a
    /// standalone ring file and a nested body can move on different schedules even
    /// though today they happen to share readers. Version 1 wrote a nested body in
    /// `ReadVersion2`'s shape (frozen at nine values); version 2 moved to
    /// `ReadVersion3`'s, the same day `FleetMetric` grew `DispatchAllExcluded`; version 3
    /// to `ReadVersion4`'s, the day it grew `CpuBusyPermille` and buckets their absence mask.
    constexpr std::array<HistoryFormat, 3> NestedBodyFormats {
        HistoryFormat { .version = 1, .read = &ReadVersion2 },
        HistoryFormat { .version = 2, .read = &ReadVersion3 },
        HistoryFormat { .version = 3, .read = &ReadVersion4 },
    };

    static_assert(NestedBodyFormats.back().version == NodeStoreFile.newest,
                  "the newest received-store format this build writes must be one it can read back");

    /// The first `NodeStoreFile` version that files each series under the identity key it was proven under. Older
    /// stores carry none, so every series they hold is dropped on load (`FleetNodeHistories::DroppedOnLoad`).
    constexpr std::uint8_t NodeStoreKeyedSince = 3;

    /// The nested-body reader for one `NodeStoreFile` version, if this build has one.
    /// @param version What `NodeStoreFile`'s envelope said.
    /// @return The row, or nullptr.
    [[nodiscard]] HistoryFormat const* NestedFormatFor(std::uint8_t version) noexcept
    {
        for (auto const& row: NestedBodyFormats)
            if (row.version == version)
                return &row;
        return nullptr;
    }
} // namespace

std::optional<FleetRange> FleetRangeFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetRangeTable)
        if (row.key == key)
            return row.range;
    return std::nullopt;
}

FleetHistory::FleetHistory(core::platform::WallClockRef wall):
    _wall { wall }
{
    // Sized from the table rather than from three named constants, so a ring added
    // there is allocated here without this constructor being edited.
    for (auto const& row: FleetRingTable)
        _rings[static_cast<std::size_t>(row.ring)].assign(row.slots, FleetBucket {});
}

std::int64_t FleetHistory::NowMillis() const noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(_wall.now().time_since_epoch()).count();
}

void FleetHistory::Record(EnumTable<FleetMetric, std::uint64_t> const& values, FleetSlotMask absent)
{
    RecordAt(NowMillis(), values, absent);
}

void FleetHistory::Adopt(FleetBucket const& bucket)
{
    if (!bucket.present)
        return;
    // Its own sample instant, not this leader's clock. See the header: stamping a
    // catch-up with the moment it arrived would pile a day into one window.
    RecordAt(bucket.sampleMillis, bucket.values, bucket.absent);
}

void FleetHistory::RecordAt(std::int64_t now, EnumTable<FleetMetric, std::uint64_t> const& values, FleetSlotMask absent)
{
    auto const guard = std::scoped_lock { _mutex };

    // A delta is only a rate when the two readings are ADJACENT. A node samples only
    // while it leads, so a leadership change of three hours ends with one sample
    // carrying three hours of work -- folded in as a per-minute peak that would be a
    // spike that never happened, persisted, and in the day ring for over a year. The
    // same rule the renderer applies to a rate across a gap, enforced here because
    // this fold cannot be undone afterwards. One interval of slack for a timer that
    // fired late.
    auto const adjacent = _hasLast && (now - _lastMillis) <= (2 * FleetSampleInterval.count() * 1000);
    auto const* previous = adjacent ? &_last : nullptr;

    // EVERY ring takes EVERY sample -- online accumulation, rather than a coarse ring
    // fed by buckets ageing out of the one below it. Both give the same numbers, and
    // this one has a single writer per ring and no second code path that first runs
    // twenty-four hours in and is therefore exercised by nothing short of a day-long
    // test.
    auto opened = false;
    for (auto const& row: FleetRingTable)
    {
        auto const index = static_cast<std::size_t>(row.ring);
        auto const openedHere = AccumulateInto(
            _rings[index], RingWidthMillis(row.ring), now, values, absent & EveryFleetSlot, previous, _lastAbsent);
        // The generation follows the BASE ring, which is the one whose buckets close
        // fastest: an ETag that only moved when the day ring rolled would hold a
        // stale chart for a day.
        if (row.ring == FleetRing::Minute)
            opened = openedHere;
    }

    _last = values;
    _lastAbsent = absent & EveryFleetSlot;
    _lastMillis = now;
    _hasLast = true;
    if (opened)
        ++_generation;
}

std::vector<FleetBucket> FleetHistory::Buckets(FleetRange range) const
{
    auto const& row = FleetRangeTable[static_cast<std::size_t>(range)];
    auto const width = WidthMillisFor(range);
    auto const subWidth = RingWidthMillis(row.ring);
    auto const now = NowMillis();

    auto const guard = std::scoped_lock { _mutex };

    // Which ring is a COLUMN of the range row, not a conditional. The two-range
    // version asked `range == FleetRange::Week ? _hours : _minutes`, which is a
    // ladder that grows an arm per window -- and the eighth arm is the one somebody
    // forgets, silently drawing a twelve-month view out of a ring holding a day.
    auto const& source = _rings[static_cast<std::size_t>(row.ring)];

    auto const newest = now / width;
    auto const oldest = newest - static_cast<std::int64_t>(row.points) + 1;

    std::vector<FleetBucket> out;
    out.reserve(row.points);
    for (auto const index: std::views::iota(std::size_t { 0 }, row.points))
    {
        auto const start = (oldest + static_cast<std::int64_t>(index)) * width;
        auto found = FoldWithin(source, subWidth, start, start + width);
        if (found.has_value())
        {
            // Only `startMillis` is restamped, to the window's start, so the chart
            // plots evenly. `sampleMillis` is carried through untouched: it is the
            // instant `AccumulateInto` recorded, and deriving it from the sub-bucket's
            // alignment here would throw away exactly what it exists to keep.
            found->startMillis = start;
            out.push_back(*found);
        }
        else
            out.push_back(FleetBucket { .startMillis = start, .sampleMillis = start, .values = {}, .present = false });
    }
    return out;
}

std::vector<FleetBucket> FleetHistory::ClosedBucketsAfter(std::int64_t afterMillis, std::size_t limit) const
{
    if (limit == 0)
        return {};

    auto const width = RingWidthMillis(FleetRing::Minute);
    auto const now = NowMillis();
    // The bucket `now` falls in is still OPEN -- it gains a reading every sample --
    // so the newest one that may travel is the one before it. Sending an open bucket
    // would hand the leader a partial window it could never be told to correct.
    auto const newestClosedStart = ((now / width) * width) - width;

    auto const guard = std::scoped_lock { _mutex };
    auto const& ring = _rings[static_cast<std::size_t>(FleetRing::Minute)];

    // Selected by INDEX first, and the buckets copied only once the batch is known.
    // A node catching up after a restart matches most of a day's ring, and a bucket
    // is three hundred bytes: sorting the buckets themselves moved half a megabyte
    // per heartbeat round to keep a hundred and twenty-eight of them, under this
    // lock, while the sampler waited to record the next one.
    std::vector<std::pair<std::int64_t, std::size_t>> chosen;
    for (auto const index: std::views::iota(std::size_t { 0 }, ring.size()))
    {
        auto const& slot = ring[index];
        if (!slot.present || slot.startMillis <= afterMillis || slot.startMillis > newestClosedStart)
            continue;
        chosen.emplace_back(slot.startMillis, index);
    }

    // A ring is stored by `number % slots`, so its iteration order is wherever the
    // lap happens to sit rather than time order. Ordered here because "oldest first"
    // is the whole of how a catch-up converges: taking an arbitrary 128 would leave
    // the gap open forever. Only the prefix that fits is fully ordered.
    auto const taken = std::min(limit, chosen.size());
    std::ranges::partial_sort(chosen, chosen.begin() + static_cast<std::ptrdiff_t>(taken));

    std::vector<FleetBucket> out;
    out.reserve(taken);
    for (auto const& [start, index]: std::span { chosen }.first(taken))
        out.push_back(ring[index]);
    return out;
}

std::uint64_t FleetHistory::Generation() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return _generation;
}

bool FleetHistory::Empty() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return std::ranges::none_of(_rings, [](std::vector<FleetBucket> const& ring) {
        return std::ranges::any_of(ring, [](FleetBucket const& bucket) { return bucket.present; });
    });
}

std::chrono::seconds FleetHistory::UntilBucketCloses(FleetRange range) const noexcept
{
    auto const width = FleetRangeTable[static_cast<std::size_t>(range)].bucket;
    auto const widthMillis = std::chrono::milliseconds { width }.count();
    auto const elapsed = NowMillis() % widthMillis;
    auto const remaining = std::chrono::milliseconds { widthMillis - elapsed };
    // Rounded up and floored at a second: a `max-age=0` would make a browser
    // revalidate every image on every refresh, which is the cost this exists to
    // avoid rather than a freshness guarantee.
    return std::max(std::chrono::seconds { 1 }, std::chrono::ceil<std::chrono::seconds>(remaining));
}

bool FleetHistory::ReadOnly() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return _readOnly;
}

void FleetHistory::AppendBody(std::string& out) const
{
    // Its OWN lock, not the caller's. `FleetNodeHistories::Save` walks a map of
    // these and holds only the map's mutex, so a body composed under that alone
    // would be read while the machine it belongs to was still recording into it.
    auto const guard = std::scoped_lock { _mutex };

    // The whole of what this appends, so a store of a hundred machines does not
    // grow its buffer twenty times per machine.
    auto reserved = sizeof(std::uint64_t);
    for (auto const& row: FleetRingTable)
        reserved += sizeof(std::uint64_t) + (_rings[static_cast<std::size_t>(row.ring)].size() * BucketBytes);
    out.reserve(out.size() + reserved);

    AppendU64(out, _generation);
    // One ring per table row, in enumerator order -- the same order `ReadVersion2`
    // reads them back in, and the reason neither writes a ring count.
    for (auto const& row: FleetRingTable)
    {
        auto const& ring = _rings[static_cast<std::size_t>(row.ring)];
        AppendU64(out, ring.size());
        for (auto const& bucket: ring)
        {
            AppendU64(out, static_cast<std::uint64_t>(bucket.startMillis));
            AppendU64(out, static_cast<std::uint64_t>(bucket.sampleMillis));
            AppendU64(out, bucket.present ? 1U : 0U);
            AppendU64(out, bucket.coverage);
            AppendU64(out, bucket.absent);
            for (auto const value: bucket.values)
                AppendU64(out, value);
            for (auto const& fold: bucket.fold)
            {
                AppendU64(out, fold.low);
                AppendU64(out, fold.high);
                AppendU64(out, fold.total);
            }
        }
    }
}

bool FleetHistory::ReadBody(std::string_view body, std::uint8_t version)
{
    // There is no envelope on a nested body itself -- `version` is the OUTER
    // `NodeStoreFile`'s, and `NestedFormatFor` is what turns that into the shape
    // this body was actually written in.
    auto const* const format = NestedFormatFor(version);
    if (format == nullptr)
        return false;

    LoadedHistory loaded;
    BodyReader reader { body };
    if (!format->read(reader, loaded))
        return false;

    auto const guard = std::scoped_lock { _mutex };
    _rings = std::move(loaded.rings);
    _generation = loaded.generation;
    return true;
}

bool FleetHistory::Save(std::filesystem::path const& path, StateFile which) const
{
    // Refused BEFORE a byte is composed, and the file is left exactly as it was. A
    // node rolled back to an older build read a history it could not understand;
    // overwriting it would destroy a year of readings that the build it was rolled
    // back FROM could still have read. Everything else in this class is recoverable
    // by waiting.
    if (ReadOnly())
        return false;

    auto file = FramedBuffer();
    AppendBody(file);
    return WriteFramed(path, RingFile, file, which);
}

bool FleetHistory::Load(std::filesystem::path const& path)
{
    auto const opened = ReadFramed(path, RingFile);
    if (opened.outcome == FramedOutcome::Newer)
    {
        auto const guard = std::scoped_lock { _mutex };
        _readOnly = true;
        return false;
    }
    if (opened.outcome != FramedOutcome::Ok)
        return false;

    // A version at or below the newest this build writes, and still no reader, is a
    // format deliberately dropped from the table. Overwriting that is the decision
    // the row's removal already made.
    auto const* const format = FormatFor(opened.version);
    if (format == nullptr)
        return false;

    LoadedHistory loaded;
    BodyReader reader { opened.body };
    if (!format->read(reader, loaded))
        return false;

    auto const guard = std::scoped_lock { _mutex };
    _rings = std::move(loaded.rings);
    // Restored, not reset: a generation that started again from zero would let a
    // client's cached chart match an ETag it had already been served.
    _generation = loaded.generation;
    return true;
}

FleetNodeHistories::FleetNodeHistories(core::platform::WallClockRef wall):
    _wall { wall }
{
}

FleetNodeHistories::Series::Series(core::platform::WallClockRef wall, std::string at):
    history { wall },
    endpoint { std::move(at) }
{
}

void FleetNodeHistories::EvictForOneMoreLocked()
{
    if (_byKey.size() < MaxReceivedHistories)
        return;
    // The series whose newest reading is OLDEST: a machine still reporting is never the one dropped while a silent one
    // is kept. Each mark is read under its series' lock -- the map's lock is held first, in the one order both are
    // ever taken in.
    auto const markOf = [](auto const& each) {
        auto const guard = std::scoped_lock { each.second->mutex };
        return each.second->highWater;
    };
    auto const oldest = std::ranges::min_element(_byKey, {}, markOf);
    if (oldest != _byKey.end())
        _byKey.erase(oldest);
}

std::size_t FleetNodeHistories::AcceptHistory(std::string_view endpoint,
                                              std::string_view provenKey,
                                              std::span<FleetBucket const> buckets)
{
    // A connection that proved no key is not filed at all. Its series could answer for no machine, and it may be the
    // same machine a proven connection also reports -- filing both would sum it twice into every fleet window it
    // fills. NODE-ANNOUNCE is a proven-only verb, so in production this is a refusal the gate already made.
    if (provenKey.empty())
        return 0;

    // The map's lock covers finding or opening the series, and nothing else: adopting a batch walks three rings up
    // to a hundred and twenty-eight times, and doing that here would put every other machine's announcement behind
    // this one's. The series is shared, so an eviction meanwhile cannot free it.
    std::shared_ptr<Series> series;
    {
        auto const guard = std::scoped_lock { _mutex };
        auto found = _byKey.find(provenKey);
        if (found == _byKey.end())
        {
            EvictForOneMoreLocked();
            found =
                _byKey.emplace(std::string { provenKey }, std::make_shared<Series>(_wall, std::string { endpoint })).first;
        }
        series = found->second;
    }

    // The series' OWN lock across claiming and adding, so neither can be seen without the other: a `Save` copies the
    // mark and the rings under this same lock and so never persists a mark past buckets it does not hold, and two
    // announcements under one key add their buckets in the order they claimed them.
    auto const guard = std::scoped_lock { series->mutex };
    // Where it reports from NOW: a machine that moved keeps its series and changes its label.
    series->endpoint = std::string { endpoint };
    std::size_t taken = 0;
    for (auto const& bucket: buckets)
    {
        // At or below the mark is a replay, not news. Ignored rather than refused:
        // the node is behaving correctly when it resends after a reply it never saw,
        // and the only wrong answer here is to add the reading twice.
        if (!bucket.present || bucket.startMillis <= series->highWater)
            continue;
        series->history.Adopt(bucket);
        series->highWater = bucket.startMillis;
        ++taken;
    }
    return taken;
}

void FleetNodeHistories::BackfillInto(std::vector<FleetBucket>& into, FleetRange range) const
{
    // The steady-state case on a leader that has been up as long as the view is
    // wide: every window is its own, and there is nothing to fill. Asked before a
    // single machine's series is rendered, because rendering them all is the
    // expensive half of this function.
    if (std::ranges::none_of(into, [](FleetBucket const& each) { return !each.present; }))
        return;

    // Shared, so an eviction while this renders cannot free one.
    std::vector<std::shared_ptr<Series const>> histories;
    {
        auto const guard = std::scoped_lock { _mutex };
        histories.reserve(_byKey.size());
        for (auto const& [key, series]: _byKey)
            histories.push_back(series);
    }
    if (histories.empty())
        return;

    // Each machine's own view of the same windows, so the sums line up with the
    // fleet view being filled rather than with whatever resolution happened to be
    // stored. Rendered with the map's lock RELEASED: this is a page being drawn, and
    // a heartbeat must not wait behind it.
    std::vector<std::vector<FleetBucket>> perNode;
    perNode.reserve(histories.size());
    for (auto const& series: histories)
        perNode.push_back(series->history.Buckets(range));

    for (auto const index: std::views::iota(std::size_t { 0 }, into.size()))
    {
        // A window this leader sampled is left alone. Its readings are the registry's
        // own totals -- what the page has always plotted -- and replacing them with a
        // sum assembled from a different set of machines would make the chart step
        // wherever the two disagreed.
        if (into[index].present)
            continue;

        FleetBucket filled = into[index];
        // Every slot starts absent and becomes present only where some machine read it; a slot with no fleet
        // total, or one only a scheduler answers for, never does.
        filled.absent = EveryFleetSlot;
        auto contributors = std::size_t { 0 };
        for (auto const& node: perNode)
        {
            if (index >= node.size() || !node[index].present)
                continue;
            ++contributors;
            filled.sampleMillis = std::max(filled.sampleMillis, node[index].sampleMillis);
            filled.coverage = std::max(filled.coverage, node[index].coverage);
            for (auto const& row: FleetMetricTable)
            {
                // NODE-scoped only. A machine cannot answer for a dispatch outcome,
                // so summing one would be summing zeroes into a number a reader would
                // take for a measurement. And only a slot whose sum MEANS something:
                // a load ratio summed over machines is nobody's load.
                if (row.scope != FleetMetricScope::Node || row.fleetTotal != FleetTotal::Summed)
                    continue;
                // A machine that could not read a slot contributes nothing to it, rather than a zero.
                if (IsAbsent(node[index], row.metric))
                    continue;
                auto const slot = static_cast<std::size_t>(row.metric);
                filled.absent &= ~SlotBit(row.metric);
                filled.values[slot] += node[index].values[slot];
                filled.fold[slot].low += node[index].fold[slot].low;
                filled.fold[slot].high += node[index].fold[slot].high;
                filled.fold[slot].total += node[index].fold[slot].total;
            }
        }

        if (contributors == 0)
            continue;
        filled.present = true;
        filled.backfilled = true;
        into[index] = filled;
    }
}

bool FleetNodeHistories::Save(std::filesystem::path const& path, StateFile which) const
{
    // The same refusal `FleetHistory::Save` makes, and for the same reason: a build
    // that cannot read this file must not replace it with one it can.
    std::vector<std::pair<std::string, std::shared_ptr<Series const>>> listed;
    {
        auto const guard = std::scoped_lock { _mutex };
        if (_readOnly)
            return false;
        listed.reserve(_byKey.size());
        for (auto const& [key, series]: _byKey)
            listed.emplace_back(key, series);
    }

    // Composed with the map's lock RELEASED. One machine's series is most of a
    // megabyte, so serializing a fleet of them under the lock every announcement has to
    // take would put each inbound `Accept` behind all of it.
    auto file = FramedBuffer();
    AppendU64(file, listed.size());
    for (auto const& [key, series]: listed)
    {
        // Under the series' own lock, which `AcceptHistory` holds across claiming and adding: the mark written is
        // never one past buckets the body does not hold.
        auto const guard = std::scoped_lock { series->mutex };
        // `[endpoint][high-water][key]`: the endpoint first, where versions 1 and 2 had it, then the key the series
        // is filed under.
        AppendU64(file, series->endpoint.size());
        file += series->endpoint;
        AppendU64(file, static_cast<std::uint64_t>(series->highWater));
        AppendU64(file, key.size());
        file += key;
        // The nested body, written by the very code that reads it back. A second copy
        // of the ring encoding here would drift from that one the first time a bucket
        // grew a field -- and would drift silently, because both halves would still
        // round-trip against themselves.
        //
        // Its length is stamped afterwards rather than composed apart and joined,
        // because joining copies the whole of it a second time.
        auto const lengthAt = file.size();
        AppendU64(file, 0);
        auto const bodyAt = file.size();
        series->history.AppendBody(file);
        WriteU64(file, lengthAt, file.size() - bodyAt);
    }
    return WriteFramed(path, NodeStoreFile, file, which);
}

bool FleetNodeHistories::Load(std::filesystem::path const& path)
{
    auto const opened = ReadFramed(path, NodeStoreFile);
    if (opened.outcome == FramedOutcome::Newer)
    {
        auto const guard = std::scoped_lock { _mutex };
        _readOnly = true;
        return false;
    }
    if (opened.outcome != FramedOutcome::Ok)
        return false;

    BodyReader reader { opened.body };
    std::uint64_t count = 0;
    if (!reader.Take(count))
        return false;
    // A count no body could hold is refused before anything is allocated against it,
    // the same guard `ReadRing` applies to a bucket count.
    if (count > opened.body.size())
        return false;

    std::map<std::string, std::shared_ptr<Series>, std::less<>> byKey;
    std::size_t dropped = 0;
    for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, count))
    {
        std::uint64_t length = 0;
        std::string_view endpoint;
        std::uint64_t highWater = 0;
        if (!reader.Take(length) || !reader.Take(length, endpoint) || !reader.Take(highWater))
            return false;

        // Keyed stores only: an older one recorded no key at all.
        std::uint64_t keyLength = 0;
        std::string_view provenKey;
        if (opened.version >= NodeStoreKeyedSince && (!reader.Take(keyLength) || !reader.Take(keyLength, provenKey)))
            return false;

        std::uint64_t nestedSize = 0;
        std::string_view nested;
        if (!reader.Take(nestedSize) || !reader.Take(nestedSize, nested))
            return false;

        // A series filed under no key is DROPPED, and counted so the start can say so: it answers for no machine, and
        // the machine it came from files its readings under its key once it reports again -- the same minutes in two
        // series, summed twice into every fleet window either fills. The leader's own fleet series is untouched;
        // only handed-over copies go. Its body is still parsed first, so a damaged one refuses the file as before.
        auto series = std::make_shared<Series>(_wall, std::string { endpoint });
        if (!series->history.ReadBody(nested, opened.version))
            return false;
        if (provenKey.empty())
        {
            ++dropped;
            continue;
        }
        series->highWater = static_cast<std::int64_t>(highWater);
        // A file naming one machine twice is not one this build wrote: refused whole, like any other damage.
        if (!byKey.emplace(provenKey, std::move(series)).second)
            return false;
    }

    auto const guard = std::scoped_lock { _mutex };
    _byKey = std::move(byKey);
    _droppedOnLoad = dropped;
    // A store written under a larger ceiling keeps the series that reported most recently.
    while (_byKey.size() > MaxReceivedHistories)
    {
        auto const before = _byKey.size();
        EvictForOneMoreLocked();
        if (_byKey.size() == before)
            break;
    }
    return !_byKey.empty();
}

bool FleetNodeHistories::ReadOnly() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _readOnly;
}

std::size_t FleetNodeHistories::DroppedOnLoad() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _droppedOnLoad;
}

std::optional<std::vector<FleetBucket>> FleetNodeHistories::MachineBuckets(std::string_view provenKey,
                                                                           FleetRange range) const
{
    std::shared_ptr<Series const> series;
    {
        auto const guard = std::scoped_lock { _mutex };
        auto const found = provenKey.empty() ? _byKey.end() : _byKey.find(provenKey);
        if (found == _byKey.end())
            return std::nullopt;
        series = found->second;
    }

    // Rendered with the map's lock released, as `BackfillInto` renders; the history guards itself.
    auto buckets = series->history.Buckets(range);
    for (auto& bucket: buckets)
        bucket.absent |= FleetScopedSlots();
    return buckets;
}

std::size_t FleetNodeHistories::Count() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _byKey.size();
}

std::int64_t FleetNodeHistories::HighWaterFor(std::string_view endpoint) const
{
    auto const guard = std::scoped_lock { _mutex };
    auto newest = std::int64_t { -1 };
    for (auto const& [key, series]: _byKey)
    {
        auto const seriesGuard = std::scoped_lock { series->mutex };
        if (series->endpoint == endpoint)
            newest = std::max(newest, series->highWater);
    }
    return newest;
}

} // namespace Pace::Distributed
