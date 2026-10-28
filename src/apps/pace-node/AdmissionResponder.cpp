// SPDX-License-Identifier: Apache-2.0
#include "AdmissionResponder.hpp"

#include <Pace/Core/HostPort.hpp>
#include <Pace/Protocol/AdmitKeepAlive.hpp>
#include <Pace/Protocol/ConnectionScope.hpp>
#include <Pace/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/InterruptibleSleep.hpp>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A key as the registry stores one: the raw bytes, in a string.
    /// @param key An identity public key.
    /// @return Its bytes.
    [[nodiscard]] std::string MachineIdOf(Ed25519PublicKey const& key)
    {
        std::string out;
        out.reserve(key.size());
        for (auto const byte: key)
            out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        return out;
    }

    /// Encode what the service decided: an `Ok` carrying its payload, or its refusal.
    /// @param reply The service reply.
    /// @return The frame to write.
    [[nodiscard]] std::vector<std::byte> EncodeServiceReply(Distributed::SchedulerReply const& reply)
    {
        if (reply.status == Wire::Status::Ok)
            return Wire::EncodeReply(Wire::Status::Ok, reply.payload);
        return Cc::RefuseWithoutCounter(
            { .code = reply.error,
              .rationale = "counted at the decision, in SchedulerService::Refuse, which triages every code it can "
                           "produce; an admission refusal is that same refusal reaching a parked connection" },
            reply.message);
    }

    /// The `NotLeader` a parked request is answered when this node stops leading.
    ///
    /// Not `EncodeServiceReply`: that one says the service counted the refusal, and a demotion is
    /// decided by no gate -- the queue forgot every waiter at once (`LoseLeadership`).
    /// @param leader Where the new leader is, or empty.
    /// @return The frame to write.
    [[nodiscard]] std::vector<std::byte> RefuseDemoted(std::string_view leader)
    {
        return Cc::RefuseWithoutCounter(
            { .code = Wire::ErrorCode::NotLeader,
              .rationale = "a demotion is one event answered to every parked request at once, not a refusal of "
                           "this caller: a row per waiter would count one leadership change once per queued "
                           "compile, and the client follows the NotLeader to the new leader, where its request "
                           "is queued and counted again" },
            leader);
    }

    /// A refusal this responder decides itself, before the service is asked.
    /// @param code What the client is told.
    /// @param detail Words for a person.
    /// @return The frame to write.
    [[nodiscard]] std::vector<std::byte> RefuseHere(Wire::ErrorCode code, std::string_view detail)
    {
        return Cc::RefuseWithoutCounter({ .code = code,
                                          .rationale = "a frame this connection cannot be served -- no connection to "
                                                       "park on, no machine to charge, nothing to settle -- is a client "
                                                       "or wiring mistake, not a fleet condition" },
                                        detail);
    }
} // namespace

/// What one parked `ADMIT` waits on: written by whoever decides, read by the parked frame.
struct AdmissionWaiter
{
    core::async::StopSource wake;                         ///< Requested when there is something to read.
    std::optional<Distributed::AdmissionVerdict> verdict; ///< Set by a pump; guarded by the core mutex.
    std::optional<std::string> demotedTo;                 ///< Set by a demotion: where the new leader is.
};

/// The state a responder shares with its parked frames and the holds it leaves on connections.
///
/// A `shared_ptr` of its own because those outlive the responder: a frame the reactor frees at teardown
/// and a hold the connection destroys run after the responder is gone, and a pointer to the responder
/// itself would then be a use-after-free. `service` is nulled by the responder destructor, under the
/// lock, so a late destructor finds nobody to tell and does nothing.
struct AdmissionCore
{
    std::mutex mutex;
    Distributed::SchedulerService* service { nullptr }; ///< Null once detached.
    Distributed::IMembershipOracle const* membership { nullptr };
    core::net::EventLoop* loop { nullptr };
    IMetricsSink* metrics { nullptr };
    HeldChargeRegistry* held { nullptr };
    std::chrono::milliseconds progressInterval {};
    std::unordered_map<Distributed::Ticket, std::shared_ptr<AdmissionWaiter>> waiters; ///< Guarded by `mutex`.

