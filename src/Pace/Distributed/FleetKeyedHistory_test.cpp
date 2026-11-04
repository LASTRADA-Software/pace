// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/ByteAppender.hpp>
#include <Pace/Core/Crc32c.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Distributed/FleetKeyedHistory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FleetHistoryFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Distributed;
using Pace::Testing::PlacedWallClock;
using Pace::Testing::Unwrap;

namespace
{

/// @param index Which machine.
/// @return A key that reads like one, distinct per index.
[[nodiscard]] std::string MachineKey(std::size_t index)
{
    return std::format("machine-{:04}", index);
}

/// @param buckets A series.
/// @return The sum of its present buckets.
[[nodiscard]] std::uint64_t SumOf(std::vector<std::optional<std::uint64_t>> const& buckets)
{
    return std::accumulate(buckets.begin(), buckets.end(), std::uint64_t { 0 }, [](std::uint64_t sum, auto const& each) {
        return sum + each.value_or(0);
    });
}

/// @param view A view.
/// @return Every key's total plus `other`'s.
[[nodiscard]] std::uint64_t SumOfRows(KeyedView const& view)
{
    auto sum = view.other.has_value() ? Unwrap(view.other).total : std::uint64_t { 0 };
    for (auto const& key: view.keys)
        sum += key.total;
    return sum;
}

/// @param view A view.
/// @param key A key.
/// @return Its row, or null when the view holds none.
[[nodiscard]] KeyedSeries const* RowOf(KeyedView const& view, std::string_view key)
{
    auto const found = std::ranges::find(view.keys, key, &KeyedSeries::key);
    return found == view.keys.end() ? nullptr : &*found;
}

/// Write @p body behind a valid envelope, so a damaged BODY is what a case is about.
void WriteFile(std::filesystem::path const& path, std::array<char, 4> magic, std::uint8_t version, std::string const& body)
{
    std::string file { magic.data(), magic.size() };
    file.push_back(static_cast<char>(version));
    ByteAppender { file }.AppendU64(body.size());
    ByteAppender { file }.AppendU64(
        Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;
    std::ofstream { path, std::ios::binary }.write(file.data(), static_cast<std::streamsize>(file.size()));
}

constexpr auto RequesterMagic = KeyedDimensionTable[static_cast<std::size_t>(KeyedDimension::Requesters)].magic;

} // namespace

TEST_CASE("257th key folds the least recent into other and totals stay exact", "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, DefaultHistoryKeys, wall };

    // Machine i counts i + 1 compiles, one minute after machine i - 1: machine 0 is the least recently active.
    auto counted = std::uint64_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 0 }, DefaultHistoryKeys))
    {
        REQUIRE(history.Count(MachineKey(index), index + 1));
        counted += index + 1;
        wall.Advance(std::chrono::seconds { 1 });
    }
    REQUIRE(history.KeyCount() == DefaultHistoryKeys);
    CHECK_FALSE(history.View(FleetRange::Day).other.has_value());

    // Machine 0 counts AGAIN, so it is no longer the least recent: machine 1 is.
    REQUIRE(history.Count(MachineKey(0), 5));
    counted += 5;
    wall.Advance(std::chrono::seconds { 1 });

    REQUIRE(history.Count("newcomer", 7));
    counted += 7;

    auto const view = history.View(FleetRange::Day);
    CHECK(history.KeyCount() == DefaultHistoryKeys);
    CHECK(RowOf(view, MachineKey(1)) == nullptr);
    REQUIRE(RowOf(view, MachineKey(0)) != nullptr);
    CHECK(RowOf(view, MachineKey(0))->total == 6);
    REQUIRE(RowOf(view, "newcomer") != nullptr);
    REQUIRE(view.other.has_value());
    // Machine 1's two compiles moved into `other`, bucket by bucket.
    CHECK(Unwrap(view.other).total == 2);
    CHECK(SumOf(Unwrap(view.other).buckets) == 2);
    CHECK(view.total == counted);
    CHECK(SumOfRows(view) == counted);
    // The newest count is the first row: most compiles first.
    CHECK(view.keys.front().key == MachineKey(DefaultHistoryKeys - 1));
}

