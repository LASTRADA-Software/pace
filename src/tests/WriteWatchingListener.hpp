// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/NetError.hpp>
#include <tests/BoundedWait.hpp>

namespace Pace::Testing
{

/// What the server's writes on a `WriteWatchingListener`'s connections have done so far, read from the case's thread.
class WriteWatch
{
  public:
    /// @return Every byte the kernel has taken off a write.
    [[nodiscard]] std::size_t Written() const noexcept
    {
        return _written.load();
    }

    /// @return Whether a write call is under way at this moment.
    [[nodiscard]] bool InWrite() const noexcept
    {
        return _inWrite.load();
    }

    /// Record that a write call is starting.
    void Entering() noexcept
    {
        _inWrite.store(true);
    }

    /// Record that a write call returned, having moved @p bytes.
    /// @param bytes What the kernel took; zero for a call that failed.
    void Left(std::size_t bytes) noexcept
    {
        _inWrite.store(false);
        _written.fetch_add(bytes);
    }

    /// Record that a write ended in @p error, now.
    /// @param error What the inner socket answered.
    void Failed(core::net::NetError const& error)
    {
        auto const guard = std::scoped_lock { _mutex };
        _error = error.toString();
        _failedAt = std::chrono::steady_clock::now();
    }

    /// What ended the connection's writes, in words a failure message can carry: the error and when it came,
    /// relative to @p since -- so a red on a platform nobody here can run says whether a send timeout, a reset or the
    /// cancel ended the write, and how long after the stop began.
    /// @param since The instant the times are measured from.
    /// @return The account.
    [[nodiscard]] std::string Describe(std::chrono::steady_clock::time_point since) const
    {
        auto const guard = std::scoped_lock { _mutex };
        auto const account = std::format("written {}, in a write {}", Written(), InWrite());
        if (!_failedAt.has_value())
            return account + ", no write failed";
        return std::format("{}, a write failed {} ms after that with {}",
                           account,
                           std::chrono::duration_cast<std::chrono::milliseconds>(*_failedAt - since).count(),
                           _error);
    }

  private:
    std::atomic<std::size_t> _written { 0 };
    std::atomic<bool> _inWrite { false };
    mutable std::mutex _mutex;
    std::string _error;
    std::optional<std::chrono::steady_clock::time_point> _failedAt;
};

/// An accepted connection whose writes go to the kernel in chunks of `Chunk`, saying how far each has got.
///
/// **Why a slow-reader case needs one: a write the kernel ABSORBED is not blocked, and there is nothing to cancel.**
/// A blocking `send` of a whole answer returns once the kernel has taken it, and how much it takes is the platform's:
/// Linux bounds it by the send buffer, while Winsock took a 4 MiB send whole past its 1 MiB `SO_SNDBUF` and then
/// delivered it after the close. So a case cannot infer from the answer's size that the server is still writing; it
/// must OBSERVE it -- a write under way that moves no byte for a stretch -- before it starts the stop. Chunks of a
/// size no platform takes past its send buffer make that observable everywhere, and every chunk is still the real
/// socket's blocking write: the cancel must end the one the server is parked in.
///
/// Everything but the write is forwarded unchanged.
class WriteWatchingSocket final: public core::net::ISocket
{
  public:
    /// How much one write call hands the kernel at most: well under any send buffer, so the kernel refuses a chunk
    /// once the buffers under the connection are full rather than taking it whole.
    static constexpr std::size_t Chunk = std::size_t { 8 } * 1024;

    /// @param inner The accepted socket (owned).
    /// @param watch Where progress is recorded; shared with the case.
    WriteWatchingSocket(std::unique_ptr<core::net::ISocket> inner, std::shared_ptr<WriteWatch> watch) noexcept:
        _inner { std::move(inner) },
        _watch { std::move(watch) }
    {
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
        return core::net::IoAwaitable { WriteInChunks(_inner.get(), _watch.get(), buffer) };
    }

    /// Not chunked: nothing the stop cases drive writes vectored, and a decorator that silently changed what it
    /// does not need to would be a second subject.
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

    void close() noexcept override
    {
        _inner->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return _inner->isClosed();
    }

  private:
    /// The whole of @p buffer, a chunk per write call, or the error that stopped it.
    static core::async::Task<core::net::IoResult> WriteInChunks(core::net::ISocket* inner,
                                                                WriteWatch* watch,
                                                                std::span<std::byte const> buffer)
    {
        std::size_t total = 0;
        while (total < buffer.size())
        {
            auto const chunk = buffer.subspan(total, std::min(Chunk, buffer.size() - total));
            watch->Entering();
            auto const wrote = co_await inner->write(chunk);
            watch->Left(wrote.has_value() ? *wrote : 0);
            if (!wrote.has_value())
            {
                watch->Failed(wrote.error());
                co_return std::unexpected(wrote.error());
            }
            total += *wrote;
        }
        co_return total;
    }

    std::unique_ptr<core::net::ISocket> _inner;
    std::shared_ptr<WriteWatch> _watch;
};

/// A bound listener whose accepted connections write in watched chunks (`WriteWatchingSocket`), all recording into
/// one `WriteWatch` -- the stop cases serve one connection, so one record is unambiguous.
class WriteWatchingListener final: public core::net::IListener
{
  public:
    /// @param inner The bound listener (owned).
    explicit WriteWatchingListener(std::unique_ptr<core::net::IListener> inner):
        _inner { std::move(inner) },
        _watch { std::make_shared<WriteWatch>() }
    {
    }

    /// Shared, so a case still holds it once the listener has gone with the server that owned it.
    /// @return What the accepted connections' writes have done.
    [[nodiscard]] std::shared_ptr<WriteWatch const> Watch() const noexcept
    {
        return _watch;
    }

    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override
    {
        auto accepted = co_await _inner->accept();
        if (!accepted.has_value())
            co_return accepted;
        co_return core::net::AcceptResult { std::make_unique<WriteWatchingSocket>(std::move(*accepted), _watch) };
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override
    {
        return _inner->boundPort();
    }

  protected:
    void doClose() noexcept override
    {
        _inner->close();
    }

  private:
    std::unique_ptr<core::net::IListener> _inner;
    std::shared_ptr<WriteWatch> _watch;
};

/// Wait until a write on @p watch is under way and has moved no byte for @p still: the server is PARKED in a write,
/// not merely between two, which is the only state a cancel of a blocked write can be judged from.
/// @param watch What the server's writes have done.
/// @param still How long progress must have stopped.
/// @param bound How long to wait for it.
/// @return Whether it was reached; on the case's thread, its account rides the next assertion.
[[nodiscard]] inline bool WaitUntilParkedInWrite(WriteWatch const& watch,
                                                 std::chrono::milliseconds still,
                                                 std::chrono::milliseconds bound)
{
    auto lastWritten = watch.Written();
    auto lastMoved = std::chrono::steady_clock::now();
    return WaitUntil(
        "the server to be parked in a write that moves nothing",
        [&] {
            auto const now = std::chrono::steady_clock::now();
            if (auto const seen = watch.Written(); seen != lastWritten)
            {
                lastWritten = seen;
                lastMoved = now;
            }
            return watch.InWrite() && now - lastMoved >= still;
        },
        [&] { return std::format("written {}, in a write {}", watch.Written(), watch.InWrite()); },
        WaitOptions { .step = {}, .context = {}, .bound = bound });
}

} // namespace Pace::Testing
