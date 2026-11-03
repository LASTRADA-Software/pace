// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file NodeStatusDetailWire.hpp
/// The detail `NodeStatus` carries for `node_status` and `pacectl node`: the slot ceilings a worker is held
/// to with the binding one named, the cache tiers it holds, the configuration in force with every credential
/// redacted AT THE SENDER, and the budget overrides it applies from the cluster.
///
/// Each is one nested record at its own position of `NodeRuntimeFields` (26 to 29), absent as a zero-length
/// field, and every engaged record encodes NON-empty, so an engaged value never reads back as absent.
///
/// **Names travel as text, never as an enumerator** where the vocabulary is a table on the sender's side
/// (`Distributed::SlotLimitTable`, `NodeOptions()`): a reader older than the sender renders a ceiling or a
/// setting it has never heard of exactly as the sender named it. The cache tiers are the exception the whole
/// wire already makes: they travel POSITIONALLY, by `StorageTier` ordinal, which is that enum's contract.
///
/// Header-only and dependency-free for the reason `CompileCacheWire.hpp` is, which includes it: `pace-cc`
/// compiles that header in without linking `Pace`.

namespace Pace::CompileCacheWire
{

namespace Detail
{
    /// Frame a list as a COUNT then one field per row: what makes an engaged EMPTY list encode non-empty, so
    /// *says it holds none* never reads back as *does not say*.
    /// @param rows The rows' own bytes.
    /// @return The list's bytes; never empty.
    [[nodiscard]] inline std::vector<std::byte> EncodeCountedList(std::vector<std::vector<std::byte>> const& rows)
    {
        auto const count = WireFields::ToBigEndian<std::uint32_t>(static_cast<std::uint32_t>(rows.size()));
        std::vector<std::span<std::byte const>> fields;
        fields.reserve(1 + rows.size());
        fields.emplace_back(count);
        for (auto const& row: rows)
            fields.emplace_back(row);
        return WireFields::Encode(WireFields::FieldList { fields });
    }

