// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/Core/WireFields.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file FingerprintInputsWire.hpp
/// The grammar of `Op::FingerprintInputs`: one node's record of what a toolchain fingerprint was computed from, one
/// page at a time, and the index that resolves a toolchain named by label or compiler path to one fingerprint.
///
/// **Header-only**, beside `CompileCacheWire.hpp` rather than inside it: that header is the dependency-free one the
/// launcher compiles, and this one names `Cc::FingerprintInputs` (`CompileCache/ToolchainFingerprint.hpp`), which
/// it carries -- one record type at both ends, never a wire-shaped copy of it.
///
/// **Every decoder returns OWNED values.** The record is diffed and recomputed long after the frame is gone, so a
/// view into the payload would be a use-after-free the moment a caller kept a page.

namespace Pace::CompileCacheWire
{

/// Which shape a FINGERPRINT-INPUTS reply carries: its first field, one byte.
///
/// **TRANSMITTED**: the explicit values are the wire contract, so an enumerator is appended and never renumbered.
enum class FingerprintInputsReplyKind : std::uint8_t
{
    Index = 0,  ///< The served fingerprints, each with its label and compiler path (asked with no fingerprint).
    Inputs = 1, ///< One page of one fingerprint's inputs.
    Last = 2,   ///< Not a kind.
};

/// A FINGERPRINT-INPUTS request.
struct FingerprintInputsRequest
{
    std::string fingerprint;    ///< The fingerprint asked about; EMPTY asks for the index.
    std::uint64_t cursor { 0 }; ///< The index of the first file the page starts at; ignored for the index.
};

/// One served toolchain, as the index lists it.
struct FingerprintIndexEntry
{
    std::string fingerprint; ///< What it is filed under.
    std::string label;       ///< What a person calls it (`gcc 13.2.0`); empty when nothing said.
    std::string compiler;    ///< The compiler path the node serves it with.
    bool recorded { false }; ///< Whether the node holds its inputs -- false for an operator's pin, never probed.
};

/// The index: every toolchain the node serves, as far as the reply ceiling holds them.
struct FingerprintIndexFields
{
    std::string version;                        ///< The answering node's `pace-node` version.
    std::uint32_t total { 0 };                  ///< How many toolchains it serves; more than listed when cut short.
    std::vector<FingerprintIndexEntry> entries; ///< As many as fit, in fingerprint order.
};

/// One page of one fingerprint's inputs.
struct FingerprintInputsPage
{
    std::string fingerprint;                 ///< The fingerprint the record is filed under.
    std::string version;                     ///< The answering node's `pace-node` version.
    std::uint64_t cursor { 0 };              ///< The index of the page's first file.
    std::uint64_t totalFiles { 0 };          ///< How many files the whole record holds.
    std::optional<std::uint64_t> nextCursor; ///< Where the next page starts; nothing on the last page.
    Cc::FingerprintInputs inputs;            ///< Schema, banner, grammar -- and this page's files only.
};

/// The bytes one file adds to a page's file list: two length-prefixed fields.
/// @param file The file.
/// @return Its encoded size.
[[nodiscard]] constexpr std::size_t EncodedFileBytes(Cc::ToolchainFile const& file) noexcept
{
    return (2 * WireFields::FieldPrefixSize) + file.relativePath.size() + file.contentHash.size();
}

/// The bytes a page costs before its first file: every field but the file list's contents.
/// @param fingerprint The fingerprint.
/// @param version The answering node's version.
/// @param inputs The record whose schema, banner and grammar the page repeats.
/// @return The encoded size of an empty page.
[[nodiscard]] constexpr std::size_t FingerprintInputsPageOverhead(std::string_view fingerprint,
                                                                  std::string_view version,
                                                                  Cc::FingerprintInputs const& inputs) noexcept
{
    // Ten fields: kind, fingerprint, version, schema, banner, grammar, cursor, total, next, files.
    constexpr std::size_t Fields = 10;
    return (Fields * WireFields::FieldPrefixSize) + 1 + fingerprint.size() + version.size() + inputs.schema.size()
           + inputs.banner.size() + inputs.driverGrammar.size() + (3 * sizeof(std::uint64_t));
}

/// The bytes one index entry adds: four length-prefixed fields.
/// @param entry The entry.
/// @return Its encoded size.
[[nodiscard]] constexpr std::size_t EncodedIndexEntryBytes(FingerprintIndexEntry const& entry) noexcept
{
    return (4 * WireFields::FieldPrefixSize) + entry.fingerprint.size() + entry.label.size() + entry.compiler.size() + 1;
}

/// The bytes an index costs before its first entry.
/// @param version The answering node's version.
/// @return The encoded size of an empty index.
[[nodiscard]] constexpr std::size_t FingerprintIndexOverhead(std::string_view version) noexcept
{
    // Four fields: kind, version, total, entries.
    return (4 * WireFields::FieldPrefixSize) + 1 + version.size() + sizeof(std::uint32_t);
}

/// Frame a FINGERPRINT-INPUTS request.
/// @param request What to ask.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFingerprintInputsRequest(FingerprintInputsRequest const& request,
                                                                           WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(
        version,
        Op::FingerprintInputs,
        { AsBytes(request.fingerprint), std::span<std::byte const> { EncodeU64Field(request.cursor) } });
}

