// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/NumericText.hpp>
#include <Pace/Platform/MemoryPressure.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <format>
#include <ranges>
#include <set>
#include <utility>

namespace Pace::Platform
{

namespace
{

    /// The value of `key=value` among the fields of a line.
    [[nodiscard]] std::optional<std::string_view> FieldOf(std::string_view line, std::string_view key)
    {
        for (auto const word: std::views::split(line, ' '))
        {
            auto const text = std::string_view { word.begin(), word.end() };
            if (text.size() > key.size() && text.starts_with(key) && text[key.size()] == '=')
                return text.substr(key.size() + 1);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<PsiLine> ParseLine(std::string_view line)
    {
        auto parsed = PsiLine {};
        auto const avg10 = FieldOf(line, "avg10");
        auto const avg60 = FieldOf(line, "avg60");
        auto const total = FieldOf(line, "total");
        if (!avg10 || !avg60 || !total)
            return std::nullopt;
        if (!ParseFiniteDouble(*avg10, parsed.avg10) || !ParseFiniteDouble(*avg60, parsed.avg60))
            return std::nullopt;
        auto const [end, ec] = std::from_chars(total->data(), total->data() + total->size(), parsed.totalMicros);
        if (ec != std::errc {} || end != total->data() + total->size())
            return std::nullopt;
        return parsed;
    }

    /// A whole control file holding one decimal number, as `memory.current` and `memory.max` do.
    /// @param text The file's text.
    /// @return The number; disengaged for anything else, `max` included.
    [[nodiscard]] std::optional<std::uint64_t> DecimalOf(std::string_view text)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
            text.remove_suffix(1);
        auto value = std::uint64_t { 0 };
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc {} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    /// The value of the `<key> <number>` line of a flat-keyed control file such as `memory.events`.
    /// @param document The file's text.
    /// @param key The line's key.
    /// @return The number; disengaged when the line is absent or unreadable.
    [[nodiscard]] std::optional<std::uint64_t> KeyedOf(std::string_view document, std::string_view key)
    {
        for (auto const lineRange: std::views::split(document, '\n'))
        {
            auto const line = std::string_view { lineRange.begin(), lineRange.end() };
            if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == ' ')
                return DecimalOf(line.substr(key.size() + 1));
        }
        return std::nullopt;
    }

    /// One memory limit a cgroup may set on itself, and the `memory.events` key the kernel counts reaching it under.
    struct OwnLimitRow
    {
        std::string_view file;      ///< The limit's control file.
        std::string_view eventsKey; ///< Its `memory.events` count.
    };

    /// Every limit a stall can be confined to: the hard one, and the throttling one systemd's `MemoryHigh=` sets.
    constexpr std::array OwnLimitTable {
        OwnLimitRow { .file = "memory.max", .eventsKey = "max" },
        OwnLimitRow { .file = "memory.high", .eventsKey = "high" },
    };

    /// The cgroup core's own control files, present in every cgroup whatever its controllers.
    constexpr std::string_view CoreControlPrefix = "cgroup";

    /// Whether @p cgroup carries @p share of @p system's `some` stall. Compared as a product, never a ratio,
    /// and only against a stall there is: nothing is carried of none.
    [[nodiscard]] bool Carries(PsiReading const& cgroup, PsiReading const& system, double share) noexcept
    {
        // `some` alone: in a machine-wide freeze every busy cgroup's `full` reads about the system's, so `full` says
        // nothing about whose the stall is -- the governor discounts a cgroup's `full` only where it carries `some`.
        return system.some.avg10 > 0.0 && cgroup.some.avg10 >= share * system.some.avg10;
    }

    /// @return Whether @p a is @p b, or an ancestor or descendant of it, component by component.
    [[nodiscard]] bool Related(std::filesystem::path const& a, std::filesystem::path const& b)
    {
        auto const [left, right] = std::ranges::mismatch(a, b);
        return left == a.end() || right == b.end();
    }

    /// The names in @p entries that may be child cgroups: not a control file. A control file is
    /// `<controller>.<name>` for one of @p controllers or the cgroup core, so the set is the kernel's own answer, never
    /// a list kept here. A child cgroup NAMED like one is skipped: its stall then counts as the machine's, the safe
    /// direction.
    [[nodiscard]] std::vector<std::string> ChildCandidates(std::vector<std::string> const& entries,
                                                           std::string_view controllers)
    {
        auto prefixes = std::set<std::string, std::less<>> { std::string { CoreControlPrefix } };
        for (auto const word: std::views::split(controllers, ' '))
            if (auto const name = std::string_view { word.begin(), word.end() }; !name.empty())
                prefixes.emplace(name.ends_with('\n') ? name.substr(0, name.size() - 1) : name);
        auto children = std::vector<std::string> {};
        for (auto const& entry: entries)
        {
            auto const dot = entry.find('.');
            if (dot == std::string::npos || !prefixes.contains(std::string_view { entry }.substr(0, dot)))
                children.push_back(entry);
        }
        return children;
    }

} // namespace

ConfinedScanBounds ConfinedScanBoundsFor(ICgroupHost const& host,
                                         std::optional<std::filesystem::path> const& compiles,
                                         double share)
{
    auto bounds = ConfinedScanBounds {};
    bounds.share = share;
    if (auto const document = host.ReadFile("/proc/self/cgroup"); document)
        if (auto const own = ParseUnifiedCgroupPath(*document); own)
            bounds.excluded.push_back(bounds.root / std::filesystem::path { *own }.relative_path());
    if (compiles)
        bounds.excluded.push_back(*compiles);
    return bounds;
}

ConfinedStallScan::ConfinedStallScan(std::shared_ptr<ICgroupHost const> host,
                                     ConfinedScanBounds bounds,
                                     core::platform::IClock const& clock):
    _host { std::move(host) },
    _bounds { std::move(bounds) },
    _clock { clock }
{
}

std::uint32_t ConfinedStallScan::Walks() const noexcept
{
    return _walks;
}

bool ConfinedStallScan::Excluded(std::filesystem::path const& cgroup) const
{
    return std::ranges::any_of(_bounds.excluded, [&](auto const& excluded) { return Related(cgroup, excluded); });
}

bool ConfinedStallScan::InsideExcluded(std::filesystem::path const& cgroup) const
{
    return std::ranges::any_of(_bounds.excluded, [&](auto const& excluded) {
        return std::ranges::mismatch(excluded, cgroup).in1 == excluded.end();
    });
}

std::optional<ConfinedStall> ConfinedStallScan::Look(std::filesystem::path const& cgroup)
{
    auto const text = _host->ReadFile(cgroup / PsiCgroupFileName);
    if (!text)
        return std::nullopt; // a control file, or a cgroup without the memory controller
    auto const psi = ParsePsi(*text);
    if (!psi)
        return std::nullopt;

    // `memory.events` is hierarchical: a child reaching ITS limit counts there too. The cgroup's own limit is judged by
    // `memory.events.local` where the kernel offers it (5.8 and later), the hierarchical file only where it does not.
    auto events = _host->ReadFile(cgroup / "memory.events.local");
    if (!events)
        events = _host->ReadFile(cgroup / "memory.events");
    auto limited = false;
    auto atLimit = false;
    for (auto const& row: OwnLimitTable)
    {
        auto const limit = _host->ReadFile(cgroup / row.file);
        auto const count = events ? KeyedOf(*events, row.eventsKey) : std::nullopt;
        auto const key = std::format("{}|{}", cgroup.generic_string(), row.eventsKey);
        auto const seen = _eventsSeen.find(key);
        auto const rose = count && seen != _eventsSeen.end() && *count > seen->second;
        if (count)
            _eventsSeen[key] = *count;
        // No limit of this kind (`max`) is never AT it, whatever the count says.
        if (!(limit && DecimalOf(*limit)))
            continue;
        limited = true;
        // Only the kernel counting the cgroup reaching its limit says it stalls ON it: standing near a limit it never
        // reaches -- page cache filling up to it, then nothing -- is no stall of its own.
        atLimit = atLimit || rose;
    }
    return ConfinedStall { .cgroup = cgroup, .psi = *psi, .limited = limited, .atLimit = atLimit };
}

std::expected<void, std::string> ConfinedStallScan::Walk(PsiReading const& system)
{
    ++_walks;
    auto const rootEntries = _host->ListDirectory(_bounds.root);
    if (!rootEntries)
    {
        _candidates.clear();
        return std::unexpected(std::format(
            "cannot list the cgroup tree at {}: {}", _bounds.root.generic_string(), rootEntries.error().message()));
    }

    struct Pending
    {
        std::filesystem::path cgroup;
        std::uint32_t depth;
    };
    auto found = std::vector<ConfinedStall> {};
    auto pending = std::deque<Pending> { Pending { .cgroup = _bounds.root, .depth = 0 } };
    auto visited = std::uint32_t { 0 };
    while (!pending.empty() && visited < _bounds.maxVisited)
    {
        auto const [directory, depth] = pending.front();
        pending.pop_front();
        if (depth >= _bounds.maxDepth)
            continue;
        auto const entries = directory == _bounds.root ? rootEntries : _host->ListDirectory(directory);
        if (!entries)
            continue; // a subtree this node may not read: its stall counts as the machine's
        auto const controllers = _host->ReadFile(directory / "cgroup.controllers");
        for (auto const& name: ChildCandidates(*entries, controllers ? std::string_view { *controllers } : ""))
        {
            if (visited >= _bounds.maxVisited)
                break;
            auto const child = directory / name;
            auto look = Look(child);
            if (!look)
                continue;
            ++visited;
            // A cgroup's PSI covers its subtree: none below one that carries too little can carry enough.
            if (!Carries(look->psi, system, _bounds.share))
                continue;
            // Kept by its LIMIT, not by being at it: a rise is seen only between two looks, so a cgroup the walk first
            // meets cannot show one yet. Each sample after re-judges it.
            if (!Excluded(child) && look->limited)
                found.push_back(std::move(*look));
            // Never into this node's own subtree; through its ancestors, whose other children may be confined.
            if (!InsideExcluded(child))
                pending.push_back(Pending { .cgroup = child, .depth = depth + 1 });
        }
    }

    std::ranges::sort(found, std::ranges::greater {}, [](ConfinedStall const& stall) { return stall.psi.some.avg10; });
    if (found.size() > _bounds.maxCandidates)
        found.resize(_bounds.maxCandidates);
    _candidates.clear();
    for (auto const& stall: found)
        _candidates.push_back(stall.cgroup);
    return {};
}

std::expected<std::vector<ConfinedStall>, std::string> ConfinedStallScan::Read(PsiReading const& system)
{
    auto const now = _clock.now();
    auto const stalled = system.some.avg10 > 0.0 || (system.full && system.full->avg10 > 0.0);
    // Walked only while the machine stalls at all: nothing is carried of no stall, and a calm machine spends nothing.
    if (stalled && (!_walkedAt || now - *_walkedAt >= _bounds.walkInterval))
    {
        _walkedAt = now;
        if (auto walked = Walk(system); !walked)
            return std::unexpected(std::move(walked.error()));
    }
    auto stalls = std::vector<ConfinedStall> {};
    for (auto const& cgroup: _candidates)
        if (auto look = Look(cgroup); look)
            stalls.push_back(std::move(*look));
    return stalls;
}

std::expected<PsiReading, std::string> ParsePsi(std::string_view document)
{
    auto reading = PsiReading {};
    auto sawSome = false;
    for (auto const lineRange: std::views::split(document, '\n'))
    {
        auto const line = std::string_view { lineRange.begin(), lineRange.end() };
        if (line.empty())
            continue;
        auto const isSome = line.starts_with("some ");
        auto const isFull = line.starts_with("full ");
        auto const parsed = (isSome || isFull) ? ParseLine(line) : std::nullopt;
        if (!parsed)
            return std::unexpected(std::format("unreadable PSI line: \"{}\"", line));
        if (isSome)
        {
            reading.some = *parsed;
            sawSome = true;
        }
        else
            reading.full = *parsed;
    }
    if (!sawSome)
        return std::unexpected(std::string { "PSI document has no \"some\" line" });
    return reading;
}

PsiPressureSource::PsiPressureSource(std::shared_ptr<ICgroupHost const> host,
                                     std::optional<std::filesystem::path> compilesCgroup,
                                     std::unique_ptr<ConfinedStallScan> confined):
    _host { std::move(host) },
    _compiles { std::move(compilesCgroup) },
    _confined { std::move(confined) }
{
}

std::expected<PressureSample, std::string> PsiPressureSource::Sample()
{
    auto const read = [&](std::filesystem::path const& path) -> std::expected<PsiReading, std::string> {
        auto const text = _host->ReadFile(path);
        if (!text)
            return std::unexpected(std::format("cannot read {}: {}", path.generic_string(), text.error().message()));
        return ParsePsi(*text).transform_error(
            [&](std::string const& why) { return std::format("{}: {}", path.generic_string(), why); });
    };

    auto sample = PressureSample {};
    auto system = read(PsiSystemPath);
    if (!system)
        return std::unexpected(std::move(system.error()));
    sample.system = *system;
    if (_compiles)
    {
        auto compiles = read(*_compiles / PsiCgroupFileName);
        if (compiles)
            sample.compiles = *compiles;
        else
            sample.compilesFault = std::move(compiles.error());
        auto atLimit = CompilesAtLimit(*_compiles);
        if (atLimit)
            sample.compilesAtLimit = *atLimit;
        else
            sample.compilesLimitFault = std::move(atLimit.error());
    }
    if (_confined)
    {
        auto confined = _confined->Read(sample.system);
        if (confined)
            sample.confined = std::move(*confined);
        else
            sample.confinedFault = std::move(confined.error());
    }
    return sample;
}

std::expected<bool, std::string> PsiPressureSource::CompilesAtLimit(std::filesystem::path const& compiles)
{
    // Rising since the previous sample: the kernel counted the compiles reaching their ceiling in between.
    auto const eventsPath = compiles / "memory.events";
    auto const events = _host->ReadFile(eventsPath);
    auto const maxEvents = events ? KeyedOf(*events, "max") : std::nullopt;
    auto const rose = maxEvents && _maxEventsSeen && *maxEvents > *_maxEventsSeen;
    _maxEventsSeen = maxEvents;

    // Standing just under a numeric ceiling now. `max` (no ceiling) is a READING -- never near -- and not a fault.
    auto const current = _host->ReadFile(compiles / "memory.current");
    auto const ceiling = _host->ReadFile(compiles / "memory.max");
    auto const currentBytes = current ? DecimalOf(*current) : std::nullopt;
    auto const unlimited = ceiling && ceiling->starts_with("max");
    auto const ceilingBytes = ceiling ? DecimalOf(*ceiling) : std::nullopt;
    auto const near = currentBytes && ceilingBytes && *currentBytes >= *ceilingBytes - (*ceilingBytes / CeilingNearDivisor);

    if (!maxEvents && !(currentBytes && (ceilingBytes || unlimited)))
        return std::unexpected(std::format("neither {} nor memory.current and memory.max can be read in {}{}",
                                           eventsPath.filename().generic_string(),
                                           compiles.generic_string(),
                                           events ? std::string {} : std::format(" ({})", events.error().message())));
    return rose || near;
}

} // namespace Pace::Platform
