// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "HeldCharges.hpp"
#include "NodeConfig.hpp"
#include "NodeProofClient.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/AdmissionQueue.hpp>
#include <Pace/Distributed/NodePolicy.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>

namespace Pace::Node
{

/// The share of this machine's memory, after what the node holds for itself, that compiles may be
/// charged: 75%. The rest is the machine's own -- the person using it, the build's linker, the page
/// cache -- which a queue that spent all of it would trade for an out-of-memory kill.
inline constexpr std::uint64_t MachineBudgetSharePercent = 75;

/// What this machine tells the fleet's queue it may be charged, and what it holds (#1728, R6).
///
/// **A pure function, and stated for EVERY node** -- a node with `--slots=0` runs no worker and still
/// has a machine, so this reads the configuration and the capacity and never `WorkerSlotsOf`.
///
/// - Slots: `--machine-slots`, or the machine's logical cores (at least one: zero cores is "did not
///   say", `OfferableSlots`' reading).
/// - Budget: `--machine-memory-budget` when typed, and then exactly that. Otherwise 75% of physical
///   memory less what the node reserves for its own cache tier (`NodeCapacity::reservedMemoryBytes`,
///   which `NodeCapacityOf` derives from what the tier BUILT), floored at the default estimate so a
///   small machine can still hold one unknown compile rather than queue everything for ever.
/// - Held: passed through as given.
/// @param cfg The parsed configuration.
/// @param capacity What `NodeCapacityOf` made of this machine.
/// @param held What compiles on this machine hold now.
/// @return The report.
[[nodiscard]] Distributed::MachineReport MachineBudgetOf(NodeConfig const& cfg,
                                                         Distributed::NodeCapacity const& capacity,
                                                         HeldCharges const& held) noexcept;

/// One measured peak, as it is kept and sent.
using MeasuredPeak = std::pair<CompileCacheWire::EstimateKeyFields, std::uint64_t>;

/// The peaks this worker has measured and not yet reported, bounded.
///
/// Implements the protocol's `IJobMeasurementSink`, so the worker states what it measured and the
/// presence loop drains it on its next round. **Bounded at `MaxMachineBudgetPeaks`, oldest first
/// out**: a report carries a sample and the leader's estimates only want the freshest measurement of a
/// key, so a worker compiling faster than it reports loses the stale ones, never the process's memory.
/// One entry per key, the newest figure -- a recompile of one unit replaces its earlier peak.
///
/// Thread-safe: the worker's pool thread writes, the presence thread drains.
class MeasuredPeaks final: public Cc::IJobMeasurementSink
{
  public:
    /// Keep @p peakBytes for @p key, unless the key could not travel: a field over its wire cap is
    /// dropped rather than truncated, because a peak filed under half a key estimates nothing.
    /// @param key What the peak was measured for.
    /// @param peakBytes The measured peak.
    void OnPeak(CompileCacheWire::EstimateKeyFields const& key, std::uint64_t peakBytes) override;

    /// Take everything held, leaving it empty.
    /// @return The peaks, oldest first.
    [[nodiscard]] std::vector<MeasuredPeak> Take();

    /// Put back peaks a send did not deliver, behind any measured since: a key measured again in the
    /// meantime keeps its newer figure.
    /// @param undelivered What `Take` returned and no scheduler recorded.
    void Restore(std::vector<MeasuredPeak> undelivered);

  private:
    /// Drop the oldest until at most `MaxMachineBudgetPeaks` remain. `_mutex` held.
    void Trim();

    std::mutex _mutex;
    std::vector<MeasuredPeak> _peaks; ///< Oldest first; at most one per key.
};

/// The wire form of @p report with @p peaks beside it.
/// @param report The machine's budget and what it holds.
/// @param peaks The measured peaks to send.
/// @return The record `Op::MachineBudget` carries.
[[nodiscard]] CompileCacheWire::MachineBudgetReport MachineBudgetToWire(Distributed::MachineReport const& report,
                                                                        std::vector<MeasuredPeak> peaks);

/// What one machine-budget round borrows. All of it outlives the round.
struct MachineBudgetRound
{
    NodeConfig const& cfg;                     ///< The configuration the node runs.
    Distributed::NodeCapacity const& capacity; ///< What this machine is.
    IHeldCharges const& held;                  ///< What compiles on it hold: the sum of every holder.
    MeasuredPeaks& peaks;                      ///< Drained into the report; put back if nobody recorded it.
    ILogger& logger;                           ///< Where a refusal is named.
    NodeProofClient const* prover;             ///< How this machine proves itself; null where nothing proves.
    /// How loudly a scheduler that does not answer, or refuses, is said; the process's one.
    SchedulerReachability& reachability;
};

/// Report this machine's compile budget once: dial, prove, follow a redirect, and fall back.
///
/// **Sent on every presence round, whether or not this node runs a worker** (R6): the presence loop is
/// the one that runs everywhere. Through `DialAndAnnounce`, so the scheduler-dial rules -- which
/// endpoint, the proof, the redirect -- have the one answer they have everywhere else, and a failure is
/// said through `SchedulerReachability` on its TRANSITION (`SchedulerOutcome::BudgetRefused`), never once
/// per round.
///
/// A scheduler that cannot take the report -- an older build, or one that serves no queue yet -- refuses
/// it like any other refusal: logged, dampened, and the peaks go back to be offered again.
/// @param round What to say.
/// @param link Which scheduler to try, and what an answer teaches it.
/// @param dialer How a connection is made.
/// @return Whether a scheduler recorded the report.
[[nodiscard]] bool SendMachineBudgetOnce(MachineBudgetRound const& round, SchedulerLink& link, IEndpointDialer& dialer);

} // namespace Pace::Node