/// Read a FINGERPRINT-INPUTS request's payload.
/// @param payload The bytes following the request header.
/// @return The request, or nullopt when its fields do not exactly fill the payload, the fingerprint is longer than
///         `MaxToolchainFingerprintBytes`, or the cursor is not eight bytes.
[[nodiscard]] inline std::optional<FingerprintInputsRequest> DecodeFingerprintInputsRequest(
    std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, OpFieldCount(Op::FingerprintInputs));
    if (!fields.has_value() || (*fields)[0].size() > MaxToolchainFingerprintBytes)
        return std::nullopt;
    auto const cursor = DecodeU64Field((*fields)[1]);
    if (!cursor.has_value())
        return std::nullopt;
    return FingerprintInputsRequest { .fingerprint = std::string { AsStringView((*fields)[0]) }, .cursor = *cursor };
}

/// Encode one page of a fingerprint's inputs.
///
/// The caller decides which files the page holds (`FingerprintInputsResponder`); this writes them.
/// @param page The page; `inputs.files` holds this page's files only.
/// @return The reply body.
[[nodiscard]] inline std::vector<std::byte> EncodeFingerprintInputsPage(FingerprintInputsPage const& page)
{
    std::vector<std::span<std::byte const>> fileFields;
    fileFields.reserve(page.inputs.files.size() * 2);
    for (auto const& file: page.inputs.files)
    {
        fileFields.push_back(AsBytes(file.relativePath));
        fileFields.push_back(AsBytes(file.contentHash));
    }
    auto const files = WireFields::Encode(WireFields::FieldList { fileFields });
    auto const kind = std::array { static_cast<std::byte>(FingerprintInputsReplyKind::Inputs) };
    auto const cursor = EncodeU64Field(page.cursor);
    auto const total = EncodeU64Field(page.totalFiles);
    auto const next = EncodeU64Field(page.nextCursor.value_or(0));
    return WireFields::Encode(
        { std::span<std::byte const> { kind },
          AsBytes(page.fingerprint),
          AsBytes(page.version),
          AsBytes(page.inputs.schema),
          AsBytes(page.inputs.banner),
          AsBytes(page.inputs.driverGrammar),
          std::span<std::byte const> { cursor },
          std::span<std::byte const> { total },
          page.nextCursor.has_value() ? std::span<std::byte const> { next } : std::span<std::byte const> {},
          std::span<std::byte const> { files } });
}

/// Encode the index.
/// @param index The served toolchains that fit, and how many there are.
/// @return The reply body.
[[nodiscard]] inline std::vector<std::byte> EncodeFingerprintIndex(FingerprintIndexFields const& index)
{
    std::vector<std::span<std::byte const>> entryFields;
    entryFields.reserve(index.entries.size() * 4);
    static constexpr std::array recordedFlags { std::byte { 0 }, std::byte { 1 } };
    for (auto const& entry: index.entries)
    {
        entryFields.push_back(AsBytes(entry.fingerprint));
        entryFields.push_back(AsBytes(entry.label));
        entryFields.push_back(AsBytes(entry.compiler));
        entryFields.emplace_back(&recordedFlags[entry.recorded ? 1 : 0], 1);
    }
    auto const entries = WireFields::Encode(WireFields::FieldList { entryFields });
    auto const kind = std::array { static_cast<std::byte>(FingerprintInputsReplyKind::Index) };
    auto const total = EncodeU32Field(index.total);
    return WireFields::Encode({ std::span<std::byte const> { kind },
                                AsBytes(index.version),
                                std::span<std::byte const> { total },
                                std::span<std::byte const> { entries } });
}