TEST_CASE("other reports k", "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::ToolchainDemand, 2, wall };
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 5 }))
    {
        REQUIRE(history.Count(MachineKey(index)));
        wall.Advance(std::chrono::seconds { 1 });
    }
    auto const view = history.View(FleetRange::OneHour);
    REQUIRE(view.other.has_value());
    CHECK(Unwrap(view.other).keysFolded == 3);
    CHECK(Unwrap(view.other).total == 3);
    CHECK(view.keys.size() == 2);
    CHECK(view.total == 5);
}

TEST_CASE("a folded key that returns says its past is in other, and is not double counted",
          "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, 1, wall };
    REQUIRE(history.Count("a", 3));
    wall.Advance(std::chrono::seconds { 1 });
    REQUIRE(history.Count("b", 1)); // folds a
    wall.Advance(std::chrono::seconds { 1 });
    REQUIRE(history.Count("a", 2)); // folds b; a returns

    auto const view = history.View(FleetRange::OneHour);
    REQUIRE(view.keys.size() == 1);
    CHECK(view.keys.front().key == "a");
    // Its series counts only what it did since it returned, and says the rest is in `other`.
    CHECK(view.keys.front().total == 2);
    CHECK(view.keys.front().earlier == EarlierCounts::InOther);
    REQUIRE(view.other.has_value());
    CHECK(Unwrap(view.other).keysFolded == 2);
    CHECK(Unwrap(view.other).total == 4);
    CHECK(view.total == 6);
}

TEST_CASE("a store that forgot which keys it folded says a new key's past is unknown", "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, 1, wall };
    // One kept key remembers `RememberedFoldsPerKey` folded names; one more and the oldest is forgotten.
    for (auto const index: std::views::iota(std::size_t { 0 }, RememberedFoldsPerKey + 2))
    {
        REQUIRE(history.Count(MachineKey(index)));
        wall.Advance(std::chrono::seconds { 1 });
    }
    REQUIRE(history.Count("fresh"));
    auto const view = history.View(FleetRange::OneHour);
    REQUIRE(RowOf(view, "fresh") != nullptr);
    CHECK(RowOf(view, "fresh")->earlier == EarlierCounts::Unknown);
}

TEST_CASE("a keyed bucket nobody counted in is absent, one observed with no count is zero",
          "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, DefaultHistoryKeys, wall };
    REQUIRE(history.Count("m", 4));
    wall.Advance(std::chrono::minutes { 1 });
    history.Fold();                           // observed, nothing counted
    wall.Advance(std::chrono::minutes { 2 }); // one minute nobody watched, then now

    auto const view = history.View(FleetRange::OneHour);
    REQUIRE(view.keys.size() == 1);
    auto const& buckets = view.keys.front().buckets;
    REQUIRE(buckets.size() == 60);
    CHECK(buckets[56] == std::optional<std::uint64_t> { 4 });
    CHECK(buckets[57] == std::optional<std::uint64_t> { 0 });
    CHECK_FALSE(buckets[58].has_value());
    CHECK_FALSE(buckets[59].has_value());
    CHECK(view.coverage[56] == 1);
    CHECK(view.coverage[57] == 1);
    CHECK(view.coverage[58] == 0);
}

TEST_CASE("a key that is not text, or too long, is refused and counts nothing", "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::ToolchainDemand, DefaultHistoryKeys, wall };
    CHECK_FALSE(history.Count(""));
    CHECK_FALSE(history.Count("tab\there"));
    CHECK_FALSE(history.Count("\xff\xfe"));
    CHECK_FALSE(history.Count(std::string(MaxKeyedKeyBytes + 1, 'x')));
    CHECK(history.Count(std::string(MaxKeyedKeyBytes, 'x')));
    CHECK(history.View(FleetRange::OneHour).total == 1);
}

