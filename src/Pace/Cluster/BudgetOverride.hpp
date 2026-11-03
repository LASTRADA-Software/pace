// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Cache/StorageTier.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/Errors/ConfigError.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace Pace::Cluster
{

/// @file BudgetOverride.hpp
/// A per-node cache budget the cluster records for one member, and the ONE grammar every door that sets one types it
/// in -- `pace-node --cluster-budget`, `pacectl cluster-budget` and the MCP `set_cache_budget` tool alike -- so a
/// client refusing early can never refuse what the leader accepts, nor the reverse.

/// A cache budget the cluster records for one member's tier, overriding what the machine's own flag configured.
///
/// **Recorded by the leader, applied by the member** (`BudgetOverrideApplier`, on the node): the leader cannot see a
/// node's cache flags, so it records the figure an operator asked for and the node decides, against its own
/// configuration, whether the override still stands. It decides nothing about where anything is SENT -- no address,
/// no credential, no peer -- so a replicated budget cannot be the rule `RefusedSettingTable` refuses: it changes how
/// much one machine keeps, on that machine.
struct BudgetOverride
{
    /// The budget, in bytes. Never zero: zero is how both tiers spell *unbounded* or *no tier* at startup, and an
    /// override removes no tier (`TierOverrideCannotRemoveTier`).
    std::uint64_t bytes {};

    /// What the member itself had configured for the tier when it first applied this override, reported by the member
    /// (MEMBER-EDIT `RecordBaseline`) and never by the leader, which cannot see a node's flags. Unset until the member
    /// reports it -- an override set while the member is down stays unset until it returns. A present zero is a real
    /// configuration (*unbounded*), never an absence.
    std::optional<std::uint64_t> baseline {};

    /// When the leader recorded it, in seconds since the Unix epoch, by the LEADER's clock: stamped when the leader
    /// proposes the change, never read inside `Apply`, which reads no clock. For display and for telling one override
    /// from the next (`RecordBaseline` names the override it answers by this and `bytes`); nothing compares it with
    /// another machine's clock.
    std::int64_t setAtUnixSeconds {};

    [[nodiscard]] friend bool operator==(BudgetOverride const&, BudgetOverride const&) = default;
};

/// What an override of zero is told.
///
/// Zero is how both tiers spell UNBOUNDED at startup, and `--cache-memory 0` is how a node builds no memory tier at
/// all -- so a zero arriving at a running tier would mean either "remove this tier", which a live store cannot do (the
/// tier stack is built once), or "unbound it", which is the opposite of what an override sent to shrink a node meant.
/// It is refused rather than given either reading, by the leader (`Validate`) and by every client before it sends.
inline constexpr std::string_view TierOverrideCannotRemoveTier = "an override cannot remove a tier";

/// One row per tier: how an override for it is spelled, and the startup flag whose grammar it is typed in.
struct BudgetTierRow
{
    StorageTier tier;      ///< The tier this row describes.
    std::string_view flag; ///< The startup flag that sizes this tier; a refusal names it.
};

/// One row per `StorageTier`, in enumerator order: the next tier is a row here, and an override for it cannot be
/// typed in a grammar its flag does not have.
inline constexpr EnumTable<StorageTier, BudgetTierRow> BudgetTierTable { {
    { .tier = StorageTier::Memory, .flag = "--cache-memory" },
    { .tier = StorageTier::Disk, .flag = "--cache-disk" },
} };

static_assert(RowsInEnumeratorOrder(BudgetTierTable, &BudgetTierRow::tier),
              "BudgetTierTable must hold one row per StorageTier, in enumerator order");

/// The tier @p name spells, by `StorageTierTable`'s names (`memory`, `disk`), exactly.
/// @param name A tier name as typed or as recorded.
/// @return The tier, or nullopt when no tier is called that.
[[nodiscard]] std::optional<StorageTier> TierNamed(std::string_view name) noexcept;

/// How @p tier is spelled: `StorageTierTable`'s name, the one word a typed override and a recorded command share.
/// @param tier A tier.
/// @return Its spelling, for a refusal and a column.
[[nodiscard]] constexpr std::string_view TierName(StorageTier tier) noexcept
{
    return TraitsFor(tier).name;
}

/// A budget override typed as text, held to the tier's startup flag's own grammar and limits.
///
/// `k`/`m`/`g` sizes through the very parser the flags use (`ParseByteSize`), and the zero the flags accept is refused
/// (`TierOverrideCannotRemoveTier`). **A share of RAM (`N%`) is refused**, although `--cache-memory` takes one: an
/// override is typed on one machine, recorded by the leader and applied on a third, and a percentage is resolved
/// against the RAM of whichever machine reads it. Resolving it here sizes the target by the wrong machine; recording
/// it verbatim would make a replicated figure that means a different number on every machine that reads the state.
/// So an override is an absolute size, and the refusal says so.
/// @param tier Which tier the budget is for.
/// @param text The budget as typed.
/// @return The budget in bytes, or why it is not one; `field` names the tier's flag.
[[nodiscard]] std::expected<std::uint64_t, ConfigError> ParseOverrideBudget(StorageTier tier, std::string_view text);

/// `<node>:memory=<size>,disk=<size>`, as `--cluster-budget` and `pacectl cluster-budget` take it.
struct BudgetSpec
{
    std::string memberId; ///< The member, by its id exactly.

    /// The budget asked for each tier, positionally by `StorageTier`; absent leaves that tier's override as it is.
    std::array<std::optional<std::uint64_t>, EnumeratorCount<StorageTier>> bytes {};
};

/// Parse `<node>:<tier>=<size>[,<tier>=<size>]`.
///
/// Split at the LAST `:`, since a tier list holds none and an id is opaque. Each tier at most once, at least one, each
/// size through `ParseOverrideBudget`. The id through `ParseMemberId`'s rule, and never empty.
/// @param text The value as typed.
/// @return The spec, or why it is not one, naming what was wrong.
[[nodiscard]] std::expected<BudgetSpec, std::string> ParseBudgetSpec(std::string_view text);

/// `<node>:<memory|disk>`, as `--cluster-budget-clear` and `pacectl cluster-budget-clear` take it.
struct BudgetClearSpec
{
    std::string memberId; ///< The member, by its id exactly.
    StorageTier tier {};  ///< Whose override to clear.
};

/// Parse `<node>:<tier>`, split at the LAST `:`.
/// @param text The value as typed.
/// @return The spec, or why it is not one.
[[nodiscard]] std::expected<BudgetClearSpec, std::string> ParseBudgetClearSpec(std::string_view text);

/// An override as one cell of text: `64M`, then `baseline=<size>` or `baseline=unset`, then `set=<UTC time>`.
///
/// The one rendering `--cluster-status` and `pacectl cluster-members` print, so the two say the same thing.
/// @param budget The override.
/// @return The text.
[[nodiscard]] std::string FormatBudgetOverride(BudgetOverride const& budget);

} // namespace Pace::Cluster