    /// @return The service, or null once detached.
    [[nodiscard]] Distributed::SchedulerService* Service()
    {
        auto const guard = std::scoped_lock { mutex };
        return service;
    }

    /// Hand each verdict to the connection waiting on it and wake that connection.
    /// A verdict nobody waits for is given back at once -- the charge it took is nobody's.
    /// @param verdicts What a pump decided.
    void Deliver(std::vector<Distributed::AdmissionVerdict> verdicts)
    {
        std::vector<core::async::StopSource> wakes;
        std::vector<Distributed::Ticket> orphans;
        {
            auto const guard = std::scoped_lock { mutex };
            for (auto& verdict: verdicts)
            {
                auto const found = waiters.find(verdict.ticket);
                if (found == waiters.end())
                {
                    if (!verdict.refusal.has_value() && verdict.kind != Wire::GrantKind::Stored)
                        orphans.push_back(verdict.ticket);
                    continue;
                }
                found->second->verdict = std::move(verdict);
                wakes.push_back(found->second->wake);
            }
        }
        // Outside the lock: a stop runs the park cancellation on this thread.
        for (auto& wake: wakes)
            wake.request_stop();
        if (auto* const svc = Service(); svc != nullptr)
            for (auto const ticket: orphans)
                svc->AbandonAdmission(ticket);
    }

    /// Ask the queue what now fits and deliver it.
    void PumpAndDeliver()
    {
        if (auto* const svc = Service(); svc != nullptr)
            Deliver(svc->PumpAdmission());
    }

    /// This node stopped leading: answer every waiter `NotLeader` naming @p leader.
    /// @param lost The tickets the queue forgot.
    /// @param leader Where the new leader is, or empty.
    void Demote(std::vector<Distributed::Ticket> const& lost, std::string const& leader)
    {
        std::vector<core::async::StopSource> wakes;
        {
            auto const guard = std::scoped_lock { mutex };
            for (auto const ticket: lost)
                if (auto const found = waiters.find(ticket); found != waiters.end())
                {
                    found->second->demotedTo = leader;
                    wakes.push_back(found->second->wake);
                }
        }
        for (auto& wake: wakes)
            wake.request_stop();
    }

    /// Give a ticket back because its connection ended or its next frame was not a settle.
    /// @param ticket Any ticket.
    /// @param counted Whether the endpoint has already filed this departure under `AdmissionAbandoned`
    ///        (`ConnectionScope::DepartureCounted`): one departure is one count.
    void Abandon(Distributed::Ticket ticket, bool counted = false)
    {
        auto* const svc = Service();
        if (svc == nullptr)
            return;
        svc->AbandonAdmission(ticket);
        if (!counted)
            metrics->Increment(IMetricsSink::Counter::AdmissionAbandoned);
        // Freed room may admit whoever waits behind it.
        PumpAndDeliver();
    }

    /// Forget a waiter.
    /// @param ticket The ticket it waited on.
    void Forget(Distributed::Ticket ticket)
    {
        auto const guard = std::scoped_lock { mutex };
        waiters.erase(ticket);
    }
};

namespace
{
    /// Owns a ticket for the length of one parked `ADMIT`: forgets its waiter, and abandons the ticket
    /// unless the answer took it over. RAII for the reason every other release here is -- a parked frame
    /// ends in several ways, a reactor torn down with it among them.
    class Registration
    {
      public:
        /// @param core The shared state; kept alive by this.
        /// @param ticket The ticket this guards.
        Registration(std::shared_ptr<AdmissionCore> core, Distributed::Ticket ticket) noexcept:
            _core { std::move(core) },
            _ticket { ticket }
        {
        }

        Registration(Registration const&) = delete;
        Registration& operator=(Registration const&) = delete;
        Registration(Registration&&) = delete;
        Registration& operator=(Registration&&) = delete;

        /// Forgets the waiter, then abandons the ticket when nothing took it over.
        ~Registration()
        {
            _core->Forget(_ticket);
            if (!_done)
                _core->Abandon(_ticket);
        }

        /// The ticket needs no abandoning: the service already did (a refusal), it holds nothing
        /// (`Stored`), or a connection hold now owns it.
        void Done() noexcept
        {
            _done = true;
        }