TEST_CASE("new leader reports coverage_since", "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, DefaultHistoryKeys, wall };
    // A node that has never counted -- a leader just elected -- answers for nothing yet.
    CHECK_FALSE(history.View(FleetRange::Day).coverageSince.has_value());
    CHECK(history.Empty());

    // Elected mid-day, mid-minute: its record starts at that instant -- not at the start of the minute, and not at
    // midnight, which is where the day ring's bucket begins.
    wall.Advance(std::chrono::hours { 9 } + std::chrono::seconds { 90 });
    history.Fold();
    auto const elected = std::chrono::duration_cast<std::chrono::milliseconds>(wall.now().time_since_epoch()).count();
    wall.Advance(std::chrono::minutes { 30 });
    REQUIRE(history.Count("m"));

    auto const view = history.View(FleetRange::OneHour);
    REQUIRE(view.coverageSince.has_value());
    CHECK(Unwrap(view.coverageSince) == elected);
    // A restart keeps it: the file records when this store first counted.
    Testing::ScratchDirectory const scratch { "keyed-history-coverage" };
    REQUIRE(history.Save(scratch.Path() / "requester-history.bin"));
    FleetKeyedHistory restored { KeyedDimension::Requesters, DefaultHistoryKeys, wall };
    REQUIRE(restored.Load(scratch.Path() / "requester-history.bin"));
    CHECK(restored.View(FleetRange::TwelveMonths).coverageSince == std::optional<std::int64_t> { elected });
    // Windows before that are absent, never zero.
    CHECK(view.coverage.front() == 0);
}

TEST_CASE("persist/load round trip", "[distributed][fleet-keyed-history]")
{
    Testing::ScratchDirectory const scratch { "keyed-history-roundtrip" };
    auto const file = scratch.Path() / "requester-history.bin";
    PlacedWallClock wall;
    {
        FleetKeyedHistory history { KeyedDimension::Requesters, 2, wall };
        REQUIRE(history.Count("a", 3));
        wall.Advance(std::chrono::minutes { 1 });
        REQUIRE(history.Count("b", 1));
        wall.Advance(std::chrono::minutes { 1 });
        REQUIRE(history.Count("c", 2)); // folds a
        wall.Advance(std::chrono::minutes { 1 });
        history.Fold();
        REQUIRE(history.Save(file));
    }

    FleetKeyedHistory restored { KeyedDimension::Requesters, 2, wall };
    REQUIRE(restored.Load(file));
    auto const view = restored.View(FleetRange::OneHour);
    CHECK(view.total == 6);
    REQUIRE(view.other.has_value());
    CHECK(Unwrap(view.other).keysFolded == 1);
    CHECK(Unwrap(view.other).total == 3);
    REQUIRE(RowOf(view, "c") != nullptr);
    CHECK(RowOf(view, "c")->total == 2);
    CHECK(view.coverage[59] == 1);
    CHECK(view.coverage[56] == 1);

    // The folded name survived: "a" coming back says so.
    wall.Advance(std::chrono::seconds { 1 });
    REQUIRE(restored.Count("a"));
    auto const after = restored.View(FleetRange::OneHour);
    REQUIRE(RowOf(after, "a") != nullptr);
    CHECK(RowOf(after, "a")->earlier == EarlierCounts::InOther);

    // A store of another dimension is refused by its magic, not read as this one.
    FleetKeyedHistory wrong { KeyedDimension::ToolchainDemand, 2, wall };
    CHECK_FALSE(wrong.Load(file));
    CHECK(wrong.Empty());
}

TEST_CASE("a keyed file written under a larger ceiling folds the excess into other on load",
          "[distributed][fleet-keyed-history]")
{
    Testing::ScratchDirectory const scratch { "keyed-history-ceiling" };
    auto const file = scratch.Path() / "toolchain-history.bin";
    PlacedWallClock wall;
    {
        FleetKeyedHistory history { KeyedDimension::ToolchainDemand, 4, wall };
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
        {
            REQUIRE(history.Count(MachineKey(index), 10));
            wall.Advance(std::chrono::seconds { 1 });
        }
        REQUIRE(history.Save(file));
    }
    FleetKeyedHistory smaller { KeyedDimension::ToolchainDemand, 1, wall };
    REQUIRE(smaller.Load(file));
    auto const view = smaller.View(FleetRange::OneHour);
    CHECK(smaller.KeyCount() == 1);
    REQUIRE(view.keys.size() == 1);
    CHECK(view.keys.front().key == MachineKey(3));
    REQUIRE(view.other.has_value());
    CHECK(Unwrap(view.other).keysFolded == 3);
    CHECK(view.total == 40);
}

