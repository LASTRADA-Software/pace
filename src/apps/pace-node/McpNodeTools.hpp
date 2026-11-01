// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"
#include "NodeNameResolver.hpp"

#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <string>
#include <vector>

namespace Pace::Node
{

/// @file McpNodeTools.hpp
/// The node-scope MCP read tools: what ONE node says about itself, asked of this node by default or of a member
/// named exactly, over the proven `0xFC` connection.
///
/// **One derivation.** `node_status` decodes the very `NodeStatus` reply `pacectl node` decodes
/// (`CompileCacheWire::DecodeNodeStatus`) and reads its detail through `NodeStatusView`, the functions `pacectl
/// node` renders from, so the two cannot disagree about a quantity both show.
///
/// **A name resolves exactly** (`ResolveNodeName`): an id, never a prefix of one. A name that resolves nowhere is
/// a tool error listing the members, never an answer about some other machine.
///
/// **Credentials are redacted at the node that holds them**, before its reply exists, so nothing here ever sees a
/// value to leak.

/// The node-scope read tools: `node_status`.
/// @param upstream How a node is asked; must outlive every row returned.
/// @param cluster The state names resolve in; must outlive every row returned.
/// @param selfId This node's id: the one name that resolves on a node that has applied no cluster state.
/// @return The rows, in the order `tools/list` presents them.
[[nodiscard]] std::vector<Mcp::ToolRow> NodeReadTools(IMcpUpstream& upstream,
                                                      IAppliedClusterView const& cluster,
                                                      std::string selfId);

} // namespace Pace::Node
