// SPDX-License-Identifier: Apache-2.0
#include "AdmissionResponder.hpp"
#include "AdmitForwarder.hpp"
#include "NodeCredential.hpp"

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/HostPort.hpp>
#include <Pace/Protocol/ConnectionScope.hpp>
#include <Pace/Protocol/SealedFrameSocket.hpp>
#include <Pace/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>

#include <CacheProtocol.hpp>
#include <EndpointDial.hpp>
#include <ExchangeIdleWindow.hpp>
#include <WorkerProtocol.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/SocketDeadline.hpp>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// The leader could not be relayed to: not reached, not proved, lost or silent before it answered, or
    /// moved more often than a request follows it. ONE row for all of them -- the client is told the same
    /// thing, and the operator's question is *how often does the queue fail to be reached from here*; the
    /// words that ride with it say which.
    ///
    /// `EndpointBusy`-class on purpose: `pace-cc` reads it as *this answer is not coming*, and compiles
    /// without the queue at once.
    constexpr Cc::SurfaceRefusal ForwardFailed {
        .code = Wire::ErrorCode::EndpointBusy,
        .counter = IMetricsSink::Counter::AdmissionForwardFailed,
    };

    /// No leader is known, so nothing was attempted: what the redirect says in an election.
    constexpr Cc::UncountedRefusal NoLeaderKnown {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "nothing was attempted, so there is no failure to count: this is the redirect's own answer "
                     "during an election, which every client reads as no leader and compiles locally",
    };

    /// The leader named is THIS node, which does not lead: it has just stopped, and nobody else is known yet.
    constexpr Cc::UncountedRefusal NamedItself {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "nothing was attempted, so there is no failure to count: a node that stopped leading a moment "
                     "ago is in the election the redirect answers NotLeader naming nobody, and relaying to itself "
                     "would only come back",
    };

    /// A frame this connection cannot be served: no connection to park on, nothing held to settle.
    constexpr Cc::UncountedRefusal CannotRelay {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "a client or wiring mistake, not a fleet condition: the leader refuses the same frames the "
                     "same way, uncounted",
    };

    /// A settle with no grant on the connection: the leader's own `UnknownLease` for the same frame.
    constexpr Cc::UncountedRefusal NothingToSettle {
        .code = Wire::ErrorCode::UnknownLease,
        .rationale = "a client mistake, answered as the leader answers it: nothing admitted is held on this "
                     "connection, and the leader counts nothing for it",
    };

    /// A frame this node cannot decode, refused as the leader refuses it.
    constexpr Cc::UncountedRefusal MalformedRequest {
        .code = Wire::ErrorCode::MalformedFrame,
        .rationale = "a client mistake, answered as the leader answers it, uncounted",
    };

    /// What the leader still holds for a client connection, and so what that connection may send next.
    ///
    /// The leader's own two holds (`AdmissionResponder.cpp`), restated as ROWS: a grant waits for its
    /// `SETTLE`, a ticket a worker refused waits for the `ADMIT` that asks again. This node holds the
    /// upstream connection exactly as long as the leader holds the thing, and lets a frame through
    /// exactly as the leader's hold would -- any other frame closes the upstream connection, which is
    /// what the leader's own `BeforeFrame` would have done to its hold.
    enum class HeldFor : std::uint8_t
    {
        Grant,   ///< A granted compile: its settle is next, and may be a compiler's length away.
        Requeue, ///< A ticket a worker refused: the client's `ADMIT` is owed at once.
        Last,    ///< Not a state: the length of a table keyed by one.
    };

    /// One state's row.
    struct HeldForRow
    {
        HeldFor held;                         ///< The state.
        Wire::Op survives;                    ///< The one verb the hold goes on across.
        std::chrono::milliseconds idleWindow; ///< How long the connection may sit idle before it.
    };

    /// What each hold lets through and waits for.
    inline constexpr EnumTable<HeldFor, HeldForRow> HeldForTable { {
        { .held = HeldFor::Grant, .survives = Wire::Op::Settle, .idleWindow = Wire::MaxCompileLeaseLifetime },
        { .held = HeldFor::Requeue, .survives = Wire::Op::Admit, .idleWindow = FrameServer::HeaderTimeout },
    } };
    static_assert(RowsInEnumeratorOrder(HeldForTable, &HeldForRow::held),
                  "HeldForTable must hold one row per HeldFor, in enumerator order");

    /// The connection to the leader that belongs to one client connection, kept on its scope while the
    /// leader holds something for it.
    ///
    /// Closing the socket IS the release: the leader's own hold on its end of this connection gives the
    /// charge back when the connection goes, so this destructor is the whole of "a client close closes
    /// upstream". Whoever destroys it does so on the reactor thread (the scope's own), or with that
    /// reactor stopped.
    class ForwardHold final: public IConnectionHold
    {
      public:
        /// @param socket The proven connection to the leader.
        /// @param endpoint Where it goes, for the next request on it and for the words of a refusal.
        ForwardHold(std::unique_ptr<core::net::ISocket> socket, std::string endpoint):
            _socket { std::move(socket) },
            _endpoint { std::move(endpoint) }
        {
        }

        ForwardHold(ForwardHold const&) = delete;
        ForwardHold& operator=(ForwardHold const&) = delete;
        ForwardHold(ForwardHold&&) = delete;
        ForwardHold& operator=(ForwardHold&&) = delete;

        /// Closes the connection to the leader, which is what releases whatever it holds there; the
        /// local charge goes with the member.
        ~ForwardHold() override
        {
            if (_socket != nullptr)
                _socket->close();
        }

        /// @copydoc IConnectionHold::SurvivesFrame
        [[nodiscard]] bool SurvivesFrame(std::uint8_t opRaw) const noexcept override
        {
            return opRaw == static_cast<std::uint8_t>(HeldForTable[static_cast<std::size_t>(_held)].survives);
        }

        /// @copydoc IConnectionHold::IdleWindow
        [[nodiscard]] std::chrono::milliseconds IdleWindow() const noexcept override
        {
            return HeldForTable[static_cast<std::size_t>(_held)].idleWindow;
        }

        /// @return The connection to the leader.
        [[nodiscard]] core::net::ISocket* Socket() const noexcept
        {
            return _socket.get();
        }

        /// @return The endpoint the connection goes to.
        [[nodiscard]] std::string const& Endpoint() const noexcept
        {
            return _endpoint;
        }

        /// Say what the leader holds from now on.
        /// @param held The state.
        void Holding(HeldFor held) noexcept
        {
            _held = held;
        }

        /// Record the machine-own charge of a local lease for as long as this lives, or until released.
        /// @param charge The registry's handle.
        void HoldCharge(HeldChargeRegistry::Handle charge) noexcept
        {
            _charge = std::move(charge);
        }

        /// The grant is over -- its settle is being relayed -- so the charge goes, whatever the leader says.
        void ReleaseCharge() noexcept
        {
            _charge = HeldChargeRegistry::Handle {};
        }

      private:
        std::unique_ptr<core::net::ISocket> _socket;
        std::string _endpoint;
        HeldChargeRegistry::Handle _charge;
        HeldFor _held { HeldFor::Grant };
    };

    /// Take the upstream connection off the client's scope, when that is what it holds.
    /// @param scope The client connection's scope.
    /// @return The hold, or null when the connection holds none -- which is every `ADMIT` but the one
    ///         that follows a refusal by a worker, since `BeforeFrame` released anything else.
    [[nodiscard]] std::unique_ptr<ForwardHold> TakeLink(ConnectionScope& scope)
    {
        auto taken = scope.TakeHold();
        auto* const link = dynamic_cast<ForwardHold*>(taken.get());
        if (link == nullptr)
            return nullptr;
        std::ignore = taken.release();
        return std::unique_ptr<ForwardHold> { link };
    }

    /// The leader's refusal, handed on.
    /// @param outcome A rejected outcome.
    /// @return The frame to write: the leader's code and words.
    [[nodiscard]] std::vector<std::byte> Relayed(Cc::CacheOutcome const& outcome)
    {
        return Cc::RefuseWithoutCounter(
            { .code = outcome.code,
              .rationale = "the leader counted this refusal itself, at the decision; this node relays it unchanged and a "
                           "second row would count one refusal once per hop it travelled" },
            outcome.message);
    }

    /// Send @p frame up and read the leader's final reply, bounded by its silence.
    ///
    /// **The client leaving closes the connection** (`PeerGone`), which completes the read this is parked
    /// on: closing is right here and `CancelRead` would not be, since the upstream connection is
    /// abandoned either way and a retraction could be missed by a read not yet armed. The silence bound
    /// is the leader's pulse against `idle`, re-armed by every pulse (`Cc::ExchangeIdleWindow`).
    ///
    /// A free coroutine taking raw pointers and values, for the frame-lifetime reason every one in this
    /// tree is.
    /// @param loop The reactor the silence bound is armed on.
    /// @param upstream The connection to the leader; outlives this.
    /// @param frame The request, by value.
    /// @param scope The client connection's scope; its peer-gone token closes @p upstream.
    /// @param idle How long the leader may say nothing.
    /// @return The outcome; a `Transport` outcome is a lost or silent leader OR a departed client.
    core::async::Task<Cc::CacheOutcome> ExchangeUpstream(core::net::EventLoop* loop,
                                                         core::net::ISocket* upstream,
                                                         std::vector<std::byte> frame,
                                                         std::shared_ptr<ConnectionScope> scope,
                                                         std::chrono::milliseconds idle)
    {
        auto notice = Cc::CredentialNotice::Silent();
        core::net::SocketDeadlineTarget silence { .socket = upstream };
        Cc::ExchangeIdleWindow window { *loop, Cc::ExchangeBudget { .idle = idle }, &silence };
        window.MovedForward();
        // A client already gone runs this at construction, which closes the connection before anything
        // is sent: the exchange then fails at its first write, and the caller reads the token.
        core::async::StopCallback const onGone { scope->PeerGone(), [upstream] { upstream->close(); } };
        co_return co_await Cc::ExchangeFramed(upstream, &notice, std::move(frame), NoCredential(), &window);
    }

    /// Words for the client when the leader's grant did not decode.
    /// @param endpoint The leader's endpoint.
    /// @return The detail of the refusal.
    [[nodiscard]] std::string UndecodableGrant(std::string_view endpoint)
    {
        return std::format("the scheduling leader at {} answered this node's relay with a grant it could not read",
                           endpoint);
    }

    /// Words for the client when a dial failed.
    /// @param endpoint Where it dialled.
    /// @param reason The dialer's.
    /// @return The detail of the refusal.
    [[nodiscard]] std::string DialFailure(std::string_view endpoint, std::string_view reason)
    {
        return std::format("this node could not reach the scheduling leader at {}: {}", endpoint, reason);
    }
} // namespace