    /// Split a list framed by `EncodeCountedList`.
    /// @param field The list's bytes; never empty here.
    /// @param most The most rows it may hold.
    /// @return The rows, or nothing when the count is missing, disagrees with the rows, or exceeds @p most.
    [[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> SplitCountedList(
        std::span<std::byte const> field, std::size_t most)
    {
        auto parts = WireFields::SplitAll(field);
        if (!parts.has_value() || parts->empty())
            return std::nullopt;
        auto const count = WireFields::FromBigEndian<std::uint32_t>(parts->front());
        if (!count.has_value() || *count > most || *count != parts->size() - 1)
            return std::nullopt;
        parts->erase(parts->begin());
        return parts;
    }
} // namespace Detail

// ------------------------------------------------------------------------------------------- slot ceilings

/// The longest ceiling name a record may carry, in bytes. A decoder bound: `SlotLimitTable`'s longest name is
/// fourteen bytes.
inline constexpr std::size_t MaxSlotCeilingNameBytes = 64;

/// The most ceilings a record may carry. A decoder bound, with room over `SlotLimitTable`'s six rows.
inline constexpr std::size_t MaxSlotCeilings = 32;

/// One ceiling a worker's slots are held to: what `Distributed::SlotCeilingsFor` computed for one limit.
struct NodeSlotCeilingFields
{
    std::string name; ///< The limit, as `SlotLimitTable` names it; never empty.

    /// The slots this limit allows, or DISENGAGED where the limit did not apply -- a CPU figure the machine
    /// could not read, a cordon nobody set. **Absent is not zero**: zero is a limit that takes everything.
    std::optional<std::uint32_t> slots;

    [[nodiscard]] friend bool operator==(NodeSlotCeilingFields const&, NodeSlotCeilingFields const&) = default;
};

/// Every ceiling a worker's slots are held to, the count they come to and which one binds.
///
/// **One derivation**: the worker computes it with `Distributed::SlotCeilingsFor` -- the scheduler's own
/// function -- over the capacity and the load its last heartbeat SENT, so this record and the scheduler's
/// conclusion are one computation over one reading, never two spellings that can disagree.
struct NodeSlotCeilingsFields
{
    std::vector<NodeSlotCeilingFields> ceilings; ///< One per limit, in the sender's table order.
    std::uint32_t available { 0 };               ///< What the ceilings come to: the slots offerable now.
    std::string binding;                         ///< The ceiling that produced `available`; names a row above.
    std::uint64_t sampledSecondsAgo { 0 };       ///< How long ago the heartbeat reading was taken.

    [[nodiscard]] friend bool operator==(NodeSlotCeilingsFields const&, NodeSlotCeilingsFields const&) = default;
};

/// Frame a slot-ceiling record: available, binding, the reading's age, then one nested field per ceiling.
/// @param record The ceilings; at most `MaxSlotCeilings` are taken, which the sender's table never exceeds.
/// @return Its bytes; never empty.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeSlotCeilings(NodeSlotCeilingsFields const& record)
{
    auto const available = WireFields::ToBigEndian<std::uint32_t>(record.available);
    auto const age = WireFields::ToBigEndian<std::uint64_t>(record.sampledSecondsAgo);
    auto const count = std::min(record.ceilings.size(), MaxSlotCeilings);
    // Owned, every one: the spans handed to `Encode` view these.
    std::vector<std::array<std::byte, sizeof(std::uint32_t)>> slots;
    std::vector<std::vector<std::byte>> rows;
    slots.reserve(count);
    rows.reserve(count);
    for (auto const& ceiling: std::span { record.ceilings }.first(count))
    {
        slots.push_back(WireFields::ToBigEndian<std::uint32_t>(ceiling.slots.value_or(0)));
        rows.push_back(WireFields::Encode(
            { AsBytes(ceiling.name),
              ceiling.slots.has_value() ? std::span<std::byte const> { slots.back() } : std::span<std::byte const> {} }));
    }
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(3 + rows.size());
    fields.emplace_back(available);
    fields.emplace_back(AsBytes(record.binding));
    fields.emplace_back(age);
    for (auto const& row: rows)
        fields.emplace_back(row);
    return WireFields::Encode(WireFields::FieldList { fields });
}

/// Read a slot-ceiling record.
///
/// Refused rather than read: a record short of its three leading fields, a field of the wrong width, a ceiling
/// with no name or a name over `MaxSlotCeilingNameBytes`, more than `MaxSlotCeilings` ceilings, and a binding
/// that names no ceiling the record carries -- a reader told which limit binds must be able to find it.
/// @param field The nested record's bytes; empty is ABSENT.
/// @param out Set only when the field carried a record.
/// @return False when the field was present and malformed.
[[nodiscard]] inline bool ReadNodeSlotCeilings(std::span<std::byte const> field, std::optional<NodeSlotCeilingsFields>& out)
{
    if (field.empty())
        return true;
    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value() || parts->size() < 3 || parts->size() - 3 > MaxSlotCeilings)
        return false;
    auto const available = WireFields::FromBigEndian<std::uint32_t>((*parts)[0]);
    auto const age = WireFields::FromBigEndian<std::uint64_t>((*parts)[2]);
    if (!available.has_value() || !age.has_value())
        return false;

    auto record = NodeSlotCeilingsFields { .ceilings = {},
                                           .available = *available,
                                           .binding = std::string { AsStringView((*parts)[1]) },
                                           .sampledSecondsAgo = *age };
    for (auto const& row: std::span { *parts }.subspan(3))
    {
        auto const columns = WireFields::SplitAll(row);
        if (!columns.has_value() || columns->size() < 2)
            return false;
        auto const name = (*columns)[0];
        if (name.empty() || name.size() > MaxSlotCeilingNameBytes)
            return false;
        auto ceiling = NodeSlotCeilingFields { .name = std::string { AsStringView(name) }, .slots = std::nullopt };
        if (!(*columns)[1].empty())
        {
            auto const slots = WireFields::FromBigEndian<std::uint32_t>((*columns)[1]);
            if (!slots.has_value())
                return false;
            ceiling.slots = slots;
        }
        record.ceilings.push_back(std::move(ceiling));
    }
    if (std::ranges::find(record.ceilings, record.binding, &NodeSlotCeilingFields::name) == record.ceilings.end())
        return false;
    out = std::move(record);
    return true;
}

// ------------------------------------------------------------------------------------------- cache tiers

/// The most tier positions a record may carry. A decoder bound, with room over `StorageTier`'s two.
inline constexpr std::size_t MaxNodeTierPositions = 16;

/// What one cache tier holds and has answered, as its storage's own snapshot says.
struct NodeTierFields
{
    std::uint64_t entries { 0 };    ///< `StorageStats::itemCount`.
    std::uint64_t bytesUsed { 0 };  ///< `StorageStats::bytesUsed`: stored bytes, the tier's own denomination.
    std::uint64_t bytesLimit { 0 }; ///< `StorageStats::bytesLimit`; a present ZERO is unbounded, never full.
    std::uint64_t hits { 0 };       ///< `StorageStats::getHits`.
    std::uint64_t misses { 0 };     ///< `StorageStats::getMisses`.

