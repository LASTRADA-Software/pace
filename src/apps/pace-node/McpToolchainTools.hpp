// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"

#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <vector>

namespace Pace::Node
{

/// @file McpToolchainTools.hpp
/// The `toolchains` MCP tool: which toolchains the fleet serves, on which machines, and the conservative upgrade
/// report (`Distributed::AdviseUpgrades`) built on them.
///
/// **One derivation.** It reads the `workers` section of the leader's fleet document -- the rows `/fleet.json`
/// serves, fetched through `McpFleet::AskFleetDocument` exactly as the fleet tools fetch it -- so a toolchain, a
/// label and a machine are the dashboard's, never a second count.
///
/// **A node is a machine.** The registry keys an entry on (fingerprint, endpoint), so a machine serving two
/// toolchains is two rows of `workers`; every count and every "nodes behind" here is over distinct endpoints.
///
/// **Two quantities are ABSENT, and say so.** The fleet document lists the toolchains each machine SERVES and
/// nothing a machine identified but does not serve, so a machine's `unserved` is `null`, never an empty list
/// that would claim it serves everything it found. No demand per toolchain is recorded yet, so `demand` is `null`
/// and the report recommends on versions alone, with a basis that says "no demand data".

/// The toolchain tools: `toolchains`.
/// @param upstream How the leader is reached; must outlive every row returned.
/// @return The rows, in the order `tools/list` presents them.
[[nodiscard]] std::vector<Mcp::ToolRow> ToolchainTools(IMcpUpstream& upstream);

} // namespace Pace::Node
