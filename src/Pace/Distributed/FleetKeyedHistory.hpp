// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/StateFiles.hpp>
#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/FleetSample.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file FleetKeyedHistory.hpp
/// Counts kept per KEY over the history rings: compiles granted per requesting machine, and per toolchain.
///
/// A store of its own rather than new `FleetMetric` slots, because a slot is one fixed `EnumTable` column per bucket
/// and these have one column per machine or toolchain -- an open set. Bounded by keeping the most recently active
/// keys and folding the rest into one explicit `other` row whose counts are kept exactly.

namespace Pace::Distributed
{

/// How many keys a keyed history keeps before it folds one into `other`: the spec's N.
///
/// The figure `MaxReceivedHistories` uses for the same reason -- a fleet larger than this keeps the machines (and the
/// toolchains) that were active most recently, and the totals stay exact through `other`.
inline constexpr std::size_t DefaultHistoryKeys = 256;

/// The most keys `--history-requesters` may ask for.
///
/// A ceiling on what one store can hold: a key active in every bucket of every ring holds one tally per ring slot
/// (`KeyedHistoryWorstCaseBytes`), about 40 KiB, so 1024 keys bound one store near 40 MiB even in that worst case --
/// and a leader keeps two. Past this the answer is a smaller `other` row, never a larger process.
inline constexpr std::size_t MaxHistoryKeys = 1024;

/// The longest key a keyed history accepts, in bytes.
///
/// A requester is keyed by its identity key's text (43 characters) and a toolchain by its fingerprint, which the wire
/// caps at 64 bytes (`CompileCacheWire::MaxToolchainFingerprintBytes`). Twice the longer, so a key the wire admits is
/// never refused here, while a damaged file's declared length is refused before anything is allocated against it.
inline constexpr std::size_t MaxKeyedKeyBytes = 128;

/// How many folded keys a store remembers per key it keeps, so a key that comes BACK can say its past is in `other`.
///
/// Bounded because a stream of fresh keys would otherwise grow the list without limit; past it the oldest name is
/// forgotten and every key that arrives later says its earlier counts are UNKNOWN rather than claiming none.
inline constexpr std::size_t RememberedFoldsPerKey = 4;

/// Which keyed history.
///
/// **Private: never transmitted or persisted** -- each store's file carries its own magic, and the documents spell
/// the row's words.
enum class KeyedDimension : std::uint8_t
{
    Requesters,      ///< Compiles granted, per requesting machine.
    ToolchainDemand, ///< Compiles granted, per toolchain fingerprint.
    Last,            ///< Not a dimension: the length of a table keyed by one.
};

/// How one keyed history is named, stored and rendered.
struct KeyedDimensionRow
{
    KeyedDimension dimension;  ///< The dimension this row describes.
    std::string_view section;  ///< Its `/fleet.json` section.
    std::string_view series;   ///< Its MCP `history` series key.
    std::string_view keyField; ///< What a row's key is called in that section.
    std::string_view noun;     ///< What a key IS, plural, for `other (k <noun>)`.
    StateFile file;            ///< Where it is kept.
    std::array<char, 4> magic; ///< What its file says it is, so a file typed into the wrong place is refused.
    /// Whether a key names a machine, so a row also carries its member's node id and display name.
    bool namesMachines;
};

/// One row per `KeyedDimension`, in enumerator order.
inline constexpr EnumTable<KeyedDimension, KeyedDimensionRow> KeyedDimensionTable { {
    { .dimension = KeyedDimension::Requesters,
      .section = "requesters",
      .series = "requesters",
      .keyField = "key",
      .noun = "machines",
      .file = StateFile::RequesterHistory,
      .magic = { 'F', 'C', 'R', 'Q' },
      .namesMachines = true },
    { .dimension = KeyedDimension::ToolchainDemand,
      .section = "toolchain_demand",
      .series = "toolchain-demand",
      .keyField = "fingerprint",
      .noun = "toolchains",
      .file = StateFile::ToolchainHistory,
      .magic = { 'F', 'C', 'T', 'D' },
      .namesMachines = false },
} };

static_assert(RowsInEnumeratorOrder(KeyedDimensionTable, &KeyedDimensionRow::dimension),
              "KeyedDimensionTable must hold one row per KeyedDimension, in enumerator order");

/// @param dimension A keyed history.
/// @return Its row.
[[nodiscard]] constexpr KeyedDimensionRow const& KeyedDimensionOf(KeyedDimension dimension) noexcept
{
    return KeyedDimensionTable[static_cast<std::size_t>(dimension)];
}

/// What a key's own series says about counts it had BEFORE it was last folded into `other`.
///
/// Three answers, because a key that returns after being folded has a past this store keeps only inside `other`, and
/// a store that has forgotten which keys it folded cannot say "none" either. **Private**: rendered as a word.
enum class EarlierCounts : std::uint8_t
{
    None,    ///< This key's series is its whole history in this store.
    InOther, ///< It was folded before: its earlier counts are inside `other`, not in its series.
    Unknown, ///< Folded names were forgotten (`RememberedFoldsPerKey`), so whether it has a past in `other` is unknown.
    Last,    ///< Not an answer: the length of a table keyed by one.
};

/// How one `EarlierCounts` is spelled.
struct EarlierCountsRow
{
    EarlierCounts answer;  ///< The answer this row spells.
    std::string_view word; ///< Its one spelling.
};

/// One row per `EarlierCounts`, in enumerator order.
inline constexpr EnumTable<EarlierCounts, EarlierCountsRow> EarlierCountsTable { {
    { .answer = EarlierCounts::None, .word = "none" },
    { .answer = EarlierCounts::InOther, .word = "in-other" },
    { .answer = EarlierCounts::Unknown, .word = "unknown" },
} };

static_assert(RowsInEnumeratorOrder(EarlierCountsTable, &EarlierCountsRow::answer),
              "EarlierCountsTable must hold one row per EarlierCounts, in enumerator order");

/// @param answer An answer.
/// @return Its spelling.
[[nodiscard]] constexpr std::string_view EarlierCountsWord(EarlierCounts answer) noexcept
{
    return EarlierCountsTable[static_cast<std::size_t>(answer)].word;
}

/// One key's counts over a view.
struct KeyedSeries
{
    std::string key;                                   ///< The key: a requester's identity key text, or a fingerprint.
    std::uint64_t total {};                            ///< Its counts over the view's observed buckets.
    EarlierCounts earlier { EarlierCounts::None };     ///< Whether it has a past this series does not show.
    std::vector<std::optional<std::uint64_t>> buckets; ///< One per view bucket; absent where nobody was counting.
};

/// Every folded key's counts, as one row.
struct OtherRow
{
    std::size_t keysFolded {};                         ///< How many keys were folded into it, ever: the `k`.
    std::uint64_t total {};                            ///< Its counts over the view's observed buckets.
    std::vector<std::optional<std::uint64_t>> buckets; ///< One per view bucket; absent where nobody was counting.
};

/// What a keyed history holds over one range.
///
/// Totals are exact: `total` is the sum of every key's total and `other`'s, over the observed buckets.
struct KeyedView
{
    FleetRange range { FleetRange::Day };   ///< The window.
    std::vector<std::int64_t> bucketStarts; ///< Each view bucket's start, in ms since the epoch, oldest first.
    std::vector<bool> observed;             ///< Whether this store was counting during each bucket.
    /// From when it has counted: the first instant it observed while the rings hold it, else the oldest bucket start
    /// they still hold; absent when it never observed anything.
    std::optional<std::int64_t> coverageSince;
    std::uint64_t total {};        ///< Every count in the window: the keys' and `other`'s.
    std::vector<KeyedSeries> keys; ///< Keys with counts in the window, most first, then by key.
    std::optional<OtherRow> other; ///< Engaged once any key was folded, whatever it holds in the window.
};

/// What one store can hold at most, exactly: every kept key and `other` holding one tally in every slot of every
/// ring, plus the observation rings. The steady state of a store whose every key is busy every minute for a year.
/// @param maxKeys The store's key ceiling.
/// @return Bytes.
[[nodiscard]] constexpr std::size_t KeyedHistoryWorstCaseBytes(std::size_t maxKeys) noexcept
{
    std::size_t slots = 0;
    for (auto const& row: FleetRingTable)
        slots += row.slots;
    constexpr std::size_t TallyBytes = 2 * sizeof(std::int64_t);
    return ((maxKeys + 1) * slots * TallyBytes) + (slots * sizeof(std::int64_t));
}

/// Counts per key over the history rings, kept while this node LEADS (the caller decides that).
///
/// **A count is a tally of events this store saw, stored per bucket and never as a running total.** The rule that a
/// history stores a counter RAW exists because a cumulative counter OUTSIDE the store resets at a restart, and only a
/// raw reading shows the step; here the store does the counting itself, so nothing outside it can reset, and a
/// per-bucket tally is the raw reading. A running total would instead lose every key's first bucket and the first
/// bucket after every gap, and the totals would stop being exact. **A bucket nobody counted in is absent, never
/// zero**: `Fold` and `Count` mark the bucket they land in as observed, and only an observed bucket reads a number.
///
/// Thread-safe: the scheduler counts, the sampler folds, and the admin thread reads.
class FleetKeyedHistory
{
  public:
    /// @param dimension Which keyed history this is: its file's magic and what its folded keys are called.
    /// @param maxKeys The most keys kept before the least recently active is folded into `other`; at least one.
    /// @param wall Where a count's bucket comes from.
    FleetKeyedHistory(KeyedDimension dimension, std::size_t maxKeys, core::platform::WallClockRef wall);

