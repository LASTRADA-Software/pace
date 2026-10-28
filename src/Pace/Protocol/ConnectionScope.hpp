// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
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
};

} // namespace Pace
