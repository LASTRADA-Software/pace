// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pace::Cc
{

/// Expanding `@file` response-file arguments before the command line is analysed (#1756).
///
/// ## Why expand, rather than key the file's contents
///
/// A response file IS command line: `cl @args.rsp` compiles exactly as if the file's tokens were
/// typed in its place. Every question the launcher asks of a command line -- what the key
/// hashes, whether the compile writes a second artefact (`/Zi`, `-gsplit-dwarf`), what a worker
/// may be sent -- was asked of the file's NAME instead, so changing `/DPROBE=rsp1` to
/// `/DPROBE=rsp2` inside it kept the key and served the old object. Hashing the contents would
/// close the key half and leave every other check reading a name. Expanding first means
/// everything downstream reads what the compiler reads, and it is what ccache does.
///
/// ## The two grammars
///
/// - **Windows** (`cl`, `clang-cl`): the Microsoft C runtime's argument rules -- whitespace
///   (newlines included) separates; `"` toggles quoting; `2n` backslashes before a quote are `n`
///   backslashes and the quote toggles, `2n+1` are `n` and a literal quote; a backslash anywhere
///   else is literal; `""` inside quotes is one literal quote. A UTF-16LE file (BOM `FF FE`) is
///   decoded; a UTF-8 BOM is dropped.
/// - **GNU** (`gcc`, `clang`): libiberty's `buildargv` -- whitespace separates; `'...'` is
///   literal; inside `"..."` and outside any quote a backslash escapes the next character.
///
/// ## What is refused, and what a refusal costs
///
/// A refusal means the compile runs exactly as written and is not cached: never a guess at what
/// the compiler would have read.
/// - `Unreadable` -- the file could not be opened. The compiler will say so itself.
/// - `Nested` -- an expanded token begins with `@`. Drivers disagree on whether that nests.
/// - `NotText` -- a Windows-grammar file without a BOM carrying a byte above 0x7F: `cl` reads
///   such a file in the ANSI code page, which this process cannot name, so its paths are unknown.
/// - `TooLong` -- the expanded command line would not fit the Windows `CreateProcess` limit,
///   which is usually why the build used a response file at all.
///
/// Private to the launcher, never transmitted or persisted.
enum class ResponseFileRefusal : std::uint8_t
{
    Unreadable,
    Nested,
    NotText,
    TooLong,
    Last, ///< The count, not a refusal (`EnumTable`).
};

/// A response file the launcher would not expand, and why.
struct ResponseFileFailure
{
    ResponseFileRefusal reason; ///< Which refusal.
    std::string argument;       ///< The `@file` argument it concerns, as written.

    [[nodiscard]] friend bool operator==(ResponseFileFailure const&, ResponseFileFailure const&) = default;
};

/// Reads a whole file as bytes; `std::nullopt` when it cannot be read. Injected so expansion is
/// testable without a filesystem, and so a relative `@file` resolves exactly where the compiler's
/// would: against the process working directory the reader was handed.
using ResponseFileReader = std::function<std::optional<std::string>(std::string_view path)>;

/// The longest command line, in characters, `CreateProcess` accepts (32767), less a margin for
/// the program path and the quoting the spawn adds. A Windows-grammar expansion longer than this
/// is refused `TooLong` rather than handed to a spawn that would fail.
inline constexpr std::size_t WindowsCommandLineBudget = 30'000;

/// Split one response file's text into arguments by @p family's grammar.
/// @param text   The file's contents, already decoded to UTF-8.
/// @param family `DriverFamily::Msvc` for the Windows grammar, anything else for GNU.
/// @return The arguments, in order.
[[nodiscard]] std::vector<std::string> SplitResponseFile(std::string_view text, DriverFamily family);

/// Replace every `@file` argument of @p args with the file's arguments.
/// @param args   The full invocation; `args[0]` is the compiler and is never expanded.
/// @param family The driver's family, choosing the grammar.
/// @param read   Reads a response file.
/// @return The expanded invocation (identical to @p args when there was nothing to expand), or
///         the first response file that could not be expanded.
[[nodiscard]] std::expected<std::vector<std::string>, ResponseFileFailure> ExpandResponseFiles(
    std::span<std::string const> args, DriverFamily family, ResponseFileReader const& read);

/// One sentence for a refusal, for the launcher's verbose note.
/// @param failure The refusal.
/// @return Human-readable text naming the argument.
[[nodiscard]] std::string DescribeResponseFileFailure(ResponseFileFailure const& failure);

} // namespace Pace::Cc
