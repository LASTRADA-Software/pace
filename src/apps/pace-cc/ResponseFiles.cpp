// SPDX-License-Identifier: Apache-2.0
#include "ResponseFiles.hpp"

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Platform/NarrowText.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <utility>

namespace Pace::Cc
{

namespace
{
    /// One refusal and the sentence that explains it.
    struct RefusalRow
    {
        ResponseFileRefusal reason; ///< The refusal this row describes.
        std::string_view text;      ///< What an operator reads.
    };

    constexpr EnumTable<ResponseFileRefusal, RefusalRow> RefusalTable { {
        { .reason = ResponseFileRefusal::Unreadable, .text = "the response file could not be read" },
        { .reason = ResponseFileRefusal::Nested, .text = "the response file names another response file" },
        { .reason = ResponseFileRefusal::NotText,
          .text = "the response file has no byte-order mark and is not ASCII, so its encoding is unknown" },
        { .reason = ResponseFileRefusal::TooLong,
          .text = "the expanded command line is longer than a Windows process can be started with" },
    } };

    static_assert(RowsInEnumeratorOrder(RefusalTable, &RefusalRow::reason),
                  "RefusalTable must hold one row per ResponseFileRefusal, in enumerator order");

    /// Whitespace that separates arguments in both grammars.
    [[nodiscard]] constexpr bool IsSeparator(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }

    /// The Microsoft C runtime's argument rules, as `cl` and `clang-cl` apply them to a response file.
    [[nodiscard]] std::vector<std::string> SplitWindows(std::string_view text)
    {
        std::vector<std::string> args;
        std::string current;
        bool inArgument = false;
        bool inQuotes = false;
        std::size_t at = 0;
        while (at < text.size())
        {
            auto const c = text[at];
            if (!inQuotes && IsSeparator(c))
            {
                if (inArgument)
                    args.push_back(std::exchange(current, {}));
                inArgument = false;
                ++at;
                continue;
            }
            inArgument = true;
            if (c == '\\')
            {
                // A run of backslashes means something only when a quote follows it.
                auto const runEnd = std::min(text.find_first_not_of('\\', at), text.size());
                auto const run = runEnd - at;
                if (runEnd < text.size() && text[runEnd] == '"')
                {
                    current.append(run / 2, '\\');
                    if (run % 2 == 1)
                    {
                        current.push_back('"'); // an escaped, literal quote
                        at = runEnd + 1;
                        continue;
                    }
                    at = runEnd; // an even run: the quote toggles, on the next turn
                    continue;
                }
                current.append(run, '\\');
                at = runEnd;
                continue;
            }
            if (c == '"')
            {
                // `""` inside quotes is one literal quote.
                if (inQuotes && at + 1 < text.size() && text[at + 1] == '"')
                {
                    current.push_back('"');
                    at += 2;
                    continue;
                }
                inQuotes = !inQuotes;
                ++at;
                continue;
            }
            current.push_back(c);
            ++at;
        }
        if (inArgument)
            args.push_back(std::move(current));
        return args;
    }

    /// libiberty's `buildargv`, as gcc and clang apply it to a response file.
    [[nodiscard]] std::vector<std::string> SplitGnu(std::string_view text)
    {
        std::vector<std::string> args;
        std::string current;
        bool inArgument = false;
        bool singleQuoted = false;
        bool doubleQuoted = false;
        bool escaped = false;
        for (auto const c: text)
        {
            if (!singleQuoted && !doubleQuoted && !escaped && IsSeparator(c))
            {
                if (inArgument)
                    args.push_back(std::exchange(current, {}));
                inArgument = false;
                continue;
            }
            inArgument = true;
            // libiberty's order: a pending escape first, then a backslash, then the quotes --
            // which is why a backslash escapes even inside single quotes there.
            if (escaped)
            {
                current.push_back(c);
                escaped = false;
            }
            else if (c == '\\')
                escaped = true;
            else if (singleQuoted)
            {
                if (c == '\'')
                    singleQuoted = false;
                else
                    current.push_back(c);
            }
            else if (doubleQuoted)
            {
                if (c == '"')
                    doubleQuoted = false;
                else
                    current.push_back(c);
            }
            else if (c == '\'')
                singleQuoted = true;
            else if (c == '"')
                doubleQuoted = true;
            else
                current.push_back(c);
        }
        if (inArgument)
            args.push_back(std::move(current));
        return args;
    }

