// SPDX-License-Identifier: Apache-2.0
#include "FrameEndpoint.hpp"
#include "McpUpstream.hpp"
#include "NodeCredential.hpp"
#include "SchedulingRedirect.hpp"

#include <Pace/Protocol/LeaderRedirect.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>

#include <CacheProtocol.hpp>
#include <ExchangeIdleWindow.hpp>
#include <WorkerProtocol.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/SocketDeadline.hpp>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// One call, shared by the MCP thread that waits on it and the flow on the loop that answers it.
    ///
    /// Shared ownership because either may finish first: the flow outlives an MCP thread that gave up,
    /// and the MCP thread may read the answer after the flow's frame has been freed.
    struct CallState
    {
        std::mutex mutex;                     ///< Guards `result`.
        std::condition_variable answered;     ///< Told once `result` is set.
        std::optional<UpstreamResult> result; ///< The answer; set once, by the flow.

        /// Requested when the MCP thread gives up. **Touched on the loop thread only** -- the request is
        /// posted there -- so the callback it runs, which closes a socket, runs where that socket lives.
        core::async::StopSource cancel;
    };

    /// Where a call goes first.
    struct CallPlan
    {
        UpstreamTarget target { UpstreamTarget::Leader }; ///< Whom it asks; only `Leader` follows redirects.
        std::string endpoint;                             ///< The `0xFC` endpoint to ask first.
        bool local { false };                             ///< Whether that is this node, answered in-process.
    };

    /// What a flow reaches, as pointers: a coroutine's reference parameter dangles once its frame
    /// outlives the caller's expression.
    struct Collaborators
    {
        core::net::EventLoop* loop;          ///< Where the exchange's idle window is armed.
        IUpstreamDialer* dialer;             ///< How a remote node is reached.
        IUpstreamDirectory const* directory; ///< Who answers where, and the term.
        IInProcessAnswerer* local;           ///< How this node answers itself.
        std::chrono::milliseconds idle;      ///< How long a remote node may say nothing.
    };

    /// @param failure Why.
    /// @param detail Who, and what happened.
    /// @return The failure as `Ask` answers it.
    [[nodiscard]] UpstreamResult Failed(UpstreamFailure failure, std::string detail)
    {
        return std::unexpected { std::pair { failure, std::move(detail) } };
    }

    /// @param endpoint Where it was asked.
    /// @return The failure for a call cancelled because the MCP thread stopped waiting.
    [[nodiscard]] UpstreamResult Abandoned(std::string_view endpoint)
    {
        return Failed(UpstreamFailure::Timeout, std::format("the request to {} was abandoned at its deadline", endpoint));
    }

    /// Decode a whole reply frame the in-process surface wrote into the outcome a remote exchange gives.
    ///
    /// The same three answers `Cc::ExchangeFramed` reads off the wire, so the hop loop has one shape to
    /// judge whichever way the frame was answered; anything that does not decode is a lost answer.
    /// @param reply The frame.
    /// @return The outcome.
    [[nodiscard]] Cc::CacheOutcome OutcomeOf(std::span<std::byte const> reply)
    {
        auto outcome = Cc::CacheOutcome {};
        auto const header = Wire::DecodeReplyHeader(reply);
        if (!header.has_value() || reply.size() != Wire::ReplyHeaderSize + header->payloadLength)
            return outcome;
        auto const payload = reply.subspan(Wire::ReplyHeaderSize);
        switch (header->status)
        {
            case Wire::Status::Ok:
                outcome.kind = Cc::CacheOutcomeKind::Hit;
                outcome.value.assign(payload.begin(), payload.end());
                break;
            case Wire::Status::Miss:
                outcome.kind = Cc::CacheOutcomeKind::Miss;
                break;
            case Wire::Status::Error:
                if (auto const decoded = Wire::DecodeErrorPayload(payload); decoded.has_value())
                {
                    outcome.kind = Cc::CacheOutcomeKind::Rejected;
                    outcome.code = decoded->first;
                    outcome.message = std::string { decoded->second };
                }
                break;
            case Wire::Status::Progress:
            case Wire::Status::Push:
                break; // Never a terminal answer: a lost one.
        }
        if (outcome.kind != Cc::CacheOutcomeKind::Transport)
            outcome.transportFailure = Cc::TransportFailure::None;
        return outcome;
    }

    /// Dial @p endpoint and put @p frame to it.
    ///
    /// **The cancellation closes the connection**, which completes the read this is parked on: closing
    /// is right here and `CancelRead` would not be, since the connection is abandoned either way and a
    /// retraction could be missed by a read not yet armed (the admission relay's `ExchangeUpstream`, for
    /// the same reason). The dial is not cancelled: it runs to its own bounds (`ForwardDialBudget`) and
    /// the cancellation is read once it ends.
    /// @param call The call; its cancellation closes the connection.
    /// @param with The collaborators.
    /// @param endpoint Where.
    /// @param frame The request, by value.
    /// @return The outcome, or why there is none.
    core::async::Task<std::expected<Cc::CacheOutcome, std::pair<UpstreamFailure, std::string>>> AskRemote(
        std::shared_ptr<CallState> call, Collaborators with, std::string endpoint, std::vector<std::byte> frame)
    {
        auto opened = co_await with.dialer->Open(endpoint);
        if (opened.socket == nullptr)
        {
            auto const failure = OpeningFaultTable[static_cast<std::size_t>(opened.fault)].failure;
            co_return std::unexpected { std::pair {
                failure, std::format("{} at {}: {}", UpstreamFailureRowOf(failure).sentence, endpoint, opened.reason) } };
        }
        auto* const socket = opened.socket.get();
        auto notice = Cc::CredentialNotice::Silent();
        core::net::SocketDeadlineTarget silence { .socket = socket };
        Cc::ExchangeIdleWindow window { *with.loop, Cc::ExchangeBudget { .idle = with.idle }, &silence };
        window.MovedForward();
        // A call already cancelled runs this at construction, which closes the connection before anything
        // is sent: the exchange then fails at its first write, and the cancellation is read below.
        core::async::StopCallback const onCancel { call->cancel.get_token(), [socket] { socket->close(); } };
        auto outcome = co_await Cc::ExchangeFramed(socket, &notice, std::move(frame), NoCredential(), &window);
        if (call->cancel.stop_requested())
            co_return std::unexpected { Abandoned(endpoint).error() };
        if (outcome.kind == Cc::CacheOutcomeKind::Transport)
            co_return std::unexpected { std::pair { UpstreamFailure::Unreachable,
                                                    std::format("the node asked at {} {} before it answered",
                                                                endpoint,
                                                                Cc::DescribeTransportFailure(outcome.transportFailure)) } };
        co_return outcome;
    }

    /// Put the call's frame to its target, following `NotLeader` for a `Leader` call.
    /// @param call The call.
    /// @param with The collaborators.
    /// @param plan Where it goes first.
    /// @param frame The request.
    /// @return The answer.
    core::async::Task<UpstreamResult> Follow(std::shared_ptr<CallState> call,
                                             Collaborators with,
                                             CallPlan plan,
                                             std::vector<std::byte> frame)
    {
        auto endpoint = plan.endpoint;
        auto local = plan.local;
        // `MaxLeaderRedirects + 1` because the bound is inclusive and `iota` is half-open: three redirects
        // means four asks, as `AskTheLeader` counts them.
        for (auto const hop: std::views::iota(0, MaxLeaderRedirects + 1))
        {
            if (call->cancel.stop_requested())
                co_return Abandoned(endpoint);

            auto outcome = Cc::CacheOutcome {};
            if (local)
            {
                outcome = OutcomeOf(co_await with.local->Answer(frame));
                if (outcome.kind == Cc::CacheOutcomeKind::Transport)
                    co_return Failed(UpstreamFailure::Unreachable,
                                     "this node's own 0xFC surface answered the request with no reply it could read");
            }
            else
            {
                auto asked = co_await AskRemote(call, with, endpoint, frame);
                if (!asked.has_value())
                    co_return std::unexpected { std::move(asked.error()) };
                outcome = std::move(*asked);
            }

            if (outcome.kind == Cc::CacheOutcomeKind::Rejected && outcome.code == Wire::ErrorCode::NotLeader
                && plan.target == UpstreamTarget::Leader)
            {
                auto target = Cc::RedirectTarget(outcome);
                if (!target.has_value())
                    co_return Failed(UpstreamFailure::NoLeaderKnown,
                                     std::format("{}: {} answered that it does not lead and named nobody",
                                                 UpstreamFailureRowOf(UpstreamFailure::NoLeaderKnown).sentence,
                                                 endpoint));
                if (hop == MaxLeaderRedirects)
                    co_return Failed(UpstreamFailure::TooManyRedirects,
                                     std::format("gave up after {} leader redirect(s); the last, from {}, named {}",
                                                 MaxLeaderRedirects,
                                                 endpoint,
                                                 *target));
                // Followed only after a refusal, so nothing was applied where this landed.
                endpoint = std::move(*target);
                local = endpoint == with.directory->SelfEndpoint();
                continue;
            }

            if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
                co_return Failed(UpstreamFailure::Refused,
                                 std::format("{} refused the request: {}", endpoint, Cc::DescribeOutcome(outcome)));

            auto reply = UpstreamReply {
                .status = outcome.kind == Cc::CacheOutcomeKind::Hit ? Wire::Status::Ok : Wire::Status::Miss,
                .payload = std::string(outcome.value.size(), '\0'),
                .answeredBy = endpoint.empty() ? with.directory->SelfId() : endpoint,
                .term = with.directory->Term(),
            };
            std::ranges::transform(outcome.value, reply.payload.begin(), [](std::byte const byte) {
                return static_cast<char>(std::to_integer<unsigned char>(byte));
            });
            co_return reply;
        }
        // Unreachable: the last hop returns above whatever it was answered.
        co_return Failed(UpstreamFailure::TooManyRedirects,
                         std::format("gave up after {} leader redirect(s)", MaxLeaderRedirects));
    }

    /// The flow posted onto the loop: answer the call, then tell the MCP thread.
    ///
    /// A free coroutine taking its state by value, for the frame-lifetime reason every one in this tree
    /// is: a capturing lambda's closure would not outlive the expression that spawned it.
    /// @param call The call.
    /// @param with The collaborators.
    /// @param plan Where it goes first.
    /// @param frame The request.
    core::async::Task<void> RunCall(std::shared_ptr<CallState> call,
                                    Collaborators with,
                                    CallPlan plan,
                                    std::vector<std::byte> frame)
    {
        auto result = co_await Follow(call, with, std::move(plan), std::move(frame));
        {
            auto const guard = std::scoped_lock { call->mutex };
            call->result = std::move(result);
        }
        call->answered.notify_all();
    }
} // namespace

