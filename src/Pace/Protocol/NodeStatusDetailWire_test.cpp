// SPDX-License-Identifier: Apache-2.0
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::CompileCacheWire;
using Pace::Testing::Unwrap;

namespace
{

/// @param values Byte values.
/// @return Them as bytes.
[[nodiscard]] std::vector<std::byte> Bytes(std::initializer_list<int> values)
{
    auto out = std::vector<std::byte> {};
    out.reserve(values.size());
    for (auto const value: values)
        out.push_back(static_cast<std::byte>(value));
    return out;
}

/// A runtime record whose four detail positions are all engaged, every value distinct.
[[nodiscard]] NodeRuntimeFields EveryDetail()
{
    auto runtime = NodeRuntimeFields {};
    runtime.slotCeilings = NodeSlotCeilingsFields {
        .ceilings = { NodeSlotCeilingFields { .name = "registered", .slots = 8 },
                      NodeSlotCeilingFields { .name = "memory", .slots = 3 },
                      NodeSlotCeilingFields { .name = "cordoned", .slots = std::nullopt } },
        .available = 3,
        .binding = "memory",
        .sampledSecondsAgo = 11,
    };
    runtime.tiers = NodeTiersFields {
        NodeTierFields { .entries = 5, .bytesUsed = 600, .bytesLimit = 0, .hits = 70, .misses = 9 },
        std::nullopt,
    };
    runtime.effectiveConfig = std::vector {
        NodeSettingFields { .name = "--slots", .kind = WireSettingValue::Unset, .text = {} },
        NodeSettingFields { .name = "--listen-node", .kind = WireSettingValue::Value, .text = {} },
        NodeSettingFields { .name = "--requirepass", .kind = WireSettingValue::Redacted, .text = {} },
        NodeSettingFields { .name = "--toolchain", .kind = WireSettingValue::Omitted, .text = "3 more" },
    };
    runtime.budgetOverrides =
        std::vector { NodeBudgetOverrideFields { .tier = 1, .bytes = 1024, .baselineBytes = std::uint64_t { 512 } } };
    return runtime;
}

} // namespace

TEST_CASE("A setting's value kind travels as its pinned byte", "[wire][node-status][node-status-detail]")
{
    // The BYTES, beside the names both ends spell: a consistent renumbering keeps every in-tree test agreeing
    // while a deployed reader prints a redaction as a value.
    CHECK(static_cast<std::uint8_t>(WireSettingValue::Value) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireSettingValue::Unset) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireSettingValue::Redacted) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireSettingValue::Omitted) == 0x04);
    CHECK(RedactedSettingText == "<redacted>");
}

TEST_CASE("The NODE-STATUS runtime record's positions 26 to 29 hold exactly these bytes",
          "[wire][node-status][node-status-detail]")
{
    auto runtime = NodeRuntimeFields {};
    runtime.slotCeilings = NodeSlotCeilingsFields { .ceilings = { NodeSlotCeilingFields { .name = "r", .slots = 2 },
                                                                  NodeSlotCeilingFields { .name = "c", .slots = {} } },
                                                    .available = 2,
                                                    .binding = "r",
                                                    .sampledSecondsAgo = 4 };
    runtime.tiers =
        NodeTiersFields { std::nullopt,
                          NodeTierFields { .entries = 1, .bytesUsed = 2, .bytesLimit = 3, .hits = 4, .misses = 5 } };
    runtime.effectiveConfig =
        std::vector { NodeSettingFields { .name = "-a", .kind = WireSettingValue::Value, .text = "v" } };
    runtime.budgetOverrides = std::vector<NodeBudgetOverrideFields> {};
    // Held, so the spans the split hands back view bytes that outlive them.
    auto const encoded = EncodeNodeRuntime(runtime);
    auto const parts = WireFields::SplitAll(encoded);
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() == 30);
    auto const at = [&parts](std::size_t index) {
        return Unwrap(parts)[index];
    };

    // clang-format off
    CHECK(std::ranges::equal(at(26), Bytes({
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02,                         // available
        0x00, 0x00, 0x00, 0x01, 'r',                                            // binding
        0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, // seconds since sampled
        0x00, 0x00, 0x00, 0x0d,                                                 // ceiling "r": 2
            0x00, 0x00, 0x00, 0x01, 'r', 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x09,                                                 // ceiling "c": did not apply
            0x00, 0x00, 0x00, 0x01, 'c', 0x00, 0x00, 0x00, 0x00 })));
    CHECK(std::ranges::equal(at(27), Bytes({
        0x00, 0x00, 0x00, 0x00,                                                 // tier 0: absent
        0x00, 0x00, 0x00, 0x3c,                                                 // tier 1: five u64s
            0x00, 0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 1,
            0x00, 0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 2,
            0x00, 0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 3,
            0x00, 0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 4,
            0x00, 0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 5 })));
    CHECK(std::ranges::equal(at(28), Bytes({
        0x00, 0x00, 0x00, 0x10,                                                 // one setting
            0x00, 0x00, 0x00, 0x02, '-', 'a',                                   // name
            0x00, 0x00, 0x00, 0x01, 0x01,                                       // kind: Value
            0x00, 0x00, 0x00, 0x01, 'v' })));                                   // text
    // An engaged EMPTY list -- no overrides -- is its count alone, never zero length.
    CHECK(std::ranges::equal(at(29), Bytes({ 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00 })));
    // clang-format on
}

