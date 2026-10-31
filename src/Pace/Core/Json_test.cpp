// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/IRandomSource.hpp>
#include <Pace/Core/Json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace std::string_view_literals;
using Pace::Testing::Unwrap;
namespace J = Pace::Json;

namespace
{
std::string Repeat(std::string_view piece, std::size_t count)
{
    auto out = std::string {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
    {
        std::ignore = index;
        out += piece;
    }
    return out;
}

/// `[` repeated @p depth times, then `]` the same number of times.
std::string Nested(std::size_t depth)
{
    return Repeat("[", depth) + Repeat("]", depth);
}

/// An array of @p count ones.
std::string OnesArray(std::size_t count)
{
    return "[" + Repeat("1,", count == 0 ? 0 : count - 1) + (count == 0 ? "" : "1") + "]";
}

/// An object of @p count distinct keys.
std::string KeysObject(std::size_t count)
{
    auto out = std::string { "{" };
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        out += std::format("{}\"k{}\":0", index == 0 ? "" : ",", index);
    return out + "}";
}

struct Refusal
{
    std::string input;
    J::ParseLimits limits;
    J::ErrorCode code;
    std::size_t offset;
};

} // namespace

TEST_CASE("json parses every value kind", "[json]")
{
    auto const text = R"({"a":[1,-2,3.5,true,false,null,"x\u00e9"]})"sv;
    auto const parsed = J::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(J::Serialize(*parsed) == "{\"a\":[1,-2,3.5,true,false,null,\"x\xC3\xA9\"]}"sv);

    auto const* array = parsed->Find("a")->AsArray();
    REQUIRE(array != nullptr);
    REQUIRE(array->size() == 7);
    CHECK((*array)[0].AsInt() == 1);
    CHECK((*array)[1].AsInt() == -2);
    CHECK(std::get<double>((*array)[2].v) == 3.5);
    CHECK((*array)[3].AsBool() == true);
    CHECK((*array)[4].AsBool() == false);
    CHECK((*array)[5].IsNull());
    CHECK(*(*array)[6].AsString() == "x\xC3\xA9"sv);

    CHECK(parsed->Find("missing") == nullptr);
    CHECK((*array)[0].Find("a") == nullptr);
    CHECK((*array)[0].AsString() == nullptr);
    CHECK_FALSE((*array)[0].AsBool().has_value());
}

TEST_CASE("json object keeps insertion order", "[json]")
{
    auto const parsed = Unwrap(J::Parse(R"({"b":1,"a":2,"c":3})"sv));
    auto const* object = parsed.AsObject();
    REQUIRE(object != nullptr);
    REQUIRE(object->size() == 3);
    CHECK((*object)[0].first == "b");
    CHECK((*object)[1].first == "a");
    CHECK((*object)[2].first == "c");
    CHECK(J::Serialize(parsed) == R"({"b":1,"a":2,"c":3})");
}

TEST_CASE("json integers outside int64 become double, never wrap", "[json]")
{
    auto const big = Unwrap(J::Parse("9223372036854775808"sv));
    CHECK_FALSE(big.AsInt().has_value());
    CHECK(std::get<double>(big.v) == 9223372036854775808.0);

    auto const small = Unwrap(J::Parse("-9223372036854775809"sv));
    CHECK(std::get<double>(small.v) == -9223372036854775809.0);

    // The boundaries themselves are integers.
    CHECK(Unwrap(J::Parse("9223372036854775807"sv)).AsInt() == INT64_MAX);
    CHECK(Unwrap(J::Parse("-9223372036854775808"sv)).AsInt() == INT64_MIN);

    // A fraction or an exponent makes a double even when the value is whole.
    CHECK(std::holds_alternative<double>(Unwrap(J::Parse("1.0"sv)).v));
    CHECK(std::holds_alternative<double>(Unwrap(J::Parse("1e2"sv)).v));
}

TEST_CASE("json writes a double so it parses back as a double", "[json]")
{
    CHECK(J::Serialize(J::Value { .v = 2.0 }) == "2.0");
    CHECK(J::Serialize(J::Value { .v = -0.0 }) == "-0.0");
    CHECK(J::Serialize(J::Value { .v = 0.1 }) == "0.1");
    CHECK(J::Serialize(J::Value { .v = 1e300 }) == "1e+300");
    CHECK(J::Serialize(J::Value { .v = std::numeric_limits<double>::infinity() }) == "null");
    CHECK(J::Serialize(J::Value { .v = std::numeric_limits<double>::quiet_NaN() }) == "null");

    for (auto const real: std::array { 2.0, 0.1, 1e300, -1.5e-7, 123456.789 })
    {
        auto const again = Unwrap(J::Parse(J::Serialize(J::Value { .v = real })));
        CHECK(std::get<double>(again.v) == real);
    }
}

TEST_CASE("json escapes strings per RFC 8259", "[json]")
{
    auto out = std::string {};
    J::AppendString(out, "q\"b\\n\n\t\r\b\f\x01\x1f /\x7f\xC3\xA9\xF0\x9F\x98\x80"sv);
    CHECK(out == "\"q\\\"b\\\\n\\n\\t\\r\\b\\f\\u0001\\u001f /\x7f\xC3\xA9\xF0\x9F\x98\x80\""sv);

    // A byte that is no UTF-8 is replaced, so what is written is always valid JSON.
    auto bad = std::string {};
    J::AppendString(bad, "a\xffz"sv);
    CHECK(bad == "\"a\\ufffdz\""sv);
    CHECK(J::Parse(bad).has_value());

    // Appends, rather than replacing.
    auto prefixed = std::string { "k=" };
    J::AppendString(prefixed, "v"sv);
    CHECK(prefixed == "k=\"v\"");
}

TEST_CASE("json decodes escapes and surrogate pairs", "[json]")
{
    CHECK(*Unwrap(J::Parse(R"("\"\\\/\b\f\n\r\t")"sv)).AsString() == "\"\\/\b\f\n\r\t"sv);
    CHECK(*Unwrap(J::Parse(R"("\u0041\u00E9\u20ac")"sv)).AsString() == "A\xC3\xA9\xE2\x82\xAC"sv);
    CHECK(*Unwrap(J::Parse(R"("\ud83d\ude00")"sv)).AsString() == "\xF0\x9F\x98\x80"sv);
    CHECK(*Unwrap(J::Parse(R"("\u0000")"sv)).AsString() == "\0"sv);
    // Whitespace around and inside is skipped.
    CHECK(Unwrap(J::Parse(" \t\r\n[ 1 , 2 ]\n"sv)).AsArray()->size() == 2);
}

TEST_CASE("json refuses", "[json]")
{
    auto const defaults = J::ParseLimits {};
    auto const rows = std::vector<Refusal> {
        { .input = "[1,2,3]", .limits = { .maxBytes = 4 }, .code = J::ErrorCode::TooLarge, .offset = 4 },
        { .input = Nested(33), .limits = defaults, .code = J::ErrorCode::TooDeep, .offset = 32 },
        { .input = "\"\xff\"", .limits = defaults, .code = J::ErrorCode::InvalidUtf8, .offset = 1 },
        { .input = "\"\xed\xa0\x80\"", .limits = defaults, .code = J::ErrorCode::InvalidUtf8, .offset = 1 },
        { .input = "[1,", .limits = defaults, .code = J::ErrorCode::UnexpectedEnd, .offset = 3 },
        { .input = "", .limits = defaults, .code = J::ErrorCode::UnexpectedEnd, .offset = 0 },
        { .input = "\"abc", .limits = defaults, .code = J::ErrorCode::UnexpectedEnd, .offset = 4 },
        { .input = "tru", .limits = defaults, .code = J::ErrorCode::UnexpectedEnd, .offset = 3 },
        { .input = "[1,]", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 3 },
        { .input = "NaN", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 0 },
        { .input = "nul1", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 3 },
        { .input = R"({"a" 1})", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 5 },
        { .input = "{1:2}", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 1 },
        { .input = "\"a\nb\"", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 2 },
        { .input = "// c\n1", .limits = defaults, .code = J::ErrorCode::UnexpectedCharacter, .offset = 0 },
        { .input = R"("\ud800")", .limits = defaults, .code = J::ErrorCode::InvalidEscape, .offset = 1 },
        { .input = R"("\ud800\u0041")", .limits = defaults, .code = J::ErrorCode::InvalidEscape, .offset = 1 },
        { .input = R"("\udc00")", .limits = defaults, .code = J::ErrorCode::InvalidEscape, .offset = 1 },
        { .input = R"("\q")", .limits = defaults, .code = J::ErrorCode::InvalidEscape, .offset = 1 },
        { .input = R"("ab\u12G4")", .limits = defaults, .code = J::ErrorCode::InvalidEscape, .offset = 3 },
        { .input = "01", .limits = defaults, .code = J::ErrorCode::InvalidNumber, .offset = 0 },
        { .input = "[-]", .limits = defaults, .code = J::ErrorCode::InvalidNumber, .offset = 1 },
        { .input = "1.", .limits = defaults, .code = J::ErrorCode::InvalidNumber, .offset = 0 },
        { .input = "1e+", .limits = defaults, .code = J::ErrorCode::InvalidNumber, .offset = 0 },
        { .input = "1e999", .limits = defaults, .code = J::ErrorCode::InvalidNumber, .offset = 0 },
        { .input = R"({"a":1,"a":2})", .limits = defaults, .code = J::ErrorCode::DuplicateKey, .offset = 7 },
        { .input = "1 2", .limits = defaults, .code = J::ErrorCode::TrailingData, .offset = 2 },
        { .input = "{}x", .limits = defaults, .code = J::ErrorCode::TrailingData, .offset = 2 },
        { .input = "[1,2,3]", .limits = { .maxContainerElements = 2 }, .code = J::ErrorCode::TooManyElements, .offset = 5 },
        { .input = "\"abc\"", .limits = { .maxStringBytes = 2 }, .code = J::ErrorCode::StringTooLong, .offset = 3 },
    };

    // One row per code at least: a new ErrorCode with no row fails here.
    auto covered = std::set<J::ErrorCode> {};
    for (auto const& row: rows)
        covered.insert(row.code);
    for (auto const code: Pace::Enumerators<J::ErrorCode>())
        CHECK(covered.contains(code));

    for (auto const& row: rows)
    {
        CAPTURE(row.input);
        auto const parsed = J::Parse(row.input, row.limits);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == row.code);
        CHECK(parsed.error().offset == row.offset);
        CHECK(parsed.error().offset <= row.input.size());
    }
}

TEST_CASE("json limits are each enforced", "[json]")
{
    SECTION("maxBytes")
    {
        auto const limits = J::ParseLimits { .maxBytes = 7 };
        CHECK(J::Parse("[1,2,3]"sv, limits).has_value());
        CHECK(J::Parse("[1,2,3] "sv, limits).error().code == J::ErrorCode::TooLarge);
    }
    SECTION("maxDepth")
    {
        auto const limits = J::ParseLimits {};
        CHECK(J::Parse(Nested(limits.maxDepth), limits).has_value());
        CHECK(J::Parse(Nested(limits.maxDepth + 1), limits).error().code == J::ErrorCode::TooDeep);
        // Objects count too, and a sibling does not add depth.
        CHECK(J::Parse(R"([[],[],{"a":{}}])"sv, J::ParseLimits { .maxDepth = 3 }).has_value());
        CHECK(J::Parse(R"({"a":{"b":{}}})"sv, J::ParseLimits { .maxDepth = 2 }).error().code == J::ErrorCode::TooDeep);
    }
    SECTION("maxStringBytes")
    {
        auto const limits = J::ParseLimits {};
        auto const atLimit = "\"" + Repeat("a", limits.maxStringBytes) + "\"";
        auto const over = "\"" + Repeat("a", limits.maxStringBytes + 1) + "\"";
        CHECK(J::Parse(atLimit, limits).has_value());
        CHECK(J::Parse(over, limits).error().code == J::ErrorCode::StringTooLong);
        // Counted on the DECODED bytes: an escape expands, a pair of them counts as four.
        auto const small = J::ParseLimits { .maxStringBytes = 4 };
        CHECK(J::Parse(R"("\ud83d\ude00")"sv, small).has_value());
        CHECK(J::Parse(R"("\ud83d\ude00a")"sv, small).error().code == J::ErrorCode::StringTooLong);
        // A key is a string.
        CHECK(J::Parse(R"({"abcde":1})"sv, small).error().code == J::ErrorCode::StringTooLong);
    }
    SECTION("maxContainerElements, arrays")
    {
        auto const limits = J::ParseLimits { .maxBytes = 1 << 20 };
        CHECK(J::Parse(OnesArray(limits.maxContainerElements), limits).has_value());
        CHECK(J::Parse(OnesArray(limits.maxContainerElements + 1), limits).error().code == J::ErrorCode::TooManyElements);
    }
    SECTION("maxContainerElements, objects")
    {
        auto const limits = J::ParseLimits { .maxBytes = 1 << 20 };
        CHECK(J::Parse(KeysObject(limits.maxContainerElements), limits).has_value());
        CHECK(J::Parse(KeysObject(limits.maxContainerElements + 1), limits).error().code == J::ErrorCode::TooManyElements);
    }
}

TEST_CASE("json survives mutated input", "[json]")
{
    constexpr auto Documents = std::size_t { 200 };
    constexpr auto MutationsPerDocument = std::size_t { 1000 };

    auto seeds = std::vector<std::string> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, Documents))
    {
        auto const doc = std::format(
            R"({{"id":{},"name":"n\u00e9{}\n","ok":{},"nil":null,"r":{}.5e-{},"list":[{},"{}",[true,{{"k":[]}}],{{}}],"u":"\ud83d\ude00"}})",
            index,
            index,
            index % 2 == 0 ? "true" : "false",
            index,
            index % 7,
            index * 31,
            Repeat("z", index % 17));
        REQUIRE(J::Parse(doc).has_value());
        seeds.push_back(doc);
    }

    auto random = Pace::SystemRandomSource { 0x5eed'1234 };
    auto accepted = std::size_t { 0 };
    auto refused = std::size_t { 0 };
    for (auto const& seed: seeds)
    {
        for (auto const mutation: std::views::iota(std::size_t { 0 }, MutationsPerDocument))
        {
            std::ignore = mutation;
            auto text = seed;
            switch (random.UniformInRange(0, 2))
            {
                case 0: { // flip a byte
                    auto const at = random.UniformInRange(0, text.size() - 1);
                    text[at] = static_cast<char>(random.UniformInRange(0, 255));
                    break;
                }
                case 1: // truncate
                    text.resize(random.UniformInRange(0, text.size() - 1));
                    break;
                default: { // duplicate a span
                    auto const from = random.UniformInRange(0, text.size() - 1);
                    auto const length = random.UniformInRange(1, text.size() - from);
                    text.insert(random.UniformInRange(0, text.size()), text.substr(from, length));
                    break;
                }
            }

            auto const parsed = J::Parse(text);
            if (parsed.has_value())
            {
                ++accepted;
                // Whatever is accepted must write to something that parses to the same text again.
                auto const written = J::Serialize(*parsed);
                auto const again = J::Parse(written);
                REQUIRE(again.has_value());
                REQUIRE(J::Serialize(*again) == written);
            }
            else
            {
                ++refused;
                REQUIRE(parsed.error().offset <= text.size());
            }
        }
    }
    // Both outcomes occur, or the mutator is not exercising what it claims to.
    CHECK(accepted > 0);
    CHECK(refused > 0);
}
