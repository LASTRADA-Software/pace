// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <core/async/StopToken.hpp>

namespace Pace
{

/// An obligation a reply leaves on its connection: something that must be given back when the
/// connection ends, or when the peer sends something other than what the obligation waits for.
///
/// The admission queue is the one holder (#1728): an `ADMIT` answered with a grant keeps the
/// connection open, the grant's charge lives exactly as long as the connection, and the only frame
/// that may follow is the `SETTLE` that gives it back. A close, or any other frame, is an abandon --
/// which is why this is an object that releases in its DESTRUCTOR: every way out of a connection,
/// including a reactor torn down with the connection's frame still parked, runs it.
class IConnectionHold
{
  public:
    IConnectionHold() = default;
    IConnectionHold(IConnectionHold const&) = delete;
    IConnectionHold& operator=(IConnectionHold const&) = delete;
    IConnectionHold(IConnectionHold&&) = delete;
    IConnectionHold& operator=(IConnectionHold&&) = delete;
    /// Releases what is held. Runs on the connection's own reactor thread, or while that reactor is stopped.
    virtual ~IConnectionHold() = default;

    /// @param opRaw The verb of the next frame on the connection, as received.
    /// @return True when the hold goes on across a frame with that verb; false when that frame ends it.
    [[nodiscard]] virtual bool SurvivesFrame(std::uint8_t opRaw) const noexcept = 0;

    /// How long the connection may sit idle before its next frame while this is held.
    ///
    /// A property of what is held, never of the endpoint: a granted compile waits for its settle for
    /// as long as a compiler runs, while a request put back in the queue is owed its re-`ADMIT` at
    /// once and must not keep its place at the head for an hour behind a client that went quiet.
    /// @return The sweeper's window for the gap before the next request.
    [[nodiscard]] virtual std::chrono::milliseconds IdleWindow() const noexcept = 0;
};

/// What one connection owns beyond its bytes: whether its peer has gone, and a hold a reply left on it.
///
/// One per connection, created by the serve loop and shared with the responders through
/// `ConnectionFacts::scope`; a transport that has no connection (a unit test over a bare responder)
/// leaves that pointer null and a responder that needs one refuses to park.
///
/// **Two threads touch it, in different ways.** The stop source is thread-safe and may be requested
/// from anywhere; the hold is the connection's own and is touched only by the reactor thread serving
/// it (and by the destructor once that has stopped), so it carries no lock.
class ConnectionScope
{
  public:
    ConnectionScope() = default;
    ConnectionScope(ConnectionScope const&) = delete;
    ConnectionScope& operator=(ConnectionScope const&) = delete;
    ConnectionScope(ConnectionScope&&) = delete;
    ConnectionScope& operator=(ConnectionScope&&) = delete;
    /// Gives back whatever the connection still holds.
    ~ConnectionScope()
    {
        End();
    }

    /// @return A token that is stopped once the endpoint's read watch saw this peer leave while a
    ///         request was being answered. A pipelined request is not a departure and does not stop it.
    [[nodiscard]] core::async::StopToken PeerGone() const
    {
        return _peerGone.get_token();
    }

    /// Record that the peer left. Called by the endpoint's watch; idempotent.
    void NotePeerGone() noexcept
    {
        _peerGone.request_stop();
    }

    /// Record that the endpoint has already filed this peer's departure under the answered verb's
    /// own counter (`IFrameResponder::PeerWatchCounter`): a reply was ready and nobody was left to
    /// take it. A hold released by that same departure reads this and does not count it a second time.
    void NoteDepartureCounted() noexcept
    {
        _departureCounted = true;
    }

    /// @return True once the endpoint has counted this connection's departure itself.
    [[nodiscard]] bool DepartureCounted() const noexcept
    {
        return _departureCounted;
    }

    /// Ask the endpoint to judge the request being answered by its progress pulse rather than by a
    /// total ceiling: every pulse the kernel ACCEPTS moves the request's sweep deadline on by its
    /// whole answer window. Reset at every frame (`BeforeFrame`), so it covers one request only.
    ///
    /// The fleet compile queue's wait (#1728): a queue wait has no total ceiling. A peer whose receive
    /// window fills stops the pulse writes completing and is swept one window after the last; a host
    /// that vanished keeps them completing into the send buffer until TCP retransmission fails the
    /// socket, so that case is bounded by the kernel, not by this window (`RenewAfterPulse`).
    void JudgeByPulse() noexcept
    {
        _judgedByPulse = true;
    }

    /// @return True while the request being answered is judged by its pulse (`JudgeByPulse`).
    [[nodiscard]] bool JudgedByPulse() const noexcept
    {
        return _judgedByPulse;
    }

    /// Keep @p hold until the connection ends or a frame it does not survive arrives, releasing any
    /// earlier one first.
    /// @param hold What to hold; null is the same as `Release`.
    void Hold(std::unique_ptr<IConnectionHold> hold) noexcept
    {
        _hold = std::move(hold);
    }

    /// @return True while a hold is kept.
    [[nodiscard]] bool Holds() const noexcept
    {
        return _hold != nullptr;
    }

    /// @return The kept hold, still owned here, or null when nothing is held: for a router that must
    ///         send the next frame to whichever responder left it, without taking it.
    [[nodiscard]] IConnectionHold const* Held() const noexcept
    {
        return _hold.get();
    }

    /// @return The idle window the kept hold asks for, or nothing when nothing is held.
    [[nodiscard]] std::optional<std::chrono::milliseconds> HeldIdleWindow() const noexcept
    {
        if (_hold == nullptr)
            return std::nullopt;
        return _hold->IdleWindow();
    }

    /// Hand the hold back to the caller, who owns releasing it.
    /// @return The hold, or null when there is none.
    [[nodiscard]] std::unique_ptr<IConnectionHold> TakeHold() noexcept
    {
        return std::exchange(_hold, nullptr);
    }

    /// Called by the endpoint when a frame arrives, before it is answered: a hold the frame does not
    /// survive is released here.
    /// @param opRaw The frame's verb, as received.
    void BeforeFrame(std::uint8_t opRaw) noexcept
    {
        _judgedByPulse = false;
        if (_hold != nullptr && !_hold->SurvivesFrame(opRaw))
            _hold.reset();
    }

    /// The connection ended: release the hold.
    void End() noexcept
    {
        _hold.reset();
    }

  private:
    core::async::StopSource _peerGone;
    std::unique_ptr<IConnectionHold> _hold;
    bool _departureCounted { false }; ///< Reactor thread only, like the hold.
    bool _judgedByPulse { false };    ///< Reactor thread only: set by the responder, read by the pulse.
};

} // namespace Pace
