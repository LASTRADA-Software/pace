// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollAutoApprove.hpp"
#include "McpUpstream.hpp"
#include "NodeNameResolver.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Core/WireFields.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Pace::Node
{

/// @file McpActionTools.hpp
/// The MCP tools that ACT: decide a pending joiner, arm or end the leader's auto-approve deadline, cordon or
/// uncordon this node's worker -- and `explain_admission`, the read that says why those may be refused.
///
/// **Only fixed frames.** Every tool builds its one request from arguments it has validated, through the wire's
/// own encoders (`EncodeEnrollApprove`, `EncodeEnrollControl`, `EncodeEnrollAutoApprove`, `EncodeCordonRequest`,
/// `EncodeExplainAdmissionRequest`), and no caller-supplied byte is passed through: the self-call path
/// (`FrameRouterAnswerer`) presents this node's proven identity and skips the surface's size ceilings, so a tool
/// that relayed a frame would hand the model every verb this node may send.
///
/// **No gate is loosened.** The node that answers enforces exactly what it enforces for `pace-node --enroll-*` and
/// `pacectl cordon`: an enrollment decision needs an operator's STANDING at the leader (`OperatorStanding`:
/// loopback there, or an identified machine holding a voter's seat), and a cordon is answered only from the machine
/// it cordons (`CompileResponder::RefusePeer`, `CordonNotLocal`). So a cordon tool names THIS node or nothing, and a
/// refusal of this node as the caller is reported by name with its remedy (`CallerRefusalSentence`).
///
/// **Every successful action is logged** at Info by this node, naming the tool, its validated arguments, the MCP
/// client's self-reported name and who answered -- after the answer, so the log records what happened rather than
/// what was asked. No argument here is secret; no frame is logged.

namespace McpAction
{
    /// The longest auto-approve window the tool arms, in whole minutes: `AutoApproveCeiling`, the bound the CLI and
    /// the leader judge a duration by, in the tool's unit. Derived, never restated.
    inline constexpr std::int64_t MaxAutoApproveMinutes =
        std::chrono::duration_cast<std::chrono::minutes>(AutoApproveCeiling).count();

    static_assert(std::chrono::minutes { MaxAutoApproveMinutes } == AutoApproveCeiling,
                  "the auto-approve ceiling must be whole minutes, or the tool's bound would round it");

    /// The longest machine id or key text `explain_admission` puts in its one field: what the verb's own payload
    /// ceiling leaves after the field's length prefix, so the tool never builds a frame the node refuses for size.
    inline constexpr std::size_t MaxExplainSubjectBytes =
        CompileCacheWire::MaxExplainAdmissionPayload - WireFields::FieldPrefixSize;

    static_assert(CompileCacheWire::MaxIdBytes <= MaxExplainSubjectBytes,
                  "every machine id must fit explain-admission's subject field");

    /// The most bytes of the client's self-reported name an audit line carries.
    inline constexpr std::size_t MaxAuditClientNameBytes = 128;
} // namespace McpAction

/// What a tool answers when the authority refused THIS node as the caller -- for want of an operator's standing
/// (`UpstreamFailure::StandingRequired`) or because it could not identify it (`UpstreamFailure::NotIdentified`).
///
/// Two sentences, one per failure, because the remedies differ: a node without standing is told to find a voter or
/// be promoted, while a node that was not identified is told to identify itself -- the wire's own instruction for
/// `IdentifiedCallerRequired`, whose caller must not be sent looking for a voter.
/// @param failure Why the upstream did not answer.
/// @param selfId This node's id: the node that was judged.
/// @return The sentence, with its remedy; nothing for a failure that is not about this node as the caller.
[[nodiscard]] std::optional<std::string> CallerRefusalSentence(UpstreamFailure failure, std::string const& selfId);

/// The action tools: `enrollment_approve`, `enrollment_reject`, `enrollment_auto_approve`, `cordon`, `uncordon`
/// and `explain_admission`.
/// @param upstream How the leader and this node are asked; must outlive every row returned.
/// @param cluster The state a `node` argument resolves in; must outlive every row returned.
/// @param selfId This node's id: the one `node` a cordon may name, and the node a standing refusal names.
/// @param logger Where each successful action is recorded; must outlive every row returned.
/// @return The rows, in the order `tools/list` presents them.
[[nodiscard]] std::vector<Mcp::ToolRow> ActionTools(IMcpUpstream& upstream,
                                                    IAppliedClusterView const& cluster,
                                                    std::string selfId,
                                                    ILogger& logger);

} // namespace Pace::Node
