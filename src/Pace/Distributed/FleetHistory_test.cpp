// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/ByteAppender.hpp>
#include <Pace/Core/Crc32c.hpp>
#include <Pace/Distributed/FleetChart.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/SchedulerProtocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include <tests/FleetHistoryFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Distributed;
using Pace::Testing::PlacedWallClock;

namespace
{

/// One reading with every slot set, so a case varies only what it is about.
[[nodiscard]] EnumTable<FleetMetric, std::uint64_t> Reading(std::uint64_t granted, std::uint64_t inFlight = 0)
{
    EnumTable<FleetMetric, std::uint64_t> values {};
    values[static_cast<std::size_t>(FleetMetric::DispatchGranted)] = granted;
    values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] = inFlight;
    return values;
}

[[nodiscard]] std::uint64_t GrantedOf(FleetBucket const& bucket)
{
    return bucket.values[static_cast<std::size_t>(FleetMetric::DispatchGranted)];
}

[[nodiscard]] FleetFold const& FoldOf(FleetBucket const& bucket, FleetMetric metric)
{
    return bucket.fold[static_cast<std::size_t>(metric)];
}

/// Big-endian, matching the file format, so a hand-built fixture is byte-exact.
///
/// Through `ByteAppender` rather than a shift loop of its own (#305): a hand-rolled
/// width conversion in a test is a second implementation of the convention under test.
/// What stays hand-built is the LAYOUT below -- which is the whole point of the v1
/// fixture, since a file produced by this build would be v2 and would exercise nothing.
void AppendU64(std::string& out, std::uint64_t value)
{
    ByteAppender { out }.AppendU64(value);
}

/// A history file in the ORIGINAL v1 layout, written by hand.
///
/// By hand and not by an older binary, because that is the only way this stays a
/// test of the v1 reader: a fixture produced by the current build would be v2 and
/// would exercise nothing. v1 was a generation, then two rings -- minute of 1440 and
/// hour of 168 -- each a count followed by `[start][present][9 values]` per bucket,
/// with no coverage and no fold.
///
/// @param generation What the file claims.
/// @param minuteStart Bucket start of the one present minute bucket.
/// @param minuteGranted Its `DispatchGranted` reading.
/// @param hourStart Bucket start of the one present hour bucket.
/// @param hourGranted Its `DispatchGranted` reading.
/// @return The complete file, header included.
[[nodiscard]] std::string VersionOneFile(std::uint64_t generation,
                                         std::int64_t minuteStart,
                                         std::uint64_t minuteGranted,
                                         std::int64_t hourStart,
                                         std::uint64_t hourGranted)
{
    // NOT `EnumeratorCount<FleetMetric>`: that is THIS build's width, and v1 wrote
    // nine values forever, whatever this build's enum has grown to since.
    constexpr std::size_t Slots = 9;
    constexpr std::size_t V1MinuteSlots = 24 * 60;
    constexpr std::size_t V1HourSlots = 7 * 24;
    constexpr std::int64_t MinuteMillis = 60 * 1000;
    constexpr std::int64_t HourMillis = 60 * MinuteMillis;

    std::string body;
    AppendU64(body, generation);

    auto const ring = [&](std::size_t slots, std::int64_t widthMillis, std::int64_t presentStart, std::uint64_t granted) {
        AppendU64(body, slots);
        auto const presentIndex = static_cast<std::size_t>(presentStart / widthMillis) % slots;
        for (auto const index: std::views::iota(std::size_t { 0 }, slots))
        {
            auto const here = index == presentIndex;
            AppendU64(body, here ? static_cast<std::uint64_t>(presentStart) : 0U);
            AppendU64(body, here ? 1U : 0U);
            for (auto const slot: std::views::iota(std::size_t { 0 }, Slots))
                AppendU64(body, here && slot == static_cast<std::size_t>(FleetMetric::DispatchGranted) ? granted : 0U);
        }
    };
    ring(V1MinuteSlots, MinuteMillis, minuteStart, minuteGranted);
    ring(V1HourSlots, HourMillis, hourStart, hourGranted);

    std::string file;
    file += "FCFH";
    file.push_back(static_cast<char>(1));
    AppendU64(file, body.size());
    AppendU64(file, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;
    return file;
}

/// The body a v2 `FleetHistory` writes -- generation, then the minute/hour/day
/// rings, at `LegacyValueWidth`. Shared between `VersionTwoFile`, which frames it as
/// a standalone `FCFH` file, and a hand-built `FCNH` nested body, which is exactly
/// this with no framing of its own.
/// @param generation What the file claims.
/// @param minuteStart Bucket start of the one present minute bucket.
/// @param minuteGranted Its `DispatchGranted` reading (Fleet-scoped: readable back
///        only by loading the ring directly, never through `BackfillInto`).
/// @param minuteJobsInFlight Its `JobsInFlight` reading (Node-scoped, at the LAST of
///        the nine legacy slots -- the one a width bug would misplace first).
/// @return The body, unframed.
[[nodiscard]] std::string VersionTwoBody(std::uint64_t generation,
                                         std::int64_t minuteStart,
                                         std::uint64_t minuteGranted,
                                         std::uint64_t minuteJobsInFlight = 0)
{
    constexpr std::size_t LegacyValueWidth = 9;

    std::string body;
    AppendU64(body, generation);

    AppendU64(body, 1U); // the minute ring: one bucket
    AppendU64(body, static_cast<std::uint64_t>(minuteStart));
    AppendU64(body, static_cast<std::uint64_t>(minuteStart)); // sample == start
    AppendU64(body, 1U);                                      // present
    AppendU64(body, 1U);                                      // coverage
    for (auto const slot: std::views::iota(std::size_t { 0 }, LegacyValueWidth))
    {
        auto value = std::uint64_t { 0 };
        if (slot == static_cast<std::size_t>(FleetMetric::DispatchGranted))
            value = minuteGranted;
        else if (slot == static_cast<std::size_t>(FleetMetric::JobsInFlight))
            value = minuteJobsInFlight;
        AppendU64(body, value);
    }
    for ([[maybe_unused]] auto const slot: std::views::iota(std::size_t { 0 }, LegacyValueWidth))
    {
        AppendU64(body, 0U); // low
        AppendU64(body, 0U); // high
        AppendU64(body, 0U); // total
    }
    AppendU64(body, 0U); // the hour ring: empty
    AppendU64(body, 0U); // the day ring: empty
    return body;
}

/// A history file in the v2 layout, written by hand -- frozen forever at the nine
/// values and nine folds per bucket every version 1 and 2 file was ever written with.
///
/// By hand and not by an older binary, for the same reason `VersionOneFile` is: a
/// fixture produced by the current build is v3 and exercises nothing. This is what
/// proves `ReadVersion2` still reads a real v2 file once `FleetMetric` has grown a
/// slot past the width this format froze at.
/// @param generation What the file claims.
/// @param minuteStart Bucket start of the one present minute bucket.
/// @param minuteGranted Its `DispatchGranted` reading.
/// @return The complete file, header included.
[[nodiscard]] std::string VersionTwoFile(std::uint64_t generation, std::int64_t minuteStart, std::uint64_t minuteGranted)
{
    auto const body = VersionTwoBody(generation, minuteStart, minuteGranted);

    std::string file;
    file += "FCFH";
    file.push_back(static_cast<char>(2));
    AppendU64(file, body.size());
    AppendU64(file, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;
    return file;
}

/// A `NodeStoreFile` in the v1 layout, written by hand -- one machine, whose nested
/// body carries no envelope of its own and is exactly `VersionTwoBody`'s shape,
/// because that is the shape every v1 `NodeStoreFile` was ever written with.
///
/// By hand for the same reason the other two hand-built fixtures are: a file this
/// build produces is v2 and would exercise nothing. This is what proves the OUTER
/// envelope's version, not a per-nested-body one, is what selects `ReadVersion2` for
/// a real v1 file once `FleetMetric` has grown past the width it was written with.
/// @param endpoint The one machine's key in the store.
/// @param highWater Its recorded high-water mark.
/// @param generation What its nested history claims.
/// @param minuteStart Bucket start of the one present minute bucket.
/// @param minuteGranted Its `DispatchGranted` reading.
/// @param minuteJobsInFlight Its `JobsInFlight` reading -- Node-scoped, so it is the
///        one `BackfillInto` will actually sum back out, unlike the dispatch counter.
/// @return The complete file, header included.
[[nodiscard]] std::string VersionOneNodeStoreFile(std::string_view endpoint,
                                                  std::uint64_t highWater,
                                                  std::uint64_t generation,
                                                  std::int64_t minuteStart,
                                                  std::uint64_t minuteGranted,
                                                  std::uint64_t minuteJobsInFlight)
{
    auto const nested = VersionTwoBody(generation, minuteStart, minuteGranted, minuteJobsInFlight);

    std::string body;
    AppendU64(body, 1U); // one machine
    AppendU64(body, endpoint.size());
    body += endpoint;
    AppendU64(body, highWater);
    AppendU64(body, nested.size());
    body += nested;

    std::string file;
    file += "FCNH";
    file.push_back(static_cast<char>(1));
    AppendU64(file, body.size());
    AppendU64(file, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;
    return file;
}

/// Write @p bytes to @p path.
void WriteFile(std::filesystem::path const& path, std::string_view bytes)
{
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Read @p path back whole.
[[nodiscard]] std::string ReadFile(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("FleetMetric's ordinals are pinned, not just documented", "[distributed][fleethistory]")
{
    // The header's own static_assert catches a renumbering that keeps every
    // enumerator's declaration in the same relative order; this is the anchor beside
    // it that would still catch a rewrite creative enough to dodge that -- the same
    // reason a wire constant is pinned by value as well as by name. `DispatchAllExcluded`
    // is the one enumerator this rule has actually had to protect so far: it landed
    // at the END, at 9, rather than beside `DispatchWithdrawn` where `LeaseOutcomeTable`
    // carries it.
    CHECK(static_cast<std::uint8_t>(FleetMetric::DispatchGranted) == 0);
    CHECK(static_cast<std::uint8_t>(FleetMetric::DispatchNoWorker) == 1);
    CHECK(static_cast<std::uint8_t>(FleetMetric::DispatchNoCapacity) == 2);
    CHECK(static_cast<std::uint8_t>(FleetMetric::DispatchWithdrawn) == 3);
    CHECK(static_cast<std::uint8_t>(FleetMetric::DispatchDuplicate) == 4);
    CHECK(static_cast<std::uint8_t>(FleetMetric::CacheHits) == 5);
    CHECK(static_cast<std::uint8_t>(FleetMetric::CacheMisses) == 6);
    CHECK(static_cast<std::uint8_t>(FleetMetric::OfferableSlots) == 7);
    CHECK(static_cast<std::uint8_t>(FleetMetric::JobsInFlight) == 8);
    CHECK(static_cast<std::uint8_t>(FleetMetric::DispatchAllExcluded) == 9);
}

TEST_CASE("A fresh history is empty and every bucket is a gap", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    CHECK(history.Empty());
    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE(day.size() == 288);
    // Absent, not zero. A fleet that did nothing and a fleet nobody was watching
    // are different facts, and only one of them is true here.
    CHECK(std::ranges::none_of(day, [](auto const& b) { return b.present; }));
}

TEST_CASE("A sample lands in the bucket its instant belongs to", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(10));
    CHECK_FALSE(history.Empty());

    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE(!day.empty());
    // The newest point is the one just written; everything before it is a gap.
    CHECK(day.back().present);
    CHECK(GrantedOf(day.back()) == 10);
    CHECK_FALSE(day.front().present);
}

TEST_CASE("Sampling twice inside one bucket overwrites rather than accumulating", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(10));
    auto const first = history.Generation();
    clock.Advance(std::chrono::seconds { 5 });
    history.Record(Reading(14));

    auto const day = history.Buckets(FleetRange::Day);
    // Counters are stored raw and cumulative, so the newest reading in a bucket is
    // the right one -- a jittery timer must not be able to double-count.
    CHECK(GrantedOf(day.back()) == 14);
    CHECK(history.Generation() == first);
}

TEST_CASE("A closed bucket advances the generation", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(1));
    auto const before = history.Generation();
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(2));

    // The whole of an ETag: it moves exactly when a rendered chart would.
    CHECK(history.Generation() > before);
}

TEST_CASE("Downtime leaves a gap rather than a flat line", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(5));
    clock.Advance(std::chrono::hours { 3 });
    history.Record(Reading(9));

    auto const day = history.Buckets(FleetRange::Day);
    auto const present = std::ranges::count_if(day, [](auto const& b) { return b.present; });
    // Two readings three hours apart are two points, not a line drawn across the
    // hours nobody observed.
    CHECK(present == 2);
    CHECK(day.back().present);
}