TEST_CASE("a keyed file a later build wrote is kept and never written over", "[distributed][fleet-keyed-history]")
{
    Testing::ScratchDirectory const scratch { "keyed-history-newer" };
    auto const file = scratch.Path() / "requester-history.bin";
    WriteFile(file, RequesterMagic, 2, "future");
    auto const before = std::filesystem::file_size(file);

    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, 4, wall };
    CHECK_FALSE(history.Load(file));
    CHECK(history.ReadOnly());
    REQUIRE(history.Count("m"));
    CHECK_FALSE(history.Save(file));
    CHECK(std::filesystem::file_size(file) == before);
}

TEST_CASE("a hostile keyed file starts empty and never crashes", "[distributed][fleet-keyed-history]")
{
    Testing::ScratchDirectory const scratch { "keyed-history-hostile" };
    auto const file = scratch.Path() / "requester-history.bin";
    PlacedWallClock wall;

    auto const u64 = [](std::uint64_t value) {
        std::string out;
        ByteAppender { out }.AppendU64(value);
        return out;
    };
    auto const emptyRings = [&] {
        std::string out;
        for ([[maybe_unused]] auto const& row: FleetRingTable)
            out += u64(0);
        return out;
    };
    auto const head = u64(0) + u64(0) + u64(0) + u64(0) + emptyRings();

    struct Hostile
    {
        std::string_view what;
        std::string body;
    };
    auto const cases = std::vector<Hostile> {
        { .what = "truncated", .body = head },
        { .what = "more keys than any build keeps", .body = head + u64(MaxHistoryKeys + 1) },
        { .what = "a key that is too long",
          .body = head + u64(1) + u64(MaxKeyedKeyBytes + 1) + std::string(MaxKeyedKeyBytes + 1, 'x') },
        { .what = "a key that is not text",
          .body = head + u64(1) + u64(2) + "\xff\xfe" + u64(0) + u64(0) + emptyRings() + u64(0) + u64(0) },
        { .what = "an earlier-counts word past the table",
          .body = head + u64(1) + u64(1) + "k" + u64(0) + u64(7) + emptyRings() + u64(0) + u64(0) },
        { .what = "a negative observed bucket",
          .body = u64(0) + u64(0) + u64(0) + u64(0) + u64(1) + u64(~std::uint64_t { 0 }) },
        { .what = "trailing bytes", .body = head + u64(0) + u64(0) + u64(0) + u64(0) + "x" },
        { .what = "an observation counting more minutes than its bucket spans",
          .body = u64(0) + u64(0) + u64(0) + u64(0) + u64(1) + u64(5) + u64(2) },
        { .what = "an observation counting no minute",
          .body = u64(0) + u64(0) + u64(0) + u64(0) + u64(1) + u64(5) + u64(0) },
        { .what = "a forgotten flag that is not a flag",
          .body = u64(0) + u64(2) + u64(0) + u64(0) + emptyRings() + u64(0) + u64(0) + u64(0) },
    };
    for (auto const& each: cases)
    {
        INFO(each.what);
        WriteFile(file, RequesterMagic, 1, each.body);
        FleetKeyedHistory history { KeyedDimension::Requesters, 4, wall };
        CHECK_FALSE(history.Load(file));
        CHECK(history.Empty());
        CHECK_FALSE(history.ReadOnly());
    }

    // The control: the well-formed empty body loads.
    WriteFile(file, RequesterMagic, 1, head + u64(0) + u64(0) + u64(0) + u64(0));
    FleetKeyedHistory history { KeyedDimension::Requesters, 4, wall };
    CHECK(history.Load(file));
}

