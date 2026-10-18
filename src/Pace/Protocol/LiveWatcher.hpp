// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Protocol/ProvenIdentity.hpp>

namespace Pace
{

/// Who a live subscription belongs to, for as long as it streams.
///
/// **The same facts every membership gate folds** (`ConnectionFacts`), and the stream is re-gated on
/// them every tick ([#1428](https://github.com/LASTRADA-Software/pace/issues/1428), #178): a key
/// revoked while a dashboard watches ends that dashboard's stream on the next tick, as the forgotten
/// machine's. An alias rather than a copy of the struct, so a fact a connection establishes reaches
/// the per-tick re-gate by construction rather than by somebody remembering to copy it.
///
/// Shareable: `pace-kv` implements `ILiveGate` too and runs no node handshake, so its identity
/// facts are honestly disengaged there rather than plausible defaults it must invent. It OWNS its
/// host because `ILiveGate::Recheck` reads it on every tick for the life of a subscription (#366).
using LiveWatcher = ConnectionFacts;

} // namespace Pace