TEST_CASE("The week view folds to hourly buckets", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(1));
    clock.Advance(std::chrono::minutes { 30 });
    history.Record(Reading(7));

    auto const week = history.Buckets(FleetRange::Week);
    REQUIRE(week.size() == 168);
    auto const present = std::ranges::count_if(week, [](auto const& b) { return b.present; });
    // Both samples fall inside one hour, so the week view shows one point carrying
    // the later reading.
    CHECK(present == 1);
    CHECK(GrantedOf(week.back()) == 7);
}

TEST_CASE("A range is looked up by what the URL said, and an unknown one is refused", "[distributed][fleethistory]")
{
    CHECK(FleetRangeFromKey("24h") == FleetRange::Day);
    CHECK(FleetRangeFromKey("7d") == FleetRange::Week);
    // Never a silent default: a typo that quietly served a different range would
    // put a reader on the wrong axis without saying so.
    CHECK_FALSE(FleetRangeFromKey("30d").has_value());
    CHECK_FALSE(FleetRangeFromKey("").has_value());
}

TEST_CASE("A history file's words are big-endian, and not only self-consistent", "[distributed][fleethistory]")
{
    // **The assertion that distinguishes.** Every other case here saves with `Save` and
    // reads back with `Load`, which agrees with whatever byte order the two halves
    // share -- so it passes under the `HostToBigEndian` + `memcpy` this writer used to
    // carry and under `ByteAppender` alike (#305), and it would go on passing if both
    // halves became little-endian together. This reads the FILE.
    //
    // The envelope is fixed and documented in `FleetHistory.cpp`: four magic bytes, a
    // version byte, then the body length and the body's CRC32C, both `u64`. The body
    // then opens with the history's generation. Two present buckets means a generation
    // of two, which is a number small enough that a byte-swapped word is unmistakable:
    // big-endian puts it in the LAST byte of its eight.
    Testing::ScratchDirectory const scratch { "fleet-history-byte-order" };
    auto const file = scratch.Path() / "history.bin";

    PlacedWallClock clock;
    FleetHistory history { clock };
    history.Record(Reading(11));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(23));
    REQUIRE(history.Generation() == 2);
    REQUIRE(history.Save(file, StateFile::FleetHistory));

    auto const raw = ReadFile(file);
    REQUIRE(raw.size() > 21);
    CHECK(raw.substr(0, 4) == "FCFH");
    CHECK(raw[4] == char { 4 }); // the version this build writes

    // The body length, big-endian, and it really is the body's length.
    std::string expectedLength;
    AppendU64(expectedLength, raw.size() - 21);
    CHECK(raw.substr(5, 8) == expectedLength);

    // And the first word of the body is the generation, big-endian.
    std::string expectedGeneration;
    AppendU64(expectedGeneration, 2);
    CHECK(raw.substr(21, 8) == expectedGeneration);
    CHECK(expectedGeneration == std::string(7, '\0') + std::string(1, char { 2 }));
}

TEST_CASE("History survives a save and reload", "[distributed][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-history-roundtrip" };
    auto const file = scratch.Path() / "history.bin";

    PlacedWallClock clock;
    std::uint64_t generation = 0;
    {
        FleetHistory history { clock };
        history.Record(Reading(11));
        clock.Advance(std::chrono::minutes { 1 });
        history.Record(Reading(23));
        generation = history.Generation();
        REQUIRE(history.Save(file, StateFile::FleetHistory));
    }

    FleetHistory restored { clock };
    REQUIRE(restored.Load(file));
    CHECK_FALSE(restored.Empty());
    CHECK(GrantedOf(restored.Buckets(FleetRange::Day).back()) == 23);
    // Restored rather than reset: a generation starting again from zero would let a
    // client's cached chart match an ETag it had already been served.
    CHECK(restored.Generation() == generation);
}

TEST_CASE("A history file that cannot be trusted starts empty rather than throwing", "[distributed][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-history-damaged" };
    PlacedWallClock clock;

    SECTION("absent")
    {
        FleetHistory history { clock };
        CHECK_FALSE(history.Load(scratch.Path() / "nothing-here.bin"));
        CHECK(history.Empty());
    }

    SECTION("truncated")
    {
        auto const file = scratch.Path() / "short.bin";
        {
            std::ofstream out { file, std::ios::binary };
            out << "FCFH";
        }
        FleetHistory history { clock };
        CHECK_FALSE(history.Load(file));
        CHECK(history.Empty());
    }

    SECTION("corrupt body")
    {
        auto const file = scratch.Path() / "corrupt.bin";
        {
            FleetHistory good { clock };
            good.Record(Reading(3));
            REQUIRE(good.Save(file, StateFile::FleetHistory));
        }
        // Flip a byte well past the header, so the checksum is what catches it.
        {
            std::fstream patch { file, std::ios::binary | std::ios::in | std::ios::out };
            patch.seekp(64);
            patch.put('\xFF');
        }
        FleetHistory history { clock };
        CHECK_FALSE(history.Load(file));
        // The node still starts. History is a convenience, and no state of this file
        // may keep one from coming up.
        CHECK(history.Empty());
    }
}

