// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

#include <core/net/KeepAlive.hpp>

namespace Pace
{

/// The TCP keepalive probe every held admission connection carries (Ruling 18), at BOTH ends: this
/// launcher's dial of its node, the node's relay of it to the leader (`Node::ForwardDialBudget`), and the
/// leader's own arming of the connection it holds (`IConnectionHold::KeepAlive`, #1736) -- one value, so
/// the three cannot drift apart.
///
/// **What it is for.** A held admission connection is the lease (R5): the charge it holds is given
/// back when it closes, and a connection to a host that VANISHED -- powered off, cable pulled,
/// suspended -- never closes on its own, because nothing ever arrives to say so. The pulse answers a
/// node that is alive and silent; this answers one that is gone, which no pulse can.
///
/// **Its values are core-cpp's, never this file's.** `core::net::DialOptions` takes keepalive as
/// `Yes` or `No` and arms `KeepAliveSettings {}` for `Yes`: a dial carries no intervals of its own.
/// They are pinned here rather than restated because the bound the rulebook gives for a vanished
/// peer -- `idle + count * interval`, 16 s on Linux and macOS, and 30 s on Windows, whose probe count
/// is fixed at 10 by the OS -- is derived from them, and a core-cpp bump that moved them would
/// otherwise move that bound in silence.
inline constexpr core::net::KeepAliveSettings AdmitKeepAliveProbe {};
static_assert(AdmitKeepAliveProbe.idle == std::chrono::seconds { 10 }
                  && AdmitKeepAliveProbe.interval == std::chrono::seconds { 2 } && AdmitKeepAliveProbe.count == 3,
              "core-cpp moved its keepalive intervals: re-derive the vanished-peer bound in "
              ".agent/rules/distributed-compilation.md, then update this pin");

} // namespace Pace