    /// Count @p n events against @p key, in the bucket holding now.
    ///
    /// A key this store does not keep is added; at the ceiling, the key whose newest count is OLDEST is first folded
    /// into `other`, so a key still active is never the one dropped while an idle one is held.
    /// @param key UTF-8 text of at most `MaxKeyedKeyBytes` bytes and no control character.
    /// @param n How many.
    /// @return False when the key is not text this store accepts; nothing is counted then.
    bool Count(std::string_view key, std::uint64_t n = 1);

    /// Mark the bucket holding now as observed in every ring, so a minute with no event reads zero rather than absent.
    ///
    /// Called once a sample interval while this node leads. Folding is per bucket and idempotent.
    void Fold();

    /// What a range shows.
    /// @param range Which view.
    /// @return The view; `FleetRangeTable[range].points` buckets.
    [[nodiscard]] KeyedView View(FleetRange range) const;

    /// How many keys this store keeps now, not counting `other`.
    [[nodiscard]] std::size_t KeyCount() const;

    /// Whether nothing was ever observed.
    [[nodiscard]] bool Empty() const;

    /// Read a saved store back.
    ///
    /// Every failure -- absent, short, wrong version, bad checksum, a key that is not text or too long, more keys
    /// than this build ever keeps -- starts empty and says so by returning false. No state of this file may keep a node
    /// from starting. A file holding more keys than `maxKeys` folds the least recently active into `other`.
    /// @param path Where `Save` wrote.
    /// @return True when anything was restored.
    bool Load(std::filesystem::path const& path);

