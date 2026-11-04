// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"
#include "NodeNameResolver.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file McpUpstreamFakes.hpp
/// The one scripted `IMcpUpstream` every MCP tool test of this node uses.
///
/// **Shared, not copied per test file** (`.agent/rules/testing.md`, *a test FAKE is a shared helper too*): four
/// private copies had already drifted -- one dropped the frames it was asked, so a tool sending the wrong verb
/// passed its tests. This one records every call whole, and takes the PARTICIPANT it answers as -- who answered and
/// the term this node holds -- as a required, undefaulted argument, so no case inherits another file's idea of
/// who the leader is.

namespace Pace::Node::McpFakes
{

/// One call the scripted upstream was asked.
struct AskedCall
{
    UpstreamTarget target;     ///< Whom.
    std::string nodeIdOrEmpty; ///< Which node, for a node target; empty otherwise.
    std::string frame;         ///< The whole request frame.
};

/// Who a scripted upstream answers as.
struct UpstreamParticipant
{
    std::string answeredBy;            ///< What every `Ok` names as the answering node.
    std::optional<std::uint64_t> term; ///< The term this node holds, or nothing when it knows none.
};

/// @param bytes A frame or payload.
/// @return The same bytes, as the upstream carries them.
[[nodiscard]] inline std::string AsUpstreamText(std::span<std::byte const> bytes)
{
    return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

/// An upstream that answers every call with one scripted result and records every call whole.
class ScriptedUpstream final: public IMcpUpstream
{
  public:
    /// @param who Who an `Ok` names as the answering node, and under which term.
    /// @param unscripted The failure every call answers until a case scripts an answer.
    ScriptedUpstream(UpstreamParticipant who, UpstreamFailure unscripted):
        answer { std::unexpected { std::pair { unscripted, std::string { "unscripted" } } } },
        participant { std::move(who) }
    {
    }

    /// @copydoc IMcpUpstream::Ask
    [[nodiscard]] UpstreamResult Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame) override
    {
        calls.push_back(
            AskedCall { .target = target, .nodeIdOrEmpty = std::string { nodeIdOrEmpty }, .frame = std::move(frame) });
        return answer;
    }

    /// Answer every call `Ok` with @p payload, as the participant.
    /// @param payload The reply body.
    void AnswerOk(std::string payload)
    {
        answer = UpstreamReply { .status = CompileCacheWire::Status::Ok,
                                 .payload = std::move(payload),
                                 .answeredBy = participant.answeredBy,
                                 .term = participant.term };
    }

    /// Answer every call `Ok` with @p payload, as the participant.
    /// @param payload The reply body, as the wire encoders produce it.
    void AnswerOk(std::span<std::byte const> payload)
    {
        AnswerOk(AsUpstreamText(payload));
    }

    UpstreamResult answer;           ///< What every call answers; a case may set it whole.
    std::vector<AskedCall> calls;    ///< Every call, in order.
    UpstreamParticipant participant; ///< Who every `Ok` names, and under which term.
};

/// An upstream that answers each call through a function of that call, and records every call whole.
///
/// For a tool that asks more than once and whose later questions depend on the earlier answers -- paging a record,
/// resolving a name and then asking about what it resolved to -- which one scripted answer cannot exercise.
class RespondingUpstream final: public IMcpUpstream
{
  public:
    /// What answers one call.
    using Responder = std::function<UpstreamResult(AskedCall const& call)>;

    /// @param respond Answers every call; called on the thread that asks.
    explicit RespondingUpstream(Responder respond):
        _respond { std::move(respond) }
    {
    }

    /// @copydoc IMcpUpstream::Ask
    [[nodiscard]] UpstreamResult Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame) override
    {
        _calls.push_back(
            AskedCall { .target = target, .nodeIdOrEmpty = std::string { nodeIdOrEmpty }, .frame = std::move(frame) });
        return _respond(_calls.back());
    }

    /// @return Every call, in order.
    [[nodiscard]] std::vector<AskedCall> const& Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::vector<AskedCall> _calls;
    Responder _respond;
};

/// @param ids The members' ids.
/// @return A cluster state holding exactly those members, in that order.
[[nodiscard]] inline Cluster::ClusterState StateOf(std::initializer_list<std::string_view> ids)
{
    auto state = Cluster::ClusterState {};
    for (auto const id: ids)
    {
        auto member = Cluster::ClusterMember {};
        member.id = std::string { id };
        state.members.push_back(std::move(member));
    }
    return state;
}

/// A cluster view holding one state, or none.
class FixedCluster final: public IAppliedClusterView
{
  public:
    /// @param state The state applied, or nothing.
    explicit FixedCluster(std::optional<Cluster::ClusterState> state):
        _state { std::move(state) }
    {
    }

    /// @copydoc IAppliedClusterView::AppliedState
    [[nodiscard]] std::optional<Cluster::ClusterState> AppliedState() const override
    {
        return _state;
    }

  private:
    std::optional<Cluster::ClusterState> _state;
};

} // namespace Pace::Node::McpFakes