/// @param payload A FINGERPRINT-INPUTS reply body.
/// @return The kind it declares, or nullopt when it is empty or names none.
[[nodiscard]] inline std::optional<FingerprintInputsReplyKind> FingerprintInputsReplyKindOf(
    std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitAll(payload);
    if (!fields.has_value() || fields->empty() || (*fields)[0].size() != 1)
        return std::nullopt;
    auto const raw = std::to_integer<std::uint8_t>((*fields)[0][0]);
    if (raw >= static_cast<std::uint8_t>(FingerprintInputsReplyKind::Last))
        return std::nullopt;
    return static_cast<FingerprintInputsReplyKind>(raw);
}

/// Read one page of a fingerprint's inputs.
/// @param payload The reply body.
/// @return The page, owning every byte it holds, or nullopt when the body is not exactly an `Inputs` page: a field
///         count, a width, a file list of odd length, or a next cursor that does not move forward.
[[nodiscard]] inline std::optional<FingerprintInputsPage> DecodeFingerprintInputsPage(std::span<std::byte const> payload)
{
    constexpr std::size_t Fields = 10;
    auto const fields = WireFields::SplitExactly(payload, Fields);
    if (!fields.has_value() || (*fields)[0].size() != 1
        || std::to_integer<std::uint8_t>((*fields)[0][0]) != static_cast<std::uint8_t>(FingerprintInputsReplyKind::Inputs))
        return std::nullopt;
    auto const cursor = DecodeU64Field((*fields)[6]);
    auto const total = DecodeU64Field((*fields)[7]);
    auto const files = WireFields::SplitAll((*fields)[9]);
    if (!cursor.has_value() || !total.has_value() || !files.has_value() || files->size() % 2 != 0)
        return std::nullopt;
    auto next = std::optional<std::uint64_t> {};
    if (!(*fields)[8].empty())
    {
        next = DecodeU64Field((*fields)[8]);
        // A next page that does not start past this one is a loop a reader would follow forever.
        if (!next.has_value() || *next <= *cursor || *next > *total)
            return std::nullopt;
    }

    auto page =
        FingerprintInputsPage { .fingerprint = std::string { AsStringView((*fields)[1]) },
                                .version = std::string { AsStringView((*fields)[2]) },
                                .cursor = *cursor,
                                .totalFiles = *total,
                                .nextCursor = next,
                                .inputs =
                                    Cc::FingerprintInputs { .schema = std::string { AsStringView((*fields)[3]) },
                                                            .banner = std::string { AsStringView((*fields)[4]) },
                                                            .driverGrammar = std::string { AsStringView((*fields)[5]) },
                                                            .files = {} } };
    page.inputs.files.reserve(files->size() / 2);
    for (auto const pair: std::views::iota(std::size_t { 0 }, files->size() / 2))
        page.inputs.files.push_back(
            Cc::ToolchainFile { .relativePath = std::string { AsStringView((*files)[2 * pair]) },
                                .contentHash = std::string { AsStringView((*files)[(2 * pair) + 1]) } });
    return page;
}

/// Read the index.
/// @param payload The reply body.
/// @return The index, owning every byte it holds, or nullopt when the body is not exactly an `Index`.
[[nodiscard]] inline std::optional<FingerprintIndexFields> DecodeFingerprintIndex(std::span<std::byte const> payload)
{
    constexpr std::size_t Fields = 4;
    auto const fields = WireFields::SplitExactly(payload, Fields);
    if (!fields.has_value() || (*fields)[0].size() != 1
        || std::to_integer<std::uint8_t>((*fields)[0][0]) != static_cast<std::uint8_t>(FingerprintInputsReplyKind::Index))
        return std::nullopt;
    auto const total = DecodeU32Field((*fields)[2]);
    auto const entries = WireFields::SplitAll((*fields)[3]);
    if (!total.has_value() || !entries.has_value() || entries->size() % 4 != 0 || entries->size() / 4 > *total)
        return std::nullopt;
    auto index =
        FingerprintIndexFields { .version = std::string { AsStringView((*fields)[1]) }, .total = *total, .entries = {} };
    index.entries.reserve(entries->size() / 4);
    for (auto const row: std::views::iota(std::size_t { 0 }, entries->size() / 4))
    {
        auto const recorded = (*entries)[(4 * row) + 3];
        if (recorded.size() != 1 || std::to_integer<std::uint8_t>(recorded[0]) > 1)
            return std::nullopt;
        index.entries.push_back(FingerprintIndexEntry { .fingerprint = std::string { AsStringView((*entries)[4 * row]) },
                                                        .label = std::string { AsStringView((*entries)[(4 * row) + 1]) },
                                                        .compiler = std::string { AsStringView((*entries)[(4 * row) + 2]) },
                                                        .recorded = std::to_integer<std::uint8_t>(recorded[0]) == 1 });
    }
    return index;
}

} // namespace Pace::CompileCacheWire
