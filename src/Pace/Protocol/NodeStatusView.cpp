// SPDX-License-Identifier: Apache-2.0
#include <Pace/Protocol/NodeStatusView.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <utility>

namespace Pace::NodeStatusView
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Which `NodeComponentBit` each component name stands for: one row per bit, so a bit added is a row.
    struct ComponentBit
    {
        std::uint32_t bit;     ///< The mask bit.
        std::string_view name; ///< What to call it.
    };

    constexpr std::array<ComponentBit, 4> ComponentBits { {
        { .bit = Wire::NodeComponentBit::CacheTier, .name = "cache-tier" },
        { .bit = Wire::NodeComponentBit::Worker, .name = "worker" },
        { .bit = Wire::NodeComponentBit::Scheduler, .name = "scheduler" },
        { .bit = Wire::NodeComponentBit::Consensus, .name = "consensus" },
    } };

    /// @param setting One row as it travelled.
    /// @return How a reader prints its value, or nothing for an unset row.
    [[nodiscard]] std::optional<std::string> ValueText(Wire::NodeSettingFields const& setting)
    {
        switch (setting.kind)
        {
            case Wire::WireSettingValue::Value:
                return setting.text;
            case Wire::WireSettingValue::Unset:
                return std::nullopt;
            case Wire::WireSettingValue::Redacted:
                return std::string { Wire::RedactedSettingText };
            case Wire::WireSettingValue::Omitted:
                return std::format("<omitted: {}>", setting.text);
        }
        // Unreachable: `ReadNodeSettings` skips a kind this build does not name.
        return std::nullopt;
    }
} // namespace

std::optional<std::vector<TierView>> TiersOf(CompileCacheWire::NodeRuntimeFields const& runtime)
{
    if (!runtime.tiers.has_value())
        return std::nullopt;
    auto views = std::vector<TierView> {};
    views.reserve(StorageTierTable.size());
    for (auto const& row: StorageTierTable)
    {
        auto const index = static_cast<std::size_t>(row.tier);
        views.push_back(TierView { .tier = row.tier,
                                   .name = row.name,
                                   .stats = index < runtime.tiers->size() ? (*runtime.tiers)[index] : std::nullopt });
    }
    return views;
}

std::optional<double> HitRatio(CompileCacheWire::NodeTierFields const& stats) noexcept
{
    auto const lookups = stats.hits + stats.misses;
    if (lookups == 0)
        return std::nullopt;
    return static_cast<double>(stats.hits) / static_cast<double>(lookups);
}

std::optional<std::uint64_t> BudgetBytes(CompileCacheWire::NodeTierFields const& stats) noexcept
{
    if (stats.bytesLimit == 0)
        return std::nullopt;
    return stats.bytesLimit;
}

std::optional<std::vector<SettingView>> SettingsOf(CompileCacheWire::NodeRuntimeFields const& runtime)
{
    if (!runtime.effectiveConfig.has_value())
        return std::nullopt;
    auto views = std::vector<SettingView> {};
    for (auto const& setting: *runtime.effectiveConfig)
    {
        auto value = ValueText(setting);
        if (views.empty() || views.back().name != setting.name)
            views.push_back(SettingView { .name = setting.name, .values = std::nullopt });
        if (!value.has_value())
            continue;
        auto& values = views.back().values;
        if (!values.has_value())
            values.emplace();
        values->push_back(std::move(*value));
    }
    return views;
}

std::string_view ToolchainStateName(CompileCacheWire::ToolchainState state) noexcept
{
    switch (state)
    {
        case CompileCacheWire::ToolchainState::Surveying:
            return "surveying";
        case CompileCacheWire::ToolchainState::Serving:
            return "serving";
        case CompileCacheWire::ToolchainState::NothingToServe:
            return "nothing-to-serve";
    }
    // Unreachable: `DecodeNodeRuntime` leaves a state this build has no name for DISENGAGED rather than
    // passing it through, so nothing but a named one arrives. Closed anyway -- falling off the end of a
    // function returning a view is a dangling one.
    return "unknown";
}

std::string_view SchedulerRoleName(CompileCacheWire::WireSchedulerRole role) noexcept
{
    switch (role)
    {
        case CompileCacheWire::WireSchedulerRole::Follower:
            return "follower";
        case CompileCacheWire::WireSchedulerRole::Undecided:
            return "undecided";
        case CompileCacheWire::WireSchedulerRole::Leader:
            return "leader";
    }
    // Unreachable for `ToolchainStateName`'s reason: `DecodeNodeRuntime` leaves a role
    // this build has no name for disengaged rather than passing it through.
    return "unknown";
}

std::string_view CordonStateName(CompileCacheWire::WireCordonState state) noexcept
{
    switch (state)
    {
        case CompileCacheWire::WireCordonState::Serving:
            return "serving";
        case CompileCacheWire::WireCordonState::Draining:
            return "draining";
        case CompileCacheWire::WireCordonState::Drained:
            return "drained";
    }
    // Unreachable for `ToolchainStateName`'s reason.
    return "unknown";
}

std::string_view ConsensusStandingName(CompileCacheWire::WireConsensusStanding standing) noexcept
{
    switch (standing)
    {
        case CompileCacheWire::WireConsensusStanding::NoCluster:
            return "no-cluster";
        case CompileCacheWire::WireConsensusStanding::Voter:
            return "voter";
        case CompileCacheWire::WireConsensusStanding::Learner:
            return "learner";
        case CompileCacheWire::WireConsensusStanding::Outsider:
            return "outsider";
    }
    // Unreachable for `ToolchainStateName`'s reason.
    return "unknown";
}

std::string_view SurfaceName(CompileCacheWire::WireSurface surface) noexcept
{
    switch (surface)
    {
        case CompileCacheWire::WireSurface::Admin:
            return "admin";
        case CompileCacheWire::WireSurface::Raft:
            return "raft";
        case CompileCacheWire::WireSurface::Discovery:
            return "discovery";
        case CompileCacheWire::WireSurface::Mcp:
            return "mcp";
    }
    // A tag this build does not name never reaches here -- `DecodeNodeStatus` SKIPS
    // one rather than refusing the whole reply, which is what lets an older client
    // read a newer node at all. Closed anyway, because falling off the end of a
    // function returning a view is a dangling one.
    return "unknown";
}

std::vector<std::string> ComponentNames(std::uint32_t mask)
{
    auto names = std::vector<std::string> {};
    std::uint32_t named = 0;
    for (auto const& row: ComponentBits)
        if ((mask & row.bit) != 0)
        {
            named |= row.bit;
            names.emplace_back(row.name);
        }
    if (auto const unknown = mask & ~named; unknown != 0)
        names.push_back(std::format("unknown(0x{:x})", unknown));
    return names;
}

} // namespace Pace::NodeStatusView
