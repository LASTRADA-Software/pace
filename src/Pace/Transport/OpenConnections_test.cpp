// SPDX-License-Identifier: Apache-2.0
#include <Pace/Transport/NativeListen.hpp>
#include <Pace/Transport/OpenConnections.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>
#include <tests/BoundedWait.hpp>

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

/// A listener with NO send timeout armed, so a blocking write to a client that does not read blocks until something
/// ends it from outside -- the arrangement a cancel exists for, with nothing else that could pass for it.
[[nodiscard]] std::unique_ptr<Pace::BlockingListener> Listening()
{
    auto listener = Pace::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    REQUIRE(listener->IsBound());
    listener->SetTimeouts(std::chrono::milliseconds { 2000 }, std::chrono::milliseconds { 0 });
    return listener;
}

/// Dial @p listener and accept the connection.
/// @param listener Where to connect.
/// @return Both ends.
[[nodiscard]] Connected Connect(Pace::BlockingListener& listener)
{
    core::net::BlockingConnector connector;
    auto client = core::async::syncRun(
        connector.connect("127.0.0.1", listener.boundPort(), core::net::DialOptions { .connectTimeout = 5s }));
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

    std::atomic<bool> writeReturned { false };
    std::atomic<bool> writeFailed { false };
    std::jthread writer { [&, socket = served.get()] {
        std::vector<std::byte> const payload(std::size_t { 64 } * 1024 * 1024);
        auto const written = core::async::syncRun(WriteOnce(socket, std::span<std::byte const> { payload }));
        writeFailed.store(!written.has_value());
        writeReturned.store(true);
    } };
    // The write is parked in the kernel, not merely late: it would never return by itself.
    std::this_thread::sleep_for(300ms);
    // CHECK, never REQUIRE: an unwinding failure here would leave the join below waiting on a write nobody ends.
    CHECK_FALSE(writeReturned.load());

    CHECK(listener->Connections().CancelOpenConnections() == 1);
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
    late.served->setReceiveDeadline(5s);
    std::vector<std::byte> buffer(16);
    auto const read = core::async::syncRun(ReadOnce(late.served.get(), std::span<std::byte> { buffer }));
    // A shut-down read side answers end-of-stream at once, where an open one would wait out the deadline.
    REQUIRE(read.has_value());
    CHECK(*read == 0);

    SECTION("and a later cancel does not count it a second time")
    {
        CHECK(listener->Connections().CancelOpenConnections() == 0);
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
