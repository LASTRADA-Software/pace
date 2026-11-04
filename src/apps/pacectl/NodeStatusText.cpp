// SPDX-License-Identifier: Apache-2.0
#include "NodeStatusText.hpp"

#include <Pace/Protocol/NodeStatusView.hpp>

#include <array>
#include <format>

namespace Pace::Cli
{

std::string DescribeComponents(std::uint32_t mask)
{
    // The names are `NodeStatusView`'s, which the MCP `node_status` tool reads too, so the two cannot name one
    // component differently; a bit this build does not name arrives as `unknown(0x..)` rather than dropped.
    std::string out;
    for (auto const& name: NodeStatusView::ComponentNames(mask))
    {
        if (!out.empty())
            out += ", ";
        out += name;
    }
    return out.empty() ? std::string { "none" } : out;
}

std::string_view NameOfToolchainState(CompileCacheWire::ToolchainState state) noexcept
{
    return NodeStatusView::ToolchainStateName(state);
}

std::string_view NameOfSchedulerRole(CompileCacheWire::WireSchedulerRole role) noexcept
{
    return NodeStatusView::SchedulerRoleName(role);
}

std::string_view NameOfSharedCacheSource(CompileCacheWire::WireSharedCacheSource source) noexcept
{
    // A `switch` for `NameOfToolchainState`'s reason and one more: the enum is transmitted with
    // explicit values and no `Last`, so no `EnumTable` can hold it. A source added and not spelled
    // here fails the GCC and Clang legs (`-Wswitch`, fatal under `PEDANTIC_COMPILER_WERROR`) and the
    // clang-tidy sweep; the MSVC and clang-cl legs do not fail, having no `/WX` or `-Werror` and cl's
    // C4062 being off even at `/W4` (`SharedCacheStatusOf` says the same about its own switch).
    switch (source)
    {
        case CompileCacheWire::WireSharedCacheSource::None:
            return "none";
        case CompileCacheWire::WireSharedCacheSource::Setting:
            return "setting";
        case CompileCacheWire::WireSharedCacheSource::Override:
            return "override";
        case CompileCacheWire::WireSharedCacheSource::ThisMachine:
            return "this-machine";
    }
    // Unreachable: `ReadSharedCacheStatus` leaves a record carrying a source this build has no
    // name for ABSENT rather than passing the byte through.
    return "unknown";
}

std::string_view NameOfSharedCacheState(CompileCacheWire::WireSharedCacheState state) noexcept
{
    switch (state)
    {
        case CompileCacheWire::WireSharedCacheState::NotTried:
            return "not-tried";
        case CompileCacheWire::WireSharedCacheState::Proven:
            return "proven";
        case CompileCacheWire::WireSharedCacheState::Unresolved:
            return "unresolved";
        case CompileCacheWire::WireSharedCacheState::WrongKey:
            return "wrong-key";
        case CompileCacheWire::WireSharedCacheState::Unreachable:
            return "unreachable";
        case CompileCacheWire::WireSharedCacheState::Serving:
            return "serving";
        case CompileCacheWire::WireSharedCacheState::Unavailable:
            return "unavailable";
        case CompileCacheWire::WireSharedCacheState::ProofRefused:
            return "proof-refused";
    }
    // Unreachable for `NameOfSharedCacheSource`'s reason.
    return "unknown";
}

std::optional<std::vector<ConditionMention>> ConditionsAskingForAttention(
    std::optional<std::vector<CompileCacheWire::NodeConditionFields>> const& conditions)
{
    if (!conditions.has_value())
        return std::nullopt;

    std::vector<ConditionMention> mentions;
    for (auto const& row: *conditions)
    {
        if (!CompileCacheWire::AsksForAttention(row))
            continue;
        // The state rides along only where it is not the plain `raised`, so the ordinary line stays
        // short and the unusual one cannot be mistaken for it.
        auto const plain = CompileCacheWire::ConditionStateNamed(row.state) == CompileCacheWire::ConditionState::Raised;
        mentions.push_back(
            ConditionMention { .id = row.id,
                               .persistence = row.persistence,
                               .unusualState = plain ? std::nullopt : std::optional<std::string> { row.state },
                               .severity = CompileCacheWire::ConditionSeverityNamed(row.severity) });
    }
    return mentions;
}

std::optional<std::string> DescribeConditions(
    std::optional<std::vector<CompileCacheWire::NodeConditionFields>> const& conditions)
{
    auto const mentions = ConditionsAskingForAttention(conditions);
    if (!mentions.has_value())
        return std::nullopt;
    if (mentions->empty())
        return std::string { NoConditionsRaised };

    std::string raised;
    for (auto const& mention: *mentions)
        raised += std::format("{}{} ({}{}{})",
                              raised.empty() ? "" : ", ",
                              mention.id,
                              mention.persistence,
                              mention.unusualState.has_value() ? ", " : "",
                              mention.unusualState.value_or(std::string {}));
    return std::format("raised: {}", raised);
}

bool ReportsConsensus(CompileCacheWire::NodeStatusFields const* status) noexcept
{
    return status != nullptr
           && ((status->components & CompileCacheWire::NodeComponentBit::Consensus) != 0
               || status->runtime.schedulerRole.has_value());
}

std::optional<std::string> NamedLeader(CompileCacheWire::NodeStatusFields const& status)
{
    auto const& runtime = status.runtime;
    if (!runtime.leaderEndpoint.empty())
        return runtime.leaderEndpoint;
    if (runtime.schedulerRole == CompileCacheWire::WireSchedulerRole::Leader)
        return std::string { SelfLeader };
    return std::nullopt;
}

} // namespace Pace::Cli