    [[nodiscard]] friend bool operator==(NodeTierFields const&, NodeTierFields const&) = default;
};

/// The fields a `NodeTierFields` record carries. A reader accepts more and ignores the surplus.
inline constexpr std::size_t NodeTierFieldCount = 5;

/// Every cache tier a node holds, POSITIONALLY by `StorageTier` ordinal: an engaged position is a tier the
/// cache has, a disengaged one a tier it lacks -- which is not a tier holding nothing.
using NodeTiersFields = std::vector<std::optional<NodeTierFields>>;

/// Frame the tier record: one field per position, empty where the cache has no such tier.
/// @param tiers The positions; at most `MaxNodeTierPositions` are taken.
/// @return Its bytes: a count, then one field per position; never empty, so an engaged empty list stays engaged.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeTiers(NodeTiersFields const& tiers)
{
    auto const count = std::min(tiers.size(), MaxNodeTierPositions);
    std::vector<std::vector<std::byte>> rows;
    rows.reserve(count);
    for (auto const& tier: std::span { tiers }.first(count))
    {
        if (!tier.has_value())
        {
            rows.emplace_back();
            continue;
        }
        auto const entries = WireFields::ToBigEndian<std::uint64_t>(tier->entries);
        auto const used = WireFields::ToBigEndian<std::uint64_t>(tier->bytesUsed);
        auto const limit = WireFields::ToBigEndian<std::uint64_t>(tier->bytesLimit);
        auto const hits = WireFields::ToBigEndian<std::uint64_t>(tier->hits);
        auto const misses = WireFields::ToBigEndian<std::uint64_t>(tier->misses);
        rows.push_back(WireFields::Encode({ std::span<std::byte const> { entries },
                                            std::span<std::byte const> { used },
                                            std::span<std::byte const> { limit },
                                            std::span<std::byte const> { hits },
                                            std::span<std::byte const> { misses } }));
    }
    return Detail::EncodeCountedList(rows);
}

/// Read the tier record.
/// @param field The nested record's bytes; empty is ABSENT: a node with no cache.
/// @param out Set only when the field carried a record.
/// @return False when the field was present and malformed: more than `MaxNodeTierPositions` positions, a tier
///         short of its five fields, or a field of the wrong width.
[[nodiscard]] inline bool ReadNodeTiers(std::span<std::byte const> field, std::optional<NodeTiersFields>& out)
{
    if (field.empty())
        return true;
    auto const positions = Detail::SplitCountedList(field, MaxNodeTierPositions);
    if (!positions.has_value())
        return false;
    auto tiers = NodeTiersFields {};
    tiers.reserve(positions->size());
    for (auto const& position: *positions)
    {
        if (position.empty())
        {
            tiers.emplace_back();
            continue;
        }
        auto const columns = WireFields::SplitAll(position);
        if (!columns.has_value() || columns->size() < NodeTierFieldCount)
            return false;
        auto const read = [&columns](std::size_t index) {
            return WireFields::FromBigEndian<std::uint64_t>((*columns)[index]);
        };
        auto const entries = read(0);
        auto const used = read(1);
        auto const limit = read(2);
        auto const hits = read(3);
        auto const misses = read(4);
        if (!entries.has_value() || !used.has_value() || !limit.has_value() || !hits.has_value() || !misses.has_value())
            return false;
        tiers.emplace_back(NodeTierFields {
            .entries = *entries, .bytesUsed = *used, .bytesLimit = *limit, .hits = *hits, .misses = *misses });
    }
    out = std::move(tiers);
    return true;
}

// ------------------------------------------------------------------------------------------- effective config

/// How one setting's value travels.
///
/// **TRANSMITTED**: explicit values from `0x01`, and a byte this build does not name SKIPS the setting rather
/// than reading it as a neighbour -- the enrollment state's rule.
enum class WireSettingValue : std::uint8_t
{
    Value = 0x01,    ///< The value in force, as `text`; an empty text is a value (`--listen-node=` serves nothing).
    Unset = 0x02,    ///< Not set: the node derives it or does without; `text` is empty.
    Redacted = 0x03, ///< A credential is set and its value was NOT sent; `text` is empty.
    Omitted = 0x04,  ///< A value the record could not carry whole; `text` says why, never a part of the value.
};

/// The longest setting name a record may carry, in bytes. A decoder bound over the option table's flags.
inline constexpr std::size_t MaxNodeSettingNameBytes = 64;

/// The longest setting text a record may carry, in bytes: a path at Linux's `PATH_MAX`. A value longer than
/// this travels as `Omitted`, never cut, since a cut path names a different file.
inline constexpr std::size_t MaxNodeSettingTextBytes = 4096;

/// The most settings a record may carry, one per value of a list setting. A value past it travels as one
/// `Omitted` row naming how many were left out, never dropped silently.
inline constexpr std::size_t MaxNodeSettings = 512;

/// What a reader prints for a `Redacted` value: one spelling, so `pacectl node` and `node_status` say the
/// same words.
inline constexpr std::string_view RedactedSettingText = "<redacted>";

/// One setting in force, as the sender's configuration table names it.
struct NodeSettingFields
{
    std::string name;                                  ///< The flag, e.g. `--slots`; never empty.
    WireSettingValue kind { WireSettingValue::Unset }; ///< How `text` reads.
    std::string text;                                  ///< The value, or why it was omitted; empty otherwise.