    /// Write the store out, atomically; refused, leaving the file untouched, when `Load` found a later build's.
    /// @param path Where to write.
    /// @return True on success.
    [[nodiscard]] bool Save(std::filesystem::path const& path) const;

    /// Whether `Load` found a file a LATER build wrote, which is never written over.
    [[nodiscard]] bool ReadOnly() const;

  private:
    /// One bucket's count, by its bucket number within a ring.
    struct Tally
    {
        std::int64_t bucket {}; ///< `startMillis / ringWidth`.
        std::uint64_t count {}; ///< Events counted in it.
    };

    /// One key's tallies, and when it last counted anything.
    struct KeyRecord
    {
        EnumTable<FleetRing, std::deque<Tally>> rings {}; ///< Sorted by bucket, pruned to each ring's span.
        std::int64_t lastActiveMillis {};                 ///< Its newest count's instant.
        EarlierCounts earlier { EarlierCounts::None };    ///< Whether it has a past in `other`.
    };

    void CountLocked(std::string_view key, std::uint64_t n, std::int64_t now);
    void ObserveLocked(std::int64_t now);
    void FoldIntoOtherLocked(std::map<std::string, KeyRecord, std::less<>>::iterator victim);
    void EvictToCeilingLocked();
    void RememberFoldedLocked(std::string key);
    [[nodiscard]] std::int64_t NowMillis() const noexcept;