AppliedUpstreamDirectory::AppliedUpstreamDirectory(std::string selfId,
                                                   ISchedulingLeaderSource const& leader,
                                                   Cc::IAdvertisedEndpointSource const& self):
    _selfId { std::move(selfId) },
    _leader { leader },
    _self { self }
{
}

void AppliedUpstreamDirectory::Applied(Cluster::ClusterState const& state)
{
    auto endpoints = std::map<std::string, std::string, std::less<>> {};
    for (auto const& member: state.members)
        endpoints.emplace(member.id, member.schedulerEndpoint);
    auto const guard = std::scoped_lock { _lock };
    _endpoints = std::move(endpoints);
}

void AppliedUpstreamDirectory::ObservedRole(std::uint64_t term, bool selfLeads)
{
    auto const guard = std::scoped_lock { _lock };
    _term = term;
    _selfLeads = selfLeads;
}

std::string AppliedUpstreamDirectory::LeaderEndpoint() const
{
    {
        auto const guard = std::scoped_lock { _lock };
        if (!_selfLeads)
            return _leader.LeaderSchedulingEndpoint();
    }
    // This node leads: its own endpoint, which `NodeIoUpstream` recognises as itself and answers in-process.
    return _self.Current();
}

std::optional<std::string> AppliedUpstreamDirectory::EndpointOf(std::string_view nodeId) const
{
    auto const guard = std::scoped_lock { _lock };
    auto const found = _endpoints.find(nodeId);
    if (found == _endpoints.end())
        return std::nullopt;
    return found->second;
}