ProvenSchedulerDialer::ProvenSchedulerDialer(NodeProofClient const& prover,
                                             core::net::IConnector& connector,
                                             core::net::EventLoop& reactor,
                                             ForwardDialBudget budget) noexcept:
    _prover { prover },
    _connector { connector },
    _reactor { reactor },
    _budget { budget }
{
}

core::async::Task<UpstreamOpening> ProvenSchedulerDialer::Open(std::string endpoint)
{
    auto raw = co_await Cc::DialEndpoint(
        &_connector,
        endpoint,
        core::net::DialOptions { .connectTimeout = _budget.connectTimeout, .keepAlive = _budget.keepAlive });
    if (raw == nullptr)
        co_return UpstreamOpening { .socket = nullptr, .reason = std::format("nothing answered at {}", endpoint) };

    // No budget: this end reads replies, and what it holds is bounded by the payload ceiling alone.
    auto sealed = std::make_unique<SealedFrameSocket>(std::move(raw), SealedFrameEnd::Caller, MaxForwardReplyBytes, nullptr);

    auto attempt = NodeProofAttempt {};
    {
        // Bounds the handshake alone; each exchange over the session arms its own.
        core::net::SocketDeadlineTarget deadline { .socket = sealed.get() };
        auto const bound = core::net::armSocketDeadline(&_reactor, _budget.handshakeTimeout, &deadline);
        attempt = co_await _prover.ProveAsync(sealed.get(), &_prover.DefaultTrust());
    }
    if (attempt.result != NodeProofResult::Proved)
        co_return UpstreamOpening { .socket = nullptr,
                                    .reason = attempt.reason.empty()
                                                  ? std::format("{} did not accept this node's proof", endpoint)
                                                  : std::move(attempt.reason) };
    co_return UpstreamOpening { .socket = std::move(sealed), .reason = {} };
}

