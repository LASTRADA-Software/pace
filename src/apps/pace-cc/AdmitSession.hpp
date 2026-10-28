// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"

#include <Pace/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>

namespace Pace::Cc
{

/// What an admission granted, as the launcher reads it. Owns its bytes.
struct AdmitTicket
{
    CompileCacheWire::GrantKind kind {};      ///< Where the compile runs, or that another already stored it.
    std::string endpoint;                     ///< The worker to dial; empty unless `kind` is `Worker`.
    std::string leaseToken;                   ///< Authorizes the job at that worker; empty unless `Worker`.
    CompileCacheWire::CodecList workerCodecs; ///< What that worker can decode.
    std::uint64_t chargedBytes {};            ///< The memory estimate the leader charged for this admission.
    std::uint64_t queuedMs {};                ///< How long the request waited in the queue.
    /// When THIS side received the grant, on a monotonic clock (R2).
    ///
    /// A grant's age is measured from here and never from when the launcher asked: a request that
    /// queued for a minute is not a minute-old lease, and one that queued for a second may be
    /// nearly spent.
    core::platform::SteadyTimePoint receivedAt {};
};

/// A held admission: one connection to the local node, kept open on a thread of its own for the
/// length of a compile (R5).
///
/// **Why a thread and not the compile's own reactor.** The compile blocks the launcher's main
/// thread for as long as a compiler runs, and the leader reads this connection's end as a release
/// (`Abandon`). So the connection must outlive every exchange the compile makes and must be kept
/// by something that is not waiting on the compile.
class IAdmitSession
{
  public:
    IAdmitSession() = default;
    virtual ~IAdmitSession() = default;
    IAdmitSession(IAdmitSession const&) = delete;
    IAdmitSession(IAdmitSession&&) = delete;
    IAdmitSession& operator=(IAdmitSession const&) = delete;
    IAdmitSession& operator=(IAdmitSession&&) = delete;

    /// Block until granted or until the session fails open. Never throws.
    ///
    /// Asked once for the opening admission, and once more after each `Requeue`; a second call
    /// with no request outstanding fails open at once rather than blocking.
    /// @return The grant, or the reason this session fails open: the caller then compiles where
    ///         it is, exactly as it did before admission existed.
    [[nodiscard]] virtual std::expected<AdmitTicket, std::string> Await() = 0;

    /// The worker refused the job: send `SETTLE{WorkerRefused}` on the SAME connection, then ask
    /// again on it and wait for the next grant, exactly as `Await` does.
    ///
    /// Unlike the opening ADMIT, the second one is NOT retried on `NotLeader`: it rides a connection the
    /// node already granted on, so any refusal there fails open.
    /// @return The next grant, or the reason this session fails open.
    [[nodiscard]] virtual std::expected<AdmitTicket, std::string> Requeue() = 0;

    /// Send `SETTLE` on the held connection and close it. Best effort: a failure is reported on
    /// stderr and never fatal, and a session that never held a grant does nothing.
    /// @param settle How the compile ended.
    virtual void Settle(CompileCacheWire::SettleRequest const& settle) = 0;
};

/// The fail-open reason of an admission that could not reach its node at all (`TransportFailure::Unreached`).
///
/// One spelling, shared by the session that says it and the launcher that remembers it, so a node that
/// is down is learned once and not dialled again by every launcher of a build for the next fifteen
/// seconds -- a refused connect on Windows can take the whole connect budget.
/// @return The reason text.
[[nodiscard]] std::string AdmitUnreachedReason();

/// The longest a SETTLE waits for its answer.
///
/// **Its own bound, because a SETTLE cannot pulse**: the idle bound is sized for a queue that pulses
/// every `DefaultProgressInterval`, and spent here it would hold the launcher up to
/// `DefaultCompileIdleTimeout` after its compile is already done. The same order as the node's relay of
/// SETTLE to the leader (`FrameServer::HeaderTimeout / 2`, `AdmitForwarder.cpp`), which this launcher cannot
/// include; a leader that has not answered by then has the connection's end to read as an abandonment.
inline constexpr std::chrono::milliseconds SettleBound { 2'500 };

/// The budget an admission runs under: a one second connect, NO total bound, and the compile
/// idle bound -- the queue may take as long as it takes while the node says it is alive.
/// @return The budget.
[[nodiscard]] constexpr ExchangeBudget DefaultAdmitBudget() noexcept
{
    return ExchangeBudget { .connect = std::chrono::milliseconds { 1'000 },
                            .total = std::chrono::milliseconds { 0 },
                            .idle = CompileCacheWire::DefaultCompileIdleTimeout,
                            .keepAlive = core::net::KeepAlive::No };
}

/// Open an admission session and send the ADMIT. Returns at once; `Await` is where it blocks.
///
/// A `NotLeader` answer is retried ONCE against the same @p hostPort (the local node forwards);
/// every other way of not being granted fails open.
/// @param hostPort The local node's `0xFC` endpoint, `host:port`.
/// @param credential Presented with the first frame.
/// @param request What to admit.
/// @param budget Its deadlines; `total` is ignored, the session has no total bound.
/// @return The session; never null.
[[nodiscard]] std::unique_ptr<IAdmitSession> OpenAdmitSession(std::string_view hostPort,
                                                              Credential credential,
                                                              CompileCacheWire::AdmitRequest const& request,
                                                              ExchangeBudget budget);

} // namespace Pace::Cc
