// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"

#include <Pace/Core/Json.hpp>
#include <Pace/Distributed/FleetView.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <cstddef>
#include <vector>

namespace Pace::Node
{

/// @file McpFleetTools.hpp
/// The fleet-scope MCP read tools: what the leader's fleet model and enrollment window say, put to the leader
/// over the proven `0xFC` connection and projected for a model to read.
///
/// **One derivation.** Every fleet tool reads `RenderFleetJson`'s document -- the bytes `/fleet.json` serves,
/// carried by FLEET-TEXT's `Json` format -- or FLEET-TEXT's `series` section, which is `/fleet/series.json`'s
/// values on their side; `enrollment_status` reads the reply `pace-node --enroll-list` renders. Nothing is
/// recomputed from a second source, so a tool cannot disagree with the dashboard about a quantity both show.
///
/// **The projection is mechanical**, so it adds no fact of its own: a fleet document key is spelled in
/// snake_case (`heartbeat-age` becomes `heartbeat_age`), and a column the fleet tables say is in
/// milliseconds (`CellFormat::Millis`) is converted to SECONDS and its name suffixed `_s` -- the unit in the
/// name, everywhere, as every tool's description states. A `null` cell stays `null`: absent is never `0` or
/// `""`.
///
/// **Only verbs the leader gates go to `UpstreamTarget::Leader`** -- FLEET-TEXT and ENROLL-CONTROL both refuse
/// a node that does not lead with `NotLeader`, which the upstream follows -- so an answer can never be a
/// non-leader's `Ok` presented as the leader's.
///
/// **`term` is THIS node's view**, never a field of the answer: no `0xFC` reply carries a term. Every result
/// says so in `term_scope` and in its text.

namespace McpFleet
{
    /// What a fleet document from the leader may cost to parse.
    ///
    /// Bounded by what the upstream already accepted (`McpUpstream::MaxReplyBytes`), so the parse refuses nothing
    /// the wire let through for size alone. The element ceiling covers the document's largest arrays: the
    /// compile and lease lists are cut at `FleetJsonCompileCeiling` and `FleetJsonLeaseCeiling`, and the
    /// conditions list is a row per (machine, condition) -- generous at a few thousand machines. The depth is
    /// the document's own (an object of arrays of objects, `admission` one level deeper) with room to spare.
    inline constexpr Json::ParseLimits FleetDocumentLimits {
        .maxBytes = McpUpstream::MaxReplyBytes,
        .maxDepth = 16,
        .maxStringBytes = std::size_t { 64 } * 1024U,
        .maxContainerElements = std::size_t { 64 } * 1024U,
    };

    static_assert(FleetDocumentLimits.maxContainerElements > Distributed::FleetJsonCompileCeiling
                      && FleetDocumentLimits.maxContainerElements > Distributed::FleetJsonLeaseCeiling,
                  "a fleet document's longest lists must parse whole");
} // namespace McpFleet

/// The fleet-scope read tools: `fleet_overview`, `running_compiles`, `conditions`, `history` and
/// `enrollment_status`.
///
/// Each asks the leader through @p upstream on the MCP thread that calls it, and answers a tool EXECUTION error
/// (`isError`) carrying the upstream's sentence when the leader could not be asked, or refused.
/// @param upstream How the leader is reached; must outlive every row returned.
/// @return The rows, in the order `tools/list` presents them.
[[nodiscard]] std::vector<Mcp::ToolRow> FleetReadTools(IMcpUpstream& upstream);

} // namespace Pace::Node
