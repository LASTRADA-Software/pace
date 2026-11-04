// SPDX-License-Identifier: Apache-2.0
#include <Pace/Transport/OpenConnections.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
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

struct OpenConnections::State
{
    std::mutex mutex;
    /// Every tracked connection still open, by the serial `Track` gave it.
    std::map<std::uint64_t, core::platform::NativeHandle> open;
    std::uint64_t nextSerial { 0 };
    /// Raised by the first `CancelOpenConnections`; a connection tracked afterwards is cancelled at once.
    bool cancelled { false };
    /// Every serial below this has been cancelled already, so a later cancel does not count it twice.
    std::uint64_t cancelledBelow { 0 };
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
    auto const guard = std::scoped_lock { _state->mutex };
    auto const serial = _state->nextSerial++;
    _state->open.emplace(serial, handle);
    if (_state->cancelled)
    {
        CancelNative(handle);
        _state->cancelledBelow = _state->nextSerial;
    }
    return std::make_unique<TrackedSocket>(std::move(socket), _state, serial);
}

std::size_t OpenConnections::CancelOpenConnections() noexcept
{
    auto const guard = std::scoped_lock { _state->mutex };
    _state->cancelled = true;
    std::size_t cancelled = 0;
    // Each is counted once: a connection a cancel has shut down stays tracked until its owner closes it, and a
    // second cancel names only what was opened since the first.
    for (auto const& [serial, handle]: _state->open)
    {
        if (serial < _state->cancelledBelow)
            continue;
        CancelNative(handle);
        ++cancelled;
    }
    _state->cancelledBelow = _state->nextSerial;
    return cancelled;
}

std::size_t OpenConnections::OpenCount() const noexcept
{
    auto const guard = std::scoped_lock { _state->mutex };
    return _state->open.size();
}

} // namespace Pace
