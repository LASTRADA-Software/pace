// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Pace::Distributed
{

/// @file ShownName.hpp
/// Which name a surface shows for a machine, and where it came from: the one rule the fleet page, `pacectl node` and
/// the MCP `node_status` tool share, so a member row, a machine row and a node report cannot disagree about which name
/// wins.

/// Where a shown name came from: the cluster's record, or the machine's own word.
///
/// **Private**: rendered as a word (`NameSourceWord`), never transmitted or persisted as an ordinal.
enum class NameSource : std::uint8_t
{
    Replicated,   ///< `Cluster::ClusterMember::displayName`: an operator named the member, and every node agrees.
    SelfReported, ///< What the machine calls itself: its hostname.
    Last,         ///< Not a source, and has no row: the length of a table keyed by one.
};

/// How one `NameSource` is spelled.
struct NameSourceRow
{
    NameSource source;     ///< The source this row describes.
    std::string_view word; ///< Its one spelling: a table cell, a JSON value and a record field alike.
};

/// One row per `NameSource`, in enumerator order.
inline constexpr EnumTable<NameSource, NameSourceRow> NameSourceTable { {
    { .source = NameSource::Replicated, .word = "replicated" },
    { .source = NameSource::SelfReported, .word = "self-reported" },
} };

static_assert(RowsInEnumeratorOrder(NameSourceTable, &NameSourceRow::source),
              "NameSourceTable must hold one row per NameSource, in enumerator order");

/// @param source A source.
/// @return Its spelling.
[[nodiscard]] constexpr std::string_view NameSourceWord(NameSource source) noexcept
{
    return NameSourceTable[static_cast<std::size_t>(source)].word;
}

/// The name a surface shows and where it came from.
struct ShownName
{
    std::string_view text;            ///< The name; empty when there is none.
    std::optional<NameSource> source; ///< Where it came from; absent exactly when `text` is empty.
};

/// The replicated name when the cluster records one, else what the machine calls itself -- or nothing, when neither
/// says. The replicated name wins because an operator CHOSE it and every node agrees on it; the hostname is the
/// machine's own word, and only stands in where nobody named it.
/// @param replicated The cluster's record, or empty.
/// @param selfReported The machine's own word, or empty.
/// @return What a surface shows; views into the arguments.
[[nodiscard]] constexpr ShownName NameShown(std::string_view replicated, std::string_view selfReported) noexcept
{
    if (!replicated.empty())
        return ShownName { .text = replicated, .source = NameSource::Replicated };
    if (!selfReported.empty())
        return ShownName { .text = selfReported, .source = NameSource::SelfReported };
    return ShownName { .text = {}, .source = std::nullopt };
}

} // namespace Pace::Distributed