    [[nodiscard]] friend bool operator==(NodeSettingFields const&, NodeSettingFields const&) = default;
};

namespace Detail
{
    /// Whether this build names a setting-value byte. A total `switch` with no `default:`, so an enumerator added
    /// without a case fails the build.
    /// @param kind The byte, cast.
    /// @return True when it is one of the enumerators.
    [[nodiscard]] constexpr bool NamesSettingValue(WireSettingValue kind) noexcept
    {
        switch (kind)
        {
            case WireSettingValue::Value:
            case WireSettingValue::Unset:
            case WireSettingValue::Redacted:
            case WireSettingValue::Omitted:
                return true;
        }
        return false;
    }
} // namespace Detail

/// Frame the configuration record: a count, then one nested `[name, kind, text]` field per setting -- never empty,
/// so an engaged empty list stays engaged.
/// @param settings The settings; at most `MaxNodeSettings` are taken -- the producer folds a surplus into one
///        `Omitted` row before it gets here.
/// @return Its bytes.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeSettings(std::span<NodeSettingFields const> settings)
{
    auto const count = std::min(settings.size(), MaxNodeSettings);
    std::vector<std::vector<std::byte>> rows;
    rows.reserve(count);
    for (auto const& setting: settings.first(count))
    {
        auto const kind = std::array { static_cast<std::byte>(setting.kind) };
        rows.push_back(
            WireFields::Encode({ AsBytes(setting.name), std::span<std::byte const> { kind }, AsBytes(setting.text) }));
    }
    return Detail::EncodeCountedList(rows);
}

/// Read the configuration record.
///
/// **A `Redacted` or `Unset` setting that carries text refuses the whole record**: a sender that put bytes
/// beside a redaction is not one whose redactions a reader can trust, and printing none of its record is the
/// safe direction for a credential. A kind byte this build does not name skips that one setting.
/// @param field The nested record's bytes; empty is ABSENT.
/// @param out Set only when the field carried a record.
/// @return False when the field was present and malformed.
[[nodiscard]] inline bool ReadNodeSettings(std::span<std::byte const> field,
                                           std::optional<std::vector<NodeSettingFields>>& out)
{
    if (field.empty())
        return true;
    auto const rows = Detail::SplitCountedList(field, MaxNodeSettings);
    if (!rows.has_value())
        return false;
    auto settings = std::vector<NodeSettingFields> {};
    settings.reserve(rows->size());
    for (auto const& row: *rows)
    {
        auto const columns = WireFields::SplitAll(row);
        if (!columns.has_value() || columns->size() < 3 || (*columns)[1].size() != 1)
            return false;
        auto const name = (*columns)[0];
        auto const text = (*columns)[2];
        if (name.empty() || name.size() > MaxNodeSettingNameBytes || text.size() > MaxNodeSettingTextBytes)
            return false;
        auto const kind = static_cast<WireSettingValue>((*columns)[1][0]);
        if (!Detail::NamesSettingValue(kind))
            continue;
        if ((kind == WireSettingValue::Redacted || kind == WireSettingValue::Unset) && !text.empty())
            return false;
        settings.push_back(NodeSettingFields {
            .name = std::string { AsStringView(name) }, .kind = kind, .text = std::string { AsStringView(text) } });
    }
    out = std::move(settings);
    return true;
}

// ------------------------------------------------------------------------------------------- budget overrides

/// The most budget overrides a record may carry. A decoder bound: one per tier, with room.
inline constexpr std::size_t MaxNodeBudgetOverrides = 16;

/// The longest word, member id or display name a budget row carries, in bytes. A decoder bound over a member id's and
/// a display name's own ceilings, with room.
inline constexpr std::size_t MaxBudgetOverrideNameBytes = 256;

/// The longest note a budget row carries, in bytes: one sentence naming why an override is not in force, with the
/// reason a tier or the leader gave. A longer one is cut by the SENDER, on a code-point boundary.
inline constexpr std::size_t MaxBudgetOverrideNoteBytes = 1024;

/// One cache tier's budget as the node runs it: what is in force, where it came from, and the override the cluster
/// records for it -- one row per tier the node has, or that the cluster records an override for.
///
/// Every quantity optional where it can be ABSENT rather than zero: a tier the node lacks has no budget in force, an
/// override not yet reported has no baseline, and a tier that fits is a present ZERO of excess, never an absent one.
struct NodeBudgetOverrideFields
{
    std::uint8_t tier { 0 };                         ///< The tier, by `StorageTier` ordinal.
    std::optional<std::uint64_t> bytes {};           ///< The budget in force; absent when the node has no such tier.
    std::optional<std::uint64_t> baselineBytes {};   ///< The configured value the override was set against; absent until
                                                     ///< recorded, or with no override.
    std::string source {};                           ///< Where `bytes` came from, as a word: `configured` or `cluster`.
    std::uint64_t configuredBytes { 0 };             ///< The machine's own budget in the configuration in force.
    std::optional<std::uint64_t> overrideBytes {};   ///< The override the cluster records; absent when none is.
    std::optional<std::int64_t> setAtUnixSeconds {}; ///< When the leader recorded it, by the leader's clock; display.
    std::string setBy {};                            ///< The member it is attributed to; empty when none was.
    std::string setByName {};                        ///< That member's display name; empty when it has none.
    std::optional<std::uint64_t> excessBytes {};     ///< Stored bytes still over the budget in force; absent with no tier.
    std::string note {}; ///< Why the recorded override is not in force, or a budget was refused; empty when all is.

