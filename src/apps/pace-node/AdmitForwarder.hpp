// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "HeldCharges.hpp"
#include "NodeProofClient.hpp"
#include "SchedulingRedirect.hpp"

#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>

namespace Pace::Node
{

/// @file AdmitForwarder.hpp
/// The follower's half of the fleet compile queue (#1728): a node that does not lead relays the
/// admission exchange a launcher on ITS machine opens to the leader, instead of answering `NotLeader`.
///
/// Every `pace-cc` talks only to its own machine's node, and the queue lives on the leader alone, so on
/// a learner the launcher would otherwise be sent away from the one door it is configured to use. The
/// forwarder is that door: one upstream connection per client connection, for the client connection's
/// whole life, over which it relays every request frame in order and every final reply.

/// How many times a request follows a `NotLeader` that names another leader, before it is answered
/// refused: the leader moved while the request was on its way, and it moved again.
inline constexpr std::size_t MaxForwardHops = 2;

/// How long a relayed reply may be, in payload bytes: the scheduler surface's own request ceiling.
/// A grant carries one signed token and an endpoint, a few hundred bytes.
inline constexpr std::size_t MaxForwardReplyBytes = SchedulingRedirectResponder::RequestBytes;

/// What opening the connection to the leader came to.
struct UpstreamOpening
{
    /// The connection, proven and sealed in both directions as production needs it; null on failure.
    std::unique_ptr<core::net::ISocket> socket;
    /// Why not, for the log and the refusal's words; empty when `socket` is set.
    std::string reason;
};

/// Opens a connection to the leader's scheduling endpoint.
///
/// A seam at the dial, for the reason `IEndpointDialer` is one: what a request does when the leader
/// cannot be reached, or answers `NotLeader`, is a property of what the OTHER end does, so the other
/// end is what a test has to be able to place. Unlike that one it runs on the reactor and suspends --
/// a loop thread never blocks on a dial (`wire-and-protocol.md`).
class IUpstreamDialer
{
  public:
    IUpstreamDialer() = default;
    IUpstreamDialer(IUpstreamDialer const&) = delete;
    IUpstreamDialer& operator=(IUpstreamDialer const&) = delete;
    IUpstreamDialer(IUpstreamDialer&&) = delete;
    IUpstreamDialer& operator=(IUpstreamDialer&&) = delete;
    virtual ~IUpstreamDialer() = default;