TEST_CASE("A bucket remembers when it was sampled, not when its window opened", "[distributed][fleethistory]")
{
    // The two are the same thing for a reading that lands on a boundary and
    // different for every other one, and only the second is what a rate may be
    // divided by. Stamping the alignment at RECORD time throws the instant away
    // where no later fold can recover it -- which made the Week view, whose bucket
    // is its own sub-bucket, always report exactly one nominal width between two
    // adjacent points however far apart they were really taken.
    PlacedWallClock clock;
    FleetHistory history { clock };

    // Two readings in consecutive hours, but only ten minutes apart: the first at
    // the end of its hour, the second at the start of the next.
    clock.Set(std::chrono::sys_days { std::chrono::January / 1 / 2030 } + std::chrono::hours { 5 }
              + std::chrono::minutes { 55 });
    history.Record(Reading(100));
    clock.Set(std::chrono::sys_days { std::chrono::January / 1 / 2030 } + std::chrono::hours { 6 }
              + std::chrono::minutes { 5 });
    history.Record(Reading(130));

    auto const week = history.Buckets(FleetRange::Week);
    REQUIRE(week.size() >= 2);
    auto const& last = week.back();
    auto const& prior = week[week.size() - 2];
    REQUIRE(last.present);
    REQUIRE(prior.present);

    // Drawn one hour apart, as the chart needs.
    CHECK(last.startMillis - prior.startMillis == 3'600'000);
    // Taken ten minutes apart, as a rate needs.
    CHECK(last.sampleMillis - prior.sampleMillis == 600'000);
}

TEST_CASE("Every range folds from a ring that actually holds it", "[distributed][fleethistory]")
{
    // Eight views over three rings, and the geometry is checked at compile time by
    // `RangesFitTheirRings`. Asserted here as well because that check is a
    // `consteval` a reader has to go looking for, and because this is the thing the
    // whole storage split exists to guarantee: a view reaching past its ring's lap
    // would render a gap that reads as "nobody was watching".
    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        auto const& ring = FleetRingTable[static_cast<std::size_t>(row.ring)];
        CHECK(row.bucket >= ring.bucket);
        CHECK(row.bucket.count() % ring.bucket.count() == 0);
        CHECK(row.bucket * static_cast<std::int64_t>(row.points) <= ring.bucket * static_cast<std::int64_t>(ring.slots));
    }

    PlacedWallClock clock;
    FleetHistory history { clock };
    history.Record(Reading(1));
    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        CHECK(history.Buckets(row.range).size() == row.points);
    }
}