      private:
        std::shared_ptr<AdmissionCore> _core;
        Distributed::Ticket _ticket;
        bool _done { false };
    };

    /// A granted admission, held on its connection until the settle or the close.
    class AdmissionHold final: public IConnectionHold
    {
      public:
        /// @param core The shared state.
        /// @param scope The connection this is held on; it owns this, so it outlives it.
        /// @param ticket The granted ticket.
        /// @param charge The machine own record of a loopback local lease; empty for any other.
        AdmissionHold(std::shared_ptr<AdmissionCore> core,
                      ConnectionScope const& scope,
                      Distributed::Ticket ticket,
                      HeldChargeRegistry::Handle charge):
            _core { std::move(core) },
            _scope { &scope },
            _ticket { ticket },
            _charge { std::move(charge) }
        {
        }

        /// Abandons the ticket unless a settle took it: the connection ended, or another frame came.
        /// A departure the endpoint already counted -- the grant was ready and nobody took it -- is
        /// given back without being counted again.
        ~AdmissionHold() override
        {
            if (!_settled)
                _core->Abandon(_ticket, _scope->DepartureCounted());
        }

        AdmissionHold(AdmissionHold const&) = delete;
        AdmissionHold& operator=(AdmissionHold const&) = delete;
        AdmissionHold(AdmissionHold&&) = delete;
        AdmissionHold& operator=(AdmissionHold&&) = delete;

        /// @copydoc IConnectionHold::SurvivesFrame
        [[nodiscard]] bool SurvivesFrame(std::uint8_t opRaw) const noexcept override
        {
            return opRaw == static_cast<std::uint8_t>(Wire::Op::Settle);
        }

        /// @copydoc IConnectionHold::IdleWindow
        /// As long as a granted compile may run: the settle comes when the compiler is done.
        [[nodiscard]] std::chrono::milliseconds IdleWindow() const noexcept override
        {
            return Wire::MaxCompileLeaseLifetime;
        }

        /// @copydoc IConnectionHold::KeepAlive
        /// The one probe every held admission connection carries, at either end of it.
        [[nodiscard]] std::optional<core::net::KeepAliveSettings> KeepAlive() const noexcept override
        {
            return AdmitKeepAliveProbe;
        }

        /// @return The ticket this holds.
        [[nodiscard]] Distributed::Ticket Ticket() const noexcept
        {
            return _ticket;
        }

        /// The settle took the ticket over: this releases nothing more but the local record.
        void Settled() noexcept
        {
            _settled = true;
        }

      private:
        std::shared_ptr<AdmissionCore> _core;
        ConnectionScope const* _scope;
        Distributed::Ticket _ticket;
        HeldChargeRegistry::Handle _charge;
        bool _settled { false };
    };

    /// A ticket a worker refused, back at the front of the queue (R7, Ruling 9), held on its connection
    /// until that connection's next `ADMIT` waits on it again.
    ///
    /// The waiter is registered from the moment the settle put the ticket back, so a pump that decides
    /// it before the client asks again has somewhere to leave the decision. Only an `ADMIT` survives;
    /// any other frame, or the close, abandons the ticket -- exactly as for a grant nobody settles.
    class RequeuedHold final: public IConnectionHold
    {
      public:
        /// @param core The shared state.
        /// @param ticket The ticket the settle put back.
        /// @param waiter Its waiter, already registered in the core.
        RequeuedHold(std::shared_ptr<AdmissionCore> core,
                     Distributed::Ticket ticket,
                     std::shared_ptr<AdmissionWaiter> waiter) noexcept:
            _core { std::move(core) },
            _ticket { ticket },
            _waiter { std::move(waiter) }
        {
        }

        /// Forgets the waiter and abandons the ticket, unless an `ADMIT` took both over.
        ~RequeuedHold() override
        {
            if (_waiter == nullptr)
                return;
            _core->Forget(_ticket);
            _core->Abandon(_ticket);
        }

        RequeuedHold(RequeuedHold const&) = delete;
        RequeuedHold& operator=(RequeuedHold const&) = delete;
        RequeuedHold(RequeuedHold&&) = delete;
        RequeuedHold& operator=(RequeuedHold&&) = delete;

