// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory>

#include <core/net/ISocket.hpp>
#include <core/platform/Types.hpp>

namespace Pace
{

/// Ends, from any thread, every connection a listener handed out that is still open.
///
/// **A blocking write is bounded per CALL, never per answer.** Every socket a blocking listener
/// accepts carries a send timeout, but a peer that reads just often enough to keep each call moving
/// holds the write -- and the thread it blocks -- for as long as it likes, and a stop that joins that
/// thread waits with it. Nothing on the serving thread can end the write it is inside, so the stop
/// ends it from OUTSIDE: it shuts the connection down under the write, which then fails at once, and
/// the thread runs to its end and is joined.
///
/// A seam rather than a concrete type so a server whose connections are not native sockets (an
/// in-memory transport in a test) states that it has nothing to cancel instead of pretending to.
class IConnectionCanceller
{
  public:
    IConnectionCanceller() = default;
    IConnectionCanceller(IConnectionCanceller const&) = delete;
    IConnectionCanceller(IConnectionCanceller&&) = delete;
    IConnectionCanceller& operator=(IConnectionCanceller const&) = delete;
    IConnectionCanceller& operator=(IConnectionCanceller&&) = delete;
    virtual ~IConnectionCanceller() = default;

    /// Shut down every connection still open, and every one handed out from now on as it is handed out.
    ///
    /// **It latches**: a stop that cancels has decided this listener's connections are over, and one
    /// accepted after the call -- by an accept that was already in flight -- would otherwise be served to
    /// a client the stop has stopped waiting for. Idempotent; a later call counts only what was opened
    /// since.
    /// @return How many connections were open, and are now cancelled.
    [[nodiscard]] virtual std::size_t CancelOpenConnections() noexcept = 0;
};

/// The connections a blocking listener has handed out and the owner has not yet closed, so that a
/// stop can cancel them (`IConnectionCanceller`).
///
/// **The registry, never the caller, decides whether the handle is still the connection's.** A
/// socket is closed by whoever owns it, on its own thread, and a closed descriptor's number is reused
/// by the next one the process opens -- so shutting down a handle the owner had already closed could
/// end an unrelated connection. Each tracked socket therefore leaves the registry under the
/// registry's lock BEFORE its descriptor is closed, and a cancel shuts a handle down only under that
/// same lock: while the cancel runs no tracked descriptor can close, and once a socket has left no
/// cancel can reach it.
///
/// **What a cancel does is a shutdown, never a close.** A shutdown makes the owner's write in flight
/// fail and every later read answer end-of-stream, and leaves the descriptor to its owner, who still
/// closes it exactly once. On Windows a blocking call another thread is inside is also cancelled at the
/// handle (`CancelIoEx`), since a shutdown alone does not promise to end one there.
class OpenConnections final: public IConnectionCanceller
{
  public:
    OpenConnections();
    ~OpenConnections() override;
    OpenConnections(OpenConnections const&) = delete;
    OpenConnections& operator=(OpenConnections const&) = delete;
    OpenConnections(OpenConnections&&) = delete;
    OpenConnections& operator=(OpenConnections&&) = delete;

    /// Track @p socket until it is closed or destroyed.
    ///
    /// On a registry that has already cancelled, the connection is cancelled at once and handed out
    /// regardless, so its owner's first operation fails rather than being served.
    /// @param socket The accepted connection (owned).
    /// @param handle Its native descriptor; must stay @p socket's until @p socket closes it.
    /// @return The socket to serve: @p socket, decorated so that closing it leaves the registry first.
    [[nodiscard]] std::unique_ptr<core::net::ISocket> Track(std::unique_ptr<core::net::ISocket> socket,
                                                            core::platform::NativeHandle handle);

    /// @copydoc IConnectionCanceller::CancelOpenConnections
    [[nodiscard]] std::size_t CancelOpenConnections() noexcept override;

    /// @return How many tracked connections are still open.
    [[nodiscard]] std::size_t OpenCount() const noexcept;

    /// The registry's state, shared with every socket it tracks, so a socket that outlives the registry
    /// still has a lock to leave under.
    struct State;

  private:
    std::shared_ptr<State> _state;
};

} // namespace Pace