    [[nodiscard]] friend bool operator==(NodeBudgetOverrideFields const&, NodeBudgetOverrideFields const&) = default;
};

/// The fields a `NodeBudgetOverrideFields` row carries; a reader accepts more and ignores the surplus.
inline constexpr std::size_t NodeBudgetOverrideFieldCount = 11;

namespace Detail
{
    /// An optional figure as a field: eight bytes, or empty for absent.
    /// @param value The figure.
    /// @return Its bytes.
    [[nodiscard]] inline std::vector<std::byte> OptionalFigure(std::optional<std::uint64_t> value)
    {
        if (!value.has_value())
            return {};
        auto const bytes = WireFields::ToBigEndian<std::uint64_t>(*value);
        return { bytes.begin(), bytes.end() };
    }

    /// Read an optional figure: empty is absent, eight bytes a value, anything else malformed.
    /// @param field The field.
    /// @param out Set when the field carried a figure.
    /// @return False when malformed.
    [[nodiscard]] inline bool ReadOptionalFigure(std::span<std::byte const> field, std::optional<std::uint64_t>& out)
    {
        if (field.empty())
            return true;
        auto const value = WireFields::FromBigEndian<std::uint64_t>(field);
        if (!value.has_value())
            return false;
        out = value;
        return true;
    }
} // namespace Detail

/// Frame the budget rows: a count, then one nested field each. The count is what makes an engaged EMPTY list -- a node
/// with no tier and no override -- encode non-empty, so it never reads back as absent.
/// @param overrides The rows; at most `MaxNodeBudgetOverrides` are taken.
/// @return Its bytes; never empty.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeBudgetOverrides(std::span<NodeBudgetOverrideFields const> overrides)
{
    auto const taken = overrides.first(std::min(overrides.size(), MaxNodeBudgetOverrides));
    std::vector<std::vector<std::byte>> rows;
    rows.reserve(taken.size());
    for (auto const& entry: taken)
    {
        auto const tier = std::array { static_cast<std::byte>(entry.tier) };
        auto const bytes = Detail::OptionalFigure(entry.bytes);
        auto const baseline = Detail::OptionalFigure(entry.baselineBytes);
        auto const configured = WireFields::ToBigEndian<std::uint64_t>(entry.configuredBytes);
        auto const recorded = Detail::OptionalFigure(entry.overrideBytes);
        auto const setAt = Detail::OptionalFigure(entry.setAtUnixSeconds.has_value()
                                                      ? std::optional { static_cast<std::uint64_t>(*entry.setAtUnixSeconds) }
                                                      : std::nullopt);
        auto const excess = Detail::OptionalFigure(entry.excessBytes);
        rows.push_back(WireFields::Encode({ std::span<std::byte const> { tier },
                                            std::span<std::byte const> { bytes },
                                            std::span<std::byte const> { baseline },
                                            AsBytes(entry.source),
                                            std::span<std::byte const> { configured },
                                            std::span<std::byte const> { recorded },
                                            std::span<std::byte const> { setAt },
                                            AsBytes(entry.setBy),
                                            AsBytes(entry.setByName),
                                            std::span<std::byte const> { excess },
                                            AsBytes(entry.note) }));
    }
    return Detail::EncodeCountedList(rows);
}

/// Read the budget rows. Every string is OWNED: the record outlives the reply it was read from.
/// @param field The nested record's bytes; empty is ABSENT.
/// @param out Set only when the field carried a record.
/// @return False when the field was present and malformed: a count disagreeing with the rows, more than
///         `MaxNodeBudgetOverrides`, a row short of its fields, a figure of the wrong width, or a text over its ceiling.
[[nodiscard]] inline bool ReadNodeBudgetOverrides(std::span<std::byte const> field,
                                                  std::optional<std::vector<NodeBudgetOverrideFields>>& out)
{
    if (field.empty())
        return true;
    auto const parts = Detail::SplitCountedList(field, MaxNodeBudgetOverrides);
    if (!parts.has_value())
        return false;
    auto overrides = std::vector<NodeBudgetOverrideFields> {};
    overrides.reserve(parts->size());
    for (auto const& row: *parts)
    {
        auto const columns = WireFields::SplitAll(row);
        if (!columns.has_value() || columns->size() < NodeBudgetOverrideFieldCount || (*columns)[0].size() != 1)
            return false;
        auto const& c = *columns;
        auto const configured = WireFields::FromBigEndian<std::uint64_t>(c[4]);
        if (!configured.has_value() || c[3].size() > MaxBudgetOverrideNameBytes || c[7].size() > MaxBudgetOverrideNameBytes
            || c[8].size() > MaxBudgetOverrideNameBytes || c[10].size() > MaxBudgetOverrideNoteBytes)
            return false;
        auto entry = NodeBudgetOverrideFields { .tier = std::to_integer<std::uint8_t>(c[0][0]),
                                                .source = std::string { AsStringView(c[3]) },
                                                .configuredBytes = *configured,
                                                .setBy = std::string { AsStringView(c[7]) },
                                                .setByName = std::string { AsStringView(c[8]) },
                                                .note = std::string { AsStringView(c[10]) } };
        auto setAt = std::optional<std::uint64_t> {};
        if (!Detail::ReadOptionalFigure(c[1], entry.bytes) || !Detail::ReadOptionalFigure(c[2], entry.baselineBytes)
            || !Detail::ReadOptionalFigure(c[5], entry.overrideBytes) || !Detail::ReadOptionalFigure(c[6], setAt)
            || !Detail::ReadOptionalFigure(c[9], entry.excessBytes))
            return false;
        if (setAt.has_value())
            entry.setAtUnixSeconds = static_cast<std::int64_t>(*setAt);
        overrides.push_back(std::move(entry));
    }
    out = std::move(overrides);
    return true;
}

} // namespace Pace::CompileCacheWire