TEST_CASE("A spike inside a coarse bucket survives the fold", "[distributed][fleethistory]")
{
    // The reason a bucket carries a fold at all. Five minutes of nothing, then one
    // minute in which sixty compiles were granted, then five more of nothing: the
    // Day view folds all of that into one 5-minute point, and a bucket that kept
    // only its last reading would show the average and hide the burst entirely.
    PlacedWallClock clock;
    FleetHistory history { clock };

    std::uint64_t granted = 0;
    for (auto const minute: std::views::iota(0, 5))
    {
        granted += (minute == 3) ? 60 : 1;
        history.Record(Reading(granted, minute == 3 ? 40 : 2));
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.Buckets(FleetRange::Day);
    auto const busy = std::ranges::find_if(day, [](FleetBucket const& b) { return b.present && b.coverage > 1; });
    REQUIRE(busy != day.end());

    // A counter's peak is the largest per-SAMPLE delta folded in, which is the one
    // number an average over the window destroys.
    CHECK(FoldOf(*busy, FleetMetric::DispatchGranted).high == 60);
    // A gauge keeps both ends: the floor is when the fleet had nothing left, and
    // that is at least as interesting as the ceiling.
    CHECK(FoldOf(*busy, FleetMetric::JobsInFlight).high == 40);
    CHECK(FoldOf(*busy, FleetMetric::JobsInFlight).low == 2);
}

TEST_CASE("Coverage counts the samples a bucket actually folded", "[distributed][fleethistory]")
{
    // The third state. `present` separates "nobody sampled" from "somebody did";
    // this separates a window a node contributed one minute to from one it
    // contributed five to, which are both present and must not be read alike.
    PlacedWallClock clock;
    FleetHistory history { clock };

    for ([[maybe_unused]] auto const minute: std::views::iota(0, 3))
    {
        history.Record(Reading(1));
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.Buckets(FleetRange::Day);
    auto const folded = std::ranges::find_if(day, [](FleetBucket const& b) { return b.present; });
    REQUIRE(folded != day.end());
    CHECK(folded->coverage == 3);

    // In the base ring a bucket IS one sample, so coverage there is always one --
    // which is what makes it a meaningful divisor rather than a tautology.
    auto const hour = history.Buckets(FleetRange::OneHour);
    for (auto const& bucket: hour)
        if (bucket.present)
            CHECK(bucket.coverage == 1);
}

TEST_CASE("A window sampled throughout reports exactly its full coverage", "[distributed][fleethistory]")
{
    // `FullCoverageOf` is what gives `coverage` a meaning, so it has to agree with
    // what recording actually produces rather than being arithmetic nobody checks.
    // One sample a minute for a whole 5-minute window: the Day point folding it must
    // read exactly the number that function names.
    PlacedWallClock clock;
    FleetHistory history { clock };

    REQUIRE(FullCoverageOf(FleetRange::Day) == 5);
    for ([[maybe_unused]] auto const minute: std::views::iota(std::uint64_t { 0 }, FullCoverageOf(FleetRange::Day)))
    {
        history.Record(Reading(1));
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.Buckets(FleetRange::Day);
    auto const full = std::ranges::find_if(day, [](FleetBucket const& b) { return b.present; });
    REQUIRE(full != day.end());
    CHECK(full->coverage == FullCoverageOf(FleetRange::Day));

    // And it is the window's own width over the base interval for every view, which
    // is why it is keyed on a range rather than on the ring underneath it.
    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        CHECK(FullCoverageOf(row.range) == static_cast<std::uint64_t>(row.bucket.count() / FleetSampleInterval.count()));
    }
}

TEST_CASE("A version 1 history is inflated forward rather than discarded", "[distributed][fleethistory]")
{
    // The load-bearing one. A format is convertible exactly as long as its reader is
    // in the table; bumping the version without adding a row is the decision to
    // throw away every history on every machine, and this is what makes that a
    // decision rather than an oversight.
    Testing::ScratchDirectory const scratch { "fleet-history-v1" };
    auto const file = scratch.Path() / "v1.bin";

    PlacedWallClock clock;
    auto const nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
    auto const minuteStart = (nowMillis / 60'000) * 60'000;
    auto const hourStart = (nowMillis / 3'600'000) * 3'600'000;

    WriteFile(file, VersionOneFile(77, minuteStart, 11, hourStart, 23));

    FleetHistory history { clock };
    REQUIRE(history.Load(file));
    CHECK_FALSE(history.Empty());
    CHECK(history.Generation() == 77);

    // The readings are there, in both rings -- and the hour ring grew from 168 slots
    // to 720 between the versions, so a bucket copied across by INDEX rather than
    // re-placed by its own start would have landed in the wrong slot and then read
    // as a previous lap.
    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE_FALSE(day.empty());
    CHECK(GrantedOf(day.back()) == 11);

    auto const week = history.Buckets(FleetRange::Week);
    REQUIRE_FALSE(week.empty());
    CHECK(week.back().present);
    CHECK(GrantedOf(week.back()) == 23);

    // v1 stored one reading per bucket and no fold, so coverage is one -- what a v1
    // bucket literally held. A counter's peak is unknowable from it and stays absent
    // rather than being invented.
    CHECK(week.back().coverage == 1);
    CHECK(FoldOf(week.back(), FleetMetric::DispatchGranted).high == 0);

    // And it saves forward, in the current format.
    auto const upgraded = scratch.Path() / "v3.bin";
    REQUIRE(history.Save(upgraded, StateFile::FleetHistory));
    FleetHistory again { clock };
    REQUIRE(again.Load(upgraded));
    CHECK(GrantedOf(again.Buckets(FleetRange::Day).back()) == 11);
}

TEST_CASE("A version 2 history reads back at the width it was written with", "[distributed][fleethistory]")
{
    // The regression `FleetMetric` growing a slot could have caused: if `ReadVersion2`
    // derived its width from the live enum instead of staying pinned at nine, a real
    // v2 file would misalign every field past the first bucket once this build's
    // count moved -- or worse, read the wrong bytes into a plausible-looking number
    // rather than failing outright.
    Testing::ScratchDirectory const scratch { "fleet-history-v2" };
    auto const file = scratch.Path() / "v2.bin";

    PlacedWallClock clock;
    auto const nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
    auto const minuteStart = (nowMillis / 60'000) * 60'000;

    WriteFile(file, VersionTwoFile(42, minuteStart, 17));

    FleetHistory history { clock };
    REQUIRE(history.Load(file));
    CHECK(history.Generation() == 42);

    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE_FALSE(day.empty());
    CHECK(GrantedOf(day.back()) == 17);

    // The slot no v2 file ever carried reads back at zero -- not garbage, and not a
    // neighbour's reading shifted into it.
    CHECK(day.back().values[static_cast<std::size_t>(FleetMetric::DispatchAllExcluded)] == 0);

    // And it saves forward, in the current format.
    auto const upgraded = scratch.Path() / "v3.bin";
    REQUIRE(history.Save(upgraded, StateFile::FleetHistory));
    FleetHistory again { clock };
    REQUIRE(again.Load(upgraded));
    CHECK(GrantedOf(again.Buckets(FleetRange::Day).back()) == 17);
}

TEST_CASE("A history newer than this build is kept, and never written over", "[distributed][fleethistory]")
{
    // The one irreversible thing here, and the reason this phase exists. A node
    // rolled back to an older build reads a file it cannot understand; if it then
    // started empty and saved, it would destroy a year of readings that the build it
    // was rolled back FROM could still have read. Everything else about this file is
    // recoverable by waiting.
    Testing::ScratchDirectory const scratch { "fleet-history-newer" };
    auto const file = scratch.Path() / "newer.bin";

    PlacedWallClock clock;

    // A real, current file with its version byte raised to something no reader
    // claims. The rest stays valid, so nothing but the version can be what refuses.
    {
        FleetHistory writer { clock };
        writer.Record(Reading(5));
        REQUIRE(writer.Save(file, StateFile::FleetHistory));
    }
    auto bytes = ReadFile(file);
    REQUIRE(bytes.size() > 4);
    bytes[4] = static_cast<char>(200);
    WriteFile(file, bytes);
    auto const before = ReadFile(file);

    FleetHistory history { clock };
    CHECK_FALSE(history.Load(file));
    CHECK(history.Empty());
    CHECK(history.ReadOnly());

    // Sampling continues -- the page is live either way, and only persistence stops.
    history.Record(Reading(9));
    CHECK_FALSE(history.Empty());

    CHECK_FALSE(history.Save(file, StateFile::FleetHistory));
    CHECK(ReadFile(file) == before);
}

TEST_CASE("A history older than every reader is not treated as newer", "[distributed][fleethistory]")
{
    // Removing a reader row is a deliberate decision to stop carrying that format,
    // and a build that made it must be free to write. Only a version ABOVE the
    // newest this build knows can have been written by a later one.
    Testing::ScratchDirectory const scratch { "fleet-history-ancient" };
    auto const file = scratch.Path() / "ancient.bin";

    PlacedWallClock clock;
    {
        FleetHistory writer { clock };
        writer.Record(Reading(5));
        REQUIRE(writer.Save(file, StateFile::FleetHistory));
    }
    auto bytes = ReadFile(file);
    bytes[4] = static_cast<char>(0);
    WriteFile(file, bytes);

    FleetHistory history { clock };
    CHECK_FALSE(history.Load(file));
    CHECK_FALSE(history.ReadOnly());
    CHECK(history.Save(file, StateFile::FleetHistory));
}

TEST_CASE("A gap in sampling is not folded in as a peak", "[distributed][fleethistory]")
{
    // A node samples only while it leads, so a leadership change of three hours ends
    // with one sample whose delta is three hours of work. Folded in as a per-minute
    // peak that is a spike which never happened -- and it would be PERSISTED, in the
    // day ring, for over a year.
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(100));
    clock.Advance(std::chrono::hours { 3 });
    history.Record(Reading(10'000));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(10'007));

    auto const week = history.Buckets(FleetRange::Week);
    auto const worst = std::ranges::max(week | std::views::transform([](FleetBucket const& b) {
                                            return b.fold[static_cast<std::size_t>(FleetMetric::DispatchGranted)].high;
                                        }));
    // Seven, from the one adjacent pair. Emphatically not 9,900.
    CHECK(worst == 7);
}

TEST_CASE("Adjacent samples still produce a peak after the gap rule", "[distributed][fleethistory]")
{
    // The other half: a guard that refused every delta would be just as wrong, and a
    // rule that only ever suppresses is indistinguishable from one that never fires.
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(0));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(42));

    auto const day = history.Buckets(FleetRange::Day);
    auto const worst = std::ranges::max(day | std::views::transform([](FleetBucket const& b) {
                                            return b.fold[static_cast<std::size_t>(FleetMetric::DispatchGranted)].high;
                                        }));
    CHECK(worst == 42);
}

TEST_CASE("A restart keeps when a bucket was sampled, not when its window opened", "[distributed][fleethistory]")
{
    // `sampleMillis` used to be derived from the stored bucket's own start, which was
    // true while the coarsest ring was the hour a Week view read one-for-one. A ring
    // that folds sixty readings has no way to recover when the last of them landed,
    // so a partly-sampled bucket restored that way has its delta divided by a nominal
    // width -- the exact error this field exists to prevent.
    Testing::ScratchDirectory const scratch { "fleet-history-sample-instant" };
    auto const file = scratch.Path() / "history.bin";

    PlacedWallClock clock;
    // Sampled near the END of an hour, so the instant and the window's start are far
    // apart and a derived value cannot accidentally match.
    clock.Advance(std::chrono::minutes { 58 });

    std::int64_t sampled = 0;
    {
        FleetHistory history { clock };
        history.Record(Reading(7));
        sampled = history.Buckets(FleetRange::Week).back().sampleMillis;
        REQUIRE(history.Save(file, StateFile::FleetHistory));
    }

    FleetHistory restored { clock };
    REQUIRE(restored.Load(file));
    auto const week = restored.Buckets(FleetRange::Week);
    REQUIRE_FALSE(week.empty());
    CHECK(week.back().sampleMillis == sampled);
    // And it is genuinely not the window's start, or this case would pass on a bug.
    CHECK(week.back().sampleMillis != week.back().startMillis);
}

TEST_CASE("A node hands over closed buckets, oldest first, and never the open one", "[distributed][fleethistory]")
{
    // What rides the heartbeat. The newest bucket is still open -- it gains a reading
    // every sample -- so shipping it would hand the leader a partial window it could
    // never be told to correct.
    PlacedWallClock clock;
    FleetHistory history { clock };

    for (auto const minute: std::views::iota(0, 5))
    {
        history.Record(Reading(static_cast<std::uint64_t>(minute) + 1));
        clock.Advance(std::chrono::minutes { 1 });
    }
    // Five recorded, and the clock now sits in a sixth, empty window. Four are
    // closed; the fifth is the one the clock left behind and is closed too.
    auto const all = history.ClosedBucketsAfter(-1, 100);
    REQUIRE(all.size() == 5);
    CHECK(std::ranges::is_sorted(all, {}, &FleetBucket::startMillis));
    CHECK(GrantedOf(all.front()) == 1);
    CHECK(GrantedOf(all.back()) == 5);

    // Record into the current window: it is now open, and must not travel.
    history.Record(Reading(6));
    auto const closed = history.ClosedBucketsAfter(-1, 100);
    CHECK(closed.size() == 5);
    CHECK(GrantedOf(closed.back()) == 5);

    // The watermark excludes what has already been handed over.
    auto const after = history.ClosedBucketsAfter(all[2].startMillis, 100);
    REQUIRE(after.size() == 2);
    CHECK(GrantedOf(after.front()) == 4);

    // And the ceiling takes the OLDEST, so a catch-up converges from the far end
    // rather than resending the newest and never closing the gap.
    auto const bounded = history.ClosedBucketsAfter(-1, 2);
    REQUIRE(bounded.size() == 2);
    CHECK(GrantedOf(bounded.front()) == 1);
    CHECK(GrantedOf(bounded.back()) == 2);

    CHECK(history.ClosedBucketsAfter(-1, 0).empty());
}

TEST_CASE("A leader fills the windows it missed from what the nodes kept", "[distributed][fleethistory]")
{
    // The whole point of the issue. This leader was elected part-way through, so its
    // own series has nothing for the earlier windows -- but every machine kept
    // recording itself throughout, and handed that over.
    PlacedWallClock clock;

    // Two machines, each recording four minutes.
    FleetHistory nodeA { clock };
    FleetHistory nodeB { clock };
    for (auto const minute: std::views::iota(0, 4))
    {
        auto const value = static_cast<std::uint64_t>(minute) + 1;
        nodeA.Record(Reading(0, value));      // in flight: 1,2,3,4
        nodeB.Record(Reading(0, value * 10)); // in flight: 10,20,30,40
        clock.Advance(std::chrono::minutes { 1 });
    }

    FleetNodeHistories received { clock };
    CHECK(received.AcceptHistory("a:1", "key-a", nodeA.ClosedBucketsAfter(-1, 100)) == 4);
    CHECK(received.AcceptHistory("b:1", "key-b", nodeB.ClosedBucketsAfter(-1, 100)) == 4);
    CHECK(received.Count() == 2);

    // A leader whose own series is empty: it was not leading for any of that.
    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    CHECK(std::ranges::none_of(view, [](FleetBucket const& b) { return b.present; }));

    received.BackfillInto(view, FleetRange::OneHour);

    auto filledCount = 0;
    for (auto const& bucket: view)
    {
        if (!bucket.present)
            continue;
        ++filledCount;
        // Summed across machines, and marked so nothing reads a dispatch counter off
        // a window this leader was not scheduling for.
        CHECK(bucket.backfilled);
        auto const inFlight = bucket.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)];
        CHECK(inFlight % 11 == 0); // n + 10n
    }
    CHECK(filledCount == 4);
}

