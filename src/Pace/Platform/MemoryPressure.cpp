// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/NumericText.hpp>
#include <Pace/Platform/MemoryPressure.hpp>

#include <array>
#include <charconv>
#include <format>
#include <ranges>
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

} // namespace

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
                                     std::optional<std::filesystem::path> compilesCgroup):
    _host { std::move(host) },
    _compiles { std::move(compilesCgroup) }
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
