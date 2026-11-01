// SPDX-License-Identifier: Apache-2.0
#include "NodeNameResolver.hpp"

#include <format>
#include <ranges>
#include <vector>

namespace Pace::Node
{

namespace
{
    /// @param name What the person typed.
    /// @return It quoted, or its length when it is too long to quote back.
    [[nodiscard]] std::string Quoted(std::string_view name)
    {
        if (name.size() > MaxQuotedNameBytes)
            return std::format("a name of {} bytes", name.size());
        return std::format("\"{}\"", name);
    }

    /// @param ids Member ids.
    /// @return Them listed, at most `MaxListedCandidates`, the rest counted.
    [[nodiscard]] std::string Listed(std::vector<std::string_view> const& ids)
    {
        auto out = std::string {};
        for (auto const id: ids | std::views::take(MaxListedCandidates))
            out += std::format("{}{}", out.empty() ? "" : ", ", id);
        if (ids.size() > MaxListedCandidates)
            out += std::format(" and {} more", ids.size() - MaxListedCandidates);
        return out;
    }
} // namespace

std::expected<Consensus::NodeId, std::string> ResolveNodeName(Cluster::ClusterState const& state,
                                                              std::string_view idOrDisplayName)
{
    if (idOrDisplayName.empty())
        return std::unexpected { std::string { "an empty name names no member; omit `node` to ask this node" } };

    for (auto const& matcher: NameMatchers)
    {
        auto matched = std::vector<std::string_view> {};
        for (auto const& member: state.members)
            if (matcher.matches(member, idOrDisplayName))
                matched.emplace_back(member.id);
        if (matched.size() == 1)
            return Consensus::NodeId { matched.front() };
        if (!matched.empty())
            return std::unexpected { std::format("{} names {} members by {}: {}; name one by its id",
                                                 Quoted(idOrDisplayName),
                                                 matched.size(),
                                                 matcher.what,
                                                 Listed(matched)) };
    }

    if (state.members.empty())
        return std::unexpected { std::format("no member is named {}: this node's applied cluster state records no members",
                                             Quoted(idOrDisplayName)) };
    auto ids = std::vector<std::string_view> {};
    ids.reserve(state.members.size());
    for (auto const& member: state.members)
        ids.emplace_back(member.id);
    return std::unexpected { std::format("no member is named {} (names match exactly, never by prefix); the members are: {}",
                                         Quoted(idOrDisplayName),
                                         Listed(ids)) };
}

} // namespace Pace::Node
