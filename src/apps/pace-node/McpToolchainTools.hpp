// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "McpUpstream.hpp"
#include "NodeNameResolver.hpp"

#include <Pace/Protocol/Mcp/McpTool.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <core/platform/Clock.hpp>

namespace Pace::Node
{

/// @file McpToolchainTools.hpp
/// The `toolchains` and `toolchain_diff` MCP tools. `toolchains`: which toolchains the fleet serves, on which machines, and
/// the conservative upgrade report (`Distributed::AdviseUpgrades`) built on them.
///
/// **One derivation.** It reads the `workers` section of the leader's fleet document -- the rows `/fleet.json`
/// serves, fetched through `McpFleet::AskFleetDocument` exactly as the fleet tools fetch it -- so a toolchain, a
/// label and a machine are the dashboard's, never a second count.
///
/// **A node is a machine.** The registry keys an entry on (fingerprint, endpoint), so a machine serving two
/// toolchains is two rows of `workers`; every count and every "nodes behind" here is over distinct endpoints.
///
/// **Two quantities can be ABSENT, and say so.** The fleet document lists the toolchains each machine SERVES and
/// nothing a machine identified but does not serve, so a machine's `unserved` is `null`, never an empty list
/// that would claim it serves everything it found. And `demand` -- the leader's toolchain-demand over seven days,
/// which the upgrade report weighs -- is `null` when the answering node keeps none or counted for no minute of the
/// window: the report then recommends on versions alone and every basis SAYS that demand was unavailable, never
/// reading absence as zero demand. Present, it is cited with its coverage (observed minutes of the window, since when)
/// and with whatever was folded into `other`, which names no toolchain and so counts toward none.

/// `toolchain_diff`: why two toolchain fingerprints differ.
///
/// **Each node sends what its survey digested** (`FINGERPRINT-INPUTS`, one fixed frame per page that this tool builds
/// -- never caller bytes), and **this node recomputes each fingerprint from it before believing it**: a record that no
/// longer reproduces its fingerprint is refused, never diffed. Two surveys are never merged -- a record that changes
/// between pages is refused too. The comparison is `Distributed::DiffFingerprintInputs`, whose verdict says only what
/// the comparison shows and marks any inference.
namespace McpToolchainDiff
{
    /// The most files `toolchain_diff` fetches per side. A side holding more is compared on its schema, banner and
    /// grammar only, and the answer says so (`complete: false`) rather than comparing half a tree.
    inline constexpr std::size_t MaxDiffFiles = 200000;

    /// The most reply bytes `toolchain_diff` reads per side before calling it incomplete.
    ///
    /// Sized from what a legitimate record IS: this host's largest toolchain, 14162 files, encodes to about 1.3 MB
    /// (a relative path, a 32-character digest and two field prefixes, ~95 bytes a file), and the `MaxDiffFiles` a
    /// side may hold at that density is about 19 MB. 64 MiB is three times that -- room for long paths -- and bounds
    /// what one call holds to two sides of it, whatever a node answers.
    inline constexpr std::size_t MaxDiffBytesPerSide = std::size_t { 64 } * 1024U * 1024U;

    /// The most pages `toolchain_diff` asks per side before calling it incomplete.
    ///
    /// A legitimate record is one to five pages of `FingerprintInputsReplyCeiling` (the 19 MB above at 4 MiB a page);
    /// 32 is six times that. It is what stops a node answering a page per file -- a page cut to one huge path, with
    /// the next cursor one further on -- from holding the call for `MaxDiffFiles` round trips.
    inline constexpr std::size_t MaxDiffPagesPerSide = 32;

    /// How long after it began `toolchain_diff` still starts a forwarded question.
    ///
    /// The per-side bounds cap how MANY questions one call asks -- two resolves and `MaxDiffPagesPerSide` pages a
    /// side, 66 in all -- and each is bounded by `McpUpstream::CallDeadline`, but nothing else bounds their SUM:
    /// a peer answering every page just inside the deadline would hold the call, and its serving thread, for
    /// eleven minutes. A legitimate diff is one resolve and one to five pages a side, each a round trip on the
    /// fleet's network, so thirty seconds is generous. Checked before every question, so the call answers within
    /// `TotalBudget + McpUpstream::CallDeadline`; past it the call is a tool error saying the budget ran out, and
    /// nothing read so far is reported -- a diff of part of what was asked would read as the whole answer.
    inline constexpr std::chrono::milliseconds TotalBudget { std::chrono::seconds { 30 } };

    /// The longest a `toolchain_diff` call can take: the budget, then the one question started just inside it.
    inline constexpr std::chrono::milliseconds LongestCall = TotalBudget + McpUpstream::CallDeadline;

    /// The most paths each differing group lists; the group's `count` is always the whole number.
    inline constexpr std::size_t MaxListedDiffPaths = 500;

    /// The longest path a group lists whole; a longer one is cut and says how long it was.
    inline constexpr std::size_t MaxListedPathBytes = 512;
} // namespace McpToolchainDiff

/// The toolchain tools: `toolchains` and `toolchain_diff`.
/// @param upstream How the leader and the nodes are reached; must outlive every row returned.
/// @param cluster The state `toolchain_diff`'s node names resolve in; must outlive every row returned.
/// @param clock What `toolchain_diff`'s `TotalBudget` is measured on; must outlive every row returned.
/// @param selfId This node's id: the one name that resolves on a node that has applied no cluster state.
/// @param thisVersion This build's `pace-node` version: `toolchain_diff` recomputes HERE, so a side that fails on
///        another build may be this build's digest moving, which the refusal then names.
/// @return The rows, in the order `tools/list` presents them.
[[nodiscard]] std::vector<Mcp::ToolRow> ToolchainTools(IMcpUpstream& upstream,
                                                       IAppliedClusterView const& cluster,
                                                       core::platform::IClock const& clock,
                                                       std::string selfId,
                                                       std::string thisVersion);

} // namespace Pace::Node
