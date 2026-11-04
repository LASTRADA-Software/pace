// SPDX-License-Identifier: Apache-2.0
#include <Pace/Config/ByteSize.hpp>
#include <Pace/Core/Utf8.hpp>
#include <Pace/Protocol/NodeStatusView.hpp>

#include <array>
#include <chrono>
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
    /// @return How a reader prints its value, or nothing for an unset row. A value that is not UTF-8 is
    ///         marked and escaped (`ReadableText`), never silently replaced.
    [[nodiscard]] std::optional<std::string> ValueText(Wire::NodeSettingFields const& setting)
    {
        switch (setting.kind)
        {
            case Wire::WireSettingValue::Value:
                return ReadableText(setting.text);
            case Wire::WireSettingValue::Unset:
                return std::nullopt;
            case Wire::WireSettingValue::Redacted:
                return std::string { Wire::RedactedSettingText };
            case Wire::WireSettingValue::Omitted:
                return std::format("<omitted: {}>", ReadableText(setting.text));
        }
        // Unreachable: `ReadNodeSettings` skips a kind this build does not name.
        return std::nullopt;
    }
} // namespace

std::string ReadableText(std::string_view text)
{
    if (IsValidUtf8(text))
        return std::string { text };
    // Every valid sequence kept as it is, every byte that starts none spelled `\xHH`, and a backslash doubled so a
    // reader can tell an escape from the text; the marker says the value was escaped at all.
    auto escaped = std::string {};
    while (!text.empty())
    {
        auto const length = Utf8SequenceLength(text);
        if (length == 0)
        {
            escaped += std::format("\\x{:02X}", static_cast<unsigned>(static_cast<unsigned char>(text.front())));
            text.remove_prefix(1);
            continue;
        }
        if (text.front() == '\\')
            escaped += '\\';
        escaped.append(text.substr(0, length));
        text.remove_prefix(length);
    }
    return std::format("{}{}>", NonUtf8Marker, escaped);
}

std::optional<CeilingsView> CeilingsOf(CompileCacheWire::NodeRuntimeFields const& runtime)
{
    if (!runtime.slotCeilings.has_value())
        return std::nullopt;
    auto const& record = *runtime.slotCeilings;
    auto view = CeilingsView { .available = record.available,
                               .binding = record.binding,
                               .sampledSecondsAgo = record.sampledSecondsAgo,
                               .ceilings = {} };
    view.ceilings.reserve(record.ceilings.size());
    for (auto const& ceiling: record.ceilings)
        view.ceilings.push_back(
            CeilingView { .name = ceiling.name, .slots = ceiling.slots, .binding = ceiling.name == record.binding });
    return view;
}

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
    // Summed as doubles, never as integers: two 64-bit counters can sum past `UINT64_MAX`, and a wrapped sum
    // reads a near-perfect cache as one that answered more than it was asked.
    if (stats.hits == 0 && stats.misses == 0)
        return std::nullopt;
    auto const hits = static_cast<double>(stats.hits);
    return hits / (hits + static_cast<double>(stats.misses));
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
        auto name = ReadableText(setting.name);
        if (views.empty() || views.back().name != name)
            views.push_back(SettingView { .name = std::move(name), .values = std::nullopt, .escaped = false });
        if (!IsValidUtf8(setting.name) || !IsValidUtf8(setting.text))
            views.back().escaped = true;
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

std::optional<std::vector<BudgetOverrideView>> BudgetOverridesOf(CompileCacheWire::NodeRuntimeFields const& runtime)
{
    if (!runtime.budgetOverrides.has_value())
        return std::nullopt;
    auto views = std::vector<BudgetOverrideView> {};
    views.reserve(runtime.budgetOverrides->size());
    for (auto const& row: *runtime.budgetOverrides)
    {
        auto setAt = std::optional<std::string> {};
        if (row.setAtUnixSeconds.has_value())
            setAt = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                std::chrono::sys_seconds { std::chrono::seconds { *row.setAtUnixSeconds } });
        views.push_back(BudgetOverrideView { .tier = row.tier < StorageTierTable.size()
                                                         ? std::string { StorageTierTable[row.tier].name }
                                                         : std::format("tier-{}", row.tier),
                                             .effectiveBytes = row.bytes,
                                             .source = ReadableText(row.source),
                                             .configuredBytes = row.configuredBytes,
                                             .overrideBytes = row.overrideBytes,
                                             .baselineBytes = row.baselineBytes,
                                             .setAt = std::move(setAt),
                                             .setBy = ReadableText(row.setBy),
                                             .setByName = ReadableText(row.setByName),
                                             .excessBytes = row.excessBytes,
                                             .note = ReadableText(row.note) });
    }
    return views;
}

std::string DescribeBudget(BudgetOverrideView const& view)
{
    auto const size = [](std::uint64_t bytes) {
        return bytes == 0 ? std::string { "unbounded" } : FormatByteSize(static_cast<std::size_t>(bytes));
    };
    auto text = view.effectiveBytes.has_value() ? std::format("{} from {}", size(*view.effectiveBytes), view.source)
                                                : std::string { "no such tier" };
    text += std::format(" (configured {}", size(view.configuredBytes));
    if (view.overrideBytes.has_value())
    {
        text += std::format(", override {}, baseline {}",
                            size(*view.overrideBytes),
                            view.baselineBytes.has_value() ? size(*view.baselineBytes) : std::string { "unset" });
        if (view.setAt.has_value())
            text += std::format(", set {}", *view.setAt);
        if (!view.setBy.empty())
            text += view.setByName.empty() ? std::format(" by {}", view.setBy)
                                           : std::format(" by {} \"{}\"", view.setBy, view.setByName);
    }
    if (view.excessBytes.has_value() && *view.excessBytes != 0)
        text += std::format(", converging: {} over", FormatByteSize(static_cast<std::size_t>(*view.excessBytes)));
    text += ")";
    if (!view.note.empty())
        text += std::format("; {}", view.note);
    return text;
}

} // namespace Pace::NodeStatusView
