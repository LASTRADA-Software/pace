// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "HeldCharges.hpp"

#include <Pace/Distributed/MembershipOracle.hpp>
#include <Pace/Distributed/SchedulerService.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

namespace Pace::Node
{

/// How long a parked `ADMIT` may go without a pulse reaching its peer before the endpoint sweeps it.
///
/// The compile surface's own pair, kept in proportion: `DefaultCompileIdleTimeout` is that many
/// `DefaultProgressInterval`s, so a cadence a test shortens shortens the bound with it, and the bound
/// always tolerates the same number of missed pulses the client does.
/// @param progressInterval The pulse cadence.
/// @return The window, renewed by every pulse.
[[nodiscard]] constexpr std::chrono::milliseconds AdmissionIdleBound(std::chrono::milliseconds progressInterval) noexcept
{
    return progressInterval * (CompileCacheWire::DefaultCompileIdleTimeout / CompileCacheWire::DefaultProgressInterval);
}

static_assert(AdmissionIdleBound(CompileCacheWire::DefaultProgressInterval) == CompileCacheWire::DefaultCompileIdleTimeout,
              "at the default cadence a parked admission is bounded exactly as a dispatched compile's silence is");

/// The state an `AdmissionResponder` shares with the frames it parks and the holds it leaves on
/// connections (defined in the implementation file).
struct AdmissionCore;

/// The leader's half of the fleet compile queue (#1728): parks `ADMIT` requests, grants them against
/// the machines' budgets and holds each grant for as long as its connection lives.
///
/// **The connection is the lease.** An `ADMIT` is answered when the queue admits it -- the endpoint
/// writes `Status::Progress` every `progressInterval` meanwhile, through the per-verb pulse it already
/// has -- and the connection then STAYS OPEN, holding the charge. The only frame that may follow is the
/// `SETTLE` that gives it back; a close, or any other frame, abandons it. The hold rides on the
/// connection's `ConnectionScope` and releases in a destructor, so every way out of a connection --
/// including a reactor stopped with the connection's frame still parked -- gives the charge back.
///
/// **A worker's refusal keeps the ticket** (R7, Ruling 9). `SETTLE{WorkerRefused}` puts the SAME ticket
/// back at the front of the queue with the refusal counted, is answered `Ok`, and leaves the connection
/// holding it; the client's next `ADMIT` on that connection waits on it again -- no new ticket, no new
/// place at the back -- and after `AdmissionQueue::MaxWorkerRefusals` it is granted on its caller. Any
/// other frame, or the close, abandons it.
///
/// **How a request parks.** `Answer` suspends in `core::net::interruptibleSleepUntil` on the node's
/// reactor: the park is the reactor's own, so a reactor that stops frees the frame, and the wake is a
/// stop token requested by whoever decides -- a pump, a demotion, or the endpoint's read watch seeing
/// the peer leave. Nothing here parks a coroutine handle of its own, which is the one thing a parked
/// frame must never be left holding.
///
/// Serves `Op::Admit`, `Op::Settle` and `Op::MachineBudget`; `SchedulerResponder` routes them here and
/// answers every other scheduler verb itself. Thread model: the answers run on the node's one
/// reactor thread; demotion arrives from the consensus driver's thread and only touches the waiters
/// under the responder's own lock.
class AdmissionResponder final
{
  public:
    /// @param service The scheduler that owns the queue; must outlive this.
    /// @param membership Folds a connection into the caller context the gate asks about; must outlive this.
    /// @param loop The reactor requests park on; must outlive this.
    /// @param metrics Where the admission counters are written; must outlive this.
    /// @param held Where a loopback-granted local lease is recorded for the machine's own report
    ///        (`HeldChargeRegistry`); null for a node that reports none.
    /// @param progressInterval How often a parked request is told the leader is still there.
    AdmissionResponder(Distributed::SchedulerService& service,
                       Distributed::IMembershipOracle const& membership,
                       core::net::EventLoop& loop,
                       IMetricsSink& metrics,
                       HeldChargeRegistry* held = nullptr,
                       std::chrono::milliseconds progressInterval = CompileCacheWire::DefaultProgressInterval);

    /// Detaches from the service: a hold or a parked frame destroyed after this finds nobody to tell.
    ~AdmissionResponder();

    AdmissionResponder(AdmissionResponder const&) = delete;
    AdmissionResponder& operator=(AdmissionResponder const&) = delete;
    AdmissionResponder(AdmissionResponder&&) = delete;
    AdmissionResponder& operator=(AdmissionResponder&&) = delete;

    /// @param opRaw A verb, as received.
    /// @return True when this responder answers it.
    [[nodiscard]] static bool Serves(std::uint8_t opRaw) noexcept;

    /// Answer one admission verb.
    /// @param frame The whole request, header included; must outlive the task.
    /// @param peer Who is at the other end, with the connection's scope.
    /// @return The reply; empty to close without one, which is a peer that left while parked.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer);

    /// @param opRaw A verb served here.
    /// @return The pulse cadence for `ADMIT`, which parks; none for the verbs answered at once.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t opRaw) const noexcept;

    /// @param opRaw A verb served here.
    /// @return The counter a reply abandoned to a departed peer is filed under, for `ADMIT` alone.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t opRaw) const noexcept;

    /// @param opRaw A verb served here.
    /// @return How long its answer may take: for `ADMIT` the silence a parked wait may keep between two
    ///         pulses that reach the peer (`AdmissionIdleBound`), since every such pulse renews it and the
    ///         wait itself has no ceiling; a round trip for the rest.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t opRaw) const noexcept;

  private:
    std::shared_ptr<AdmissionCore> _core;
};

} // namespace Pace::Node