TEST_CASE("A window the leader sampled itself is never replaced by a sum", "[distributed][fleethistory]")
{
    // Gaps only. A window this leader sampled holds the registry's own totals -- what
    // the page has always plotted -- and replacing them with a sum assembled from a
    // different set of machines would step the chart wherever the two disagreed.
    PlacedWallClock clock;

    FleetHistory node { clock };
    node.Record(Reading(0, 7));
    clock.Advance(std::chrono::minutes { 1 });

    FleetHistory leader { clock };
    leader.Record(Reading(0, 999));

    FleetNodeHistories received { clock };
    received.AcceptHistory("a:1", "key-a", node.ClosedBucketsAfter(-1, 100));

    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);

    auto const own = std::ranges::find_if(view, [](FleetBucket const& b) {
        return b.present && b.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] == 999;
    });
    REQUIRE(own != view.end());
    CHECK_FALSE(own->backfilled);
}

TEST_CASE("A redelivered heartbeat does not count twice", "[distributed][fleethistory]")
{
    // The wire carries no acknowledgement, so a node that never saw a reply resends.
    // The high-water mark is what makes that safe -- and it is why no reply field was
    // needed in the first place.
    PlacedWallClock clock;

    FleetHistory node { clock };
    for ([[maybe_unused]] auto const minute: std::views::iota(0, 3))
    {
        node.Record(Reading(0, 5));
        clock.Advance(std::chrono::minutes { 1 });
    }
    auto const batch = node.ClosedBucketsAfter(-1, 100);
    REQUIRE(batch.size() == 3);

    FleetNodeHistories received { clock };
    CHECK(received.AcceptHistory("a:1", "key-a", batch) == 3);
    // The very same batch again, as a lost reply would produce.
    CHECK(received.AcceptHistory("a:1", "key-a", batch) == 0);
    CHECK(received.HighWaterFor("a:1") == batch.back().startMillis);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);

    for (auto const& bucket: view)
        if (bucket.present)
            // Five, not ten. A machine reporting the same window twice must not
            // double the fleet's reading of it.
            CHECK(bucket.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] == 5);

    CHECK(received.HighWaterFor("nobody:1") == -1);
}

TEST_CASE("A handover carries no fold, and the leader rebuilds one", "[distributed][fleethistory]")
{
    // What the wire record leaves out and why it can. A counter's peak is a RATE and
    // cannot be recovered from a single reading -- but it can be recovered from the
    // SEQUENCE of readings, which is exactly what travels. Carrying the fold as well
    // would be a second answer to a question already answered, and the one a decoder
    // trusted would be the one nothing kept correct.
    PlacedWallClock clock;

    // A NODE-scoped counter, because that is what a machine can answer for and
    // therefore all the backfill sums: a dispatch outcome belongs to the scheduler,
    // and summing one across machines would be summing zeroes into a number a
    // reader would take for a measurement.
    constexpr auto Hits = static_cast<std::size_t>(FleetMetric::CacheHits);
    FleetHistory node { clock };
    // A counter that steps hard once: 0, then 100, then 10 more.
    for (auto const hits: std::array<std::uint64_t, 3> { 0, 100, 110 })
    {
        EnumTable<FleetMetric, std::uint64_t> values {};
        values[Hits] = hits;
        node.Record(values);
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const own = node.Buckets(FleetRange::OneHour);
    auto const spike =
        std::ranges::find_if(own, [](FleetBucket const& each) { return each.present && each.fold[Hits].high == 100; });
    REQUIRE(spike != own.end());

    // Out through the wire and back, which is the only shape the leader ever sees.
    // Three closed buckets, not two: the clock was advanced past the last reading,
    // so the window it landed in has closed as well.
    auto const handed = HistoryFromWire(HistoryToWire(node.ClosedBucketsAfter(-1, 100)));
    REQUIRE(handed.size() == 3);
    for (auto const& bucket: handed)
        CHECK(bucket.fold[Hits].high == 0);

    FleetNodeHistories received { clock };
    REQUIRE(received.AcceptHistory("a:1", "key-a", handed) == 3);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);

    auto const rebuilt = std::ranges::find_if(
        view, [&spike](FleetBucket const& each) { return each.present && each.startMillis == spike->startMillis; });
    REQUIRE(rebuilt != view.end());
    // The same peak the node itself recorded, from readings alone.
    CHECK(rebuilt->fold[Hits].high == 100);
}

TEST_CASE("A leader's record of the other machines survives a restart", "[distributed][fleethistory]")
{
    // Not an optimisation. A node advances its own watermark once a heartbeat is
    // accepted and never resends it, so a leader that forgot what it had been handed
    // would leave those windows a gap for as long as the rings hold them -- the very
    // failure the handover exists to remove, reintroduced by a restart.
    Testing::ScratchDirectory const scratch { "fleet-received-roundtrip" };
    auto const file = scratch.Path() / "received-history.bin";

    PlacedWallClock clock;
    FleetHistory nodeA { clock };
    FleetHistory nodeB { clock };
    for (auto const minute: std::views::iota(0, 4))
    {
        auto const value = static_cast<std::uint64_t>(minute) + 1;
        nodeA.Record(Reading(0, value));
        nodeB.Record(Reading(0, value * 10));
        clock.Advance(std::chrono::minutes { 1 });
    }

    std::int64_t highA = 0;
    auto const batchA = nodeA.ClosedBucketsAfter(-1, 100);
    {
        FleetNodeHistories received { clock };
        REQUIRE(received.AcceptHistory("a:1", "key-a", batchA) == 4);
        REQUIRE(received.AcceptHistory("b:1", "key-b", nodeB.ClosedBucketsAfter(-1, 100)) == 4);
        highA = received.HighWaterFor("a:1");
        REQUIRE(received.Save(file, StateFile::ReceivedHistory));
    }

    FleetNodeHistories restored { clock };
    REQUIRE(restored.Load(file));
    CHECK(restored.Count() == 2);
    // The mark as well as the readings: without it a restarted leader would take the
    // batch a node resends after a reply it never saw and count every window twice.
    CHECK(restored.HighWaterFor("a:1") == highA);
    CHECK(restored.AcceptHistory("a:1", "key-a", batchA) == 0);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    restored.BackfillInto(view, FleetRange::OneHour);

    auto filledCount = 0;
    for (auto const& bucket: view)
    {
        if (!bucket.present)
            continue;
        ++filledCount;
        CHECK(bucket.backfilled);
        CHECK(bucket.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] % 11 == 0);
    }
    CHECK(filledCount == 4);
}