    /// Dial @p endpoint and hand back a connection ready to send requests on.
    /// @param endpoint `host:port` of a scheduling surface; by value, for the coroutine frame.
    /// @return The connection, or why there is none. Never throws for an unreachable endpoint.
    [[nodiscard]] virtual core::async::Task<UpstreamOpening> Open(std::string endpoint) = 0;
};

/// How long a dial to the leader may take, in each of its two halves.
///
/// The whole of what a launcher waits before it is told the leader cannot be reached and compiles
/// without the queue, so it is short: the connect is the shared-cache leg's own, and the handshake
/// is bounded because an address that now reaches something which accepts and never answers would
/// otherwise hold the launcher for as long as the handshake takes to time out.
struct ForwardDialBudget
{
    std::chrono::milliseconds connectTimeout { 1'000 };   ///< The TCP connect, name lookup included.
    std::chrono::milliseconds handshakeTimeout { 5'000 }; ///< The proof, both exchanges.
};

/// The dialer production uses: connect from the loop, prove THIS node's identity key to the leader,
/// and seal everything after.
///
/// **The leader identifies the forwarded caller by the key proven here**, never by anything a frame
/// says (`SchedulerService::RefuseUnlessIdentified`), so an unproven connection would be refused its
/// admission and one proving another machine's key would charge that machine. The trust the leader is
/// judged by is the presence round's (`NodeProofClient::DefaultTrust`): a voter of the roster this node
/// holds.
class ProvenSchedulerDialer final: public IUpstreamDialer
{
  public:
    /// @param prover This node's proof; must outlive this.
    /// @param connector Opens reactor connections; must outlive this.
    /// @param reactor The loop every socket belongs to and the handshake's deadline is armed on; must
    ///        outlive this.
    /// @param budget How long each half of a dial may take.
    ProvenSchedulerDialer(NodeProofClient const& prover,
                          core::net::IConnector& connector,
                          core::net::EventLoop& reactor,
                          ForwardDialBudget budget = {}) noexcept;

    /// @copydoc IUpstreamDialer::Open
    [[nodiscard]] core::async::Task<UpstreamOpening> Open(std::string endpoint) override;

  private:
    NodeProofClient const& _prover;
    core::net::IConnector& _connector;
    core::net::EventLoop& _reactor;
    ForwardDialBudget _budget;
};

/// What a forwarder is built with beyond its collaborators.
struct ForwardPolicy
{
    /// How often a parked request is told this node is still there, and the unit of the silence
    /// bound on the upstream connection (`AdmissionIdleBound`).
    std::chrono::milliseconds progressInterval { CompileCacheWire::DefaultProgressInterval };
};

/// Relays `ADMIT` and `SETTLE` from a launcher on this machine to the leader (#1728).
///
/// **The two connections live and die together.** The first `ADMIT` on a client connection dials the
/// leader; the upstream connection is then the leader's half of that client's lease, and it is kept on
/// the CLIENT connection's `ConnectionScope` for as long as the leader holds something for it -- a grant
/// until its `SETTLE`, a ticket a worker refused until the `ADMIT` that asks again (Ruling 9). A client
/// that closes, or sends a frame the hold does not survive, closes the upstream connection with it,
/// which is how the leader releases the charge. Nothing is held on the leader's behalf that this node
/// cannot give back.
///
/// **What it relays, and what it does not.** Request frames go up unchanged and every FINAL reply comes
/// back unchanged, in order, over the ONE upstream connection -- a `SETTLE{WorkerRefused}` and the
/// re-`ADMIT` that follows it included. `Status::Progress` pulses are not forwarded frame by frame: the
/// endpoint already writes this node's own pulse to the client at the same cadence while `Answer` is
/// parked (`ProgressInterval`), a second writer there would interleave, and the pulse is empty, so the
/// client sees the same bytes. What the leader's pulses ARE used for is this node's own liveness
/// judgement: an upstream silent for `AdmissionIdleBound` is dropped and the client refused, so a dead
/// leader behind this node's healthy pulse is noticed rather than masked.
///
/// **Failure fails open, promptly.** A leader that cannot be dialled or proven, a connection lost or
/// silent before the reply, and a leader that moved more often than a request follows it are each
/// answered `EndpointBusy` -- one counted row, `AdmissionForwardFailed` -- so `pace-cc` compiles without
/// the queue instead of waiting. A `NotLeader` that names a new leader is followed transparently, at
/// most `MaxForwardHops` times, before any grant; the request loses its place in the queue, as the spec
/// accepts for a failover. After a grant an upstream loss ends the client connection.
///
/// **Only a caller on this machine is forwarded** (`ForwardsFor`): the leader charges a local lease to
/// the machine whose key the upstream connection proved, which is this node's, so a launcher elsewhere
/// is told where the leader is instead and asks it itself, identified by its own key.
///
/// Thread model: everything runs on the node's one reactor thread; the holds it leaves on connections
/// are destroyed there too, or with that reactor stopped.
class AdmitForwarder final: public IFrameResponder
{
  public:
    /// @copydoc SchedulingRedirectResponder::RequestBytes
    static constexpr std::size_t RequestBytes = SchedulingRedirectResponder::RequestBytes;

    /// @copydoc SchedulingRedirectResponder::OpenConnections
    static constexpr std::size_t OpenConnections = SchedulingRedirectResponder::OpenConnections;

    /// @param leader Where the leader answers, read afresh per request; must outlive this.
    /// @param dialer How the leader is reached; must outlive this.
    /// @param loop The reactor the node serves on; must outlive this.
    /// @param metrics Where the forwarding counters are written; must outlive this.
    /// @param held Where a forwarded local lease is recorded for the machine's own report
    ///        (`HeldChargeRegistry`); null for a node that reports none. Must outlive every connection.
    /// @param policy The cadence.
    AdmitForwarder(ISchedulingLeaderSource const& leader,
                   IUpstreamDialer& dialer,
                   core::net::EventLoop& loop,
                   IMetricsSink& metrics,
                   HeldChargeRegistry* held,
                   ForwardPolicy policy = {}) noexcept;

    /// @param opRaw A verb, as received.
    /// @return True for the verbs this relays: `ADMIT` and `SETTLE`.
    [[nodiscard]] static bool Serves(std::uint8_t opRaw) noexcept;

    /// @param peer Who asked.
    /// @param opRaw The verb, as received.
    /// @return True when this relays @p opRaw from @p peer: a served verb, from this machine.
    [[nodiscard]] static bool ForwardsFor(PeerIdentity const& peer, std::uint8_t opRaw) noexcept;

    /// Relay one `ADMIT` or `SETTLE`.
    /// @param frame The whole request, header included; must outlive the task.
    /// @param peer Who is at the other end, with the connection's scope.
    /// @return The leader's answer, or this node's own refusal; empty to close without one, which is a
    ///         client that left while its request was parked.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Nothing is refused at the header: membership is the redirect's gate, and a forwarded verb has no
    /// payload this node needs to avoid reading.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& /*peer*/,
                                                                   std::uint8_t /*opRaw*/) const override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
    }