        /// @copydoc IConnectionHold::SurvivesFrame
        [[nodiscard]] bool SurvivesFrame(std::uint8_t opRaw) const noexcept override
        {
            return opRaw == static_cast<std::uint8_t>(Wire::Op::Admit);
        }

        /// @copydoc IConnectionHold::IdleWindow
        /// A header's: the client asks again the moment its settle is answered, and a ticket at the
        /// head of the queue must not wait an hour for a client that went quiet.
        [[nodiscard]] std::chrono::milliseconds IdleWindow() const noexcept override
        {
            return FrameServer::HeaderTimeout;
        }

        /// @copydoc IConnectionHold::KeepAlive
        /// Never observable: a requeue follows a grant, which already armed this connection, and arming is
        /// once per connection. Answered like its sibling so the two holds cannot disagree.
        [[nodiscard]] std::optional<core::net::KeepAliveSettings> KeepAlive() const noexcept override
        {
            return AdmitKeepAliveProbe;
        }

        /// @return The ticket this holds.
        [[nodiscard]] Distributed::Ticket Ticket() const noexcept
        {
            return _ticket;
        }

        /// The `ADMIT` that came takes the waiter over, and with it the duty to abandon the ticket.
        /// @return The waiter.
        [[nodiscard]] std::shared_ptr<AdmissionWaiter> TakeWaiter() noexcept
        {
            return std::exchange(_waiter, nullptr);
        }

      private:
        std::shared_ptr<AdmissionCore> _core;
        Distributed::Ticket _ticket;
        std::shared_ptr<AdmissionWaiter> _waiter; ///< Null once an `ADMIT` took it.
    };

    /// The counters one grant moves, by what it granted.
    /// @param metrics The sink.
    /// @param verdict The grant.
    void CountGrant(IMetricsSink& metrics, Distributed::AdmissionVerdict const& verdict)
    {
        // A switch over the three kinds rather than a kind-to-counter table: `counter-attribution`
        // finds a writer by the `Increment(Counter::X)` it can read, and a table row is a write it
        // cannot see -- which would render these rows absent on a process that writes them.
        switch (verdict.kind)
        {
            case Wire::GrantKind::Worker:
                metrics.Increment(IMetricsSink::Counter::AdmissionGrantedWorker);
                break;
            case Wire::GrantKind::Local:
                metrics.Increment(IMetricsSink::Counter::AdmissionGrantedLocal);
                break;
            case Wire::GrantKind::Stored:
                metrics.Increment(IMetricsSink::Counter::AdmissionDeduplicated);
                break;
            case Wire::GrantKind::Last:
                break;
        }
        if (verdict.ranAlone)
            metrics.Increment(IMetricsSink::Counter::AdmissionRanAlone);
        // A `_sum`/`_count` pair, moved together: the wait since this request was last parked, so a
        // worker refusal's regrant does not add the wait before the refusal a second time.
        metrics.Increment(IMetricsSink::Counter::AdmissionWaitMillisTotal,
                          static_cast<std::uint64_t>(std::max<std::int64_t>(0, verdict.waited.count())));
        metrics.Increment(IMetricsSink::Counter::AdmissionWaitCount);
    }

    /// What a parked frame woke to find.
    enum class Woke : std::uint8_t
    {
        Verdict,  ///< The queue decided.
        Demoted,  ///< This node stopped leading.
        PeerGone, ///< The peer left while it waited.
    };

