// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Utf8.hpp>
#include <Pace/Distributed/FleetKeyedHistory.hpp>
#include <Pace/Distributed/HistoryEnvelope.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <utility>

namespace Pace::Distributed
{

namespace
{
    using namespace HistoryEnvelope;

    /// The one format a keyed history file has had. A later shape is a new version and a reader row beside this one.
    constexpr std::uint8_t KeyedFileVersion = 1;

    /// @param dimension Which store.
    /// @return What its file is stamped with.
    [[nodiscard]] constexpr FileEnvelope EnvelopeOf(KeyedDimension dimension) noexcept
    {
        return FileEnvelope { .magic = KeyedDimensionOf(dimension).magic, .newest = KeyedFileVersion };
    }

    /// @param ring Which ring.
    /// @return Its bucket width, in milliseconds.
    [[nodiscard]] constexpr std::int64_t RingWidth(FleetRing ring) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(FleetRingTable[static_cast<std::size_t>(ring)].bucket)
            .count();
    }

    /// @param ring Which ring.
    /// @return How many buckets it spans.
    [[nodiscard]] constexpr std::int64_t RingSlots(FleetRing ring) noexcept
    {
        return static_cast<std::int64_t>(FleetRingTable[static_cast<std::size_t>(ring)].slots);
    }

    /// A sum that stops at the ceiling rather than wrapping: a damaged file's counts are bounded by nothing, and a
    /// total that wrapped would read as a quiet fleet.
    [[nodiscard]] constexpr std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) noexcept
    {
        return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max()
                                                                        : left + right;
    }

    /// Whether @p key is text every document can carry and short enough to keep.
    /// @param key A candidate key.
    /// @return True when it is non-empty UTF-8 of at most `MaxKeyedKeyBytes` bytes with no control byte.
    [[nodiscard]] bool AcceptableKey(std::string_view key) noexcept
    {
        constexpr unsigned char FirstPrintable = 0x20;
        constexpr unsigned char Delete = 0x7F;
        return !key.empty() && key.size() <= MaxKeyedKeyBytes && IsValidUtf8(key)
               && std::ranges::none_of(key, [](char const each) {
                      auto const byte = static_cast<unsigned char>(each);
                      return byte < FirstPrintable || byte == Delete;
                  });
    }

    /// Add @p n to bucket @p number of one key's ring, keeping it sorted and within the ring's span.
    template <typename Tallies>
    void AddTally(Tallies& tallies, std::int64_t number, std::uint64_t n, std::int64_t slots)
    {
        if (tallies.empty() || tallies.back().bucket < number)
            tallies.push_back({ .bucket = number, .count = n });
        else
        {
            // A wall clock stepped backwards lands a count behind the newest; it is still a count of that bucket.
            auto const at = std::ranges::lower_bound(tallies, number, {}, [](auto const& each) { return each.bucket; });
            if (at != tallies.end() && at->bucket == number)
                at->count = SaturatingAdd(at->count, n);
            else
                tallies.insert(at, { .bucket = number, .count = n });
        }
        // What lies a whole ring behind the newest bucket has lapped out of it.
        auto const newest = tallies.back().bucket;
        while (!tallies.empty() && tallies.front().bucket <= newest - slots)
            tallies.pop_front();
    }

    /// Sum one key's tallies over ring buckets `[first, last]`.
    template <typename Tallies>
    [[nodiscard]] std::uint64_t SumWithin(Tallies const& tallies, std::int64_t first, std::int64_t last)
    {
        auto const bucketOf = [](auto const& each) {
            return each.bucket;
        };
        auto const within = std::ranges::subrange(std::ranges::lower_bound(tallies, first, {}, bucketOf),
                                                  std::ranges::upper_bound(tallies, last, {}, bucketOf));
        auto sum = std::uint64_t { 0 };
        for (auto const& tally: within)
            sum = SaturatingAdd(sum, tally.count);
        return sum;
    }
} // namespace

FleetKeyedHistory::FleetKeyedHistory(KeyedDimension dimension, std::size_t maxKeys, core::platform::WallClockRef wall):
    _dimension { dimension },
    _maxKeys { std::clamp<std::size_t>(maxKeys, 1, MaxHistoryKeys) },
    _wall { wall }
{
    for (auto const& row: FleetRingTable)
        _observed[static_cast<std::size_t>(row.ring)].assign(row.slots, std::int64_t { -1 });
}

std::int64_t FleetKeyedHistory::NowMillis() const noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(_wall.now().time_since_epoch()).count();
}

