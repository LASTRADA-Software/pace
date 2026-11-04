// SPDX-License-Identifier: Apache-2.0
#include <Pace/Transport/OpenConnections.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>

#include <core/net/NetError.hpp>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>
#endif

namespace Pace
{

/// One tracked connection still open.
struct TrackedConnectionRow
{
    core::platform::NativeHandle handle;
    /// Whether a cancel has shut it down already, so no later one counts it again.
    bool cancelled { false };
};

struct OpenConnections::State
{
    std::mutex mutex;
    /// Every tracked connection still open, by the serial `Track` gave it.
    std::map<std::uint64_t, TrackedConnectionRow> open;
    std::uint64_t nextSerial { 0 };
    /// Raised by the first `CancelOpenConnections`; a connection tracked afterwards is cancelled at once.
    bool latched { false };
    /// Every connection a cancel has shut down, at a `CancelOpenConnections` or at `Track` once latched.
    std::size_t cancelledTotal { 0 };
};

namespace
{
    /// End whatever the owner of @p handle is blocked in, and everything it does next, without closing it.
    /// Called with the registry's lock held, so the owner cannot be closing @p handle meanwhile.
    void CancelNative(core::platform::NativeHandle handle) noexcept
    {
#if defined(_WIN32)
        auto const socket = reinterpret_cast<SOCKET>(handle);
        std::ignore = ::shutdown(socket, SD_BOTH);
        // A shutdown does not promise to end a blocking `send` another thread is inside; cancelling the
        // handle's I/O does, and the call answers WSA_OPERATION_ABORTED.
        std::ignore = ::CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
#else
        std::ignore = ::shutdown(handle, SHUT_RDWR);
#endif
    }

    /// A tracked connection: the accepted socket, every operation forwarded, which leaves the registry
    /// before it closes.
    class TrackedSocket final: public core::net::ISocket
    {
      public:
        TrackedSocket(std::unique_ptr<core::net::ISocket> inner,
                      std::shared_ptr<OpenConnections::State> state,
                      std::uint64_t serial) noexcept:
            _inner { std::move(inner) },
            _state { std::move(state) },
            _serial { serial }
        {
        }

        TrackedSocket(TrackedSocket const&) = delete;
        TrackedSocket& operator=(TrackedSocket const&) = delete;
        TrackedSocket(TrackedSocket&&) = delete;
        TrackedSocket& operator=(TrackedSocket&&) = delete;

        /// Leaves the registry first, so the inner socket's destructor closes a descriptor no cancel can reach.
        ~TrackedSocket() override
        {
            Leave();
        }

        [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
        {
            return _inner->read(buffer);
        }

        [[nodiscard]] core::net::ResultAwaitable<core::net::ReadWithFd> readWithFd(std::span<std::byte> buffer) override
        {
            return _inner->readWithFd(buffer);
        }

        [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
        {
            return _inner->write(buffer);
        }

        [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                           std::shared_ptr<void const> keepAlive) override
        {
            return _inner->writeVectored(segments, std::move(keepAlive));
        }

        [[nodiscard]] core::net::ResultAwaitable<void> handshakeIfNeeded() override
        {
            return _inner->handshakeIfNeeded();
        }

        [[nodiscard]] core::net::IoAwaitable waitReadable() override
        {
            return _inner->waitReadable();
        }

        void cancelRead() noexcept override
        {
            _inner->cancelRead();
        }

        [[nodiscard]] core::net::ResultAwaitable<void> shutdownWrite() override
        {
            return _inner->shutdownWrite();
        }

        void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
        {
            _inner->setReceiveDeadline(deadline);
        }

        [[nodiscard]] std::expected<void, core::net::NetError> armKeepAlive(
            core::net::KeepAliveSettings const& settings) override
        {
            return _inner->armKeepAlive(settings);
        }

        [[nodiscard]] std::string peerAddress() const override
        {
            return _inner->peerAddress();
        }

        /// Leaves the registry, THEN closes: the order is what keeps a cancel off a reused descriptor.
        void close() noexcept override
        {
            Leave();
            _inner->close();
        }

        [[nodiscard]] bool isClosed() const noexcept override
        {
            return _inner->isClosed();
        }

      private:
        void Leave() noexcept
        {
            if (_left)
                return;
            _left = true;
            auto const guard = std::scoped_lock { _state->mutex };
            _state->open.erase(_serial);
        }

        std::unique_ptr<core::net::ISocket> _inner;
        std::shared_ptr<OpenConnections::State> _state;
        std::uint64_t _serial;
        /// Touched only by the owner's thread, as `close` and the destructor are.
        bool _left { false };
    };
} // namespace

OpenConnections::OpenConnections():
    _state { std::make_shared<State>() }
{
}

OpenConnections::~OpenConnections() = default;

std::unique_ptr<core::net::ISocket> OpenConnections::Track(std::unique_ptr<core::net::ISocket> socket,
                                                           core::platform::NativeHandle handle)
{
    auto const serial = [this] {
        auto const guard = std::scoped_lock { _state->mutex };
        return _state->nextSerial++;
    }();
    // Built BEFORE the handle is registered, outside the lock its destructor takes: an allocation that fails here
    // closes @p socket with nothing registered, and a row can never outlive the descriptor it names. Should the
    // registration below fail instead, `tracked` unwinds after the lock is released and erases a row that is absent.
    auto tracked = std::make_unique<TrackedSocket>(std::move(socket), _state, serial);
    auto const guard = std::scoped_lock { _state->mutex };
    auto& entry = _state->open.emplace(serial, TrackedConnectionRow { .handle = handle }).first->second;
    if (_state->latched)
    {
        CancelNative(handle);
        entry.cancelled = true;
        ++_state->cancelledTotal;
    }
    return tracked;
}

std::size_t OpenConnections::CancelOpenConnections() noexcept
{
    auto const guard = std::scoped_lock { _state->mutex };
    _state->latched = true;
    std::size_t cancelled = 0;
    // Each is counted once: a connection a cancel has shut down stays tracked until its owner closes it, and a
    // second cancel shuts down -- and names -- only what was opened since.
    for (auto& entry: _state->open | std::views::values)
    {
        if (entry.cancelled)
            continue;
        CancelNative(entry.handle);
        entry.cancelled = true;
        ++cancelled;
    }
    _state->cancelledTotal += cancelled;
    return cancelled;
}

std::size_t OpenConnections::CancelledTotal() const noexcept
{
    auto const guard = std::scoped_lock { _state->mutex };
    return _state->cancelledTotal;
}

std::size_t OpenConnections::OpenCount() const noexcept
{
    auto const guard = std::scoped_lock { _state->mutex };
    return _state->open.size();
}

} // namespace Pace
