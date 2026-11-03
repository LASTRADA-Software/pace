// SPDX-License-Identifier: Apache-2.0
#include <Pace/Cli/MemberId.hpp>
#include <Pace/Cli/Options.hpp>
#include <Pace/Cluster/BudgetOverride.hpp>
#include <Pace/Config/ByteSize.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace Pace::Cluster
{

std::optional<StorageTier> TierNamed(std::string_view name) noexcept
{
    for (auto const& row: StorageTierTable)
        if (row.name == name)
            return row.tier;
    return std::nullopt;
}

std::expected<std::uint64_t, ConfigError> ParseOverrideBudget(StorageTier tier, std::string_view text)
{
    auto const& row = BudgetTierTable[static_cast<std::size_t>(tier)];
    auto const refuse = [&row](ConfigErrorCode code, std::string context) {
        auto error = ArgvError(code, std::move(context));
        error.field = std::string { row.flag };
        return std::unexpected(std::move(error));
    };

    // Before the size parser, which would call it a type mismatch: the operator typed something `--cache-memory`
    // takes, and deserves to be told why an override does not.
    if (text.ends_with('%'))
        return refuse(ConfigErrorCode::TypeMismatch,
                      std::format("{} is a share of the RAM of whichever machine reads it, and an override is typed "
                                  "here, recorded by the leader and applied on its member: give an absolute size "
                                  "(k, m or g)",
                                  text));

    // No host total, so the grammar is exactly the flags' `k`/`m`/`g` and nothing reads this machine.
    auto const parsed = ParseByteSize(text);
    if (!parsed.has_value())
    {
        auto error = parsed.error();
        error.field = std::string { row.flag };
        return std::unexpected(std::move(error));
    }
    if (*parsed == 0)
        return refuse(ConfigErrorCode::OutOfRange, std::string { TierOverrideCannotRemoveTier });
    return static_cast<std::uint64_t>(*parsed);
}

namespace
{
    /// Split `<node>:<rest>` at the LAST `:`, holding the id to `ParseMemberId`'s rule and to being non-empty.
    /// @param text The value as typed.
    /// @param shape What the value should look like, for the refusal.
    /// @return The id and the rest, or why the value is not that shape.
    [[nodiscard]] std::expected<std::pair<std::string, std::string_view>, std::string> SplitMember(std::string_view text,
                                                                                                   std::string_view shape)
    {
        auto const split = text.rfind(':');
        if (split == std::string_view::npos || split == 0 || split + 1 == text.size())
            return std::unexpected(std::format("not {}: {}", shape, text));
        auto id = ParseMemberId(text.substr(0, split));
        if (!id.has_value())
            return std::unexpected(std::format("the member id {}", id.error().context));
        return std::pair { *std::move(id), text.substr(split + 1) };
    }

    /// The tier a typed name spells, or a refusal listing the names there are.
    /// @param name The name as typed.
    /// @return The tier, or why it is none.
    [[nodiscard]] std::expected<StorageTier, std::string> TierOrRefusal(std::string_view name)
    {
        if (auto const tier = TierNamed(name); tier.has_value())
            return *tier;
        auto names = std::string {};
        for (auto const& row: StorageTierTable)
            names += std::format("{}{}", names.empty() ? "" : " or ", row.name);
        return std::unexpected(std::format("{} is no cache tier; a budget is set for {}", name, names));
    }
} // namespace

std::expected<BudgetSpec, std::string> ParseBudgetSpec(std::string_view text)
{
    constexpr auto Shape = std::string_view { "<node-id>:memory=<size>[,disk=<size>]" };
    auto split = SplitMember(text, Shape);
    if (!split.has_value())
        return std::unexpected(std::move(split).error());

    auto spec = BudgetSpec { .memberId = std::move(split->first), .bytes = {} };
    for (auto const part: std::views::split(split->second, ','))
    {
        auto const item = std::string_view { part.begin(), part.end() };
        auto const equals = item.find('=');
        if (equals == std::string_view::npos)
            return std::unexpected(std::format("not {}: {}", Shape, text));
        auto const tier = TierOrRefusal(item.substr(0, equals));
        if (!tier.has_value())
            return std::unexpected(tier.error());
        auto& slot = spec.bytes[static_cast<std::size_t>(*tier)];
        if (slot.has_value())
            return std::unexpected(std::format("{} is named twice in {}", TierName(*tier), text));
        auto const bytes = ParseOverrideBudget(*tier, item.substr(equals + 1));
        if (!bytes.has_value())
            return std::unexpected(
                std::format("{} ({}'s grammar): {}", TierName(*tier), bytes.error().field, bytes.error().context));
        slot = *bytes;
    }
    return spec;
}

std::expected<BudgetClearSpec, std::string> ParseBudgetClearSpec(std::string_view text)
{
    auto split = SplitMember(text, "<node-id>:<memory|disk>");
    if (!split.has_value())
        return std::unexpected(std::move(split).error());
    auto const tier = TierOrRefusal(split->second);
    if (!tier.has_value())
        return std::unexpected(tier.error());
    return BudgetClearSpec { .memberId = std::move(split->first), .tier = *tier };
}

std::string FormatBudgetOverride(BudgetOverride const& budget)
{
    auto const at = std::chrono::sys_seconds { std::chrono::seconds { budget.setAtUnixSeconds } };
    return std::format("{} baseline={} set={:%Y-%m-%dT%H:%M:%SZ}",
                       FormatByteSize(static_cast<std::size_t>(budget.bytes)),
                       budget.baseline.has_value() ? FormatByteSize(static_cast<std::size_t>(*budget.baseline))
                                                   : std::string { "unset" },
                       at);
}

} // namespace Pace::Cluster