std::string AppliedUpstreamDirectory::SelfId() const
{
    return _selfId;
}

std::string AppliedUpstreamDirectory::SelfEndpoint() const
{
    return _self.Current();
}

std::optional<std::uint64_t> AppliedUpstreamDirectory::Term() const
{
    auto const guard = std::scoped_lock { _lock };
    return _term;
}

NodeIoUpstream::NodeIoUpstream(core::net::EventLoop& loop,
                               IUpstreamDialer& dialer,
                               IUpstreamDirectory const& directory,
                               IInProcessAnswerer& local,
                               UpstreamPolicy policy) noexcept:
    _loop { loop },
    _dialer { dialer },
    _directory { directory },
    _local { local },
    _policy { policy }
{
}

UpstreamResult NodeIoUpstream::Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame)
{
    // Resolved HERE, on the asking thread: a target nobody can name is answered without touching the loop.
    auto plan = CallPlan { .target = target, .endpoint = {}, .local = false };
    switch (target)
    {
        case UpstreamTarget::Leader:
            plan.endpoint = _directory.LeaderEndpoint();
            if (plan.endpoint.empty())
                return Failed(UpstreamFailure::NoLeaderKnown,
                              std::string { UpstreamFailureRowOf(UpstreamFailure::NoLeaderKnown).sentence });
            plan.local = plan.endpoint == _directory.SelfEndpoint();
            break;
        case UpstreamTarget::Node:
            if (nodeIdOrEmpty.empty() || nodeIdOrEmpty == _directory.SelfId())
            {
                plan.endpoint = _directory.SelfEndpoint();
                plan.local = true;
                break;
            }
            if (auto endpoint = _directory.EndpointOf(nodeIdOrEmpty); !endpoint.has_value())
                return Failed(UpstreamFailure::UnknownNode,
                              std::format("no member of this fleet has the node id \"{}\"", nodeIdOrEmpty));
            else if (endpoint->empty())
                return Failed(UpstreamFailure::Unreachable,
                              std::format("member \"{}\" has announced no 0xFC endpoint yet", nodeIdOrEmpty));
            else
                plan.endpoint = std::move(*endpoint);
            plan.local = plan.endpoint == _directory.SelfEndpoint();
            break;
        case UpstreamTarget::Last:
            return Failed(UpstreamFailure::UnknownNode, "no target was named");
    }

    auto call = std::make_shared<CallState>();
    auto bytes = std::vector<std::byte>(frame.size());
    std::ranges::transform(frame, bytes.begin(), [](char const c) { return static_cast<std::byte>(c); });
    auto const with = Collaborators {
        .loop = &_loop, .dialer = &_dialer, .directory = &_directory, .local = &_local, .idle = _policy.idle
    };
    auto const firstEndpoint = plan.endpoint;
    // `post` is safe from any thread; `spawn` is loop-thread only, so the spawn rides the post.
    _loop.post([call, with, plan = std::move(plan), bytes = std::move(bytes)]() mutable {
        with.loop->spawn(RunCall(call, with, std::move(plan), std::move(bytes)));
    });

    auto lock = std::unique_lock { call->mutex };
    std::ignore = call->answered.wait_for(lock, _policy.deadline, [&call] { return call->result.has_value(); });
    if (call->result.has_value())
        return std::move(*call->result);
    lock.unlock();

    // Not merely stopped listening: the flow is told, on its own loop, and closes what it is parked on.
    _loop.post([call] { call->cancel.request_stop(); });
    return Failed(UpstreamFailure::Timeout,
                  std::format("{}: no answer from {} within {} ms",
                              UpstreamFailureRowOf(UpstreamFailure::Timeout).sentence,
                              firstEndpoint.empty() ? _directory.SelfId() : firstEndpoint,
                              _policy.deadline.count()));
}

