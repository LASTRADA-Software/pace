// SPDX-License-Identifier: Apache-2.0
#include "AdmissionResponder.hpp"

#include <Pace/Core/HostPort.hpp>
#include <Pace/Protocol/ConnectionScope.hpp>
#include <Pace/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
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
    void Abandon(Distributed::Ticket ticket)
    {
        auto* const svc = Service();
        if (svc == nullptr)
            return;
        svc->AbandonAdmission(ticket);
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
        /// @param ticket The granted ticket.
        /// @param charge The machine own record of a loopback local lease; empty for any other.
        AdmissionHold(std::shared_ptr<AdmissionCore> core, Distributed::Ticket ticket, HeldChargeRegistry::Handle charge):
            _core { std::move(core) },
            _ticket { ticket },
            _charge { std::move(charge) }
        {
        }

        /// Abandons the ticket unless a settle took it: the connection ended, or another frame came.
        ~AdmissionHold() override
        {
            if (!_settled)
                _core->Abandon(_ticket);
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
        Distributed::Ticket _ticket;
        HeldChargeRegistry::Handle _charge;
        bool _settled { false };
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
        metrics.Increment(IMetricsSink::Counter::AdmissionWaitMillisTotal,
                          static_cast<std::uint64_t>(std::max<std::int64_t>(0, verdict.queued.count())));
    }

    /// What a parked frame woke to find.
    enum class Woke : std::uint8_t
    {
        Verdict,  ///< The queue decided.
        Demoted,  ///< This node stopped leading.
        PeerGone, ///< The peer left while it waited.
        TimedOut, ///< The admission window ran out.
    };

    /// Park until something is decided for @p waiter, the peer leaves, or the window ends.
    ///
    /// Suspends in the reactor own park (`interruptibleSleepUntil`); a frame the reactor frees at
    /// teardown is freed with it, and the guards in the caller run.
    /// @param core The shared state.
    /// @param waiter What to wait on.
    /// @param scope The connection scope, whose peer-gone token also wakes this.
    /// @param window How long to wait at most.
    /// @return Which of the four ended the wait.
    core::async::Task<Woke> Park(std::shared_ptr<AdmissionCore> core,
                                 std::shared_ptr<AdmissionWaiter> waiter,
                                 std::shared_ptr<ConnectionScope> scope,
                                 std::chrono::milliseconds window)
    {
        // The peer leaving is a second reason to wake: it requests the very stop a decision does.
        core::async::StopCallback const onGone { scope->PeerGone(), [raw = waiter.get()] { raw->wake.request_stop(); } };
        auto const deadline = core->loop->clock().now() + window;
        for (;;)
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
            if (co_await core::net::interruptibleSleepUntil(core->loop, waiter->wake.get_token(), deadline)
                == core::net::WakeReason::Deadline)
                co_return Woke::TimedOut;
        }
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
    // A queue wait is as long as the compiles ahead of it: the ceiling a lease may live is the longest
    // honest one, and the endpoint sweeps a connection past it.
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Admit) ? Wire::MaxCompileLeaseLifetime : FrameServer::HeaderTimeout;
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

    // The machine a charge lands on: the key this connection PROVED, else the one its ticket named, else
    // this node's own for a caller on this machine. Nothing a frame says.
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
            if (!caller.provenKey.has_value())
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
            service->SettleAdmission(
                hold->Ticket(), Distributed::Settlement { .outcome = request->outcome, .peakBytes = request->peakBytes });
            hold->Settled();
            if (request->outcome == Wire::SettleOutcome::WorkerRefused)
                core->metrics->Increment(IMetricsSink::Counter::AdmissionWorkerRefused);
            core->PumpAndDeliver();
            co_return FrameReply { Wire::EncodeReply(Wire::Status::Ok, {}) };
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
    if (!machine.has_value())
        co_return FrameReply { RefuseHere(Wire::ErrorCode::DispatchNotPermitted,
                                          "an admission charges a machine, and this connection identified none") };
    auto const request = Wire::DecodeAdmitPayload(payload);
    if (!request.has_value())
        co_return FrameReply { RefuseHere(Wire::ErrorCode::MalformedFrame, {}) };

    // A second admission on a connection that holds one ends the first, and the next frame's own
    // `BeforeFrame` has already done it: only a settle survives, so there is nothing held here.
    // The connection as it is when the grant comes, not as it is now: the facts are copied WITHOUT the
    // scope, which would keep the connection's hold reachable from the queue for as long as it waits.
    auto regateFacts = facts;
    regateFacts.scope = nullptr;
    auto const regate = [membership = core->membership, regateFacts = std::move(regateFacts)] {
        return Distributed::CallerContextOf(*membership, regateFacts);
    };
    auto const ticket = service->Admit(caller, regate, *machine, *request);
    if (!ticket.has_value())
        co_return FrameReply { EncodeServiceReply(ticket.error()) };

    auto waiter = std::make_shared<AdmissionWaiter>();
    {
        auto const guard = std::scoped_lock { core->mutex };
        core->waiters.emplace(*ticket, waiter);
    }
    Registration registration { core, *ticket };
    core->metrics->Increment(IMetricsSink::Counter::AdmissionQueued);

    // Registered BEFORE the first pump, so a decision made for this very request has somewhere to land.
    core->PumpAndDeliver();

    auto const woke = co_await Park(core, waiter, scope, Wire::MaxCompileLeaseLifetime);

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
            co_return FrameReply { EncodeServiceReply(
                Distributed::SchedulerReply { .status = Wire::Status::Error,
                                              .error = Wire::ErrorCode::NotLeader,
                                              .message = waiter->demotedTo.value_or(std::string {}),
                                              .payload = {} }) };
        case Woke::PeerGone:
        case Woke::TimedOut:
            // `Registration` gives the ticket back. An empty reply closes without writing: a peer that
            // left is not owed an answer, and one that waited past the window is closed by the sweep.
            co_return FrameReply {};
    }

    auto verdict = std::optional<Distributed::AdmissionVerdict> {};
    {
        auto const guard = std::scoped_lock { core->mutex };
        verdict = std::move(waiter->verdict);
    }
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
    scope->Hold(std::make_unique<AdmissionHold>(core, *ticket, std::move(charge)));
    registration.Done();
    co_return FrameReply { Wire::EncodeReply(Wire::Status::Ok, verdict->grant) };
}

} // namespace Pace::Node