TEST_CASE("A received-history file that cannot be trusted starts empty", "[distributed][fleethistory]")
{
    // The same rule as a node's own history, and for the same reason: this is a
    // convenience, and no state of the file may keep a leader from coming up.
    Testing::ScratchDirectory const scratch { "fleet-received-damaged" };
    PlacedWallClock clock;

    SECTION("absent")
    {
        FleetNodeHistories received { clock };
        CHECK_FALSE(received.Load(scratch.Path() / "nothing-here.bin"));
        CHECK(received.Count() == 0);
    }

    SECTION("another file's magic")
    {
        // A node series and a received store sit in the same directory, so pointing
        // one reader at the other's file is a configuration slip rather than a
        // fantasy -- and it must be refused rather than half-read.
        auto const file = scratch.Path() / "wrong-magic.bin";
        {
            FleetHistory own { clock };
            own.Record(Reading(3));
            REQUIRE(own.Save(file, StateFile::FleetHistory));
        }
        FleetNodeHistories received { clock };
        CHECK_FALSE(received.Load(file));
        CHECK(received.Count() == 0);
    }

    SECTION("corrupt body")
    {
        auto const file = scratch.Path() / "corrupt.bin";
        {
            FleetHistory node { clock };
            node.Record(Reading(0, 7));
            clock.Advance(std::chrono::minutes { 1 });
            FleetNodeHistories received { clock };
            REQUIRE(received.AcceptHistory("a:1", "key-a", node.ClosedBucketsAfter(-1, 100)) == 1);
            REQUIRE(received.Save(file, StateFile::ReceivedHistory));
        }
        {
            std::fstream patch { file, std::ios::binary | std::ios::in | std::ios::out };
            patch.seekp(64);
            patch.put(static_cast<char>(0xFF));
        }
        FleetNodeHistories received { clock };
        CHECK_FALSE(received.Load(file));
        CHECK(received.Count() == 0);
    }
}

