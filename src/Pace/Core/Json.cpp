// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Json.hpp>
#include <Pace/Core/Markup.hpp>
#include <Pace/Core/NumericText.hpp>
#include <Pace/Core/Utf8.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>

namespace Pace::Json
{

Value const* Value::Find(std::string_view key) const noexcept
{
    auto const* object = AsObject();
    if (object == nullptr)
        return nullptr;
    auto const found = std::ranges::find_if(*object, [key](Member const& member) { return member.first == key; });
    return found == object->end() ? nullptr : &found->second;
}

namespace
{
    /// A `\X` escape that stands for one character.
    struct SimpleEscape
    {
        char letter;  ///< The character after the backslash.
        char decoded; ///< What it stands for.
    };

    inline constexpr std::array SimpleEscapes {
        SimpleEscape { .letter = '"', .decoded = '"' },  SimpleEscape { .letter = '\\', .decoded = '\\' },
        SimpleEscape { .letter = '/', .decoded = '/' },  SimpleEscape { .letter = 'b', .decoded = '\b' },
        SimpleEscape { .letter = 'f', .decoded = '\f' }, SimpleEscape { .letter = 'n', .decoded = '\n' },
        SimpleEscape { .letter = 'r', .decoded = '\r' }, SimpleEscape { .letter = 't', .decoded = '\t' },
    };

    /// A bare word and the value it stands for.
    struct Literal
    {
        std::string_view word; ///< `true`, `false` or `null`.
        Value (*make)();       ///< The value.
    };

    inline constexpr std::array Literals {
        Literal { .word = "true", .make = [] { return Value { .v = true }; } },
        Literal { .word = "false", .make = [] { return Value { .v = false }; } },
        Literal { .word = "null", .make = [] { return Value { .v = nullptr }; } },
    };

    /// First control character: below it a string may not carry a byte raw.
    inline constexpr unsigned char FirstUnescapedByte = 0x20;
    inline constexpr char32_t FirstHighSurrogate = 0xD800;
    inline constexpr char32_t FirstLowSurrogate = 0xDC00;
    inline constexpr char32_t SurrogateSpan = 0x400;
    inline constexpr char32_t FirstSupplementary = 0x10000;
    inline constexpr unsigned HexDigitsInEscape = 4;
    inline constexpr unsigned BitsPerHexDigit = 4;

    /// @return The digit's value, or nothing when @p ch is not a hex digit.
    [[nodiscard]] std::optional<unsigned> HexValue(char ch) noexcept
    {
        if (ch >= '0' && ch <= '9')
            return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<unsigned>(ch - 'a') + 10U;
        if (ch >= 'A' && ch <= 'F')
            return static_cast<unsigned>(ch - 'A') + 10U;
        return std::nullopt;
    }

    [[nodiscard]] bool IsDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '9';
    }

    /// Append @p cp as UTF-8. @p cp is a scalar value (never a surrogate, never above U+10FFFF).
    void AppendCodePoint(std::string& out, char32_t cp)
    {
        // Lead-byte rows of the encoder, the inverse of `Utf8LeadClasses`.
        for (auto const& row: Utf8LeadClasses | std::views::drop(1) | std::views::reverse)
        {
            if (cp < row.lowest)
                continue;
            auto const shift = row.continuations * Utf8ContinuationBits;
            out += static_cast<char>(row.first | static_cast<std::uint8_t>(cp >> shift));
            for (auto const index: std::views::iota(std::size_t { 0 }, row.continuations))
            {
                auto const down = (row.continuations - 1 - index) * Utf8ContinuationBits;
                out += static_cast<char>(Utf8ContinuationMark | ((cp >> down) & Utf8ContinuationPayload));
            }
            return;
        }
        out += static_cast<char>(cp);
    }

    /// Recursive-descent parser. Depth is counted, so recursion is bounded by `ParseLimits::maxDepth`.
    class Parser
    {
      public:
        Parser(std::string_view text, ParseLimits limits) noexcept:
            _text { text },
            _limits { limits }
        {
        }

        /// @return The whole document's value, or the first refusal.
        [[nodiscard]] std::expected<Value, Error> ParseDocument()
        {
            if (_text.size() > _limits.maxBytes)
                return Fail(ErrorCode::TooLarge, _limits.maxBytes);
            auto value = ParseValue();
            if (!value)
                return value;
            SkipWhitespace();
            if (_pos < _text.size())
                return Fail(ErrorCode::TrailingData, _pos);
            return value;
        }

      private:
        [[nodiscard]] static std::unexpected<Error> Fail(ErrorCode code, std::size_t offset) noexcept
        {
            return std::unexpected { Error { .code = code, .offset = offset } };
        }

        [[nodiscard]] bool AtEnd() const noexcept
        {
            return _pos >= _text.size();
        }