AdmitForwarder::AdmitForwarder(ISchedulingLeaderSource const& leader,
                               IUpstreamDialer& dialer,
                               core::net::EventLoop& loop,
                               IMetricsSink& metrics,
                               HeldChargeRegistry* held,
                               Cc::IAdvertisedEndpointSource const* self,
                               ForwardPolicy policy) noexcept:
    _leader { leader },
    _dialer { dialer },
    _loop { loop },
    _metrics { metrics },
    _held { held },
    _self { self },
    _policy { policy }
{
}

bool AdmitForwarder::Serves(std::uint8_t opRaw) noexcept
{
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Admit) || opRaw == static_cast<std::uint8_t>(Wire::Op::Settle);
}

bool AdmitForwarder::ForwardsFor(PeerIdentity const& peer, std::uint8_t opRaw) noexcept
{
    return Serves(opRaw) && Pace::IsLoopbackHost(peer.host);
}

bool AdmitForwarder::Relays(ConnectionScope const& scope) noexcept
{
    return dynamic_cast<ForwardHold const*>(scope.Held()) != nullptr;
}

std::optional<IMetricsSink::Counter> AdmitForwarder::PeerWatchCounter(std::uint8_t opRaw) const noexcept
{
    if (opRaw != static_cast<std::uint8_t>(Wire::Op::Admit))
        return std::nullopt;
    return IMetricsSink::Counter::AdmissionForwardAbandoned;
}