    /// Park until something is decided for @p waiter or the peer leaves.
    ///
    /// **No total ceiling**, which is the spec's and not an omission: a twenty-minute wait in the queue
    /// is the system working, and a ceiling that fails open brings back the OOM kills the queue exists
    /// to end. Liveness is the pulse's -- the endpoint renews the request's answer window with every
    /// pulse that reaches the peer (`ConnectionScope::JudgeByPulse`), and sweeps a peer that stops
    /// taking them -- and a departure wakes this through the peer-gone token.
    ///
    /// Suspends in the reactor own park (`interruptibleSleepUntil`), in STEPS of @p step rather than
    /// to one far deadline: the reactor's timer cannot be cancelled, so a step is all a woken park
    /// leaves behind on the wheel. A frame the reactor frees at teardown is freed with it, and the
    /// guards in the caller run.
    /// @param core The shared state.
    /// @param waiter What to wait on.
    /// @param scope The connection scope, whose peer-gone token also wakes this.
    /// @param step How long one park lasts before it re-reads.
    /// @return Which of the three ended the wait.
    core::async::Task<Woke> Park(std::shared_ptr<AdmissionCore> core,
                                 std::shared_ptr<AdmissionWaiter> waiter,
                                 std::shared_ptr<ConnectionScope> scope,
                                 std::chrono::milliseconds step)
    {
        // The peer leaving is a second reason to wake: it requests the very stop a decision does.
        core::async::StopCallback const onGone { scope->PeerGone(), [raw = waiter.get()] { raw->wake.request_stop(); } };
        while (true)
        {
            {
                auto const guard = std::scoped_lock { core->mutex };
                if (waiter->verdict.has_value())
                    co_return Woke::Verdict;
                if (waiter->demotedTo.has_value())
                    co_return Woke::Demoted;
            }
            if (scope->PeerGone().stop_requested())
                co_return Woke::PeerGone;
            // A step that ran out is not an answer: the loop re-reads and parks again.
            (void) co_await core::net::interruptibleSleepUntil(
                core->loop, waiter->wake.get_token(), core->loop->clock().now() + step);
        }
    }
    /// Take a put-back ticket off the connection, when that is what it holds.
    /// @param scope The connection's scope.
    /// @return The hold, or null when the connection holds none -- which is every `ADMIT` but the one
    ///         that follows a worker's refusal, since `BeforeFrame` released anything else.
    [[nodiscard]] std::unique_ptr<RequeuedHold> TakeRequeued(ConnectionScope& scope)
    {
        auto taken = scope.TakeHold();
        auto* const requeued = dynamic_cast<RequeuedHold*>(taken.get());
        if (requeued == nullptr)
            return nullptr; // Nothing else survives an `ADMIT`, so nothing else is here to put back.
        std::ignore = taken.release();
        return std::unique_ptr<RequeuedHold> { requeued };
    }

    /// Give a granted ticket back by its settle.
    ///
    /// `WorkerRefused` does not end the ticket (R7, Ruling 9): the service puts the SAME ticket back at
    /// the front with the refusal counted, and the connection keeps it as a `RequeuedHold` until its
    /// next `ADMIT` waits on it again. Its waiter is registered BEFORE the settle puts it back, because
    /// a pump on any thread may decide it at once and must find somebody to leave the decision with.
    /// @param core The shared state.
    /// @param service The scheduler.
    /// @param scope The connection's scope.
    /// @param hold The grant being settled, already taken off the connection.
    /// @param request The settle.
    /// @return The reply: `Ok`, or `NotLeader` naming the leader when a refusal reached a node that no
    ///         longer leads -- the ticket is abandoned then, since a follower's queue is pumped by nobody.
    [[nodiscard]] std::vector<std::byte> SettleHeld(std::shared_ptr<AdmissionCore> const& core,
                                                    Distributed::SchedulerService& service,
                                                    ConnectionScope& scope,
                                                    AdmissionHold& hold,
                                                    Wire::SettleRequest const& request)
    {
        auto const ticket = hold.Ticket();
        if (request.outcome == Wire::SettleOutcome::WorkerRefused)
        {
            auto waiter = std::make_shared<AdmissionWaiter>();
            {
                auto const guard = std::scoped_lock { core->mutex };
                core->waiters.insert_or_assign(ticket, waiter);
            }
            scope.Hold(std::make_unique<RequeuedHold>(core, ticket, std::move(waiter)));
            core->metrics->Increment(IMetricsSink::Counter::AdmissionWorkerRefused);
        }
        auto const settled = service.SettleAdmission(
            ticket, Distributed::Settlement { .outcome = request.outcome, .peakBytes = request.peakBytes });
        hold.Settled();
        if (settled == Distributed::SettleResult::NotLeading)
        {
            // Demoted between the grant and this settle: the service abandoned the ticket, so the
            // put-back hold goes too -- it forgets the waiter and files the abandonment -- and the
            // client is sent to the leader, where it asks again from the back of a queue that is pumped.
            (void) scope.TakeHold();
            return RefuseDemoted(service.LeaderEndpoint());
        }
        core->PumpAndDeliver();
        return Wire::EncodeReply(Wire::Status::Ok, {});
    }
} // namespace