bool FleetKeyedHistory::Count(std::string_view key, std::uint64_t n)
{
    if (!AcceptableKey(key))
        return false;
    auto const now = NowMillis();
    auto const guard = std::scoped_lock { _mutex };
    CountLocked(key, n, now);
    return true;
}

void FleetKeyedHistory::Fold()
{
    auto const now = NowMillis();
    auto const guard = std::scoped_lock { _mutex };
    ObserveLocked(now);
}

void FleetKeyedHistory::ObserveLocked(std::int64_t now)
{
    if (now < 0)
        return;
    _firstObserved = std::min(_firstObserved.value_or(now), now);
    for (auto const& row: FleetRingTable)
    {
        auto& ring = _observed[static_cast<std::size_t>(row.ring)];
        auto const number = now / RingWidth(row.ring);
        auto& slot = ring[static_cast<std::size_t>(number % RingSlots(row.ring))];
        // A newer lap is never overwritten by an older one: a clock stepped backwards must not erase what was seen.
        slot = std::max(slot, number);
    }
}

void FleetKeyedHistory::CountLocked(std::string_view key, std::uint64_t n, std::int64_t now)
{
    if (now < 0)
        return;
    // Counting is watching: a bucket a count lands in reads that count, never absent.
    ObserveLocked(now);

    auto found = _keys.find(key);
    if (found == _keys.end())
    {
        // A key that was folded before keeps its past in `other`, and its series must say so rather than presenting
        // what it counts from now on as its whole history.
        auto earlier = _foldedForgotten ? EarlierCounts::Unknown : EarlierCounts::None;
        if (auto const was = _folded.find(key); was != _folded.end())
        {
            earlier = EarlierCounts::InOther;
            std::erase(_foldedOrder, *was);
            _folded.erase(was);
        }
        if (_keys.size() >= _maxKeys)
        {
            // The key whose newest count is OLDEST: a key still active is never the one folded while an idle one is
            // held. Ties go to the smaller key, the map's order, so the choice is the same on every run.
            auto const victim =
                std::ranges::min_element(_keys, {}, [](auto const& entry) { return entry.second.lastActiveMillis; });
            FoldIntoOtherLocked(victim);
        }
        found =
            _keys.emplace(std::string { key }, KeyRecord { .rings = {}, .lastActiveMillis = now, .earlier = earlier }).first;
    }

    auto& record = found->second;
    for (auto const& row: FleetRingTable)
        AddTally(record.rings[static_cast<std::size_t>(row.ring)], now / RingWidth(row.ring), n, RingSlots(row.ring));
    record.lastActiveMillis = std::max(record.lastActiveMillis, now);
}

void FleetKeyedHistory::FoldIntoOtherLocked(std::map<std::string, KeyRecord, std::less<>>::iterator victim)
{
    if (!_other.has_value())
        _other.emplace();
    // Every tally the victim held is added into `other`'s, bucket by bucket, so the totals across the kept keys and
    // `other` stay what was counted -- folding moves counts, it never drops them.
    for (auto const& row: FleetRingTable)
    {
        auto const index = static_cast<std::size_t>(row.ring);
        for (auto const& tally: victim->second.rings[index])
            AddTally(_other->rings[index], tally.bucket, tally.count, RingSlots(row.ring));
    }
    _other->lastActiveMillis = std::max(_other->lastActiveMillis, victim->second.lastActiveMillis);
    ++_keysFolded;
    auto name = victim->first;
    _keys.erase(victim);
    RememberFoldedLocked(std::move(name));
}

void FleetKeyedHistory::EvictToCeilingLocked()
{
    while (_keys.size() > _maxKeys)
        FoldIntoOtherLocked(
            std::ranges::min_element(_keys, {}, [](auto const& entry) { return entry.second.lastActiveMillis; }));
}

void FleetKeyedHistory::RememberFoldedLocked(std::string key)
{
    if (!_folded.insert(key).second)
        return;
    _foldedOrder.push_back(std::move(key));
    while (_foldedOrder.size() > RememberedFoldsPerKey * _maxKeys)
    {
        _folded.erase(_foldedOrder.front());
        _foldedOrder.pop_front();
        _foldedForgotten = true;
    }
}

