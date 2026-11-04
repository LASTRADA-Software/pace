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
#include <span>
#include <string>
#include <utility>

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/IoResult.hpp>
#include <tests/BoundedWait.hpp>

namespace Pace::Testing
{

/// What the server's writes on a `WriteWatchingListener`'s connections have done so far, read from the case's thread.
struct WriteWatch
{
    /// Every byte the kernel has taken off a write.
    std::atomic<std::size_t> written { 0 };
    /// Whether a write call is under way at this moment.
    std::atomic<bool> inWrite { false };
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
    /// How much one write call hands the kernel at most. Small, so a client reading a KiB at a time drains a whole
    /// chunk well inside one send timeout (`AdminHttpServer::RequestTimeout`): a chunk a slow reader takes longer than
    /// that to empty would end the write on a TIMEOUT -- on Windows a send that times out leaves the socket unusable --
    /// and the case would be measuring the send timeout rather than the cancel.
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
            watch->inWrite.store(true);
            auto const wrote = co_await inner->write(chunk);
            watch->inWrite.store(false);
            if (!wrote.has_value())
                co_return std::unexpected(wrote.error());
            total += *wrote;
            watch->written.fetch_add(*wrote);
        }
        co_return total;
    }

    std::unique_ptr<core::net::ISocket> _inner;
    std::shared_ptr<WriteWatch> _watch;
};

/// Whether a reader taking @p bytes every @p step empties one `WriteWatchingSocket::Chunk` in under half of
/// @p sendTimeout, so every write call of a slow-reader case makes progress rather than timing out.
/// @param bytes What the reader takes per step.
/// @param step How long it waits between steps.
/// @param sendTimeout The server's per-call send timeout.
/// @return True when the chunk drains with room to spare.
[[nodiscard]] constexpr bool WriteWatchingSocketChunkDrains(std::size_t bytes,
                                                            std::chrono::milliseconds step,
                                                            std::chrono::milliseconds sendTimeout) noexcept
{
    return bytes > 0 && step * static_cast<std::int64_t>(WriteWatchingSocket::Chunk / bytes) < sendTimeout / 2;
}

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
    auto lastWritten = watch.written.load();
    auto lastMoved = std::chrono::steady_clock::now();
    return WaitUntil(
        "the server to be parked in a write that moves nothing",
        [&] {
            auto const now = std::chrono::steady_clock::now();
            if (auto const seen = watch.written.load(); seen != lastWritten)
            {
                lastWritten = seen;
                lastMoved = now;
            }
            return watch.inWrite.load() && now - lastMoved >= still;
        },
        [&] { return std::format("written {}, in a write {}", watch.written.load(), watch.inWrite.load()); },
        WaitOptions { .step = {}, .context = {}, .bound = bound });
}

} // namespace Pace::Testing