TEST_CASE("The four detail records round-trip whole, and a record with none of them says nothing",
          "[wire][node-status][node-status-detail]")
{
    auto const sent = EveryDetail();
    auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).slotCeilings == sent.slotCeilings);
    CHECK(Unwrap(back).tiers == sent.tiers);
    CHECK(Unwrap(back).effectiveConfig == sent.effectiveConfig);
    CHECK(Unwrap(back).budgetOverrides == sent.budgetOverrides);

    // ABSENT, all four, from a sender that stated none -- never a record of zeroes or an empty list.
    auto const silent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(silent.has_value());
    CHECK_FALSE(Unwrap(silent).slotCeilings.has_value());
    CHECK_FALSE(Unwrap(silent).tiers.has_value());
    CHECK_FALSE(Unwrap(silent).effectiveConfig.has_value());
    CHECK_FALSE(Unwrap(silent).budgetOverrides.has_value());

    // And an engaged empty override list -- *no overrides* -- comes back engaged and empty.
    auto none = NodeRuntimeFields {};
    none.budgetOverrides = std::vector<NodeBudgetOverrideFields> {};
    auto const noneBack = DecodeNodeRuntime(EncodeNodeRuntime(none));
    REQUIRE(noneBack.has_value());
    CHECK(Unwrap(noneBack).budgetOverrides == std::optional { std::vector<NodeBudgetOverrideFields> {} });
}

TEST_CASE("A detail record this build cannot read refuses the reply, and an unnamed setting kind is skipped",
          "[wire][node-status][node-status-detail]")
{
    auto const replaced = [](std::size_t position, std::vector<std::byte> const& record) {
        auto emitted = EncodeNodeRuntime(NodeRuntimeFields {});
        auto parts = Unwrap(WireFields::SplitAll(emitted));
        REQUIRE(parts.size() == 30);
        parts[position] = record;
        return DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts }));
    };
    auto const setting = [](std::string_view name, std::uint8_t kind, std::string_view text) {
        auto const kindByte = std::array { static_cast<std::byte>(kind) };
        return WireFields::Encode({ AsBytes(name), std::span<std::byte const> { kindByte }, AsBytes(text) });
    };

    SECTION("a redaction that carries text refuses the record: no reader may print bytes beside it")
    {
        auto const row = setting("--requirepass", 0x03, "hunter2");
        CHECK_FALSE(replaced(28, WireFields::Encode({ std::span<std::byte const> { row } })).has_value());
    }
    SECTION("an unset setting that carries text refuses the record")
    {
        auto const row = setting("--slots", 0x02, "4");
        CHECK_FALSE(replaced(28, WireFields::Encode({ std::span<std::byte const> { row } })).has_value());
    }
    SECTION("a kind byte this build does not name skips that setting and keeps its neighbour")
    {
        auto const unknown = setting("--future", 0x7F, "x");
        auto const known = setting("--slots", 0x01, "4");
        auto const back = replaced(
            28, WireFields::Encode({ std::span<std::byte const> { unknown }, std::span<std::byte const> { known } }));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).effectiveConfig
              == std::optional {
                  std::vector { NodeSettingFields { .name = "--slots", .kind = WireSettingValue::Value, .text = "4" } } });
    }
    SECTION("a binding that names no ceiling the record carries refuses the record")
    {
        auto record = EncodeNodeSlotCeilings(NodeSlotCeilingsFields {
            .ceilings = { NodeSlotCeilingFields { .name = "registered", .slots = 4 } },
            .available = 4,
            .binding = "memory",
            .sampledSecondsAgo = 0,
        });
        CHECK_FALSE(replaced(26, record).has_value());
    }
    SECTION("a tier short of its five fields refuses the record")
    {
        auto const one = WireFields::ToBigEndian<std::uint64_t>(1);
        auto const tier = WireFields::Encode({ std::span<std::byte const> { one }, std::span<std::byte const> { one } });
        CHECK_FALSE(replaced(27, WireFields::Encode({ std::span<std::byte const> { tier } })).has_value());
    }
    SECTION("an override count disagreeing with its rows refuses the record")
    {
        auto const two = WireFields::ToBigEndian<std::uint32_t>(2);
        CHECK_FALSE(replaced(29, WireFields::Encode({ std::span<std::byte const> { two } })).has_value());
    }
}