TEST_CASE("A v1 received-history file loads with its keyless series dropped and the drop reported",
          "[distributed][fleethistory]")
{
    // A store written before keys were recorded holds every series under no key. Kept, a series would be counted a
    // second time beside the one its machine files under its key once it reports again -- so it is dropped, counted
    // for the start to say, and the file still loads rather than keeping the node from starting.
    Testing::ScratchDirectory const scratch { "fleet-received-v1" };
    auto const file = scratch.Path() / "v1.bin";

    PlacedWallClock clock;
    auto const nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
    // Comfortably closed, not the instant a fresh reader would call its open bucket.
    auto const minuteStart = ((nowMillis / 60'000) - 5) * 60'000;

    // Its body is still read through the v1 nested reader before it is dropped: a damaged one refuses the file whole.
    WriteFile(file, VersionOneNodeStoreFile("a:1", 5, 42, minuteStart, 17, 4));

    FleetNodeHistories received { clock };
    CHECK_FALSE(received.Load(file));     // nothing was restored ...
    CHECK(received.DroppedOnLoad() == 1); // ... because the one series it held was dropped, and said so
    CHECK(received.Count() == 0);
    CHECK_FALSE(received.ReadOnly());
    CHECK(received.HighWaterFor("a:1") == -1);

    // And it saves forward as an empty v3 store.
    REQUIRE(received.Save(file, StateFile::ReceivedHistory));
    CHECK(ReadFile(file)[4] == static_cast<char>(3));
}

TEST_CASE("A fleet window spanning a legacy store and the machine's keyed series counts the machine once",
          "[distributed][fleethistory]")
{
    // The upgrade: a v1 store holds machine A's minute M under its endpoint, no key. After the upgrade A announces its
    // minute M+1 under its key. The 5-minute Day window holding both must read A's in-flight once -- 4 -- never 8.
    Testing::ScratchDirectory const scratch { "fleet-received-span" };
    auto const file = scratch.Path() / "v1.bin";

    PlacedWallClock clock;
    auto const nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
    // Two minutes inside one closed 5-minute window: its first and second minute.
    auto const window = ((nowMillis / 300'000) - 1) * 300'000;
    WriteFile(file, VersionOneNodeStoreFile("a:1", static_cast<std::uint64_t>(window), 1, window, 0, 4));

    FleetNodeHistories received { clock };
    (void) received.Load(file);
    REQUIRE(received.DroppedOnLoad() == 1);

    auto next = Testing::ClosedBucket(window + 60'000);
    next.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] = 4;
    REQUIRE(received.AcceptHistory("a:1", "key-a", std::span { &next, 1 }) == 1);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::Day);
    received.BackfillInto(view, FleetRange::Day);
    auto const found =
        std::ranges::find_if(view, [window](FleetBucket const& bucket) { return bucket.startMillis == window; });
    REQUIRE(found != view.end());
    REQUIRE(found->present);
    CHECK(found->values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] == 4);
}

TEST_CASE("A received-history file newer than this build is kept, and never written over", "[distributed][fleethistory]")
{
    // `NodeStoreFile`'s own version, mirroring the rule `RingFile` already has: a
    // node rolled back to an older build must not destroy what a later one wrote and
    // could still read.
    Testing::ScratchDirectory const scratch { "fleet-received-newer" };
    auto const file = scratch.Path() / "newer.bin";

    PlacedWallClock clock;
    {
        FleetHistory node { clock };
        node.Record(Reading(0, 7));
        clock.Advance(std::chrono::minutes { 1 });
        FleetNodeHistories writer { clock };
        REQUIRE(writer.AcceptHistory("a:1", "key-a", node.ClosedBucketsAfter(-1, 100)) == 1);
        REQUIRE(writer.Save(file, StateFile::ReceivedHistory));
    }
    auto bytes = ReadFile(file);
    REQUIRE(bytes.size() > 4);
    bytes[4] = static_cast<char>(200);
    WriteFile(file, bytes);
    auto const before = ReadFile(file);

    FleetNodeHistories received { clock };
    CHECK_FALSE(received.Load(file));
    CHECK(received.Count() == 0);
    CHECK(received.ReadOnly());

    CHECK_FALSE(received.Save(file, StateFile::ReceivedHistory));
    CHECK(ReadFile(file) == before);
}

namespace
{

/// The CPU slot's index, for the cases about it.
constexpr auto CpuSlot = static_cast<std::size_t>(FleetMetric::CpuBusyPermille);

/// A history file in the v3 layout, written by hand: ten values and ten folds per bucket, no absence mask.
///
/// By hand for `VersionTwoFile`'s reason: a file this build writes is v4 and would exercise nothing. Ten is
/// frozen here, not `EnumeratorCount<FleetMetric>`: it is the width every v3 file was written at, the day
/// `DispatchAllExcluded` was the newest slot -- so the CPU slot is one v3 never carried.
/// @param generation What the file claims.
/// @param minuteStart Bucket start of the one present minute bucket.
/// @param minuteGranted Its `DispatchGranted` reading.
/// @param minuteExcluded Its `DispatchAllExcluded` reading -- the LAST slot v3 carried.
/// @return The complete file, header included.
[[nodiscard]] std::string VersionThreeFile(std::uint64_t generation,
                                           std::int64_t minuteStart,
                                           std::uint64_t minuteGranted,
                                           std::uint64_t minuteExcluded)
{
    constexpr std::size_t VersionThreeWidth = 10;

    std::string body;
    AppendU64(body, generation);
    AppendU64(body, 1U); // the minute ring: one bucket
    AppendU64(body, static_cast<std::uint64_t>(minuteStart));
    AppendU64(body, static_cast<std::uint64_t>(minuteStart)); // sample == start
    AppendU64(body, 1U);                                      // present
    AppendU64(body, 1U);                                      // coverage
    for (auto const slot: std::views::iota(std::size_t { 0 }, VersionThreeWidth))
    {
        auto value = std::uint64_t { 0 };
        if (slot == static_cast<std::size_t>(FleetMetric::DispatchGranted))
            value = minuteGranted;
        else if (slot == static_cast<std::size_t>(FleetMetric::DispatchAllExcluded))
            value = minuteExcluded;
        AppendU64(body, value);
    }
    for ([[maybe_unused]] auto const slot: std::views::iota(std::size_t { 0 }, VersionThreeWidth))
    {
        AppendU64(body, 0U); // low
        AppendU64(body, 0U); // high
        AppendU64(body, 0U); // total
    }
    AppendU64(body, 0U); // the hour ring: empty
    AppendU64(body, 0U); // the day ring: empty

    std::string file;
    file += "FCFH";
    file.push_back(static_cast<char>(3));
    AppendU64(file, body.size());
    AppendU64(file, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;
    return file;
}

/// A reading with the CPU slot set to @p permille.
[[nodiscard]] EnumTable<FleetMetric, std::uint64_t> CpuReading(std::uint64_t permille)
{
    auto values = Reading(0);
    values[CpuSlot] = permille;
    return values;
}

} // namespace

TEST_CASE("The CPU slot is pinned at 10, a node-scoped gauge no fleet total answers for", "[distributed][fleethistory]")
{
    // Appended, per the ordinal contract, and the ordinal pinned by value beside the name.
    CHECK(static_cast<std::uint8_t>(FleetMetric::CpuBusyPermille) == 10);
    auto const& row = FleetMetricTable[CpuSlot];
    CHECK(row.key == "cpu-busy");
    CHECK(row.kind == FleetMetricKind::Gauge);
    CHECK(row.scope == FleetMetricScope::Node);
    // A permille summed over machines is no machine's load and no fleet's either.
    CHECK(row.fleetTotal == FleetTotal::None);
    // A record that never carried it says nothing about it: absent, unlike an exclusion count nobody kept yet.
    CHECK(row.whenUncarried == FleetUncarried::Absent);
    CHECK(FleetMetricTable[static_cast<std::size_t>(FleetMetric::DispatchAllExcluded)].whenUncarried
          == FleetUncarried::Zero);
}

TEST_CASE("cpu sample recorded as gauge", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    // Three readings inside one 5-minute Day bucket: the level is the newest, the fold keeps floor, peak and sum.
    clock.Set(std::chrono::system_clock::time_point { std::chrono::seconds { 1'767'225'600 } });
    history.Record(CpuReading(400));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(CpuReading(900));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(CpuReading(250));

    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE(day.back().present);
    CHECK_FALSE(IsAbsent(day.back(), FleetMetric::CpuBusyPermille));
    CHECK(day.back().values[CpuSlot] == 250);
    CHECK(FoldOf(day.back(), FleetMetric::CpuBusyPermille).low == 250);
    CHECK(FoldOf(day.back(), FleetMetric::CpuBusyPermille).high == 900);
    CHECK(FoldOf(day.back(), FleetMetric::CpuBusyPermille).total == 1550);

    // Rendered as a LEVEL, never as a rate: a gauge's value is the bucket itself.
    auto const* const series = FleetSeriesFromKey("cpu-busy");
    REQUIRE(series != nullptr);
    CHECK(series->kind == FleetSeriesKind::Level);
    CHECK(series->numerator == FleetMetric::CpuBusyPermille);
    auto const values = ValuesFor(*series, day, 300);
    REQUIRE(values.back().has_value());
    CHECK(values.back() == std::optional { 250.0 });
}

TEST_CASE("absent CPU reading leaves the slot absent", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    clock.Set(std::chrono::system_clock::time_point { std::chrono::seconds { 1'767'225'600 } });
    FleetHistory history { clock };

    // A bucket whose only sample could not read the CPU: present (the other slots were read), the CPU slot not.
    history.Record(CpuReading(0), SlotBit(FleetMetric::CpuBusyPermille));
    auto const hour = history.Buckets(FleetRange::OneHour);
    REQUIRE(hour.back().present);
    CHECK(IsAbsent(hour.back(), FleetMetric::CpuBusyPermille));
    CHECK_FALSE(IsAbsent(hour.back(), FleetMetric::JobsInFlight));

    auto const* const series = FleetSeriesFromKey("cpu-busy");
    REQUIRE(series != nullptr);
    // A gap, never 0: an idle machine and one that could not say are opposite facts.
    CHECK_FALSE(ValuesFor(*series, hour, 60).back().has_value());
    // The other series in the same bucket still answer.
    auto const* const inFlight = FleetSeriesFromKey("in-flight");
    REQUIRE(inFlight != nullptr);
    CHECK(ValuesFor(*inFlight, hour, 60).back().has_value());

    // A later sample in the same Day window that DID read it makes the window answer from that reading alone:
    // the absent sample is neither its floor nor part of its sum.
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(CpuReading(600));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(CpuReading(0), SlotBit(FleetMetric::CpuBusyPermille));
    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE(day.back().present);
    CHECK_FALSE(IsAbsent(day.back(), FleetMetric::CpuBusyPermille));
    CHECK(day.back().values[CpuSlot] == 600);
    CHECK(FoldOf(day.back(), FleetMetric::CpuBusyPermille).low == 600);
    CHECK(FoldOf(day.back(), FleetMetric::CpuBusyPermille).total == 600);

    // And the absence survives a save and a reload, so a restart does not turn it into a zero.
    Testing::ScratchDirectory const scratch { "fleet-history-absent" };
    auto const file = scratch.Path() / "absent.bin";
    REQUIRE(history.Save(file, StateFile::FleetHistory));
    FleetHistory again { clock };
    REQUIRE(again.Load(file));
    auto const reloaded = again.Buckets(FleetRange::OneHour);
    CHECK(IsAbsent(reloaded.back(), FleetMetric::CpuBusyPermille));
    CHECK_FALSE(IsAbsent(reloaded[reloaded.size() - 2], FleetMetric::CpuBusyPermille));
    CHECK(reloaded[reloaded.size() - 2].values[CpuSlot] == 600);
}

TEST_CASE("v3 history file still loads; a v5 file is kept and not overwritten", "[distributed][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-history-v3-v5" };
    PlacedWallClock clock;
    auto const nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
    auto const minuteStart = (nowMillis / 60'000) * 60'000;

    SECTION("v3 reads at its own width, and the slot it never carried is absent rather than zero")
    {
        auto const file = scratch.Path() / "v3.bin";
        WriteFile(file, VersionThreeFile(42, minuteStart, 17, 3));

        FleetHistory history { clock };
        REQUIRE(history.Load(file));
        CHECK(history.Generation() == 42);
        CHECK_FALSE(history.ReadOnly());

        auto const day = history.Buckets(FleetRange::Day);
        REQUIRE(day.back().present);
        CHECK(GrantedOf(day.back()) == 17);
        CHECK(day.back().values[static_cast<std::size_t>(FleetMetric::DispatchAllExcluded)] == 3);
        CHECK(IsAbsent(day.back(), FleetMetric::CpuBusyPermille));
        CHECK_FALSE(IsAbsent(day.back(), FleetMetric::DispatchAllExcluded));

        // Saved forward as v4, and the absence travels with it.
        auto const upgraded = scratch.Path() / "v4.bin";
        REQUIRE(history.Save(upgraded, StateFile::FleetHistory));
        CHECK(ReadFile(upgraded)[4] == static_cast<char>(4));
        FleetHistory again { clock };
        REQUIRE(again.Load(upgraded));
        CHECK(IsAbsent(again.Buckets(FleetRange::Day).back(), FleetMetric::CpuBusyPermille));
        CHECK(GrantedOf(again.Buckets(FleetRange::Day).back()) == 17);
    }

    SECTION("v5, a later build's, is kept and never written over")
    {
        auto const file = scratch.Path() / "v5.bin";
        {
            FleetHistory writer { clock };
            writer.Record(CpuReading(300));
            REQUIRE(writer.Save(file, StateFile::FleetHistory));
        }
        auto bytes = ReadFile(file);
        REQUIRE(bytes[4] == static_cast<char>(4));
        bytes[4] = static_cast<char>(5);
        WriteFile(file, bytes);
        auto const before = ReadFile(file);

        FleetHistory history { clock };
        CHECK_FALSE(history.Load(file));
        CHECK(history.ReadOnly());
        history.Record(CpuReading(500));
        CHECK_FALSE(history.Save(file, StateFile::FleetHistory));
        CHECK(ReadFile(file) == before);
    }
}

TEST_CASE("A received-history store keeps each slot's absence and the key a machine proved", "[distributed][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-received-cpu" };
    auto const file = scratch.Path() / "received.bin";
    PlacedWallClock clock;

    FleetHistory node { clock };
    node.Record(CpuReading(700));
    clock.Advance(std::chrono::minutes { 1 });
    node.Record(CpuReading(0), SlotBit(FleetMetric::CpuBusyPermille));
    clock.Advance(std::chrono::minutes { 1 });

    {
        FleetNodeHistories writer { clock };
        REQUIRE(writer.AcceptHistory("a:1", "key-a", node.ClosedBucketsAfter(-1, 100)) == 2);
        REQUIRE(writer.Save(file, StateFile::ReceivedHistory));
    }
    CHECK(ReadFile(file)[4] == static_cast<char>(3));

    FleetNodeHistories received { clock };
    REQUIRE(received.Load(file));
    auto const machine = received.MachineBuckets("key-a", FleetRange::OneHour);
    REQUIRE(machine.has_value());
    auto const& buckets = Testing::Unwrap(machine);
    REQUIRE(buckets.size() >= 3);
    auto const& older = buckets[buckets.size() - 3];
    auto const& newer = buckets[buckets.size() - 2];
    REQUIRE(older.present);
    REQUIRE(newer.present);
    CHECK(older.values[CpuSlot] == 700);
    CHECK_FALSE(IsAbsent(older, FleetMetric::CpuBusyPermille));
    CHECK(IsAbsent(newer, FleetMetric::CpuBusyPermille));
    // A machine answers for no dispatch outcome: those slots are absent in its own view, never zero.
    CHECK(IsAbsent(older, FleetMetric::DispatchGranted));

    // A key nobody delivered under names no machine.
    CHECK_FALSE(received.MachineBuckets("key-b", FleetRange::OneHour).has_value());
}

namespace
{

/// @param clock Where the readings are placed.
/// @param minutes How many one-minute readings to take, each with the CPU at `100 * (minute + 1)`.
/// @return The closed buckets a node would hand over, oldest first.
[[nodiscard]] std::vector<FleetBucket> CpuMinutes(PlacedWallClock& clock, int minutes)
{
    FleetHistory node { clock };
    for (auto const minute: std::views::iota(0, minutes))
    {
        node.Record(CpuReading(static_cast<std::uint64_t>(minute + 1) * 100));
        clock.Advance(std::chrono::minutes { 1 });
    }
    return node.ClosedBucketsAfter(-1, 100);
}

/// @param buckets A machine's view.
/// @return Its CPU readings, oldest first, absent ones skipped.
[[nodiscard]] std::vector<std::uint64_t> CpuReadings(std::vector<FleetBucket> const& buckets)
{
    auto out = std::vector<std::uint64_t> {};
    for (auto const& bucket: buckets)
        if (bucket.present && !IsAbsent(bucket, FleetMetric::CpuBusyPermille))
            out.push_back(bucket.values[CpuSlot]);
    return out;
}

} // namespace

TEST_CASE("A machine that moves endpoint keeps one series across both", "[distributed][fleethistory]")
{
    // Filed under the key it proved, so the minutes it reported from its old address and those from its new one are one
    // machine's: none of them reads unsampled for having been handed over under the other label.
    PlacedWallClock clock;
    auto const batch = CpuMinutes(clock, 4);
    REQUIRE(batch.size() == 4);

    FleetNodeHistories received { clock };
    REQUIRE(received.AcceptHistory("10.0.0.7:6674", "key-a", std::span { batch }.first(2)) == 2);
    REQUIRE(received.AcceptHistory("192.168.1.7:6674", "key-a", std::span { batch }.subspan(2)) == 2);
    CHECK(received.Count() == 1);
    CHECK(received.HighWaterFor("192.168.1.7:6674") == batch.back().startMillis);
    // The old label is no longer where it reports from.
    CHECK(received.HighWaterFor("10.0.0.7:6674") == -1);

    auto const machine = received.MachineBuckets("key-a", FleetRange::OneHour);
    REQUIRE(machine.has_value());
    CHECK(CpuReadings(Testing::Unwrap(machine)) == std::vector<std::uint64_t> { 100, 200, 300, 400 });
}

TEST_CASE("A second key at one endpoint is a second machine and takes nothing from the first", "[distributed][fleethistory]")
{
    // A wiped state directory mints a new key at the same host:port, and any member may NAME another's endpoint in its
    // announcement. Either way the first machine's series stays its own and answers; the second has a series of its own.
    PlacedWallClock clock;
    auto const batch = CpuMinutes(clock, 4);
    REQUIRE(batch.size() == 4);

    FleetNodeHistories received { clock };
    REQUIRE(received.AcceptHistory("10.0.0.7:6674", "key-a", std::span { batch }.first(2)) == 2);
    REQUIRE(received.AcceptHistory("10.0.0.7:6674", "key-b", std::span { batch }.subspan(2)) == 2);
    CHECK(received.Count() == 2);

    auto const first = received.MachineBuckets("key-a", FleetRange::OneHour);
    auto const second = received.MachineBuckets("key-b", FleetRange::OneHour);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(CpuReadings(Testing::Unwrap(first)) == std::vector<std::uint64_t> { 100, 200 });
    CHECK(CpuReadings(Testing::Unwrap(second)) == std::vector<std::uint64_t> { 300, 400 });

    // And the first keeps taking its own minutes afterwards: nothing it can be blocked by was recorded.
    clock.Advance(std::chrono::minutes { 1 });
    auto later = Testing::ClosedBucket(batch.back().startMillis + 60'000);
    later.values[CpuSlot] = 900;
    CHECK(received.AcceptHistory("10.0.0.7:6674", "key-a", std::span { &later, 1 }) == 1);
    CHECK(CpuReadings(Testing::Unwrap(received.MachineBuckets("key-a", FleetRange::OneHour))).back() == 900);

    // An empty key is no key: never a match for a lookup.
    CHECK_FALSE(received.MachineBuckets("", FleetRange::OneHour).has_value());
}

TEST_CASE("A connection that proved no key files nothing", "[distributed][fleethistory]")
{
    // Its series could answer for no machine, and it may be a machine a proven connection also reports: filed, it would
    // be summed twice into every fleet window it filled.
    PlacedWallClock clock;
    auto const batch = CpuMinutes(clock, 3);
    FleetNodeHistories received { clock };
    CHECK(received.AcceptHistory("10.0.0.7:6674", batch) == 0);
    CHECK(received.AcceptHistory("10.0.0.7:6674", "", batch) == 0);
    CHECK(received.Count() == 0);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);
    CHECK(std::ranges::none_of(view, [](FleetBucket const& bucket) { return bucket.present; }));
}

TEST_CASE("A received-history store holds at most its ceiling, dropping the series heard from longest ago",
          "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetNodeHistories received { clock };
    auto const base = Testing::MinuteBucketStart(clock.now()) - (static_cast<std::int64_t>(MaxReceivedHistories) * 60'000);
    // One series per key, each reporting one minute later than the one before.
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxReceivedHistories))
    {
        auto bucket = Testing::ClosedBucket(base + (static_cast<std::int64_t>(index) * 60'000));
        REQUIRE(received.AcceptHistory("e:1", std::format("key-{}", index), std::span { &bucket, 1 }) == 1);
    }
    CHECK(received.Count() == MaxReceivedHistories);

    // One more key: the series whose newest reading is oldest -- key-0 -- goes, and nothing else does.
    auto fresh = Testing::ClosedBucket(base + (static_cast<std::int64_t>(MaxReceivedHistories) * 60'000));
    REQUIRE(received.AcceptHistory("e:1", "key-new", std::span { &fresh, 1 }) == 1);
    CHECK(received.Count() == MaxReceivedHistories);
    CHECK_FALSE(received.MachineBuckets("key-0", FleetRange::OneHour).has_value());
    CHECK(received.MachineBuckets("key-1", FleetRange::OneHour).has_value());
    CHECK(received.MachineBuckets("key-new", FleetRange::OneHour).has_value());
}

TEST_CASE("A fleet backfill never sums the CPU slot", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory nodeA { clock };
    FleetHistory nodeB { clock };
    nodeA.Record(CpuReading(400));
    nodeB.Record(CpuReading(500));
    clock.Advance(std::chrono::minutes { 1 });

    FleetNodeHistories received { clock };
    REQUIRE(received.AcceptHistory("a:1", "key-a", nodeA.ClosedBucketsAfter(-1, 100)) == 1);
    REQUIRE(received.AcceptHistory("b:1", "key-b", nodeB.ClosedBucketsAfter(-1, 100)) == 1);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);
    auto const found = std::ranges::find_if(view, [](FleetBucket const& b) { return b.present; });
    REQUIRE(found != view.end());
    CHECK(found->backfilled);
    // 900 would be no machine's load: the slot is absent from the fleet's view, never a sum.
    CHECK(IsAbsent(*found, FleetMetric::CpuBusyPermille));
    CHECK_FALSE(IsAbsent(*found, FleetMetric::JobsInFlight));
}

TEST_CASE("A handed-over bucket carries its absences, and an older peer's missing CPU slot is absent",
          "[distributed][fleethistory]")
{
    auto bucket = Testing::ClosedBucket(60'000);
    bucket.values[CpuSlot] = 0;
    bucket.absent = SlotBit(FleetMetric::CpuBusyPermille);
    auto const decoded = CompileCacheWire::DecodeHistoryBuckets(
        CompileCacheWire::EncodeHistoryBuckets(HistoryToWire(std::span { &bucket, 1 })));
    REQUIRE(decoded.has_value());
    auto const back = HistoryFromWire(Testing::Unwrap(decoded));
    REQUIRE(back.size() == 1);
    CHECK(IsAbsent(back.front(), FleetMetric::CpuBusyPermille));

    // A peer that carried ten slots and no mask: the exclusion count reads zero (no list existed), the CPU slot it
    // never sent is absent.
    auto older = CompileCacheWire::HistoryBucketFields {};
    older.startMillis = 60'000;
    older.sampleMillis = 60'000;
    older.carried = 10;
    auto const fromOlder = HistoryFromWire(std::span { &older, 1 });
    REQUIRE(fromOlder.size() == 1);
    CHECK(IsAbsent(fromOlder.front(), FleetMetric::CpuBusyPermille));
    CHECK_FALSE(IsAbsent(fromOlder.front(), FleetMetric::DispatchAllExcluded));
}
