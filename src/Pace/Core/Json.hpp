// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Pace::Json
{

/// @file Json.hpp
/// A strict, bounded JSON parser and a compact writer.
///
/// **Strict** is RFC 8259 and nothing laxer: no comments, no trailing commas, no `NaN`/`Infinity`, no
/// leading zeros, no lone surrogate escape, no byte that is not UTF-8, no duplicate object key, and
/// nothing after the value. **Bounded** because the text comes off a socket: every size, depth and count
/// is a field of `ParseLimits`, so hostile input is refused with an `Error` and never costs more than the
/// limits say. Nothing here throws, and nothing is read outside `text`.

struct Value;

/// A JSON array.
using Array = std::vector<Value>;

/// One `"key": value` pair of an object.
using Member = std::pair<std::string, Value>;

/// A JSON object. It keeps insertion order, and keys are unique (a duplicate is refused at parse time).
using Object = std::vector<Member>;

/// One JSON value.
///
/// A number with no fraction and no exponent that fits `std::int64_t` is an integer; every other number is
/// a `double`. Text that does not fit is never wrapped.
struct Value
{
    /// The alternative held, in the order `null`, boolean, integer, real, string, array, object.
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> v {};

    /// @return True when this is `null`.
    [[nodiscard]] bool IsNull() const noexcept
    {
        return std::holds_alternative<std::nullptr_t>(v);
    }

    /// @return The boolean, or nothing when this is not one.
    [[nodiscard]] std::optional<bool> AsBool() const noexcept
    {
        if (auto const* held = std::get_if<bool>(&v))
            return *held;
        return std::nullopt;
    }

    /// @return The integer, or nothing when this is not an integer (a `double` is not converted).
    [[nodiscard]] std::optional<std::int64_t> AsInt() const noexcept
    {
        if (auto const* held = std::get_if<std::int64_t>(&v))
            return *held;
        return std::nullopt;
    }

    /// @return The string, or null when this is not one. Borrows from this value.
    [[nodiscard]] std::string const* AsString() const noexcept
    {
        return std::get_if<std::string>(&v);
    }

    /// @return The array, or null when this is not one. Borrows from this value.
    [[nodiscard]] Array const* AsArray() const noexcept
    {
        return std::get_if<Array>(&v);
    }

    /// @return The object, or null when this is not one. Borrows from this value.
    [[nodiscard]] Object const* AsObject() const noexcept
    {
        return std::get_if<Object>(&v);
    }

    /// Look a member up by key.
    /// @param key The key to find.
    /// @return The member's value, or null when this is not an object or has no such key.
    [[nodiscard]] Value const* Find(std::string_view key) const noexcept;
};

/// What a parse will accept. Every field is a ceiling, and a document AT the value is accepted.
struct ParseLimits
{
    std::size_t maxBytes = 64 * 1024;        ///< Longest document, in bytes.
    std::size_t maxDepth = 32;               ///< Deepest nesting of arrays and objects.
    std::size_t maxStringBytes = 16 * 1024;  ///< Longest string or key, in DECODED bytes.
    std::size_t maxContainerElements = 4096; ///< Most elements of one array, or members of one object.
};

/// Why a parse was refused.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted.
enum class ErrorCode : std::uint8_t
{
    TooLarge,            ///< The document is longer than `maxBytes`; the offset is `maxBytes`.
    TooDeep,             ///< A container opens deeper than `maxDepth`; the offset is its bracket.
    InvalidUtf8,         ///< A byte starts no valid UTF-8 sequence; the offset is that byte.
    UnexpectedEnd,       ///< The text ended inside a value; the offset is its length.
    UnexpectedCharacter, ///< A character the grammar does not allow here (a raw control character included).
    InvalidEscape,       ///< A bad `\` escape or a lone surrogate; the offset is the backslash.
    InvalidNumber,       ///< A malformed number, or one that is not finite; the offset is its first character.
    DuplicateKey,        ///< An object repeats a key; the offset is the second occurrence's opening quote.
    TrailingData,        ///< Something other than whitespace follows the value; the offset is its first byte.
    TooManyElements,     ///< A container holds more than `maxContainerElements`; the offset is the extra element.
    StringTooLong,       ///< A string decodes past `maxStringBytes`; the offset is the character that overflowed.
    Last,
};

/// A refusal, and where in the text it was found.
struct Error
{
    ErrorCode code;     ///< Why.
    std::size_t offset; ///< Byte offset into the text; never past its size.
};

/// Parse one JSON document.
/// @param text The document, as received.
/// @param limits The ceilings to enforce.
/// @return The value, or the first refusal found.
[[nodiscard]] std::expected<Value, Error> Parse(std::string_view text, ParseLimits const& limits = {});

/// Write a value as compact JSON (no whitespace), per RFC 8259, with non-ASCII emitted as UTF-8.
///
/// A `double` that is not finite has no JSON spelling and is written as `null`; a finite one is written
/// so that it parses back as a `double` (`2.0`, never `2`).
/// @param value The value.
/// @return The text.
[[nodiscard]] std::string Serialize(Value const& value);

/// Append one JSON string literal, quotes included.
///
/// Escapes `"`, `\` and every control character; a byte that belongs to no valid UTF-8 sequence is
/// written as `�`, so what is appended is always valid JSON whatever @p utf8 held.
/// @param out Where to append.
/// @param utf8 The string's contents.
void AppendString(std::string& out, std::string_view utf8);

} // namespace Pace::Json
