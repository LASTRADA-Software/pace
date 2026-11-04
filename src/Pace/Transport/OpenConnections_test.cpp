// SPDX-License-Identifier: Apache-2.0
#include <Pace/Transport/NativeListen.hpp>
#include <Pace/Transport/OpenConnections.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/SmallSendBufferListener.hpp>

using namespace std::chrono_literals;

namespace
{

/// One read, as a task a thread can run to its end.
/// @param socket Where to read.
/// @param buffer Where the bytes go; non-empty.
/// @return What the read answered.
core::async::Task<core::net::IoResult> ReadOnce(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->read(buffer);
}

/// One write, as a task a thread can run to its end.
/// @param socket Where to write.
/// @param bytes What to write.
/// @return What the write answered.
core::async::Task<core::net::IoResult> WriteOnce(core::net::ISocket* socket, std::span<std::byte const> bytes)
{
    co_return co_await socket->write(bytes);
}

/// A real loopback connection: the client end and the end the listener accepted, tracked.
struct Connected
{
    std::unique_ptr<core::net::ISocket> client;
    std::unique_ptr<core::net::ISocket> served;
};

/// A listener with NO send timeout armed and a small send buffer, so a blocking write to a client that does not read
/// fills what is under it at once and then blocks until something ends it from outside -- the arrangement a cancel
/// exists for, with nothing else that could pass for it and nothing that depends on the host's buffer tuning.
[[nodiscard]] std::unique_ptr<Pace::BlockingListener> Listening()
{
    auto listener = Pace::Testing::BindWithSmallSendBuffer();
    listener->SetTimeouts(std::chrono::milliseconds { 2000 }, std::chrono::milliseconds { 0 });
    return listener;
}

/// Dial @p listener, with a small receive buffer, and accept the connection.
/// @param listener Where to connect.
/// @return Both ends.
[[nodiscard]] Connected Connect(Pace::BlockingListener& listener)
{
    core::net::BlockingConnector connector;
    auto client = core::async::syncRun(connector.connect(
        "127.0.0.1",
        listener.boundPort(),
        core::net::DialOptions { .connectTimeout = 5s, .buffers = { .send = {}, .receive = std::size_t { 4096 } } }));
    REQUIRE(client.has_value());
    auto served = core::async::syncRun(listener.accept());
    REQUIRE(served.has_value());
    return Connected { .client = std::move(*client), .served = std::move(*served) };
}

} // namespace

TEST_CASE("OpenConnections: a cancel ends a blocking write another thread is inside", "[net][stop]")
{
    // The client never reads and the socket has no send timeout, so the write blocks once the buffers are full and
    // nothing on its own thread can end it. The cancel, from this thread, must.
    auto listener = Listening();
    auto [client, served] = Connect(*listener);
    REQUIRE(listener->Connections().OpenCount() == 1);

    // Chunk after chunk, counting what the kernel took and saying when a write is under way: a write that has begun
    // and moves no byte for a stretch is one the kernel has parked, which a fixed sleep could only have hoped for.
    // Bounded by its count -- far past what the buffers hold -- so a writer nothing blocks ends on its own.
    constexpr auto Chunk = std::size_t { 64 } * 1024;
    std::atomic<std::size_t> written { 0 };
    std::atomic<bool> inWrite { false };
    std::atomic<bool> writeReturned { false };
    std::atomic<bool> writeFailed { false };
    std::jthread writer { [&, socket = served.get()] {
        std::vector<std::byte> const payload(Chunk);
        for ([[maybe_unused]] auto const step: std::views::iota(0, 1024))
        {
            inWrite.store(true);
            auto const wrote = core::async::syncRun(WriteOnce(socket, std::span<std::byte const> { payload }));
            inWrite.store(false);
            if (!wrote.has_value())
            {
                writeFailed.store(true);
                break;
            }
            written.fetch_add(*wrote);
        }
        writeReturned.store(true);
    } };
    auto lastWritten = written.load();
    auto lastMoved = std::chrono::steady_clock::now();
    auto const parked = Pace::Testing::WaitUntil(
        "the writer to be parked in a write that moves nothing",
        [&] {
            auto const now = std::chrono::steady_clock::now();
            if (auto const seen = written.load(); seen != lastWritten)
            {
                lastWritten = seen;
                lastMoved = now;
            }
            return inWrite.load() && !writeReturned.load() && now - lastMoved >= 250ms;
        },
        [&] { return std::format("written {}, in a write {}", written.load(), inWrite.load()); },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = 5s });
    // CHECK, never REQUIRE: an unwinding failure here would leave the join below waiting on a write nobody ends.
    CHECK(parked);
    CHECK_FALSE(writeReturned.load());

    CHECK(listener->Connections().CancelOpenConnections() == 1);
    CHECK(listener->Connections().CancelledTotal() == 1);
    auto const ended = Pace::Testing::WaitUntil(
        "the cancelled write to return",
        [&] { return writeReturned.load(); },
        [] { return std::string { "still writing" }; },
        Pace::Testing::WaitOptions { .step = {}, .context = {}, .bound = 5s });
    // Releases a writer the cancel did not reach, so the join below ends whatever happened.
    client->close();
    writer.join();
    CHECK(ended);
    CHECK(writeFailed.load());

    SECTION("and the cancelled connection is still its owner's to close, leaving the registry")
    {
        CHECK(listener->Connections().OpenCount() == 1);
        served->close();
        CHECK(listener->Connections().OpenCount() == 0);
    }
}