    KeyedDimension _dimension;
    std::size_t _maxKeys;
    core::platform::WallClockRef _wall;
    mutable std::mutex _mutex;

    /// One ring of observed bucket numbers per `FleetRingTable` row: slot `n % slots` holds `n`, or -1.
    EnumTable<FleetRing, std::vector<std::int64_t>> _observed;
    std::map<std::string, KeyRecord, std::less<>> _keys;
    std::optional<KeyRecord> _other;
    std::size_t _keysFolded { 0 };

    /// Keys folded into `other`, oldest first, so one that returns can say so; bounded (`RememberedFoldsPerKey`).
    std::deque<std::string> _foldedOrder;
    std::set<std::string, std::less<>> _folded;
    /// Set once a folded name was forgotten: every key arriving after it may have a past in `other` nobody can name.
    bool _foldedForgotten { false };

    /// The first instant this store observed, exactly: what `KeyedView::coverageSince` says while the rings hold it.
    std::optional<std::int64_t> _firstObserved;

    /// Set when `Load` read a file a LATER build wrote; see `FleetHistory::_readOnly`.
    bool _readOnly { false };
};

/// The per-toolchain totals of a demand view, for the upgrade advice (`UpgradeAdvice`): its keys alone, most first.
///
/// `other` is not a toolchain and carries no fingerprint, so it has no row; a reader wanting the whole demand reads
/// `KeyedView::total`.
/// @param view A `KeyedDimension::ToolchainDemand` view.
/// @return One row per fingerprint with compiles in the window.
[[nodiscard]] std::vector<DemandRow> DemandRowsOf(KeyedView const& view);

/// The leader's two keyed histories behind the one sink the admission queue reports to.
///
/// The requester is filed under its identity key's text (`FormatEd25519PublicKey`), which a member record and the
/// `compiles` section's `requester-key` both spell the same way; the toolchain under its fingerprint. A grant whose
/// fingerprint is empty -- a local-only request that did not know it -- counts for its requester alone, and one whose
/// fingerprint is not text counts there alone too: a toolchain row must be text every reader can carry.
class CompileDemandHistory final: public ICompileDemandSink
{
  public:
    /// @param maxKeys The ceiling each store keeps (`--history-requesters`, which governs both).
    /// @param wall Where a count's bucket comes from.
    CompileDemandHistory(std::size_t maxKeys, core::platform::WallClockRef wall);

    /// @copydoc ICompileDemandSink::CompileGranted
    void CompileGranted(std::string_view requester, std::string_view fingerprint) override;

    /// Mark the current bucket observed in both stores; see `FleetKeyedHistory::Fold`.
    void Fold();

    /// @param dimension Which store.
    /// @return It.
    [[nodiscard]] FleetKeyedHistory& Of(KeyedDimension dimension) noexcept;

    /// @param dimension Which store.
    /// @return It.
    [[nodiscard]] FleetKeyedHistory const& Of(KeyedDimension dimension) const noexcept;

  private:
    FleetKeyedHistory _requesters;
    FleetKeyedHistory _toolchains;
};

} // namespace Pace::Distributed