KeyedView FleetKeyedHistory::View(FleetRange range) const
{
    auto const& row = FleetRangeTable[static_cast<std::size_t>(range)];
    auto const width = std::chrono::duration_cast<std::chrono::milliseconds>(row.bucket).count();
    auto const ring = row.ring;
    auto const ringWidth = RingWidth(ring);
    auto const ringIndex = static_cast<std::size_t>(ring);
    // A view's bucket is a whole number of its ring's (`RangesFitTheirRings`).
    auto const perBucket = width / ringWidth;
    auto const now = std::max<std::int64_t>(NowMillis(), 0);
    auto const points = static_cast<std::int64_t>(row.points);
    auto const oldest = (now / width) - points + 1;

    KeyedView view;
    view.range = range;
    view.bucketStarts.reserve(row.points);
    view.observed.reserve(row.points);

    auto const guard = std::scoped_lock { _mutex };
    auto const& observed = _observed[ringIndex];
    auto const seen = [&observed, ring](std::int64_t number) {
        return number >= 0 && observed[static_cast<std::size_t>(number % RingSlots(ring))] == number;
    };

    // Which ring buckets each view bucket folds, computed once and shared by every key.
    std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    spans.reserve(row.points);
    for (auto const index: std::views::iota(std::int64_t { 0 }, points))
    {
        auto const start = (oldest + index) * width;
        auto const first = start / ringWidth;
        auto const last = first + perBucket - 1;
        view.bucketStarts.push_back(start);
        auto const any = std::ranges::any_of(std::views::iota(first, last + 1), seen);
        view.observed.push_back(any);
        spans.emplace_back(first, last);
    }

    auto const seriesOf = [&](KeyRecord const& record) {
        auto buckets = std::vector<std::optional<std::uint64_t>> {};
        buckets.reserve(spans.size());
        auto total = std::uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, spans.size()))
        {
            if (!view.observed[index])
            {
                buckets.emplace_back(std::nullopt);
                continue;
            }
            auto const sum = SumWithin(record.rings[ringIndex], spans[index].first, spans[index].second);
            total = SaturatingAdd(total, sum);
            buckets.emplace_back(sum);
        }
        return std::pair { std::move(buckets), total };
    };

    for (auto const& [key, record]: _keys)
    {
        auto [buckets, total] = seriesOf(record);
        if (total == 0)
            continue;
        view.total = SaturatingAdd(view.total, total);
        view.keys.push_back(
            KeyedSeries { .key = key, .total = total, .earlier = record.earlier, .buckets = std::move(buckets) });
    }
    std::ranges::sort(view.keys, [](KeyedSeries const& left, KeyedSeries const& right) {
        return left.total != right.total ? left.total > right.total : left.key < right.key;
    });

    if (_keysFolded > 0)
    {
        auto [buckets, total] = seriesOf(_other.value_or(KeyRecord {}));
        view.total = SaturatingAdd(view.total, total);
        view.other = OtherRow { .keysFolded = _keysFolded, .total = total, .buckets = std::move(buckets) };
    }

    // From when this store has counted, whichever window was asked -- the start of this leader's record, since a new
    // leader starts empty. The first instant it observed, exactly, while the rings still hold it; once that has lapped
    // out of the longest ring, the oldest bucket start any ring still holds. A bucket start alone would be the coarse
    // ring's: a leader elected at 09:12 would say it had counted since midnight.
    auto oldestHeld = std::optional<std::int64_t> {};
    for (auto const& each: FleetRingTable)
        for (auto const number: _observed[static_cast<std::size_t>(each.ring)])
            if (number >= 0)
            {
                auto const start = number * RingWidth(each.ring);
                oldestHeld = std::min(oldestHeld.value_or(start), start);
            }
    if (oldestHeld.has_value())
        view.coverageSince = std::max(*oldestHeld, _firstObserved.value_or(*oldestHeld));
    return view;
}

std::size_t FleetKeyedHistory::KeyCount() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _keys.size();
}

bool FleetKeyedHistory::Empty() const
{
    auto const guard = std::scoped_lock { _mutex };
    return std::ranges::all_of(_observed, [](auto const& ring) {
        return std::ranges::all_of(ring, [](std::int64_t const number) { return number < 0; });
    });
}

bool FleetKeyedHistory::ReadOnly() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _readOnly;
}

namespace
{
    /// One record's rings and scalars, as a file holds them: `[lastActive][earlier]`, then per ring
    /// `[count][(bucket, tally) * count]`.
    template <typename Record>
    void AppendRecord(std::string& out, Record const& record)
    {
        AppendU64(out, static_cast<std::uint64_t>(record.lastActiveMillis));
        AppendU64(out, static_cast<std::uint64_t>(record.earlier));
        for (auto const& row: FleetRingTable)
        {
            auto const& tallies = record.rings[static_cast<std::size_t>(row.ring)];
            AppendU64(out, tallies.size());
            for (auto const& tally: tallies)
            {
                AppendU64(out, static_cast<std::uint64_t>(tally.bucket));
                AppendU64(out, tally.count);
            }
        }
    }

