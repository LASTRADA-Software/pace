// SPDX-License-Identifier: Apache-2.0
#include "NodeNameResolver.hpp"

#include <algorithm>
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

std::optional<std::string> DisplayNameIn(Cluster::ClusterState const& state, std::string_view id)
{
    auto const member = std::ranges::find(state.members, id, &Cluster::ClusterMember::id);
    if (member == state.members.end() || member->displayName.empty())
        return std::nullopt;
    return member->displayName;
}

std::expected<Consensus::NodeId, std::string> ResolveNodeName(Cluster::ClusterState const& state,
                                                              std::string_view idOrDisplayName)
{
    if (idOrDisplayName.empty())
        return std::unexpected { std::string { "an empty name names no member; omit `node` to ask this node" } };

    // Every way of naming is asked, and the members they find are pooled: a name that is one member's id and ANOTHER
    // member's display name is ambiguous, and refused naming both, never resolved by the order the rows happen to
    // be in. The leader never records that pair -- an admission clears a name its id would shadow -- but a state
    // applied before that clearing reached this node, or a race between two proposals, may hold it for a moment.
    auto matched = std::vector<std::string_view> {};
    auto described = std::vector<std::string> {};
    auto labelled = std::vector<std::string_view> {};
    for (auto const& matcher: NameMatchers)
        for (auto const& member: state.members)
            if (matcher.matches(member, idOrDisplayName))
            {
                if (!std::ranges::contains(matched, std::string_view { member.id }))
                    matched.emplace_back(member.id);
                described.push_back(std::format("{} (by {})", member.id, matcher.what));
                if (matcher.kind == NameKind::Label && member.id != idOrDisplayName)
                    labelled.emplace_back(member.id);
            }
    if (matched.size() == 1)
        return Consensus::NodeId { matched.front() };
    if (!matched.empty())
    {
        // The way out has to work for EVERY candidate, and "name one by its id" does not: when the input IS one
        // member's id, typing that id is the ambiguous input itself. So the refusal names whose LABEL collides --
        // reachable by its own id now -- and the rename that leaves the input naming the other member alone.
        auto const list = std::vector<std::string_view> { described.begin(), described.end() };
        return std::unexpected { std::format(
            "{} names {} members: {}; the display name of {} collides -- rename it (pacectl --cluster-name=<id>=<name>, "
            "or set_display_name naming it by its id), after which {} names the other alone; until then name {} by its "
            "id",
            Quoted(idOrDisplayName),
            matched.size(),
            Listed(list),
            Listed(labelled),
            Quoted(idOrDisplayName),
            Listed(labelled)) };
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
