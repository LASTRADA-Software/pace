// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Consensus/RaftTypes.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace Pace::Node
{

/// @file NodeNameResolver.hpp
/// Which member a person means by a name: what the MCP node-scope tools resolve their `node` argument through.
///
/// **Exact, and only exact.** A name matches a member's id exactly, then its replicated display name
/// (`ClusterMember::displayName`) exactly -- byte for byte, never case folded: uniqueness is folded so two names cannot
/// differ only in case, and resolving exactly is what keeps a name a person typed meaning one spelling. **There is no
/// prefix matching on ids**, because a prefix that matches one member today matches
/// two after the next join, and the tool would then answer about a machine nobody named. An ambiguous or unknown
/// name is an error listing the members it could have meant, never a guess.

/// One way a name can match a member, tried in `NameMatchers` order.
struct NameMatchRow
{
    std::string_view what;                                                        ///< What it matches, in words.
    bool (*matches)(Cluster::ClusterMember const& member, std::string_view name); ///< Whether @p name names @p member.
};

/// Every way a name matches, in the order they are asked. The first way that matches ANY member decides: one
/// match is the member, several are ambiguous. **A way of naming is a row here**, so the display name is one row
/// after the id and the resolver does not change. An id wins over a display name spelled the same, which the leader
/// refuses to record (`Cluster::DisplayNameHolder`) but an older state may hold.
inline constexpr std::array NameMatchers {
    NameMatchRow {
        .what = "id",
        .matches = [](Cluster::ClusterMember const& member, std::string_view name) { return member.id == name; } },
    NameMatchRow {
        .what = "display name",
        .matches = [](Cluster::ClusterMember const& member,
                      std::string_view name) { return !member.displayName.empty() && member.displayName == name; } },
};

/// The replicated display name of member @p id, when @p state records one.
/// @param state The applied cluster state.
/// @param id A member's id.
/// @return Its display name; nothing when the state does not record the member or nobody named it.
[[nodiscard]] std::optional<std::string> DisplayNameIn(Cluster::ClusterState const& state, std::string_view id);

/// The most members an error lists by id; the rest are counted.
inline constexpr std::size_t MaxListedCandidates = 32;

/// The longest name an error quotes back; a longer one is described by its length.
inline constexpr std::size_t MaxQuotedNameBytes = 128;

/// Resolve a name to the one member it names.
/// @param state The applied cluster state.
/// @param idOrDisplayName What the person typed.
/// @return The member's id, or an error saying the name is empty, names nobody, or names several -- listing the
///         candidates.
[[nodiscard]] std::expected<Consensus::NodeId, std::string> ResolveNodeName(Cluster::ClusterState const& state,
                                                                            std::string_view idOrDisplayName);

/// The cluster state this node has applied, read where a name is resolved.
///
/// A seam so a node-scope tool resolves names against the state the consensus tier last applied, and a test can
/// state one. Read on the MCP thread while the consensus tier writes it, so an implementation is thread-safe.
class IAppliedClusterView
{
  public:
    IAppliedClusterView() = default;
    IAppliedClusterView(IAppliedClusterView const&) = delete;
    IAppliedClusterView& operator=(IAppliedClusterView const&) = delete;
    IAppliedClusterView(IAppliedClusterView&&) = delete;
    IAppliedClusterView& operator=(IAppliedClusterView&&) = delete;
    virtual ~IAppliedClusterView() = default;

    /// @return The state last applied, or DISENGAGED on a node that has applied none -- it runs no consensus, or
    ///         its log has not been read yet.
    [[nodiscard]] virtual std::optional<Cluster::ClusterState> AppliedState() const = 0;
};

} // namespace Pace::Node