std::optional<std::chrono::milliseconds> AdmitForwarder::ProgressInterval(std::uint8_t opRaw) const noexcept
{
    if (opRaw != static_cast<std::uint8_t>(Wire::Op::Admit))
        return std::nullopt;
    return _policy.progressInterval;
}

std::chrono::milliseconds AdmitForwarder::RequestTimeout(std::uint8_t opRaw) const noexcept
{
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Admit) ? AdmissionIdleBound(_policy.progressInterval)
                                                               : FrameServer::HeaderTimeout;
}

std::vector<std::byte> AdmitForwarder::RefusalReply(Wire::PrePayloadDecision decision,
                                                    std::uint8_t /*opRaw*/,
                                                    std::string_view detail) const
{
    return SchedulerSurfaceRefusalReply(decision, detail);
}

std::vector<std::byte> AdmitForwarder::EndpointRefusalReply(EndpointRefusal refusal,
                                                            std::uint8_t /*opRaw*/,
                                                            std::string_view detail) const
{
    return SchedulerSurfaceEndpointRefusalReply(refusal, detail);
}

core::async::Task<FrameReply> AdmitForwarder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value() || frame.size() != Wire::RequestHeaderSize + header->payloadLength || !Serves(header->opRaw))
        co_return FrameReply {};
    // A request relays on its connection: with none to hold the upstream on, it is refused rather than
    // relayed for a peer that can neither be pulsed nor noticed leaving.
    if (peer.scope == nullptr)
        co_return FrameReply { Cc::RefuseWithoutCounter(
            CannotRelay, "a relayed admission lives on its connection, and this caller has none") };
    if (header->opRaw == static_cast<std::uint8_t>(Wire::Op::Admit))
        co_return co_await RelayAdmit(frame, std::move(peer.scope));
    co_return co_await RelaySettle(frame, std::move(peer.scope));
}