TEST_CASE("compile demand files a requester under its key text and a toolchain under its fingerprint",
          "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    CompileDemandHistory demand { DefaultHistoryKeys, wall };
    Ed25519PublicKey key {};
    std::ranges::fill(key, std::byte { 0x42 });
    auto const raw = std::string { reinterpret_cast<char const*>(key.data()), key.size() };

    demand.CompileGranted(raw, "fp-1");
    demand.CompileGranted(raw, "fp-1");
    // A local-only request that did not know its toolchain: its toolchain is unattributed, never dropped.
    demand.CompileGranted(raw, "");
    // A requester that identified no machine (a lease under a ticket): unattributed, never guessed.
    demand.CompileGranted("", "fp-2");

    auto const requesters = demand.Of(KeyedDimension::Requesters).View(FleetRange::OneHour);
    REQUIRE(requesters.keys.size() == 1);
    CHECK(requesters.keys.front().key == FormatEd25519PublicKey(key));
    CHECK(requesters.keys.front().total == 3);
    REQUIRE(requesters.unattributed.has_value());
    CHECK(Unwrap(requesters.unattributed).total == 1);
    CHECK(requesters.total == 4);

    auto const toolchains = demand.Of(KeyedDimension::ToolchainDemand).View(FleetRange::OneHour);
    REQUIRE(toolchains.unattributed.has_value());
    CHECK(Unwrap(toolchains.unattributed).total == 1);
    // Both stores count every grant, so their totals agree.
    CHECK(toolchains.total == 4);
    auto const rows = DemandRowsOf(toolchains);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].fingerprint == "fp-1");
    CHECK(rows[0].compiles == 2);
    CHECK(rows[1].fingerprint == "fp-2");
    CHECK(rows[1].compiles == 1);
}

TEST_CASE("an hour the leader counted for one minute reads 1 of 60, never a whole hour",
          "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, DefaultHistoryKeys, wall };
    // Elected at minute 58 of an hour: one fold, one grant, in the same minute.
    wall.Advance(std::chrono::minutes { 58 });
    history.Fold();
    REQUIRE(history.Count("m"));
    REQUIRE(history.Count("m"));
    wall.Advance(std::chrono::hours { 3 });

    auto const week = history.View(FleetRange::Week);
    REQUIRE(week.fullCoverage == 60);
    auto const watched = std::ranges::find_if(week.coverage, [](std::uint32_t minutes) { return minutes > 0; });
    REQUIRE(watched != week.coverage.end());
    CHECK(*watched == 1);
    CHECK(week.observedMinutes == 1);
    CHECK(week.windowMinutes == 60ULL * 168);
    CHECK(week.total == 2);

    // A minute folded twice is ONE minute counted.
    FleetKeyedHistory twice { KeyedDimension::Requesters, DefaultHistoryKeys, wall };
    twice.Fold();
    twice.Fold();
    REQUIRE(twice.Count("m"));
    CHECK(twice.View(FleetRange::Week).observedMinutes == 1);
}

TEST_CASE("a demotion's gap shows in coverage: the leader counted before and after, not between",
          "[distributed][fleet-keyed-history]")
{
    PlacedWallClock wall;
    FleetKeyedHistory history { KeyedDimension::Requesters, DefaultHistoryKeys, wall };
    // Led for three minutes, demoted for an hour (nothing folds), re-elected for two.
    for ([[maybe_unused]] auto const minute: std::views::iota(0, 3))
    {
        history.Fold();
        REQUIRE(history.Count("m"));
        wall.Advance(std::chrono::minutes { 1 });
    }
    wall.Advance(std::chrono::hours { 1 });
    for ([[maybe_unused]] auto const minute: std::views::iota(0, 2))
    {
        history.Fold();
        REQUIRE(history.Count("m"));
        wall.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.View(FleetRange::Day);
    CHECK(day.total == 5);
    // Five minutes counted of the day's 1440: the hour another leader granted in is not counted here, and says so.
    CHECK(day.observedMinutes == 5);
    CHECK(day.windowMinutes == 1440);
    // The buckets between the two stints read zero minutes, and their counts absent.
    auto const first = std::ranges::find_if(day.coverage, [](std::uint32_t m) { return m > 0; });
    auto const last = std::ranges::find_if(day.coverage | std::views::reverse, [](std::uint32_t m) { return m > 0; });
    auto const between = std::ranges::count(std::ranges::subrange(first, last.base()), 0U);
    CHECK(between >= 10); // an hour of five-minute buckets, less the stints' own
    REQUIRE(day.keys.size() == 1);
    auto const gaps = std::ranges::count_if(day.keys.front().buckets, [](auto const& b) { return !b.has_value(); });
    CHECK(static_cast<std::size_t>(gaps) == static_cast<std::size_t>(std::ranges::count(day.coverage, 0U)));
}