    /// @copydoc IFrameResponder::RefusalReply
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// For `ADMIT` the silence a parked wait may keep between two pulses that reach the client
    /// (`AdmissionIdleBound`), since the wait itself has no ceiling; a header window for the rest.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t opRaw) const noexcept override;

    /// `RequestBytes`.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return RequestBytes;
    }

    /// `OpenConnections`.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return OpenConnections;
    }

    /// The connection cap times the request cap, as the scheduler surface's.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return OpenConnections * RequestBytes;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: the request is a few dozen bytes, and the endpoint's reservation suffices.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// `ADMIT` alone, which parks: its watch is what tells `Answer` the client left while the leader
    /// was still deciding. The departure is filed under `AdmissionForwardAbandoned`; the LEADER counts
    /// its own abandonment when the upstream connection closes.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t opRaw) const noexcept override;

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// `ADMIT` alone, at the configured cadence.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t opRaw) const noexcept override;

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream.**
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None**: an identity is the leader's question, asked on the upstream connection.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// Relay an `ADMIT`, dialling the leader when the connection has no upstream yet.
    /// @param frame The request; must outlive the task.
    /// @param scope The client connection's scope.
    /// @return The reply.
    [[nodiscard]] core::async::Task<FrameReply> RelayAdmit(std::span<std::byte const> frame,
                                                           std::shared_ptr<ConnectionScope> scope);

    /// Relay a `SETTLE` over the upstream connection the client connection holds.
    /// @param frame The request; must outlive the task.
    /// @param scope The client connection's scope.
    /// @return The reply.
    [[nodiscard]] core::async::Task<FrameReply> RelaySettle(std::span<std::byte const> frame,
                                                            std::shared_ptr<ConnectionScope> scope);

    ISchedulingLeaderSource const& _leader; ///< Where the leader answers, read per request.
    IUpstreamDialer& _dialer;               ///< How the leader is reached.
    core::net::EventLoop& _loop;            ///< Where the silence bound is armed.
    IMetricsSink& _metrics;                 ///< Where the counters go.
    HeldChargeRegistry* _held;              ///< Where a forwarded local lease is recorded; may be null.
    ForwardPolicy _policy;                  ///< The cadence.
};

} // namespace Pace::Node