LoopGatedUpstream::LoopGatedUpstream(IMcpUpstream& inner, std::function<bool()> loopTurning) noexcept:
    _inner { inner },
    _loopTurning { std::move(loopTurning) }
{
}

UpstreamResult LoopGatedUpstream::Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame)
{
    if (!_loopTurning())
        return Failed(UpstreamFailure::LoopNotRunning,
                      std::string { UpstreamFailureRowOf(UpstreamFailure::LoopNotRunning).sentence });
    return _inner.Ask(target, nodeIdOrEmpty, std::move(frame));
}

FrameRouterAnswerer::FrameRouterAnswerer(std::optional<ProvenIdentity> self) noexcept:
    _self { std::move(self) }
{
}

void FrameRouterAnswerer::RouteThrough(IFrameResponder* router) noexcept
{
    _router = router;
}

core::async::Task<std::vector<std::byte>> FrameRouterAnswerer::Answer(std::vector<std::byte> frame)
{
    // No router: the node serves no `0xFC` surface. Empty is what the hop loop reads as no answer it could read.
    if (_router == nullptr)
        co_return std::vector<std::byte> {};
    // `frame` lives in this coroutine's frame across the await, which is the span's contract.
    auto reply = co_await _router->Answer(std::span<std::byte const> { frame },
                                          PeerIdentity { .host = "127.0.0.1", .proven = _self });
    co_return std::move(reply.bytes);
}

} // namespace Pace::Node