TEST_CASE("OpenConnections: a connection its owner closed is never cancelled", "[net][stop]")
{
    // A closed descriptor's number is the next one the process opens, so a cancel reaching it would end a stranger.
    auto listener = Listening();
    auto first = Connect(*listener);
    first.served->close();
    CHECK(listener->Connections().OpenCount() == 0);

    SECTION("closed explicitly")
    {
        CHECK(listener->Connections().CancelOpenConnections() == 0);
    }
    SECTION("or destroyed")
    {
        auto second = Connect(*listener);
        CHECK(listener->Connections().OpenCount() == 1);
        second.served.reset();
        CHECK(listener->Connections().OpenCount() == 0);
        CHECK(listener->Connections().CancelOpenConnections() == 0);
    }
}

TEST_CASE("OpenConnections: a cancel latches, so a connection handed out after it is cancelled at once", "[net][stop]")
{
    // An accept already in flight when a stop cancels can still hand a connection out; it must not be served to a
    // client the stop has stopped waiting for.
    auto listener = Listening();
    REQUIRE(listener->Connections().CancelOpenConnections() == 0);
    auto late = Connect(*listener);
    // Counted where it was shut down, at the handing out, so a stop's report includes it.
    CHECK(listener->Connections().CancelledTotal() == 1);
    constexpr auto Deadline = 5s;
    late.served->setReceiveDeadline(Deadline);
    std::vector<std::byte> buffer(16);
    auto const began = std::chrono::steady_clock::now();
    auto const read = core::async::syncRun(ReadOnce(late.served.get(), std::span<std::byte> { buffer }));
    auto const took = std::chrono::steady_clock::now() - began;
    // A shut-down read side answers AT ONCE, where an open one would wait out the deadline: end-of-stream on POSIX,
    // `WSAESHUTDOWN` on Windows. What it answers is the platform's; that it did not wait is the cancel's.
    CHECK((!read.has_value() || *read == 0));
    CHECK(took < Deadline / 5);

    SECTION("and a later cancel does not count it a second time")
    {
        CHECK(listener->Connections().CancelOpenConnections() == 0);
        CHECK(listener->Connections().CancelledTotal() == 1);
        CHECK(listener->Connections().OpenCount() == 1);
    }
    SECTION("the control: before any cancel, an accepted connection is served")
    {
        auto fresh = Listening();
        auto open = Connect(*fresh);
        open.served->setReceiveDeadline(5s);
        REQUIRE(core::async::syncRun(WriteOnce(open.client.get(), std::span<std::byte const> { buffer })).has_value());
        auto const served = core::async::syncRun(ReadOnce(open.served.get(), std::span<std::byte> { buffer }));
        REQUIRE(served.has_value());
        CHECK(*served == buffer.size());
    }
}