core::async::Task<FrameReply> AdmitForwarder::RelayAdmit(std::span<std::byte const> frame,
                                                         std::shared_ptr<ConnectionScope> scope)
{
    // The liveness of this wait is its pulse, never its length: the endpoint's, towards the client, and
    // the leader's, towards this node (`ExchangeUpstream`).
    scope->JudgeByPulse();

    // The connection a worker's refusal left held, when this is the `ADMIT` that asks again (Ruling 9);
    // `BeforeFrame` released anything else, so a fresh connection or any other predecessor dials.
    auto link = TakeLink(*scope);
    auto endpoint = link != nullptr ? link->Endpoint() : _leader.LeaderSchedulingEndpoint();
    if (endpoint.empty())
        co_return FrameReply { Cc::RefuseWithoutCounter(NoLeaderKnown) };

    auto outcome = Cc::CacheOutcome {};
    auto failure = std::string {};
    for (auto const hop: std::views::iota(std::size_t { 0 }, MaxForwardHops + 1))
    {
        if (link == nullptr)
        {
            // Never to itself (see the class comment): read per dial, since a redirect can name it too.
            if (_self != nullptr && endpoint == _self->Current())
                co_return FrameReply { Cc::RefuseWithoutCounter(
                    NamedItself,
                    std::format("the scheduling leader named is this node, at {}, which does not lead", endpoint)) };
            // **Not cancelled when the client leaves**: the dial and the proof run to their own bounds
            // (`ForwardDialBudget`), and the departure is read once they end, below. A launcher that leaves
            // mid-dial costs one connection to the leader that is closed unused, never a held charge.
            auto opened = co_await _dialer.Open(endpoint);
            if (opened.socket == nullptr)
            {
                failure = DialFailure(endpoint, opened.reason);
                break;
            }
            link = std::make_unique<ForwardHold>(std::move(opened.socket), endpoint);
        }
        // Left while the dial ran: nothing was sent, and `link` goes out of scope closing what was opened.
        if (scope->PeerGone().stop_requested())
            co_return FrameReply {};

        outcome = co_await ExchangeUpstream(
            &_loop, link->Socket(), std::vector<std::byte> { frame.begin(), frame.end() }, scope, _policy.upstreamIdle);
        // Read AFTER the exchange, whichever way it ended: a client that left closed the connection it
        // was parked on, and that reads as a lost leader to anybody who does not ask this first.
        if (scope->PeerGone().stop_requested())
            co_return FrameReply {};

        if (outcome.kind != Cc::CacheOutcomeKind::Rejected || outcome.code != Wire::ErrorCode::NotLeader)
            break;
        auto const target = Cc::RedirectTarget(outcome);
        if (!target.has_value())
            break; // Names nobody: the client is told so, as the redirect tells it.
        // A demoted node answered, naming the leader that took over: followed once per hop, before any
        // grant, and what it queued is gone with the connection.
        link.reset();
        if (hop == MaxForwardHops)
        {
            failure = std::format(
                "the scheduling leader moved to {} after this request had followed it {} times", *target, MaxForwardHops);
            break;
        }
        endpoint = *target;
    }

    if (!failure.empty())
        co_return FrameReply { Cc::Refuse(_metrics, ForwardFailed, failure) };

    switch (outcome.kind)
    {
        case Cc::CacheOutcomeKind::Hit: {
            auto const grant = Wire::DecodeAdmitGrant(outcome.value);
            // A grant this node cannot read is not relayed: whether the leader now holds a charge for it,
            // and of which kind, is unknowable here, so the connection that would hold it closes (`link`
            // goes out of scope) -- which releases whatever the leader charged -- and the client fails open.
            if (!grant.has_value())
                co_return FrameReply { Cc::Refuse(_metrics, ForwardFailed, UndecodableGrant(endpoint)) };
            _metrics.Increment(IMetricsSink::Counter::AdmissionForwarded);
            // A `Stored` answer holds nothing on the leader, so nothing is kept: the connection to it ends
            // with this frame, and the client fetches what the other compile stored.
            if (grant->kind != Wire::GrantKind::Stored)
            {
                // A local lease occupies THIS machine's budget, as the leader charged it (Ruling 4).
                if (grant->kind == Wire::GrantKind::Local && _held != nullptr)
                    link->HoldCharge(_held->Hold(grant->chargedBytes));
                link->Holding(HeldFor::Grant);
                scope->Hold(std::move(link));
            }
            co_return FrameReply { Wire::EncodeReply(Wire::Status::Ok, outcome.value) };
        }
        case Cc::CacheOutcomeKind::Rejected:
            _metrics.Increment(IMetricsSink::Counter::AdmissionForwarded);
            co_return FrameReply { Relayed(outcome) };
        case Cc::CacheOutcomeKind::Miss:
        case Cc::CacheOutcomeKind::Transport:
            break;
    }
    co_return FrameReply { Cc::Refuse(
        _metrics,
        ForwardFailed,
        std::format("the scheduling leader at {} {} before it answered this node's relay",
                    endpoint,
                    outcome.transportFailure == Cc::TransportFailure::Silent ? "went silent" : "was lost")) };
}