        void SkipWhitespace() noexcept
        {
            while (!AtEnd() && (_text[_pos] == ' ' || _text[_pos] == '\t' || _text[_pos] == '\n' || _text[_pos] == '\r'))
                ++_pos;
        }

        [[nodiscard]] std::expected<Value, Error> ParseValue()
        {
            SkipWhitespace();
            if (AtEnd())
                return Fail(ErrorCode::UnexpectedEnd, _text.size());

            auto const ch = _text[_pos];
            if (ch == '{')
                return ParseObject();
            if (ch == '[')
                return ParseArray();
            if (ch == '"')
                return ParseString().transform([](std::string text) { return Value { .v = std::move(text) }; });
            if (ch == '-' || IsDigit(ch))
                return ParseNumber();

            for (auto const& literal: Literals)
            {
                if (literal.word.front() == ch)
                    return ParseLiteral(literal);
            }
            return Fail(ErrorCode::UnexpectedCharacter, _pos);
        }

        [[nodiscard]] std::expected<Value, Error> ParseLiteral(Literal const& literal)
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, literal.word.size()))
            {
                if (_pos + index >= _text.size())
                    return Fail(ErrorCode::UnexpectedEnd, _text.size());
                if (_text[_pos + index] != literal.word[index])
                    return Fail(ErrorCode::UnexpectedCharacter, _pos + index);
            }
            _pos += literal.word.size();
            return literal.make();
        }

        [[nodiscard]] std::size_t DigitRun() noexcept
        {
            auto const first = _pos;
            while (!AtEnd() && IsDigit(_text[_pos]))
                ++_pos;
            return _pos - first;
        }

        [[nodiscard]] std::expected<Value, Error> ParseNumber()
        {
            auto const start = _pos;
            auto integral = true;
            if (_text[_pos] == '-')
                ++_pos;
            if (AtEnd() || !IsDigit(_text[_pos]))
                return Fail(ErrorCode::InvalidNumber, start);
            if (_text[_pos] == '0')
            {
                ++_pos;
                if (!AtEnd() && IsDigit(_text[_pos]))
                    return Fail(ErrorCode::InvalidNumber, start); // no leading zeros
            }
            else
                std::ignore = DigitRun();

            if (!AtEnd() && _text[_pos] == '.')
            {
                integral = false;
                ++_pos;
                if (DigitRun() == 0)
                    return Fail(ErrorCode::InvalidNumber, start);
            }
            if (!AtEnd() && (_text[_pos] == 'e' || _text[_pos] == 'E'))
            {
                integral = false;
                ++_pos;
                if (!AtEnd() && (_text[_pos] == '+' || _text[_pos] == '-'))
                    ++_pos;
                if (DigitRun() == 0)
                    return Fail(ErrorCode::InvalidNumber, start);
            }

            auto const token = _text.substr(start, _pos - start);
            if (integral)
            {
                std::int64_t whole = 0;
                auto const [end, ec] = std::from_chars(token.data(), token.data() + token.size(), whole);
                if (ec == std::errc {})
                    return Value { .v = whole };
                // Out of range is a number too large for an integer, never a wrapped one; it becomes a double.
            }
            double real = 0;
            if (!ParseFiniteDouble(token, real))
                return Fail(ErrorCode::InvalidNumber, start);
            return Value { .v = real };
        }

        /// Read four hex digits at @p at.
        [[nodiscard]] std::optional<char32_t> HexQuad(std::size_t at) const noexcept
        {
            if (at + HexDigitsInEscape > _text.size())
                return std::nullopt;
            char32_t value = 0;
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { HexDigitsInEscape }))
            {
                auto const digit = HexValue(_text[at + index]);
                if (!digit)
                    return std::nullopt;
                value = (value << BitsPerHexDigit) | *digit;
            }
            return value;
        }

        /// Decode the `\u` escape whose backslash is at `_pos` (a surrogate pair included), appending it.
        [[nodiscard]] std::optional<Error> ParseUnicodeEscape(std::string& out)
        {
            auto const backslash = _pos;
            auto const bad = Error { .code = ErrorCode::InvalidEscape, .offset = backslash };
            auto const first = HexQuad(backslash + 2);
            if (!first)
                return bad;
            auto const escapeLength = std::size_t { 2 + HexDigitsInEscape };
            auto cp = *first;
            auto consumed = escapeLength;
            if (cp >= FirstHighSurrogate && cp < FirstLowSurrogate)
            {
                if (backslash + escapeLength + 1 >= _text.size() || _text[backslash + escapeLength] != '\\'
                    || _text[backslash + escapeLength + 1] != 'u')
                    return bad;
                auto const second = HexQuad(backslash + escapeLength + 2);
                if (!second || *second < FirstLowSurrogate || *second >= FirstLowSurrogate + SurrogateSpan)
                    return bad;
                cp = FirstSupplementary + ((cp - FirstHighSurrogate) * SurrogateSpan) + (*second - FirstLowSurrogate);
                consumed += escapeLength;
            }
            else if (cp >= FirstLowSurrogate && cp < FirstLowSurrogate + SurrogateSpan)
                return bad; // a low surrogate with no high one before it

            auto decoded = std::string {};
            AppendCodePoint(decoded, cp);
            if (out.size() + decoded.size() > _limits.maxStringBytes)
                return Error { .code = ErrorCode::StringTooLong, .offset = backslash };
            out += decoded;
            _pos += consumed;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Error> ParseEscape(std::string& out)
        {
            if (_pos + 1 >= _text.size())
                return Error { .code = ErrorCode::UnexpectedEnd, .offset = _text.size() };
            auto const letter = _text[_pos + 1];
            if (letter == 'u')
                return ParseUnicodeEscape(out);

            for (auto const& simple: SimpleEscapes)
            {
                if (simple.letter != letter)
                    continue;
                if (out.size() + 1 > _limits.maxStringBytes)
                    return Error { .code = ErrorCode::StringTooLong, .offset = _pos };
                out += simple.decoded;
                _pos += 2;
                return std::nullopt;
            }
            return Error { .code = ErrorCode::InvalidEscape, .offset = _pos };
        }

        /// Parse the string whose opening quote is at `_pos`.
        [[nodiscard]] std::expected<std::string, Error> ParseString()
        {
            ++_pos;
            auto out = std::string {};
            while (true)
            {
                if (AtEnd())
                    return Fail(ErrorCode::UnexpectedEnd, _text.size());
                auto const ch = _text[_pos];
                if (ch == '"')
                {
                    ++_pos;
                    return out;
                }
                if (ch == '\\')
                {
                    if (auto error = ParseEscape(out))
                        return std::unexpected { *error };
                    continue;
                }
                if (static_cast<unsigned char>(ch) < FirstUnescapedByte)
                    return Fail(ErrorCode::UnexpectedCharacter, _pos);

                auto const decoded = DecodeUtf8(_text.substr(_pos));
                if (!decoded)
                    return Fail(ErrorCode::InvalidUtf8, _pos);
                if (out.size() + decoded->length > _limits.maxStringBytes)
                    return Fail(ErrorCode::StringTooLong, _pos);
                out.append(_text.substr(_pos, decoded->length));
                _pos += decoded->length;
            }
        }

        /// Opens a container at `_pos`: counts the depth, or refuses.
        [[nodiscard]] std::optional<Error> Enter()
        {
            if (_depth >= _limits.maxDepth)
                return Error { .code = ErrorCode::TooDeep, .offset = _pos };
            ++_depth;
            ++_pos;
            return std::nullopt;
        }

        /// After an element: consume `,` (true) or the closer (false), or refuse.
        [[nodiscard]] std::expected<bool, Error> NextOrClose(char closer)
        {
            SkipWhitespace();
            if (AtEnd())
                return Fail(ErrorCode::UnexpectedEnd, _text.size());
            if (_text[_pos] == ',')
            {
                ++_pos;
                return true;
            }
            if (_text[_pos] == closer)
            {
                ++_pos;
                return false;
            }
            return Fail(ErrorCode::UnexpectedCharacter, _pos);
        }

        [[nodiscard]] std::expected<Value, Error> ParseArray()
        {
            if (auto error = Enter())
                return std::unexpected { *error };
            auto elements = Array {};
            SkipWhitespace();
            if (!AtEnd() && _text[_pos] == ']')
            {
                ++_pos;
                --_depth;
                return Value { .v = std::move(elements) };
            }
            auto more = true;
            while (more)
            {
                SkipWhitespace();
                if (elements.size() >= _limits.maxContainerElements)
                    return Fail(ErrorCode::TooManyElements, _pos);
                auto element = ParseValue();
                if (!element)
                    return element;
                elements.push_back(std::move(*element));
                auto next = NextOrClose(']');
                if (!next)
                    return std::unexpected { next.error() };
                more = *next;
            }
            --_depth;
            return Value { .v = std::move(elements) };
        }

        [[nodiscard]] std::expected<Member, Error> ParseMember(Object const& so_far)
        {
            SkipWhitespace();
            if (AtEnd())
                return Fail(ErrorCode::UnexpectedEnd, _text.size());
            if (_text[_pos] != '"')
                return Fail(ErrorCode::UnexpectedCharacter, _pos);
            auto const keyStart = _pos;
            auto key = ParseString();
            if (!key)
                return std::unexpected { key.error() };
            if (std::ranges::any_of(so_far, [&key](Member const& member) { return member.first == *key; }))
                return Fail(ErrorCode::DuplicateKey, keyStart);

            SkipWhitespace();
            if (AtEnd())
                return Fail(ErrorCode::UnexpectedEnd, _text.size());
            if (_text[_pos] != ':')
                return Fail(ErrorCode::UnexpectedCharacter, _pos);
            ++_pos;

            auto value = ParseValue();
            if (!value)
                return std::unexpected { value.error() };
            return Member { std::move(*key), std::move(*value) };
        }

        [[nodiscard]] std::expected<Value, Error> ParseObject()
        {
            if (auto error = Enter())
                return std::unexpected { *error };
            auto members = Object {};
            SkipWhitespace();
            if (!AtEnd() && _text[_pos] == '}')
            {
                ++_pos;
                --_depth;
                return Value { .v = std::move(members) };
            }
            auto more = true;
            while (more)
            {
                SkipWhitespace();
                if (members.size() >= _limits.maxContainerElements)
                    return Fail(ErrorCode::TooManyElements, _pos);
                auto member = ParseMember(members);
                if (!member)
                    return std::unexpected { member.error() };
                members.push_back(std::move(*member));
                auto next = NextOrClose('}');
                if (!next)
                    return std::unexpected { next.error() };
                more = *next;
            }
            --_depth;
            return Value { .v = std::move(members) };
        }

        std::string_view _text;
        ParseLimits _limits;
        std::size_t _pos = 0;
        std::size_t _depth = 0;
    };

    /// Write a double so it parses back as a double.
    void AppendReal(std::string& out, double real)
    {
        if (!std::isfinite(real))
        {
            out += "null"; // JSON has no spelling for NaN or infinity
            return;
        }
        auto text = std::format("{}", real);
        if (text.find_first_of(".e") == std::string::npos)
            text += ".0";
        out += text;
    }

    void Write(std::string& out, Value const& value);

    void WriteArray(std::string& out, Array const& array)
    {
        out += '[';
        auto first = true;
        for (auto const& element: array)
        {
            if (!std::exchange(first, false))
                out += ',';
            Write(out, element);
        }
        out += ']';
    }

    void WriteObject(std::string& out, Object const& object)
    {
        out += '{';
        auto first = true;
        for (auto const& [key, member]: object)
        {
            if (!std::exchange(first, false))
                out += ',';
            AppendString(out, key);
            out += ':';
            Write(out, member);
        }
        out += '}';
    }

    void Write(std::string& out, Value const& value)
    {
        std::visit(
            [&out](auto const& held) {
                using Held = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, std::nullptr_t>)
                    out += "null";
                else if constexpr (std::is_same_v<Held, bool>)
                    out += held ? "true" : "false";
                else if constexpr (std::is_same_v<Held, std::int64_t>)
                    out += std::to_string(held);
                else if constexpr (std::is_same_v<Held, double>)
                    AppendReal(out, held);
                else if constexpr (std::is_same_v<Held, std::string>)
                    AppendString(out, held);
                else if constexpr (std::is_same_v<Held, Array>)
                    WriteArray(out, held);
                else
                    WriteObject(out, held);
            },
            value.v);
    }

    /// Short escapes JSON spells with a letter.
    inline constexpr std::array JsonShortEscapes {
        TextEscape { .byte = '"', .spelling = "\\\"" }, TextEscape { .byte = '\\', .spelling = "\\\\" },
        TextEscape { .byte = '\b', .spelling = "\\b" }, TextEscape { .byte = '\f', .spelling = "\\f" },
        TextEscape { .byte = '\n', .spelling = "\\n" }, TextEscape { .byte = '\r', .spelling = "\\r" },
        TextEscape { .byte = '\t', .spelling = "\\t" },
    };
} // namespace

std::expected<Value, Error> Parse(std::string_view text, ParseLimits const& limits)
{
    return Parser { text, limits }.ParseDocument();
}

std::string Serialize(Value const& value)
{
    auto out = std::string {};
    Write(out, value);
    return out;
}

void AppendString(std::string& out, std::string_view utf8)
{
    out += '"';
    while (!utf8.empty())
    {
        auto const ch = utf8.front();
        if (auto const escape = EscapeFor(JsonShortEscapes, ch); !escape.empty())
        {
            out += escape;
            utf8.remove_prefix(1);
        }
        else if (static_cast<unsigned char>(ch) < FirstUnescapedByte)
        {
            out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
            utf8.remove_prefix(1);
        }
        else if (auto const decoded = DecodeUtf8(utf8); decoded.has_value())
        {
            out += utf8.substr(0, decoded->length);
            utf8.remove_prefix(decoded->length);
        }
        else
        {
            out += ReplacementEscape;
            utf8.remove_prefix(1);
        }
    }
    out += '"';
}

} // namespace Pace::Json
