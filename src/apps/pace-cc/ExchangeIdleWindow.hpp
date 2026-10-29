// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"

#include <optional>

#include <core/net/EventLoop.hpp>
#include <core/net/SocketDeadline.hpp>

namespace Pace::Cc
{

/// The idle half of an exchange's budget: a deadline thrown away and armed again
/// every time the peer proves it is still working.
///
/// **A sliding bound, which is a different thing from the fixed one beside it.**
/// `budget.total` is armed once and answers *has this exchange taken too long*;
/// this answers *has anything happened lately*, and only the second can be short on
/// a dispatched compile (#245).
///
/// **Re-armed by destroying and rebuilding the timer**, because `core::net::DeadlineTimer`
/// captures its deadline in its own coroutine frame and is deliberately immovable:
/// destroying one disarms it and takes its wait straight back off the reactor
/// (`CancelPending`), and constructing the next arms the following window. Those are
/// two operations that type already gets exactly right, and a `Rearm` on it would
/// put a second writer on an instant whose whole safety argument is that a late fire
/// is harmless. The cost is one coroutine frame per pulse -- one per
/// `DefaultProgressInterval` per dispatched compile -- against the minutes it is
/// measuring.
///
/// **It cannot go through `core::net::armSocketDeadline`, and that is a language fact rather
/// than a choice**: that helper hands back a `std::optional<DeadlineTimer>`, which an
/// immovable element makes initialize-only -- there is no assignment, and no
/// `emplace` that can take one. So what is shared with it is the
/// `core::net::SocketDeadlineTarget` type and its ordering contract (record the firing BEFORE
/// the close, so a caller resumed by that close never sees a shut socket with no
/// reason attached), and the *non-positive means unbounded* rule is asked of
/// `ExchangeBudget::BoundsIdle()` -- the one place this project states it, and the
/// reason that predicate lives on the type rather than at its consumers.
///
/// Everything here runs on the reactor thread: `MovedForward` is called from the
/// exchange coroutine, which this reactor resumes. So the timer is built and torn
/// down on the thread that owns it, as `core::net::DeadlineTimer` requires.
class ExchangeIdleWindow final: public IExchangeLiveness
{
  public:
    /// @param reactor Where the deadline is armed; must outlive this.
    /// @param budget The exchange's budget; only its idle bound is read.
    /// @param target What to close on expiry and where the firing is recorded; must
    ///        outlive this.
    ExchangeIdleWindow(core::net::EventLoop& reactor,
                       ExchangeBudget budget,
                       core::net::SocketDeadlineTarget* target) noexcept:
        _reactor { &reactor },
        _budget { budget },
        _target { target }
    {
    }

    /// @copydoc IExchangeLiveness::MovedForward
    ///
    /// Also how the FIRST window is armed: an exchange that has just connected has
    /// moved forward by definition, and giving the arming one spelling means there is
    /// no start-up path that could arm it differently from the pulses.
    void MovedForward() noexcept override
    {
        if (!_budget.BoundsIdle())
            return; // Unbounded, and it stays unbounded on every pulse.

        // Destroyed before the next is built rather than leaving that to `emplace`:
        // `emplace` does destroy first, and saying so keeps "two timers are never
        // armed at once" from being a reading of the standard somebody has to do at
        // this call site.
        _timer.reset();
        _timer.emplace(*_reactor, _reactor->clock().now() + _budget.idle, &OnSilence, _target);
    }

  private:
    /// Close the socket and say that the silence bound is what did it.
    ///
    /// Recorded BEFORE the close, which is `core::net::SocketDeadlineTarget`'s own rule: the
    /// close resumes whoever is parked on that socket, so a flag set afterwards
    /// would be read by a caller that has already decided what happened.
    /// @param state The `core::net::SocketDeadlineTarget`, as a `void*`.
    static void OnSilence(void* state) noexcept
    {
        auto& fired = *static_cast<core::net::SocketDeadlineTarget*>(state);
        fired.expired = true;
        fired.socket->close();
    }

    core::net::EventLoop* _reactor;
    ExchangeBudget _budget;
    core::net::SocketDeadlineTarget* _target;
    std::optional<core::net::DeadlineTimer> _timer;
};

} // namespace Pace::Cc