core::async::Task<FrameReply> AdmitForwarder::RelaySettle(std::span<std::byte const> frame,
                                                          std::shared_ptr<ConnectionScope> scope)
{
    // Decoded BEFORE the hold is taken, as the leader does: a frame that does not decode is refused and
    // leaves the grant held, where it can still be settled.
    auto const request = Wire::DecodeSettlePayload(frame.subspan(Wire::RequestHeaderSize));
    if (!request.has_value())
        co_return FrameReply { Cc::RefuseWithoutCounter(MalformedRequest) };
    // Taken whatever it holds: a settle answers the one grant this connection owns, and the connection
    // to the leader must not outlive the frame that settled it unless a worker's refusal keeps it.
    auto link = TakeLink(*scope);
    if (link == nullptr)
        co_return FrameReply { Cc::RefuseWithoutCounter(NothingToSettle, "no admitted compile is held on this connection") };
    // Whatever the leader answers, the grant this node recorded is over.
    link->ReleaseCharge();

    auto const outcome = co_await ExchangeUpstream(&_loop,
                                                   link->Socket(),
                                                   std::vector<std::byte> { frame.begin(), frame.end() },
                                                   scope,
                                                   FrameServer::HeaderTimeout / 2);
    if (scope->PeerGone().stop_requested())
        co_return FrameReply {};

    switch (outcome.kind)
    {
        case Cc::CacheOutcomeKind::Hit:
            _metrics.Increment(IMetricsSink::Counter::AdmissionForwarded);
            // The leader put the same ticket back at the front and keeps this connection for the `ADMIT`
            // that waits on it again (Ruling 9); this node keeps its end for exactly that.
            if (request->outcome == Wire::SettleOutcome::WorkerRefused)
            {
                link->Holding(HeldFor::Requeue);
                scope->Hold(std::move(link));
            }
            co_return FrameReply { Wire::EncodeReply(Wire::Status::Ok, outcome.value) };
        case Cc::CacheOutcomeKind::Rejected:
            _metrics.Increment(IMetricsSink::Counter::AdmissionForwarded);
            co_return FrameReply { Relayed(outcome) };
        case Cc::CacheOutcomeKind::Miss:
        case Cc::CacheOutcomeKind::Transport:
            break;
    }
    // A grant whose leader is gone: the client's compile has run, there is nothing left to give back
    // that this node could, and the client connection ends with the upstream one.
    auto reply = FrameReply { Cc::Refuse(
        _metrics,
        ForwardFailed,
        std::format("the scheduling leader at {} {} before it answered this node's relay of "
                    "the settle",
                    link->Endpoint(),
                    outcome.transportFailure == Cc::TransportFailure::Silent ? "went silent" : "was lost")) };
    reply.endsConnection = true;
    co_return reply;
}

} // namespace Pace::Node