    /// A response file's bytes as UTF-8 text, or why they cannot be read as such.
    [[nodiscard]] std::expected<std::string, ResponseFileRefusal> DecodeResponseFile(std::string bytes, DriverFamily family)
    {
        constexpr std::string_view Utf8Bom = "\xEF\xBB\xBF";
        constexpr std::string_view Utf16LeBom = "\xFF\xFE";
        if (bytes.starts_with(Utf16LeBom))
        {
            std::u16string units;
            auto const body = std::string_view { bytes }.substr(Utf16LeBom.size());
            // Whole units only: a trailing odd byte is no unit at all.
            units.reserve(body.size() / 2);
            for (auto const unit: std::views::iota(std::size_t { 0 }, body.size() / 2))
            {
                auto const low = static_cast<unsigned>(static_cast<unsigned char>(body[2 * unit]));
                auto const high = static_cast<unsigned>(static_cast<unsigned char>(body[(2 * unit) + 1]));
                units.push_back(static_cast<char16_t>(low | (high << 8U)));
            }
            return Wtf8FromUtf16(units);
        }
        if (bytes.starts_with(Utf8Bom))
            return bytes.substr(Utf8Bom.size());
        // `cl` reads a BOM-less response file in the ANSI code page, which this process cannot
        // name; ASCII is the one encoding every candidate agrees on.
        if (family == DriverFamily::Msvc
            && std::ranges::any_of(bytes, [](char c) { return static_cast<unsigned char>(c) > 0x7F; }))
            return std::unexpected(ResponseFileRefusal::NotText);
        return bytes;
    }

    /// A rough length of @p args as one Windows command line: each argument plus a separator and
    /// a pair of quotes. An over-estimate, which is the safe direction for a budget.
    [[nodiscard]] std::size_t WindowsCommandLineLength(std::span<std::string const> args) noexcept
    {
        std::size_t length = 0;
        for (auto const& arg: args)
            length += arg.size() + 3;
        return length;
    }
} // namespace

std::vector<std::string> SplitResponseFile(std::string_view text, DriverFamily family)
{
    return family == DriverFamily::Msvc ? SplitWindows(text) : SplitGnu(text);
}

std::expected<std::vector<std::string>, ResponseFileFailure> ExpandResponseFiles(std::span<std::string const> args,
                                                                                 DriverFamily family,
                                                                                 ResponseFileReader const& read)
{
    std::vector<std::string> expanded;
    expanded.reserve(args.size());
    if (args.empty())
        return expanded;
    // args[0] is the compiler, and is never a response file.
    expanded.push_back(args.front());
    bool expandedAny = false;
    for (auto const& arg: args.subspan(1))
    {
        // A lone `@` names no file.
        if (!arg.starts_with('@') || arg.size() == 1)
        {
            expanded.push_back(arg);
            continue;
        }
        auto const refuse = [&arg](ResponseFileRefusal reason) {
            return std::unexpected(ResponseFileFailure { .reason = reason, .argument = arg });
        };
        auto bytes = read(std::string_view { arg }.substr(1));
        if (!bytes.has_value())
            return refuse(ResponseFileRefusal::Unreadable);
        auto text = DecodeResponseFile(*std::move(bytes), family);
        if (!text.has_value())
            return refuse(text.error());
        auto tokens = SplitResponseFile(*text, family);
        if (std::ranges::any_of(tokens, [](std::string const& token) { return token.starts_with('@'); }))
            return refuse(ResponseFileRefusal::Nested);
        std::ranges::move(tokens, std::back_inserter(expanded));
        expandedAny = true;
    }
    if (expandedAny && family == DriverFamily::Msvc && WindowsCommandLineLength(expanded) > WindowsCommandLineBudget)
    {
        auto const named = args.subspan(1);
        auto const first =
            std::ranges::find_if(named, [](std::string const& arg) { return arg.starts_with('@') && arg.size() > 1; });
        return std::unexpected(ResponseFileFailure { .reason = ResponseFileRefusal::TooLong,
                                                     .argument = first == named.end() ? std::string {} : *first });
    }
    return expanded;
}

std::string DescribeResponseFileFailure(ResponseFileFailure const& failure)
{
    return std::format("{}: {}", RefusalTable[static_cast<std::size_t>(failure.reason)].text, failure.argument);
}

} // namespace Pace::Cc
