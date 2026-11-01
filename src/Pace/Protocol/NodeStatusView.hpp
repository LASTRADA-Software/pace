// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Cache/StorageTier.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file NodeStatusView.hpp
/// How a person reads the detail a `NodeStatus` reply carries: the ONE derivation `pacectl node` and the MCP
/// `node_status` tool both render from, so the two cannot disagree about a quantity both show.
///
/// Each function is pure over a DECODED reply (`CompileCacheWire::DecodeNodeStatus`): which tiers a cache has
/// and what each holds, what a tier's hit ratio is, what a tier's budget means when it is zero, and how the
/// configuration's rows read once a list setting's rows are gathered and a redaction is spelled. Nothing here
/// computes what the sender could not -- the hit ratio is the one figure derived, and it is derived HERE, once,
/// rather than in each renderer.

namespace Pace::NodeStatusView
{

/// What opens a value that is not valid UTF-8, as `ReadableText` renders it: the reader is TOLD the value was
/// escaped, rather than handed a replacement character that names a different path.
inline constexpr std::string_view NonUtf8Marker = "<non-utf8: ";

/// Text a node sent, as a reader prints it.
/// @param text The bytes as they travelled.
/// @return Them unchanged when they are UTF-8; otherwise `<non-utf8: ...>`, every byte that starts no valid
///         sequence spelled `\xHH` and a backslash doubled -- distinguishable from any UTF-8 value, and never lossy.
[[nodiscard]] std::string ReadableText(std::string_view text);

/// One ceiling on a worker's slots as a reader reads it.
struct CeilingView
{
    std::string name;                   ///< The limit, as the node named it.
    std::optional<std::uint32_t> slots; ///< What it allows, or DISENGAGED where it did not apply.
    bool binding { false };             ///< Whether it is the one that produced `available`.
};

/// Every ceiling on a worker's slots, as a reader reads them.
struct CeilingsView
{
    std::uint32_t available { 0 };         ///< The slots offerable now.
    std::string binding;                   ///< The binding ceiling's name.
    std::uint64_t sampledSecondsAgo { 0 }; ///< How old the heartbeat reading is.
    std::vector<CeilingView> ceilings;     ///< One per limit, in the node's order.
};

/// The slot ceilings a reply reports.
/// @param runtime The decoded runtime record.
/// @return The ceilings, each marked whether it binds; DISENGAGED with no worker or before its first heartbeat.
[[nodiscard]] std::optional<CeilingsView> CeilingsOf(CompileCacheWire::NodeRuntimeFields const& runtime);

/// One cache tier as a reader names it.
struct TierView
{
    StorageTier tier;      ///< The tier, as this build names it.
    std::string_view name; ///< `StorageTierTable`'s spelling.

    /// What it holds, or DISENGAGED when the cache has no such tier -- which is not a tier holding nothing.
    std::optional<CompileCacheWire::NodeTierFields> stats;
};

/// Every tier this build names, as a reply reports it.
/// @param runtime The decoded runtime record.
/// @return One row per `StorageTierTable` row, in its order; DISENGAGED when the node has no cache at all. A
///         position past this build's table names a tier this build cannot name, and is not shown.
[[nodiscard]] std::optional<std::vector<TierView>> TiersOf(CompileCacheWire::NodeRuntimeFields const& runtime);

/// The share of lookups a tier answered.
/// @param stats A tier's figures.
/// @return Hits over hits plus misses, in [0, 1] even when the two counters sum past `UINT64_MAX`; DISENGAGED when
///         nothing was looked up, since a ratio of no lookups is no reading at all, and zero would be a cache that
///         missed everything.
[[nodiscard]] std::optional<double> HitRatio(CompileCacheWire::NodeTierFields const& stats) noexcept;

/// A tier's budget as a reader takes it.
/// @param stats A tier's figures.
/// @return The budget in bytes, or DISENGAGED when it is zero -- the storage's spelling of UNBOUNDED, never a
///         budget of nothing.
[[nodiscard]] std::optional<std::uint64_t> BudgetBytes(CompileCacheWire::NodeTierFields const& stats) noexcept;

/// One setting of the configuration in force, a list setting's rows gathered.
struct SettingView
{
    std::string name; ///< The flag, as the node's table spells it.

    /// Its values as a reader prints them, in the order sent: a value as the node wrote it (through `ReadableText`,
    /// so one that is not UTF-8 is marked), `RedactedSettingText` for a credential, `<omitted: why>` for a value the
    /// record could not carry. DISENGAGED when unset.
    std::optional<std::vector<std::string>> values;

    /// Whether the name or any value was not UTF-8 and was escaped: the flag a structured reader keys on, so a value
    /// that merely LOOKS like the marker cannot pass for an escaped one, or the reverse.
    bool escaped { false };
};

/// The configuration in force, as a reader prints it.
///
/// Consecutive rows of one name are one setting -- a list setting travels a row per value -- and an `Unset`
/// row is a setting with no value rather than an empty one.
/// @param runtime The decoded runtime record.
/// @return The settings in the order sent; DISENGAGED from a node that did not say.
[[nodiscard]] std::optional<std::vector<SettingView>> SettingsOf(CompileCacheWire::NodeRuntimeFields const& runtime);

/// What a person calls one toolchain-survey state; `pacectl node` and `node_status` share the spelling.
/// @param state The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view ToolchainStateName(CompileCacheWire::ToolchainState state) noexcept;

/// What a person calls one scheduler role.
/// @param role The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view SchedulerRoleName(CompileCacheWire::WireSchedulerRole role) noexcept;

/// What a person calls one cordon state.
/// @param state The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view CordonStateName(CompileCacheWire::WireCordonState state) noexcept;

/// What a person calls one consensus standing: `Consensus::StandingTable`'s words, hyphenated where that table
/// has a space, so a record field stays one token in every format.
/// @param standing The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view ConsensusStandingName(CompileCacheWire::WireConsensusStanding standing) noexcept;

/// What a person calls one reported surface.
/// @param surface The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view SurfaceName(CompileCacheWire::WireSurface surface) noexcept;

/// The components a `NodeStatus` mask names.
/// @param mask `NodeStatusFields::components`.
/// @return Each named component, in `NodeComponentBit` order, then `unknown(0x..)` for any bit this build does
///         not name -- never dropped, since *something I do not know runs here* is a fact.
[[nodiscard]] std::vector<std::string> ComponentNames(std::uint32_t mask);

} // namespace Pace::NodeStatusView