AdmissionResponder::AdmissionResponder(Distributed::SchedulerService& service,
                                       Distributed::IMembershipOracle const& membership,
                                       core::net::EventLoop& loop,
                                       IMetricsSink& metrics,
                                       HeldChargeRegistry* held,
                                       std::chrono::milliseconds progressInterval):
    _core { std::make_shared<AdmissionCore>() }
{
    _core->service = &service;
    _core->membership = &membership;
    _core->loop = &loop;
    _core->metrics = &metrics;
    _core->held = held;
    _core->progressInterval = progressInterval;

    // Told when capacity may have appeared and when leadership is lost. The core is captured by
    // `shared_ptr`, so a callback running as this is destroyed still has something to call.
    service.ObserveAdmission(Distributed::AdmissionObserver {
        .changed = [core = _core] { core->PumpAndDeliver(); },
        .demoted = [core = _core](std::vector<Distributed::Ticket> const& lost,
                                  std::string const& leader) { core->Demote(lost, leader); } });
}

AdmissionResponder::~AdmissionResponder()
{
    // Stop being called first, then stop calling: a frame or a hold destroyed after this finds no
    // service and does nothing.
    if (auto* const service = _core->Service(); service != nullptr)
        service->ObserveAdmission({});
    auto const guard = std::scoped_lock { _core->mutex };
    _core->service = nullptr;
}

bool AdmissionResponder::Serves(std::uint8_t opRaw) noexcept
{
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Admit) || opRaw == static_cast<std::uint8_t>(Wire::Op::Settle)
           || opRaw == static_cast<std::uint8_t>(Wire::Op::MachineBudget);
}

std::optional<std::chrono::milliseconds> AdmissionResponder::ProgressInterval(std::uint8_t opRaw) const noexcept
{
    if (opRaw != static_cast<std::uint8_t>(Wire::Op::Admit))
        return std::nullopt;
    return _core->progressInterval;
}

std::optional<IMetricsSink::Counter> AdmissionResponder::PeerWatchCounter(std::uint8_t opRaw) const noexcept
{
    if (opRaw != static_cast<std::uint8_t>(Wire::Op::Admit))
        return std::nullopt;
    return IMetricsSink::Counter::AdmissionAbandoned;
}

std::chrono::milliseconds AdmissionResponder::RequestTimeout(std::uint8_t opRaw) const noexcept
{
    // A queue wait is as long as the compiles ahead of it, so it has no total ceiling: this window bounds
    // the SILENCE, and every pulse that reaches the peer renews it (`ConnectionScope::JudgeByPulse`).
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Admit) ? AdmissionIdleBound(_core->progressInterval)
                                                               : FrameServer::HeaderTimeout;
}

bool AdmissionResponder::Leads() const noexcept
{
    auto const* const service = _core->Service();
    return service != nullptr && service->Role() == Distributed::SchedulerRole::Leader;
}

bool AdmissionResponder::DecidesHere(PeerIdentity const& peer) const
{
    auto const* const service = _core->Service();
    // Detached: nothing to relay for either, and `Answer` closes on it.
    if (service == nullptr || service->Role() == Distributed::SchedulerRole::Leader)
        return true;
    auto const caller = Distributed::CallerContextOf(*_core->membership, peer);
    return caller.provenKey.has_value() && MachineIdOf(*caller.provenKey) == service->OwnMachineId();
}