    /// @param out The buffer.
    /// @param text A length-prefixed string.
    void AppendText(std::string& out, std::string_view text)
    {
        AppendU64(out, text.size());
        out.append(text);
    }

    /// Read a length-prefixed key, refusing one that is not text this store keeps.
    /// @param reader The cursor.
    /// @param into Where it lands.
    /// @return False on a truncated body or an unacceptable key.
    [[nodiscard]] bool TakeKey(BodyReader& reader, std::string& into)
    {
        std::uint64_t length = 0;
        std::string_view bytes;
        if (!reader.Take(length) || length > MaxKeyedKeyBytes || !reader.Take(length, bytes) || !AcceptableKey(bytes))
            return false;
        into = std::string { bytes };
        return true;
    }

    /// Read one record, refusing what no write of this build produces.
    template <typename Record>
    [[nodiscard]] bool TakeRecord(BodyReader& reader, Record& into)
    {
        std::uint64_t lastActive = 0;
        std::uint64_t earlier = 0;
        if (!reader.Take(lastActive) || !reader.Take(earlier) || earlier >= EnumeratorCount<EarlierCounts>)
            return false;
        into.lastActiveMillis = static_cast<std::int64_t>(lastActive);
        into.earlier = static_cast<EarlierCounts>(earlier);
        for (auto const& row: FleetRingTable)
        {
            std::uint64_t count = 0;
            if (!reader.Take(count) || count > row.slots)
                return false;
            auto& tallies = into.rings[static_cast<std::size_t>(row.ring)];
            for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, count))
            {
                std::uint64_t bucket = 0;
                std::uint64_t tally = 0;
                if (!reader.Take(bucket) || !reader.Take(tally))
                    return false;
                auto const number = static_cast<std::int64_t>(bucket);
                // Sorted strictly, and never before the epoch: that is all a write of this build produces, and a
                // negative bucket would index a ring by a negative remainder.
                if (number < 0 || (!tallies.empty() && tallies.back().bucket >= number))
                    return false;
                tallies.push_back({ .bucket = number, .count = tally });
            }
        }
        return true;
    }
} // namespace

bool FleetKeyedHistory::Save(std::filesystem::path const& path) const
{
    auto file = FramedBuffer();
    {
        auto const guard = std::scoped_lock { _mutex };
        // Refused BEFORE a byte is composed: the file on disk was written by a later build and is left as it was.
        if (_readOnly)
            return false;
        AppendU64(file, _keysFolded);
        AppendU64(file, _foldedForgotten ? 1U : 0U);
        AppendU64(file, _firstObserved.has_value() ? 1U : 0U);
        AppendU64(file, static_cast<std::uint64_t>(_firstObserved.value_or(0)));
        for (auto const& row: FleetRingTable)
        {
            auto const& ring = _observed[static_cast<std::size_t>(row.ring)];
            AppendU64(file, static_cast<std::uint64_t>(std::ranges::count_if(ring, [](auto n) { return n >= 0; })));
            for (auto const number: ring)
                if (number >= 0)
                    AppendU64(file, static_cast<std::uint64_t>(number));
        }
        AppendU64(file, _keys.size());
        for (auto const& [key, record]: _keys)
        {
            AppendText(file, key);
            AppendRecord(file, record);
        }
        AppendU64(file, _other.has_value() ? 1U : 0U);
        if (_other.has_value())
            AppendRecord(file, *_other);
        AppendU64(file, _foldedOrder.size());
        for (auto const& key: _foldedOrder)
            AppendText(file, key);
    }
    return WriteFramed(path, EnvelopeOf(_dimension), file, KeyedDimensionOf(_dimension).file);
}

