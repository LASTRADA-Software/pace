// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <memory>

#include <core/platform/Types.hpp>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>
#endif

namespace Pace::Testing
{

/// The send buffer a slow-reader case asks its server's sockets for: small, so how much of an answer the kernel
/// takes off a blocking write is this figure and not the host's tuning.
inline constexpr std::size_t SmallSendBufferBytes = std::size_t { 64 } * 1024;

/// The most a kernel may grant for `SmallSendBufferBytes` before a case stops being able to say a write is still in
/// flight: Linux doubles what it is asked for and keeps a floor, and nothing honest grants eight times the request.
inline constexpr std::size_t SmallSendBufferGrantCeiling = SmallSendBufferBytes * 8;

/// A loopback `BlockingListener` whose accepted sockets carry a send buffer of `SmallSendBufferBytes`.
///
/// **Why a case needs one: an answer is "still being written" only relative to the buffers under it.** Left to
/// autotuning, a loopback send buffer grows to `net.ipv4.tcp_wmem`'s ceiling -- 4 MiB by default, 16 MiB or more on
/// a host tuned for throughput -- and an answer smaller than that leaves the server's write before any stop can find
/// it, so a case asserting a cancel goes red on the host and not on the code. Setting `SO_SNDBUF` turns autotuning
/// off for the socket, and an accepted socket inherits it from the listening one on Linux, the BSDs and Windows.
///
/// Bound, released, sized and ADOPTED back, because the listener exposes its descriptor only to give it away: what is
/// handed back is the production listener over the same socket, so every accept still runs `AcceptNow`. The size the
/// kernel actually GRANTED is read back and refused past `SmallSendBufferGrantCeiling`, naming the buffer, so a host
/// that ignores the request fails here and says why rather than later as a missing cancel.
/// @return The bound listener; its timeouts are the caller's to arm.
[[nodiscard]] inline std::unique_ptr<BlockingListener> BindWithSmallSendBuffer()
{
    auto bound = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(bound != nullptr);
    REQUIRE(bound->IsBound());
    auto const handle = bound->Release();
#if defined(_WIN32)
    auto const socket = reinterpret_cast<SOCKET>(handle);
    auto requested = static_cast<int>(SmallSendBufferBytes);
    auto const set =
        ::setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&requested), sizeof(requested));
    int granted = 0;
    int length = sizeof(granted);
    auto const got = ::getsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&granted), &length);
#else
    auto requested = static_cast<int>(SmallSendBufferBytes);
    auto const set = ::setsockopt(handle, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested));
    int granted = 0;
    socklen_t length = sizeof(granted);
    auto const got = ::getsockopt(handle, SOL_SOCKET, SO_SNDBUF, &granted, &length);
#endif
    auto adopted = BlockingListener::Adopt(handle);
    REQUIRE(adopted != nullptr);
    INFO(adopted->BindError());
    REQUIRE(adopted->IsBound());
    REQUIRE(set == 0);
    REQUIRE(got == 0);
    INFO(std::format("SO_SNDBUF asked {} and granted {}: a case that must find an answer still being written needs a "
                     "send buffer far below the answer, whatever net.ipv4.tcp_wmem says",
                     SmallSendBufferBytes,
                     granted));
    REQUIRE(granted > 0);
    REQUIRE(static_cast<std::size_t>(granted) <= SmallSendBufferGrantCeiling);
    return adopted;
}

} // namespace Pace::Testing