core::async::Task<FrameReply> AdmissionResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    auto const core = _core;
    auto* const service = core->Service();
    auto const header = Wire::DecodeRequestHeader(frame);
    if (service == nullptr || !header.has_value() || frame.size() != Wire::RequestHeaderSize + header->payloadLength)
        co_return FrameReply {};
    auto const payload = frame.subspan(Wire::RequestHeaderSize);

    auto const scope = peer.scope;
    auto const loopback = Pace::IsLoopbackHost(peer.host);
    auto const facts = peer;
    auto const caller = Distributed::CallerContextOf(*core->membership, std::move(peer));

    // The machine a charge lands on: the key this connection PROVED, else this node's own for a caller on
    // this machine. Nothing a frame says, and nothing a machine ticket says either: a launcher asks the
    // queue only through its OWN node (Ruling 16), which proves its machine's key when it relays, so a
    // caller that reached here any other way identified no machine and is refused below.
    auto const machine = [&]() -> std::optional<Distributed::MachineId> {
        if (caller.provenKey.has_value())
            return MachineIdOf(*caller.provenKey);
        if (loopback)
            return service->OwnMachineId();
        return std::nullopt;
    }();

    if (auto refusal = service->RefuseUnlessIdentified(caller, static_cast<Wire::Op>(header->opRaw)); refusal.has_value())
        co_return FrameReply { EncodeServiceReply(*refusal) };

    switch (static_cast<Wire::Op>(header->opRaw))
    {
        case Wire::Op::MachineBudget: {
            auto const report = Wire::DecodeMachineBudgetPayload(payload);
            if (!report.has_value())
                co_return FrameReply { RefuseHere(Wire::ErrorCode::MalformedFrame, {}) };
            if (!caller.provenKey.has_value() || !machine.has_value())
                co_return FrameReply { RefuseHere(Wire::ErrorCode::DispatchNotPermitted,
                                                  "a machine budget names the machine that proved it") };
            auto const reply = service->ReportMachineBudget(caller, *machine, *report);
            // A machine that just reported may be the one a request was waiting for.
            core->PumpAndDeliver();
            co_return FrameReply { EncodeServiceReply(reply) };
        }
        case Wire::Op::Settle: {
            auto const request = Wire::DecodeSettlePayload(payload);
            if (!request.has_value())
                co_return FrameReply { RefuseHere(Wire::ErrorCode::MalformedFrame, {}) };
            // Taken whatever it holds: a settle answers the one grant this connection owns, and the
            // hold must not outlive the frame that settled it.
            auto taken = scope != nullptr ? scope->TakeHold() : nullptr;
            auto* const hold = dynamic_cast<AdmissionHold*>(taken.get());
            if (hold == nullptr)
                co_return FrameReply { RefuseHere(Wire::ErrorCode::UnknownLease,
                                                  "no admitted compile is held on this connection") };
            co_return FrameReply { SettleHeld(core, *service, *scope, *hold, *request) };
        }
        case Wire::Op::Admit:
            break;
        default:
            co_return FrameReply {};
    }

    // --- ADMIT ---
    // A request parks on its connection: with none to park on, it is refused rather than queued for a
    // peer that can neither be pulsed nor noticed leaving.
    if (scope == nullptr)
        co_return FrameReply { RefuseHere(Wire::ErrorCode::DispatchNotPermitted,
                                          "an admission waits on its connection, and this caller has none") };
    // Taken FIRST, so every refusal below gives a put-back ticket up as any other frame would.
    auto requeued = TakeRequeued(*scope);
    if (!machine.has_value())
        co_return FrameReply { RefuseHere(Wire::ErrorCode::DispatchNotPermitted,
                                          "an admission charges a machine, and this connection identified none") };
    auto const request = Wire::DecodeAdmitPayload(payload);
    if (!request.has_value())
        co_return FrameReply { RefuseHere(Wire::ErrorCode::MalformedFrame, {}) };

    // The liveness of this wait is its pulse, never its length (`Park`).
    scope->JudgeByPulse();

    // A second admission on a connection that holds a grant ends the first, and the next frame's own
    // `BeforeFrame` has already done it: only a settle survives that, so the only thing an `ADMIT` can
    // find held here is a put-back ticket, taken above.
    // The connection as it is when the grant comes, not as it is now: the facts are copied WITHOUT the
    // scope, which would keep the connection's hold reachable from the queue for as long as it waits.
    auto regateFacts = facts;
    regateFacts.scope = nullptr;
    auto const regate = [membership = core->membership, regateFacts = std::move(regateFacts)] {
        return Distributed::CallerContextOf(*membership, regateFacts);
    };
    // A connection whose ticket a worker refused asks again on the SAME ticket (Ruling 9): it is already
    // back at the front, its refusals counted, and its waiter registered since the settle -- so it is
    // neither enqueued nor counted queued a second time.
    auto ticket = Distributed::Ticket {};
    auto waiter = std::shared_ptr<AdmissionWaiter> {};
    if (requeued != nullptr)
    {
        // Its payload is deliberately NOT read: the ticket is bound to the connection (Ruling 9), so
        // what this ADMIT says it is for changes nothing -- it decoded, which is all it owes.
        //
        // And a node that stopped leading since the settle queues nothing: a follower is pumped by
        // nobody, so parking here would never end. `requeued` gives the ticket back on the way out.
        // Defence in depth -- `SettleHeld` refuses the settle itself on a demoted node.
        if (service->Role() != Distributed::SchedulerRole::Leader)
            co_return FrameReply { RefuseDemoted(service->LeaderEndpoint()) };
        ticket = requeued->Ticket();
        waiter = requeued->TakeWaiter();
    }
    else
    {
        auto const admitted = service->Admit(caller, regate, *machine, *request);
        if (!admitted.has_value())
            co_return FrameReply { EncodeServiceReply(admitted.error()) };
        ticket = *admitted;
        waiter = std::make_shared<AdmissionWaiter>();
        {
            auto const guard = std::scoped_lock { core->mutex };
            core->waiters.emplace(ticket, waiter);
        }
        core->metrics->Increment(IMetricsSink::Counter::AdmissionQueued);
    }
    Registration registration { core, ticket };

    // Registered BEFORE the first pump, so a decision made for this very request has somewhere to land.
    core->PumpAndDeliver();

    auto const woke = co_await Park(core, waiter, scope, core->progressInterval);

    switch (woke)
    {
        case Woke::Verdict:
            // Decided, and the peer left in the same breath: the charge is nobody's, so it goes back.
            if (scope->PeerGone().stop_requested())
                co_return FrameReply {};
            break;
        case Woke::Demoted:
            // The queue forgot the ticket at demotion: nothing to abandon, and the client is told
            // where to go. The same refusal `Gate` gives a request that arrives after it.
            registration.Done();
            co_return FrameReply { RefuseDemoted(waiter->demotedTo.value_or(std::string {})) };
        case Woke::PeerGone:
            // `Registration` gives the ticket back. An empty reply closes without writing: a peer that
            // left is not owed an answer.
            co_return FrameReply {};
    }

    auto verdict = std::optional<Distributed::AdmissionVerdict> {};
    {
        auto const guard = std::scoped_lock { core->mutex };
        verdict = std::move(waiter->verdict);
    }
    if (!verdict.has_value())
        co_return FrameReply {}; // Woken with no verdict: nothing was granted, so nothing is owed an answer.
    if (verdict->refusal.has_value())
    {
        // Re-gated and refused (R9): the service already gave the charge back.
        registration.Done();
        co_return FrameReply { EncodeServiceReply(*verdict->refusal) };
    }
    CountGrant(*core->metrics, *verdict);
    if (verdict->kind == Wire::GrantKind::Stored)
    {
        // Nothing was charged and nothing stays held: the client FETCHes what the other compile stored.
        registration.Done();
        co_return FrameReply { Wire::EncodeReply(Wire::Status::Ok, verdict->grant) };
    }

    // Granted. A loopback local lease is recorded in the machine own report for as long as the hold
    // lives (Ruling 4); the hold keeps the ticket and gives it back on the settle or the close.
    auto charge = HeldChargeRegistry::Handle {};
    if (verdict->kind == Wire::GrantKind::Local && loopback && core->held != nullptr)
        charge = core->held->Hold(verdict->chargedBytes);
    scope->Hold(std::make_unique<AdmissionHold>(core, *scope, ticket, std::move(charge)));
    registration.Done();
    co_return FrameReply { Wire::EncodeReply(Wire::Status::Ok, verdict->grant) };
}

} // namespace Pace::Node
