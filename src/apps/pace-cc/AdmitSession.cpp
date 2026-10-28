// SPDX-License-Identifier: Apache-2.0
#include "AdmitSession.hpp"
#include "EndpointDial.hpp"
#include "ExchangeIdleWindow.hpp"

#include <condition_variable>
#include <cstdio>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SocketDeadline.hpp>
#include <core/net/ThreadedAddressResolver.hpp>

namespace Pace::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    using Reply = std::expected<AdmitTicket, std::string>;

    /// Why a completed exchange is not a grant, in words an operator can act on.
    /// @param outcome An exchange that did not end in a hit.
    /// @return The fail-open reason.
    [[nodiscard]] std::string ReasonOf(CacheOutcome const& outcome)
    {
        if (outcome.kind == CacheOutcomeKind::Transport)
            return std::format("admission {}", DescribeTransportFailure(outcome.transportFailure));
        auto described = DescribeOutcome(outcome);
        return described.empty() ? std::string { "admission answered without a grant" } : described;
    }

    /// Read a grant out of an exchange's outcome.
    /// @param outcome The outcome.
    /// @param received When the reply arrived.
    /// @return The ticket, or why the outcome is not one.
    [[nodiscard]] Reply TicketOf(CacheOutcome const& outcome, core::platform::SteadyTimePoint received)
    {
        if (!outcome.IsHit())
            return std::unexpected { ReasonOf(outcome) };
        auto const grant = Wire::DecodeAdmitGrant(outcome.value);
        if (!grant.has_value())
            return std::unexpected { std::string { "admission reply was not a grant" } };
        return AdmitTicket { .kind = grant->kind,
                             .endpoint = grant->endpoint,
                             .leaseToken = grant->leaseToken,
                             .workerCodecs = grant->workerCodecs,
                             .chargedBytes = grant->chargedBytes,
                             .queuedMs = grant->queuedMs,
                             .receivedAt = received };
    }

    /// Where the session stands. Private to this file: never stored or sent.
    enum class Phase : std::uint8_t
    {
        Asking, ///< An ADMIT is outstanding and `Await`/`Requeue` waits on its answer.
        Held,   ///< Granted: the connection is held and the next frame must be SETTLE.
        Closed, ///< Failed open or settled: nothing more is sent.
        Last,
    };

    class Session final: public IAdmitSession
    {
      public:
        Session(std::string hostPort, Credential credential, std::vector<std::byte> admitFrame, ExchangeBudget budget):
            _hostPort { std::move(hostPort) },
            _credential { std::move(credential) },
            _admitFrame { std::move(admitFrame) },
            _budget { budget }
        {
            _budget.total = std::chrono::milliseconds { 0 }; // The queue's wait is bounded by silence alone.
            _thread = std::thread { [this] { Body(); } };
            // The loop exists on the thread that runs it, so nothing is posted before it does.
            std::unique_lock lock { _mutex };
            _cv.wait(lock, [this] { return _loop != nullptr || _ended; });
        }

        ~Session() override
        {
            Post([this] {
                _closing = true;
                // Closing completes whatever the coroutine is parked on, so it ends by itself.
                if (_socket != nullptr)
                    _socket->close();
                if (!_busy)
                    _loop->stop();
            });
            _thread.join();
        }

        Session(Session const&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session const&) = delete;
        Session& operator=(Session&&) = delete;

        [[nodiscard]] Reply Await() override
        {
            std::unique_lock lock { _mutex };
            return WaitForReply(lock);
        }

        [[nodiscard]] Reply Requeue() override
        {
            std::unique_lock lock { _mutex };
            if (_phase != Phase::Held || _loop == nullptr)
                return std::unexpected { std::string { "no admission is held to requeue" } };
            _phase = Phase::Asking;
            _reply.reset();
            _loop->post([this] {
                _busy = true;
                std::ignore = RunRequeue(this);
            });
            return WaitForReply(lock);
        }

        void Settle(Wire::SettleRequest const& settle) override
        {
            std::unique_lock lock { _mutex };
            if (_phase != Phase::Held || _loop == nullptr)
                return;
            _phase = Phase::Closed;
            _settling = true;
            _loop->post([this, settle] {
                _busy = true;
                std::ignore = RunSettle(this, settle);
            });
            _cv.wait(lock, [this] { return !_settling; });
        }

      private:
        /// Wait for the outstanding ADMIT's answer.
        /// @param lock Holding `_mutex`.
        /// @return The answer, or a fail-open when none is outstanding.
        [[nodiscard]] Reply WaitForReply(std::unique_lock<std::mutex>& lock)
        {
            if (_phase != Phase::Asking && !_reply.has_value())
                return std::unexpected { std::string { "no admission is outstanding" } };
            _cv.wait(lock, [this] { return _reply.has_value(); });
            auto reply = std::move(*_reply);
            _reply.reset();
            return reply;
        }

        /// Hand @p work to the loop thread; nothing happens when the loop has already ended.
        template <typename Work>
        void Post(Work work)
        {
            std::lock_guard lock { _mutex };
            if (_loop != nullptr)
                _loop->post(std::move(work));
        }

        /// Record the answer to the outstanding ADMIT and wake its waiter.
        void Deliver(Reply reply)
        {
            {
                std::lock_guard lock { _mutex };
                _phase = reply.has_value() ? Phase::Held : Phase::Closed;
                _reply = std::move(reply);
            }
            _cv.notify_all();
        }

        /// The thread: build the loop, run the opening ADMIT, serve until told to stop.
        void Body() noexcept
        {
            std::string failure;
            try
            {
                Run();
            }
            catch (std::exception const& error)
            {
                failure = error.what();
            }
            catch (...)
            {
                failure = "unknown error";
            }
            // Whatever ended the thread, a waiter is answered and the constructor's wait released: an
            // exception here must cost one compile its queue place, never the process.
            End(failure.empty() ? "admission session ended" : std::format("admission session failed: {}", failure));
        }

        /// Mark the thread over and answer anyone still waiting with a fail-open.
        /// @param reason Why, for the waiter.
        void End(std::string reason)
        {
            {
                std::lock_guard lock { _mutex };
                _loop = nullptr;
                _ended = true;
                // A waiter must never outlive the thread that would have answered it.
                if (_phase == Phase::Asking && !_reply.has_value())
                {
                    _phase = Phase::Closed;
                    _reply = std::unexpected { std::move(reason) };
                }
                _settling = false;
            }
            _cv.notify_all();
        }

        /// Stop handing out the loop: it is about to be destroyed.
        void Detach() noexcept
        {
            std::lock_guard lock { _mutex };
            _loop = nullptr;
        }

        /// The loop thread's work, up to the moment the loop ends.
        void Run()
        {
            core::platform::SteadyClock clock;
            core::net::PlatformLoop loop { clock };
            core::net::ThreadedAddressResolver resolver;
            auto const connector = core::net::makeConnector(loop, resolver);
            _connector = connector.get();

            // Declared AFTER the loop, so it runs BEFORE the loop is destroyed on every exit path, an
            // exception included: `_loop` is withdrawn under the mutex (nothing posts to a dead loop), the
            // socket -- which belongs to this loop -- is dropped, and the resolver is stopped before the loop
            // goes out of scope, for the reason `RunOneExchange` gives.
            struct Retire
            {
                Session* self;
                core::net::ThreadedAddressResolver* resolver;

                Retire(Session* owner, core::net::ThreadedAddressResolver* pool) noexcept:
                    self { owner },
                    resolver { pool }
                {
                }

                ~Retire()
                {
                    self->Detach();
                    self->DropSocket();
                    resolver->stop();
                }

                Retire(Retire const&) = delete;
                Retire& operator=(Retire const&) = delete;
            } const retire { this, &resolver };

            {
                std::lock_guard lock { _mutex };
                _loop = &loop;
            }
            _cv.notify_all();
            _busy = true;
            std::ignore = RunOpening(this);
            loop.run();
        }

        /// One request and its reply on the held socket, the idle bound armed for just that long.
        static core::async::Task<CacheOutcome> RoundTrip(Session* self,
                                                         std::vector<std::byte> frame,
                                                         Credential credential,
                                                         std::chrono::milliseconds idleBound)
        {
            core::net::SocketDeadlineTarget silence { .socket = self->_socket.get() };
            auto bounded = self->_budget;
            bounded.idle = idleBound;
            ExchangeIdleWindow idle { *self->_loop, bounded, &silence };
            idle.MovedForward();
            auto outcome =
                co_await ExchangeFramed(self->_socket.get(), &self->_notice, std::move(frame), std::move(credential), &idle);
            if (outcome.kind == CacheOutcomeKind::Transport && silence.expired)
                outcome.transportFailure = TransportFailure::Silent;
            co_return outcome;
        }

        /// Dial, present the credential and ask; one attempt.
        static core::async::Task<CacheOutcome> DialAndAsk(Session* self)
        {
            self->_socket = co_await DialEndpoint(
                self->_connector,
                self->_hostPort,
                core::net::DialOptions { .connectTimeout = self->_budget.connect, .keepAlive = self->_budget.keepAlive });
            if (self->_socket == nullptr || self->_closing)
            {
                CacheOutcome unreached;
                unreached.transportFailure = TransportFailure::Unreached;
                co_return unreached;
            }
            co_return co_await RoundTrip(self, self->_admitFrame, self->_credential, self->_budget.idle);
        }

        /// What the answer to an ADMIT does to the session, on the loop thread.
        void Conclude(CacheOutcome const& outcome)
        {
            auto reply = TicketOf(outcome, _loop->clock().now());
            auto const granted = reply.has_value();
            Deliver(std::move(reply));
            if (!granted)
                DropSocket();
        }

        void DropSocket() noexcept
        {
            if (_socket != nullptr)
                _socket->close();
            _socket.reset();
        }

        /// Called as a task's last act: the loop stops when nothing more can happen on it.
        void EndOfTask()
        {
            _busy = false;
            if (_closing || _finished || _socket == nullptr)
                _loop->stop();
        }

        /// The opening ADMIT.
        static core::async::DetachedTask RunOpening(Session* self)
        {
            auto outcome = co_await DialAndAsk(self);
            // `NotLeader`: the local node now forwards, so one more try against the same address.
            // Once, because a node that keeps answering it has no leader to name.
            if (outcome.kind == CacheOutcomeKind::Rejected && outcome.code == Wire::ErrorCode::NotLeader && !self->_closing)
            {
                self->DropSocket();
                outcome = co_await DialAndAsk(self);
            }
            self->Conclude(outcome);
            self->EndOfTask();
        }

        /// A requeue: settle the refusal, then ask again on the same connection.
        /// `SettleBound` applies to the SETTLE here too: a node too slow to answer it makes the requeue fail open
        /// into a local compile, by design.
        static core::async::DetachedTask RunRequeue(Session* self)
        {
            auto const refused =
                co_await RoundTrip(self,
                                   Wire::EncodeSettle(Wire::SettleRequest { .outcome = Wire::SettleOutcome::WorkerRefused,
                                                                            .peakBytes = std::nullopt }),
                                   Credential {},
                                   SettleBound);
            if (refused.IsHit())
                self->Conclude(co_await RoundTrip(self, self->_admitFrame, Credential {}, self->_budget.idle));
            else
            {
                self->Deliver(std::unexpected { ReasonOf(refused) });
                self->DropSocket();
            }
            self->EndOfTask();
        }

        /// The final SETTLE.
        static core::async::DetachedTask RunSettle(Session* self, Wire::SettleRequest settle)
        {
            auto const outcome = co_await RoundTrip(self, Wire::EncodeSettle(settle), Credential {}, SettleBound);
            if (!outcome.IsHit())
                std::fprintf(stderr, "pace-cc: settling the admission failed: %s\n", ReasonOf(outcome).c_str());
            self->DropSocket();
            self->_finished = true;
            self->EndOfTask();
        }

        std::string _hostPort;
        Credential _credential;
        std::vector<std::byte> _admitFrame;
        ExchangeBudget _budget;
        CredentialNotice _notice { CredentialNotice::Silent() };

        std::thread _thread;
        std::mutex _mutex;
        std::condition_variable _cv;
        core::net::EventLoop* _loop { nullptr }; ///< Guarded by `_mutex`; null once the loop thread ends.
        std::optional<Reply> _reply;             ///< Guarded by `_mutex`.
        Phase _phase { Phase::Asking };          ///< Guarded by `_mutex`.
        bool _settling { false };                ///< Guarded by `_mutex`.
        bool _ended { false };                   ///< Guarded by `_mutex`; the thread is over.

        // Loop thread only.
        core::net::IConnector* _connector { nullptr };
        std::unique_ptr<core::net::ISocket> _socket;
        bool _busy { false };
        bool _closing { false };
        bool _finished { false };
    };
} // namespace

std::unique_ptr<IAdmitSession> OpenAdmitSession(std::string_view hostPort,
                                                Credential credential,
                                                Wire::AdmitRequest const& request,
                                                ExchangeBudget budget)
{
    return std::make_unique<Session>(std::string { hostPort }, std::move(credential), Wire::EncodeAdmit(request), budget);
}

} // namespace Pace::Cc