bool FleetKeyedHistory::Load(std::filesystem::path const& path)
{
    auto const opened = ReadFramed(path, EnvelopeOf(_dimension));
    if (opened.outcome == FramedOutcome::Newer)
    {
        auto const guard = std::scoped_lock { _mutex };
        _readOnly = true;
        return false;
    }
    if (opened.outcome != FramedOutcome::Ok || opened.version != KeyedFileVersion)
        return false;

    // Read into locals and installed only whole: a file that fails halfway restores nothing.
    BodyReader reader { opened.body };
    std::uint64_t folded = 0;
    std::uint64_t forgotten = 0;
    if (!reader.Take(folded) || !reader.Take(forgotten) || forgotten > 1)
        return false;
    std::uint64_t hasFirst = 0;
    std::uint64_t first = 0;
    if (!reader.Take(hasFirst) || !reader.Take(first) || hasFirst > 1 || static_cast<std::int64_t>(first) < 0)
        return false;

    EnumTable<FleetRing, std::vector<std::int64_t>> observed;
    for (auto const& row: FleetRingTable)
    {
        auto& ring = observed[static_cast<std::size_t>(row.ring)];
        ring.assign(row.slots, std::int64_t { -1 });
        std::uint64_t count = 0;
        if (!reader.Take(count) || count > row.slots)
            return false;
        for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, count))
        {
            std::uint64_t raw = 0;
            if (!reader.Take(raw))
                return false;
            auto const number = static_cast<std::int64_t>(raw);
            if (number < 0)
                return false;
            auto& slot = ring[static_cast<std::size_t>(number % RingSlots(row.ring))];
            slot = std::max(slot, number);
        }
    }

    std::uint64_t keyCount = 0;
    // More keys than any build keeps is a damaged file, refused before anything is allocated against the count.
    if (!reader.Take(keyCount) || keyCount > MaxHistoryKeys)
        return false;
    std::map<std::string, KeyRecord, std::less<>> keys;
    for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, keyCount))
    {
        std::string key;
        KeyRecord record;
        if (!TakeKey(reader, key) || !TakeRecord(reader, record) || !keys.emplace(std::move(key), std::move(record)).second)
            return false;
    }

    std::uint64_t hasOther = 0;
    if (!reader.Take(hasOther) || hasOther > 1)
        return false;
    std::optional<KeyRecord> other;
    if (hasOther == 1)
    {
        other.emplace();
        if (!TakeRecord(reader, *other))
            return false;
    }

    std::uint64_t foldedCount = 0;
    if (!reader.Take(foldedCount) || foldedCount > RememberedFoldsPerKey * MaxHistoryKeys)
        return false;
    std::deque<std::string> foldedOrder;
    std::set<std::string, std::less<>> foldedSet;
    for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, foldedCount))
    {
        std::string key;
        if (!TakeKey(reader, key) || !foldedSet.insert(key).second)
            return false;
        foldedOrder.push_back(std::move(key));
    }
    if (!reader.Exhausted())
        return false;

    auto const guard = std::scoped_lock { _mutex };
    _observed = std::move(observed);
    _keys = std::move(keys);
    _other = std::move(other);
    _keysFolded = static_cast<std::size_t>(folded);
    _foldedForgotten = forgotten == 1;
    _firstObserved = hasFirst == 1 ? std::optional { static_cast<std::int64_t>(first) } : std::nullopt;
    _foldedOrder = std::move(foldedOrder);
    _folded = std::move(foldedSet);
    // A file written under a larger ceiling is brought under this one by the same rule a new key is: the least
    // recently active go into `other`, counts kept.
    EvictToCeilingLocked();
    while (_foldedOrder.size() > RememberedFoldsPerKey * _maxKeys)
    {
        _folded.erase(_foldedOrder.front());
        _foldedOrder.pop_front();
        _foldedForgotten = true;
    }
    return true;
}

std::vector<DemandRow> DemandRowsOf(KeyedView const& view)
{
    auto rows = std::vector<DemandRow> {};
    rows.reserve(view.keys.size());
    for (auto const& key: view.keys)
        rows.push_back(DemandRow { .fingerprint = key.key, .compiles = key.total });
    return rows;
}

CompileDemandHistory::CompileDemandHistory(std::size_t maxKeys, core::platform::WallClockRef wall):
    _requesters { KeyedDimension::Requesters, maxKeys, wall },
    _toolchains { KeyedDimension::ToolchainDemand, maxKeys, wall }
{
}

void CompileDemandHistory::CompileGranted(std::string_view requester, std::string_view fingerprint)
{
    // The queue's machine id is a raw identity key; anything else names no machine and is not counted as one.
    if (requester.size() == Ed25519PublicKeyBytes)
    {
        Ed25519PublicKey key {};
        std::ranges::transform(requester, key.begin(), [](char const byte) { return static_cast<std::byte>(byte); });
        _requesters.Count(FormatEd25519PublicKey(key));
    }
    if (!fingerprint.empty())
        _toolchains.Count(fingerprint);
}

void CompileDemandHistory::Fold()
{
    _requesters.Fold();
    _toolchains.Fold();
}

FleetKeyedHistory& CompileDemandHistory::Of(KeyedDimension dimension) noexcept
{
    return dimension == KeyedDimension::Requesters ? _requesters : _toolchains;
}

FleetKeyedHistory const& CompileDemandHistory::Of(KeyedDimension dimension) const noexcept
{
    return dimension == KeyedDimension::Requesters ? _requesters : _toolchains;
}

} // namespace Pace::Distributed
