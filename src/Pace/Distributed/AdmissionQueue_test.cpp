// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/AdmissionQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <ranges>

#include <core/platform/Clock.hpp>
#include <tests/Unwrap.hpp>

using Pace::Testing::Unwrap;

using namespace Pace::Distributed;

namespace
{
constexpr std::uint64_t GiB = 1ULL << 30;

/// Returns what the test scripted per fingerprint, ignoring `excluded`.
class FakePlacement final: public IPlacementSource
{
  public:
    std::map<std::string, std::vector<Placement>, std::less<>> byFingerprint;

    [[nodiscard]] std::vector<Placement> Candidates(std::string_view fingerprint,
                                                    std::span<std::string const> /*excluded*/) const override
    {
        auto const found = byFingerprint.find(fingerprint);
        return found == byFingerprint.end() ? std::vector<Placement> {} : found->second;
    }
};

struct Fixture
{
    core::platform::ManualClock clock;
    PeakEstimates estimates { 3 * GiB, 64 };
    FakePlacement placement;
    AdmissionQueue queue { clock, estimates, placement };

    /// Enqueues a request of `bytes`, the figure being taught to the estimator under its own key.
    Ticket Add(AdmitKind kind, std::string caller, std::uint64_t bytes, std::string fingerprint = {})
    {
        auto key = EstimateKey { .cacheKey = "key-" + std::to_string(++serial),
                                 .sourcePath = "a.cpp",
                                 .compilerIdentity = fingerprint };
        estimates.Record(key, bytes);
        QueueRequest request;
        request.kind = kind;
        request.caller = std::move(caller);
        request.fingerprint = std::move(fingerprint);
        request.estimate = std::move(key);
        return queue.Enqueue(std::move(request));
    }

    Ticket Local(std::uint64_t bytes, std::string caller = "m")
    {
        return Add(AdmitKind::LocalOnly, std::move(caller), bytes);
    }

    /// Enqueues `count` local-only requests of `bytes` each.
    std::vector<Ticket> LocalMany(int count, std::uint64_t bytes, std::string const& caller = "m")
    {
        std::vector<Ticket> tickets;
        tickets.reserve(static_cast<std::size_t>(count));
        for ([[maybe_unused]] auto const _: std::views::iota(0, count))
            tickets.push_back(Local(bytes, caller));
        return tickets;
    }

    /// Enqueues a request whose estimate key is `cacheKey`, so two of them collide in the dedup set.
    Ticket Keyed(AdmitKind kind, std::string caller, std::string cacheKey, bool storable, std::uint64_t bytes)
    {
        auto key = EstimateKey { .cacheKey = std::move(cacheKey), .sourcePath = "a.cpp", .compilerIdentity = {} };
        estimates.Record(key, bytes);
        QueueRequest request;
        request.kind = kind;
        request.caller = std::move(caller);
        request.estimate = std::move(key);
        request.storable = storable;
        return queue.Enqueue(std::move(request));
    }

    /// Enqueues a request for the compile keyed @p cacheKey, asked for with @p origin, and nothing taught: the
    /// estimate is whatever the queue has learned of that key -- a kill included.
    Ticket Ask(
        AdmitKind kind, std::string caller, std::string cacheKey, AskOrigin origin, std::string const& fingerprint = "fp")
    {
        QueueRequest request;
        request.kind = kind;
        request.caller = std::move(caller);
        request.fingerprint = fingerprint;
        request.estimate =
            EstimateKey { .cacheKey = std::move(cacheKey), .sourcePath = "big.cpp", .compilerIdentity = fingerprint };
        request.storable = false;
        request.origin = origin;
        return queue.Enqueue(std::move(request));
    }

    int serial {}; ///< Numbers the keys `Add` mints.
};

constexpr Settlement Done { .outcome = SettleOutcome::NotStored, .peakBytes = std::nullopt };

MachineReport Report(std::uint64_t budget, std::uint32_t slots, std::uint32_t heldSlots = 0, std::uint64_t heldBytes = 0)
{
    return MachineReport { .memoryBudgetBytes = budget,
                           .slots = slots,
                           .heldSlots = heldSlots,
                           .heldBytes = heldBytes,
                           .pressure = PressureLevel::Calm };
}

/// An idle machine's report at @p pressure.
MachineReport Pressed(std::uint64_t budget, std::uint32_t slots, PressureLevel pressure)
{
    auto report = Report(budget, slots);
    report.pressure = pressure;
    return report;
}

std::vector<Ticket> Tickets(std::vector<Decision> const& decisions)
{
    std::vector<Ticket> result;
    result.reserve(decisions.size());
    for (auto const& decision: decisions)
        result.push_back(decision.ticket);
    return result;
}
} // namespace

TEST_CASE("AdmissionQueue.unreported-machine-admits-nothing", "[distributed][admission]")
{
    Fixture f;
    f.Local(1 * GiB);
    CHECK(f.queue.Pump().empty());
    CHECK(f.queue.Depth() == 1);

    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Local);
    CHECK(f.queue.Depth() == 0);
}

TEST_CASE("AdmissionQueue.memory-bounds-admission", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 32));
    f.LocalMany(4, 3 * GiB);
    CHECK(f.queue.Pump().size() == 2);
    CHECK(f.queue.Depth() == 2);
}

TEST_CASE("AdmissionQueue.slots-bound-admission", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(64 * GiB, 2));
    f.LocalMany(3, 3 * GiB);
    CHECK(f.queue.Pump().size() == 2);
    CHECK(f.queue.Depth() == 1);
}

TEST_CASE("AdmissionQueue.held-charges-count", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4, 1, 6 * GiB));
    f.Local(3 * GiB);
    CHECK(f.queue.Pump().empty());
    CHECK(f.queue.Depth() == 1);
}

TEST_CASE("AdmissionQueue.held-and-charged-not-double-counted", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    f.Local(3 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);

    // The machine now reports the very compile the ledger charged.
    f.queue.ReportMachine("m", Report(8 * GiB, 4, 1, 3 * GiB));
    f.Local(4 * GiB);
    CHECK(f.queue.Pump().size() == 1); // 8 - max(3, 3) = 5 free; summing would leave 2

    // And a report alone counts when the ledger knows nothing (failover): 5 GiB held, 3 free.
    Fixture fresh;
    fresh.queue.ReportMachine("m", Report(8 * GiB, 4, 1, 5 * GiB));
    fresh.Local(4 * GiB);
    CHECK(fresh.queue.Pump().empty());
}

TEST_CASE("AdmissionQueue.backfill-behind-large-head", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 32));
    f.Local(4 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);

    auto const head = f.Local(6 * GiB);
    auto const a = f.Local(1 * GiB);
    auto const b = f.Local(1 * GiB);
    auto const c = f.Local(1 * GiB);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { a, b, c });
    CHECK(f.queue.Depth() == 1);
    for (auto const ticket: Tickets(f.queue.Pump()))
        CHECK(ticket != head);
}

TEST_CASE("AdmissionQueue.head-reservation-prevents-starvation", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 32));
    auto const holder = f.Local(4 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);
    auto const head = f.Local(6 * GiB);
    auto const smalls = f.LocalMany(3, 1 * GiB);
    REQUIRE(f.queue.Pump().size() == 3);

    f.queue.Settle(holder, Done); // 3 GiB in use, 5 free, head needs 6
    auto const late = f.Local(1 * GiB);
    CHECK(f.queue.Pump().empty()); // the free 5 GiB is the head's, not the latecomer's

    for (auto const small: smalls)
        f.queue.Settle(small, Done);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { head, late }); // head first, then the 1 GiB fits too
}

TEST_CASE("AdmissionQueue.over-budget-runs-alone", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4, 1, 1 * GiB));
    auto const big = f.Local(12 * GiB);
    CHECK(f.queue.Pump().empty()); // the machine is not idle

    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == big);
    CHECK(granted[0].ranAlone);

    f.Local(1 * GiB);
    CHECK(f.queue.Pump().empty()); // nothing else while it runs
    CHECK(f.queue.Depth() == 1);
}

TEST_CASE("AdmissionQueue.over-budget-head-is-not-starved-by-backfill", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4, 1, 2 * GiB));
    auto const big = f.Local(12 * GiB);
    REQUIRE(f.queue.Pump().empty()); // becomes head, the machine is busy

    f.LocalMany(3, 1 * GiB);
    f.queue.ReportMachine("m", Report(8 * GiB, 4, 1, 1 * GiB)); // memory is freed while it waits
    CHECK(f.queue.Pump().empty());                              // ...and is the head's, not the small ones'
    CHECK(f.queue.Depth() == 4);

    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == big);
    CHECK(granted[0].ranAlone);
    CHECK(f.queue.Depth() == 3);
}

TEST_CASE("AdmissionQueue.fifo-among-equals", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(3 * GiB, 4));
    auto const first = f.Local(3 * GiB);
    auto const second = f.Local(3 * GiB);
    auto const third = f.Local(3 * GiB);

    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { first });
    f.queue.Settle(first, Done);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { second });
    f.queue.Settle(second, Done);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { third });
}

TEST_CASE("AdmissionQueue.dispatchable-uses-placement", "[distributed][admission]")
{
    Fixture f;
    Placement const worker { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" };
    f.placement.byFingerprint["fp"] = { worker };
    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    f.queue.ReportMachine("c", Report(8 * GiB, 4));

    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "fp");
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Worker);
    REQUIRE(granted[0].placement.has_value());
    CHECK(Unwrap(granted[0].placement).workerId == worker.workerId);
    CHECK(Unwrap(granted[0].placement).machine == "w");
    CHECK(Unwrap(granted[0].placement).endpoint == worker.endpoint);

    // Machine w is charged: a second 5 GiB request does not fit there, and runs on the caller
    // (which has room) rather than waiting; a third fits nowhere.
    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "fp");
    auto const second = f.queue.Pump();
    REQUIRE(second.size() == 1);
    CHECK(second[0].kind == Pace::CompileCacheWire::GrantKind::Local);
    CHECK_FALSE(second[0].placement.has_value());
    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "fp");
    CHECK(f.queue.Pump().empty());
}

TEST_CASE("AdmissionQueue.dispatchable-with-full-candidates-runs-on-caller-with-room", "[distributed][admission]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("w", Report(8 * GiB, 4, 4, 8 * GiB));
    f.queue.ReportMachine("c", Report(8 * GiB, 4));
    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "fp");
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Local);
}

TEST_CASE("AdmissionQueue.dispatchable-head-reserves-across-changing-candidate-order", "[distributed][admission]")
{
    Fixture f;
    Placement const w1 { .workerId = "worker-1", .machine = "w1", .endpoint = "10.0.0.2:6680" };
    Placement const w2 { .workerId = "worker-2", .machine = "w2", .endpoint = "10.0.0.3:6680" };
    f.placement.byFingerprint["fp"] = { w1, w2 };
    f.queue.ReportMachine("w1", Report(8 * GiB, 32));
    f.queue.ReportMachine("w2", Report(8 * GiB, 32));
    f.queue.ReportMachine("c", Report(8 * GiB, 4, 4, 8 * GiB)); // the caller is full
    // Each worker machine holds 4 GiB and 2 GiB: 6 GiB in use, so a 6 GiB head fits nowhere.
    auto const big1 = f.Local(4 * GiB, "w1");
    auto const big2 = f.Local(4 * GiB, "w2");
    auto const small1 = f.Local(2 * GiB, "w1");
    auto const small2 = f.Local(2 * GiB, "w2");
    REQUIRE(f.queue.Pump().size() == 4);

    auto const head = f.Add(AdmitKind::Dispatchable, "c", 6 * GiB, "fp");
    REQUIRE(f.queue.Pump().empty()); // it becomes the head

    // The candidate order flips and the 2 GiB holders finish: 4 GiB is free on both machines, and
    // the head (needing 6) must keep it from the latecomers on BOTH, not only the first candidate.
    f.placement.byFingerprint["fp"] = { w2, w1 };
    f.queue.Settle(small1, Done);
    f.queue.Settle(small2, Done);
    f.Local(1 * GiB, "w1");
    f.Local(1 * GiB, "w2");
    CHECK(f.queue.Pump().empty());

    f.queue.Settle(big1, Done);
    f.queue.Settle(big2, Done);
    auto const granted = f.queue.Pump();
    REQUIRE_FALSE(granted.empty());
    CHECK(granted.front().ticket == head);
    CHECK(granted.front().kind == Pace::CompileCacheWire::GrantKind::Worker);
}

TEST_CASE("AdmissionQueue.dispatchable-without-candidate-runs-local", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("c", Report(8 * GiB, 4));
    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "unknown");
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Local);
    CHECK_FALSE(granted[0].placement.has_value());

    // Charged to the caller: another 5 GiB does not fit there.
    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "unknown");
    CHECK(f.queue.Pump().empty());
}

using Pace::CompileCacheWire::GrantKind;

TEST_CASE("AdmissionQueue.dedup-waiter-woken-by-store", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const first = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const second = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { first });

    f.queue.Settle(first, Settlement { .outcome = SettleOutcome::Stored, .peakBytes = std::nullopt });
    auto const woken = f.queue.Pump();
    REQUIRE(woken.size() == 1);
    CHECK(woken[0].ticket == second);
    CHECK(woken[0].kind == GrantKind::Stored);
    CHECK(f.queue.Depth() == 0);
    CHECK(f.queue.Pump().empty());
}

TEST_CASE("AdmissionQueue.dedup-waiter-holds-no-charge", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 3 * GiB);
    f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 3 * GiB);
    f.Local(1 * GiB);
    CHECK(f.queue.Pump().size() == 2); // the owner and the 1 GiB one; the waiter took nothing
    CHECK(f.queue.Depth() == 0);
}

TEST_CASE("AdmissionQueue.dedup-not-woken-by-not-stored", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const first = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const second = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);

    f.queue.Settle(first, Settlement { .outcome = SettleOutcome::NotStored, .peakBytes = std::nullopt });
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == second);
    CHECK(granted[0].kind == GrantKind::Local);
}

TEST_CASE("AdmissionQueue.dedup-failed-owner-requeues-waiters", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const first = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const second = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);

    f.queue.Settle(first, Settlement { .outcome = SettleOutcome::Failed, .peakBytes = std::nullopt });
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == second);
    CHECK(granted[0].kind == GrantKind::Local);
}

TEST_CASE("AdmissionQueue.dedup-owner-abandon-requeues-waiters", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const first = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const second = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);

    f.queue.Abandon(first);
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == second);
    CHECK(granted[0].kind == GrantKind::Local);
}

TEST_CASE("AdmissionQueue.requeued-waiter-keeps-fifo-position", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    auto const first = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 4 * GiB);
    auto const waiter = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 4 * GiB);
    auto const later = f.Local(4 * GiB);
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { first });

    f.queue.Settle(first, Settlement { .outcome = SettleOutcome::NotStored, .peakBytes = std::nullopt });
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { waiter }); // not `later`, which is younger
    f.queue.Settle(waiter, Done);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { later });
}

TEST_CASE("AdmissionQueue.abandoned-dedup-waiter-is-not-woken", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const first = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const second = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);

    f.queue.Abandon(second);
    f.queue.Settle(first, Settlement { .outcome = SettleOutcome::Stored, .peakBytes = std::nullopt });
    CHECK(f.queue.Pump().empty());
}

TEST_CASE("AdmissionQueue.non-storable-not-deduplicated", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    f.Keyed(AdmitKind::LocalOnly, "m", "k", false, 1 * GiB);
    f.Keyed(AdmitKind::LocalOnly, "m", "k", false, 1 * GiB);
    CHECK(f.queue.Pump().size() == 2);
}

TEST_CASE("AdmissionQueue.dedup-only-same-cache", "[distributed][admission]")
{
    {
        Fixture f;
        f.queue.ReportMachine("a", Report(8 * GiB, 4));
        f.queue.ReportMachine("b", Report(8 * GiB, 4));
        f.Keyed(AdmitKind::LocalOnly, "a", "k", true, 1 * GiB);
        f.Keyed(AdmitKind::LocalOnly, "b", "k", true, 1 * GiB);
        CHECK(f.queue.Pump().size() == 2); // default: only the same machine shares a cache
    }

    core::platform::ManualClock clock;
    PeakEstimates estimates { 3 * GiB, 64 };
    FakePlacement placement;
    AdmissionQueue queue { clock, estimates, placement, [](MachineId const&, MachineId const&) { return true; } };
    queue.ReportMachine("a", Report(8 * GiB, 4));
    queue.ReportMachine("b", Report(8 * GiB, 4));
    for (auto const* machine: { "a", "b" })
    {
        QueueRequest request;
        request.kind = AdmitKind::LocalOnly;
        request.caller = machine;
        request.estimate = EstimateKey { .cacheKey = "k", .sourcePath = "a.cpp", .compilerIdentity = {} };
        static_cast<void>(queue.Enqueue(std::move(request)));
    }
    CHECK(queue.Pump().size() == 1);
}

TEST_CASE("AdmissionQueue.abandon-while-queued", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    auto const holder = f.Local(4 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);
    auto const head = f.Local(4 * GiB);
    REQUIRE(f.queue.Pump().empty()); // becomes head
    REQUIRE(f.queue.Pump().empty()); // reserving
    f.queue.Abandon(head);
    CHECK(f.queue.Depth() == 0);

    f.queue.Settle(holder, Done);
    auto const next = f.Local(4 * GiB); // the whole budget, no reservation left behind
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { next });
}

TEST_CASE("AdmissionQueue.abandon-granted-releases-charge", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    auto key = EstimateKey { .cacheKey = "key-x", .sourcePath = "a.cpp", .compilerIdentity = {} };
    f.estimates.Record(key, 1 * GiB);
    QueueRequest request;
    request.kind = AdmitKind::LocalOnly;
    request.caller = "m";
    request.estimate = key;
    auto const ticket = f.queue.Enqueue(request);
    REQUIRE(f.queue.Pump().size() == 1);
    REQUIRE(f.estimates.Estimate(key) == 1 * GiB);

    f.queue.Abandon(ticket);
    CHECK(f.estimates.Estimate(key) == 3 * GiB); // RecordKilled raised it to the default

    f.Local(4 * GiB);
    CHECK(f.queue.Pump().size() == 1); // the charge went back
}

TEST_CASE("AdmissionQueue.abandon-queued-records-nothing", "[distributed][admission]")
{
    Fixture f;
    auto key = EstimateKey { .cacheKey = "key-y", .sourcePath = "a.cpp", .compilerIdentity = {} };
    f.estimates.Record(key, 1 * GiB);
    QueueRequest request;
    request.kind = AdmitKind::LocalOnly;
    request.caller = "m";
    request.estimate = key;
    auto const ticket = f.queue.Enqueue(request);
    f.queue.Abandon(ticket);
    CHECK(f.estimates.Estimate(key) == 1 * GiB);
}

TEST_CASE("AdmissionQueue.settle-records-peak", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto key = EstimateKey { .cacheKey = "key-z", .sourcePath = "a.cpp", .compilerIdentity = {} };
    f.estimates.Record(key, 1 * GiB);
    QueueRequest request;
    request.kind = AdmitKind::LocalOnly;
    request.caller = "m";
    request.estimate = key;
    auto const ticket = f.queue.Enqueue(request);
    REQUIRE(f.queue.Pump().size() == 1);

    f.queue.Settle(ticket, Settlement { .outcome = SettleOutcome::Stored, .peakBytes = 3 * GiB });
    CHECK(f.estimates.Estimate(key) == 3 * GiB);
}

TEST_CASE("AdmissionQueue.worker-refusal-reparks-at-head", "[distributed][admission]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    f.queue.ReportMachine("c", Report(8 * GiB, 4, 4, 8 * GiB)); // the caller is full
    auto const ticket = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    REQUIRE(granted[0].kind == GrantKind::Worker);
    auto const younger = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");

    f.queue.Settle(ticket, Settlement { .outcome = SettleOutcome::WorkerRefused, .peakBytes = std::nullopt });
    CHECK(f.queue.Depth() == 2);
    CHECK(f.queue.Pump().empty()); // `w` admits nothing until it reports again

    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    auto const again = f.queue.Pump();
    REQUIRE(again.size() == 2);
    CHECK(again[0].ticket == ticket); // first in the queue
    CHECK(again[1].ticket == younger);
}

TEST_CASE("AdmissionQueue.worker-refusals-bounded", "[distributed][admission]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("c", Report(8 * GiB, 4));
    auto const ticket = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    for ([[maybe_unused]] auto const round: std::views::iota(0U, AdmissionQueue::MaxWorkerRefusals))
    {
        f.queue.ReportMachine("w", Report(8 * GiB, 4));
        auto const granted = f.queue.Pump();
        REQUIRE(granted.size() == 1);
        REQUIRE(granted[0].kind == GrantKind::Worker);
        f.queue.Settle(ticket, Settlement { .outcome = SettleOutcome::WorkerRefused, .peakBytes = std::nullopt });
    }
    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    auto const last = f.queue.Pump();
    REQUIRE(last.size() == 1);
    CHECK(last[0].kind == GrantKind::Local);
    CHECK_FALSE(last[0].placement.has_value());
}

TEST_CASE("AdmissionQueue.returned-grant-goes-to-the-front-uncounted", "[distributed][admission]")
{
    // A grant whose worker left before it could be handed over: back at the front, its charge freed,
    // and NOT a refusal -- the machine is not marked refused and the request keeps its whole bound.
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("w", Report(8 * GiB, 1));
    f.queue.ReportMachine("c", Report(8 * GiB, 4, 4, 8 * GiB)); // the caller is full
    auto const ticket = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    REQUIRE(f.queue.Pump().size() == 1);
    auto const younger = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    REQUIRE(f.queue.Pump().empty()); // the one slot is taken

    f.queue.ReturnToFront(ticket);
    CHECK(f.queue.Depth() == 2);
    CHECK(f.queue.ChargedByMachine().empty());

    // No report in between: a refusal would have kept `w` shut until one. And ahead of the younger one.
    auto const again = f.queue.Pump();
    REQUIRE(again.size() == 1);
    CHECK(again[0].ticket == ticket);
    CHECK(again[0].kind == GrantKind::Worker);

    // The bound is whole: two refusals later it still goes to a worker, which a counted return would not.
    for ([[maybe_unused]] auto const round: std::views::iota(1U, AdmissionQueue::MaxWorkerRefusals))
    {
        f.queue.Settle(ticket, Settlement { .outcome = SettleOutcome::WorkerRefused, .peakBytes = std::nullopt });
        f.queue.ReportMachine("w", Report(8 * GiB, 1));
        auto const granted = f.queue.Pump();
        REQUIRE(granted.size() == 1);
        CHECK(granted[0].ticket == ticket);
        CHECK(granted[0].kind == GrantKind::Worker);
    }
    CHECK(f.queue.Depth() == 1);
    CHECK(Tickets(f.queue.Pump()).empty());
    (void) younger;
}

TEST_CASE("AdmissionQueue.leadership-loss", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    auto const owner = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const waiter = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const queued = f.Local(4 * GiB);
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { owner });
    REQUIRE(f.queue.Depth() == 1);

    auto const parked = f.queue.LoseLeadership();
    CHECK(parked == std::vector<Ticket> { waiter, queued });
    CHECK(f.queue.Depth() == 0);

    f.Local(1 * GiB);
    CHECK(f.queue.Pump().empty()); // 3 GiB would be free, but the machine is unreported

    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    CHECK(f.queue.Pump().size() == 1);

    // The granted ticket is still tracked, so its settle releases cleanly: 1 + 1 GiB charged, one given back.
    f.queue.Settle(owner, Done);
    f.Local(3 * GiB);
    CHECK(f.queue.Pump().size() == 1);
}

TEST_CASE("AdmissionQueue.requeued-waiter-stays-behind-refused-front", "[distributed][admission]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("m", Report(4 * GiB, 4));
    f.queue.ReportMachine("w", Report(4 * GiB, 4));
    f.queue.ReportMachine("c", Report(4 * GiB, 4, 4, 4 * GiB)); // the caller is full
    auto const owner = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 4 * GiB);
    auto const older = f.Local(4 * GiB, "m");
    auto const waiter = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 4 * GiB);
    auto const younger = f.Local(4 * GiB, "m");
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { owner });
    auto const refused = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp"); // a higher ticket than all of them
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { refused });

    f.queue.Settle(
        refused,
        Settlement { .outcome = SettleOutcome::WorkerRefused, .peakBytes = std::nullopt }); // queue: refused, older, younger
    f.queue.Settle(owner,
                   Settlement { .outcome = SettleOutcome::NotStored, .peakBytes = std::nullopt }); // the waiter rejoins
    f.queue.ReportMachine("w", Report(4 * GiB, 4));
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { refused, older }); // the refused ticket is still first

    f.queue.Settle(older, Done);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { waiter }); // between `older` and `younger`
    f.queue.Settle(waiter, Done);
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { younger });
}

TEST_CASE("AdmissionQueue.refused-machine-admits-no-local-request-until-reported", "[distributed][admission]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    f.queue.ReportMachine("c", Report(8 * GiB, 4, 4, 8 * GiB));
    auto const job = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { job });
    auto const local = f.Local(1 * GiB, "w"); // its caller is the machine about to refuse

    f.queue.Settle(job, Settlement { .outcome = SettleOutcome::WorkerRefused, .peakBytes = std::nullopt });
    CHECK(f.queue.Pump().empty());

    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    auto const granted = Tickets(f.queue.Pump());
    CHECK(std::ranges::find(granted, local) != granted.end());
}

TEST_CASE("AdmissionQueue.woken-waiter-is-parked-by-leadership-loss", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const owner = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const waiter = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { owner });
    f.queue.Settle(owner, Settlement { .outcome = SettleOutcome::Stored, .peakBytes = std::nullopt });

    CHECK(f.queue.LoseLeadership() == std::vector<Ticket> { waiter });
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    CHECK(f.queue.Pump().empty()); // nothing is delivered after the loss
}

TEST_CASE("AdmissionQueue.woken-waiter-abandoned-before-pump-is-not-delivered", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const owner = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const waiter = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    auto const other = f.Keyed(AdmitKind::LocalOnly, "m", "k", true, 1 * GiB);
    REQUIRE(Tickets(f.queue.Pump()) == std::vector<Ticket> { owner });
    f.queue.Settle(owner, Settlement { .outcome = SettleOutcome::Stored, .peakBytes = std::nullopt });

    f.queue.Abandon(waiter);
    auto const delivered = f.queue.Pump();
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0].ticket == other);
    CHECK(delivered[0].kind == GrantKind::Stored);
}

TEST_CASE("AdmissionQueue.not-stored-without-a-peak-teaches-the-estimator-nothing", "[distributed][admission]")
{
    // What a launcher settles when the worker its grant named failed the compile: the charge is freed,
    // and neither the key's estimate nor its shape's moves -- unlike the abandonment it replaces, which
    // reads as a KILLED compile and raises both to the default.
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    auto const key = EstimateKey { .cacheKey = "key-w", .sourcePath = "w.cpp", .compilerIdentity = "gcc-14" };
    auto const sameShape = EstimateKey { .cacheKey = "key-never-seen", .sourcePath = "w.cpp", .compilerIdentity = "gcc-14" };
    f.estimates.Record(key, 1 * GiB);
    QueueRequest request;
    request.kind = AdmitKind::LocalOnly;
    request.caller = "m";
    request.fingerprint = "gcc-14";
    request.estimate = key;
    auto const ticket = f.queue.Enqueue(request);
    REQUIRE(f.queue.Pump().size() == 1);
    auto expected = std::uint64_t { 0 };

    SECTION("settled NotStored with no peak")
    {
        f.queue.Settle(ticket, Settlement { .outcome = SettleOutcome::NotStored, .peakBytes = std::nullopt });
        expected = 1 * GiB;
    }
    SECTION("control: abandoned")
    {
        f.queue.Abandon(ticket);
        expected = 3 * GiB;
    }

    CHECK(f.estimates.Estimate(key) == expected);
    CHECK(f.estimates.Estimate(sameShape) == expected);
    // Either way the charge went back.
    f.Local(8 * GiB);
    CHECK(f.queue.Pump().size() == 1);
}

TEST_CASE("AdmissionQueue.snapshot-names-who-runs-and-who-waits", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 32));
    auto const enqueue = [&f](std::string caller, std::string tree, std::uint64_t bytes) {
        auto key = EstimateKey { .cacheKey = "key-" + caller + tree + std::to_string(bytes),
                                 .sourcePath = "a.cpp",
                                 .compilerIdentity = {} };
        f.estimates.Record(key, bytes);
        QueueRequest request;
        request.kind = AdmitKind::LocalOnly;
        request.caller = std::move(caller);
        request.estimate = std::move(key);
        request.treeLabel = std::move(tree);
        request.storable = false; // Identical keys would otherwise wait on one another.
        return f.queue.Enqueue(std::move(request));
    };
    enqueue("m", "wt7", 1 * GiB);
    enqueue("m", "wt3", 3 * GiB);
    enqueue("m", "wt3", 3 * GiB);
    enqueue("m", "wt3", 3 * GiB); // 1 + 3 + 3 of 8 GiB are granted; this one does not fit.
    REQUIRE(f.queue.Pump().size() == 3);

    auto const view = f.queue.Snapshot();
    CHECK(view.depth == 1);
    auto const find = [&view](std::string const& tree) {
        return std::ranges::find(view.clients, tree, &ClientLoad::tree);
    };
    REQUIRE(find("wt3") != view.clients.end());
    CHECK(find("wt3")->running == 2);
    CHECK(find("wt3")->queued == 1);
    REQUIRE(find("wt7") != view.clients.end());
    CHECK(find("wt7")->running == 1);
    CHECK(find("wt7")->queued == 0);
    REQUIRE(view.machines.size() == 1);
    CHECK(view.machines[0].chargedBytes == 7 * GiB);
    CHECK(view.machines[0].budgetBytes == std::optional<std::uint64_t> { 8 * GiB });
}

TEST_CASE("AdmissionQueue.snapshot-is-empty-before-anything-happens", "[distributed][admission]")
{
    Fixture f;
    auto const view = f.queue.Snapshot();
    CHECK(view.depth == 0);
    CHECK(view.clients.empty());
    CHECK(view.machines.empty());
}

TEST_CASE("AdmissionQueue.snapshot-in-use-is-what-admission-counts-after-a-failover", "[distributed][admission]")
{
    Fixture f;
    // A new leader: the ledger is empty, the machine says it holds 7 GiB of its 8.
    f.queue.ReportMachine("m", Report(8 * GiB, 32, 2, 7 * GiB));
    auto const view = f.queue.Snapshot();
    REQUIRE(view.machines.size() == 1);
    CHECK(view.machines[0].chargedBytes == 0);
    CHECK(view.machines[0].inUseBytes == 7 * GiB);
    // And it is the figure admission refuses on: 2 GiB does not fit in the 1 GiB left.
    f.Local(2 * GiB);
    CHECK(f.queue.Pump().empty());
}

TEST_CASE("AdmissionQueue.pressured-machine-never-runs-oversize-alone", "[distributed][admission]")
{
    // The run-alone exception admits a compile larger than the whole budget onto an idle machine. A
    // machine under memory pressure is idle BECAUSE it is yielding, so that exception is closed there.
    SECTION("yielding: the oversize request waits")
    {
        Fixture f;
        f.queue.ReportMachine("m", Pressed(4 * GiB, 4, PressureLevel::Yield));
        f.Local(6 * GiB);
        CHECK(f.queue.Pump().empty());
        CHECK(f.queue.Depth() == 1);
    }
    SECTION("calm: the same request runs alone (control)")
    {
        Fixture f;
        f.queue.ReportMachine("m", Pressed(4 * GiB, 4, PressureLevel::Calm));
        auto const big = f.Local(6 * GiB);
        auto const granted = f.queue.Pump();
        REQUIRE(granted.size() == 1);
        CHECK(granted[0].ticket == big);
        CHECK(granted[0].ranAlone);
    }
}

TEST_CASE("AdmissionQueue.calm-lowered-budget-oversize-request-idle-machine-admits-nothing", "[distributed][admission]")
{
    // A yield ended and the machine is Calm again, but the allowance still holds its budget below the configured one
    // while it ramps back. A compile larger than that held budget must not run alone: the ceiling holds compiles to
    // the same allowance and would kill it, the client would re-admit it, and the loop lasts as long as the ramp (C3).
    SECTION("held by the allowance: the oversize request waits")
    {
        Fixture f;
        auto held = Pressed(2 * GiB, 4, PressureLevel::Calm);
        held.standing = BudgetStanding::HeldByAllowance;
        f.queue.ReportMachine("m", held);
        f.Local(6 * GiB);
        CHECK(f.queue.Pump().empty());
        CHECK(f.queue.Depth() == 1);

        // A request that fits the held budget still runs: only the run-alone exception is closed.
        auto const small = f.Local(1 * GiB);
        auto const granted = f.queue.Pump();
        REQUIRE(granted.size() == 1);
        CHECK(granted[0].ticket == small);
    }
    SECTION("ramped back to its configured budget: the same request runs alone (control)")
    {
        Fixture f;
        f.queue.ReportMachine("m", Pressed(2 * GiB, 4, PressureLevel::Calm));
        auto const big = f.Local(6 * GiB);
        auto const granted = f.queue.Pump();
        REQUIRE(granted.size() == 1);
        CHECK(granted[0].ticket == big);
        CHECK(granted[0].ranAlone);
    }
}

TEST_CASE("AdmissionQueue.shedding-machine-admits-nothing", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Pressed(8 * GiB, 4, PressureLevel::Shed));
    auto const small = f.Local(1 * GiB);
    CHECK(f.queue.Pump().empty());
    CHECK(f.queue.Depth() == 1);

    // The level is applied on receipt, to a ticket already waiting: calm again, it runs.
    f.queue.ReportMachine("m", Pressed(8 * GiB, 4, PressureLevel::Calm));
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { small });
}

TEST_CASE("AdmissionQueue.shedding-machine-takes-no-dispatched-work-and-it-runs-elsewhere", "[distributed][admission]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("w", Pressed(8 * GiB, 4, PressureLevel::Shed));
    f.queue.ReportMachine("c", Report(8 * GiB, 4));
    auto const job = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == job);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Local); // on its caller, never on the shedding worker
}

TEST_CASE("AdmissionQueue.yield-holds-local-compiles-too", "[distributed][admission]")
{
    // The owner's decision: a local build can freeze the desktop as surely as a remote one, so a
    // request from the yielding machine itself, for itself, waits -- even one its reported room
    // holds, since a yielding machine's room is what its compiles gave back as they finished.
    SECTION("yielding: a request that fits the room and a free slot waits, and runs once calm")
    {
        Fixture f;
        auto yielding = Report(8 * GiB, 4, 2, 4 * GiB); // 4 GiB and two slots free
        yielding.pressure = PressureLevel::Yield;
        f.queue.ReportMachine("m", yielding);
        auto const local = f.Add(AdmitKind::LocalOnly, "m", 1 * GiB);
        CHECK(f.queue.Pump().empty());
        CHECK(f.queue.Depth() == 1);

        // Calm again: the waiting ticket runs, on receipt of the report.
        f.queue.ReportMachine("m", Report(8 * GiB, 4, 2, 4 * GiB));
        CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { local });
    }
    SECTION("calm: the same request runs at once (control)")
    {
        Fixture f;
        f.queue.ReportMachine("m", Report(8 * GiB, 4, 2, 4 * GiB));
        auto const local = f.Add(AdmitKind::LocalOnly, "m", 1 * GiB);
        CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { local });
    }
    SECTION("yielding: an oversize request waits too, and runs alone once calm")
    {
        Fixture f;
        f.queue.ReportMachine("m", Pressed(4 * GiB, 4, PressureLevel::Yield));
        auto const big = f.Add(AdmitKind::LocalOnly, "m", 6 * GiB);
        CHECK(f.queue.Pump().empty());
        f.queue.ReportMachine("m", Pressed(4 * GiB, 4, PressureLevel::Calm));
        CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { big });
    }
}

TEST_CASE("AdmissionQueue.yielding-worker-takes-no-dispatched-work-that-fits", "[distributed][admission]")
{
    // A dispatched request that fits a yielding worker's room is not placed there: with the caller
    // full, it waits; once the worker is calm, it runs there.
    Fixture f;
    f.placement.byFingerprint["fp"] = { Placement { .workerId = "worker-1", .machine = "w", .endpoint = "10.0.0.2:6680" } };
    f.queue.ReportMachine("c", Report(8 * GiB, 4, 4, 8 * GiB)); // the caller has no room
    f.queue.ReportMachine("w", Pressed(8 * GiB, 4, PressureLevel::Yield));
    auto const job = f.Add(AdmitKind::Dispatchable, "c", 1 * GiB, "fp");
    CHECK(f.queue.Pump().empty());

    f.queue.ReportMachine("w", Pressed(8 * GiB, 4, PressureLevel::Calm));
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].ticket == job);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Worker);
}

// --- a compile killed for memory, asked for again (#1770) -------------------------------------------------------

namespace
{
/// A worker on machine @p machine, for the toolchain "fp".
Placement WorkerOn(std::string const& machine)
{
    return Placement { .workerId = "worker-" + machine, .machine = machine, .endpoint = machine + ":6680" };
}
} // namespace

TEST_CASE("AdmissionQueue.a-killed-compile-is-charged-what-the-kill-taught-and-only-where-it-fits",
          "[distributed][admission][oom]")
{
    // The kill is learned from the admission ending unsettled: the key's estimate is raised to at least the default,
    // and the requeue is charged that. A machine whose configured budget is below it is then no target at all --
    // where the same request asked for the first time runs alone on it, and is the ceiling's next kill.
    Fixture f;
    f.placement.byFingerprint["fp"] = { WorkerOn("small") };
    f.queue.ReportMachine("small", Report(2 * GiB, 4));
    f.queue.ReportMachine("m", Report(8 * GiB, 4));
    f.estimates.Record(EstimateKey { .cacheKey = "big", .sourcePath = "big.cpp", .compilerIdentity = "fp" }, 1 * GiB);

    auto const first = f.Ask(AdmitKind::Dispatchable, "m", "big", AskOrigin::First);
    auto const placed = f.queue.Pump();
    REQUIRE(placed.size() == 1);
    REQUIRE(placed[0].placement.has_value());
    CHECK(Unwrap(placed[0].placement).machine == "small");
    CHECK(placed[0].chargedBytes == 1 * GiB);

    f.queue.Abandon(first); // the worker's machine killed it: the launcher ends the admission unsettled

    SECTION("asked for again after the kill")
    {
        f.Ask(AdmitKind::Dispatchable, "m", "big", AskOrigin::AfterKill);
        auto const again = f.queue.Pump();
        REQUIRE(again.size() == 1);
        CHECK(again[0].chargedBytes == 3 * GiB); // the default the kill raised it to
        CHECK(again[0].kind == Pace::CompileCacheWire::GrantKind::Local);
        CHECK_FALSE(again[0].placement.has_value());
        CHECK_FALSE(again[0].neverFits); // this machine holds it
    }
    SECTION("the control: asked for as a first request, it runs alone on the machine that killed it")
    {
        f.Ask(AdmitKind::Dispatchable, "m", "big", AskOrigin::First);
        auto const again = f.queue.Pump();
        REQUIRE(again.size() == 1);
        REQUIRE(again[0].placement.has_value());
        CHECK(Unwrap(again[0].placement).machine == "small");
        CHECK(again[0].ranAlone);
    }
}

TEST_CASE("AdmissionQueue.a-killed-compile-waits-for-room-on-a-machine-that-fits", "[distributed][admission][oom]")
{
    // Kill, requeue, admitted LATER: the one machine whose budget can hold it is busy, and the asking machine's is
    // too small to, so the request waits -- rather than running alone where it would be killed -- until room frees.
    Fixture f;
    f.placement.byFingerprint["fp"] = { WorkerOn("big") };
    f.queue.ReportMachine("big", Report(8 * GiB, 4));
    f.queue.ReportMachine("m", Report(2 * GiB, 4));
    auto const other = f.Add(AdmitKind::Dispatchable, "x", 6 * GiB, "fp");
    REQUIRE(f.queue.Pump().size() == 1);

    f.estimates.RecordKilled(EstimateKey { .cacheKey = "killed", .sourcePath = "big.cpp", .compilerIdentity = "fp" });
    auto const requeued = f.Ask(AdmitKind::Dispatchable, "m", "killed", AskOrigin::AfterKill);
    CHECK(f.queue.Pump().empty());
    CHECK(f.queue.Depth() == 1);

    f.queue.Settle(other, Done);
    auto const later = f.queue.Pump();
    REQUIRE(later.size() == 1);
    CHECK(later[0].ticket == requeued);
    REQUIRE(later[0].placement.has_value());
    CHECK(Unwrap(later[0].placement).machine == "big");
    CHECK_FALSE(later[0].neverFits);
}

TEST_CASE("AdmissionQueue.a-killed-compile-no-machine-can-hold-never-fits", "[distributed][admission][oom]")
{
    Fixture f;
    f.placement.byFingerprint["fp"] = { WorkerOn("w") };
    f.queue.ReportMachine("w", Report(2 * GiB, 4));
    f.queue.ReportMachine("m", Report(2 * GiB, 4));
    f.estimates.RecordKilled(EstimateKey { .cacheKey = "huge", .sourcePath = "big.cpp", .compilerIdentity = "fp" });

    SECTION("dispatchable")
    {
        f.Ask(AdmitKind::Dispatchable, "m", "huge", AskOrigin::AfterKill);
    }
    SECTION("local-only, the launcher's own requeue after a kill here")
    {
        f.Ask(AdmitKind::LocalOnly, "m", "huge", AskOrigin::AfterKill);
    }

    auto const decided = f.queue.Pump();
    REQUIRE(decided.size() == 1);
    CHECK(decided[0].kind == Pace::CompileCacheWire::GrantKind::Local);
    CHECK(decided[0].neverFits);
    // Alone on its own machine, as any compile larger than a whole budget runs.
    CHECK(decided[0].ranAlone);
}

TEST_CASE("AdmissionQueue.a-budget-the-allowance-holds-down-is-waited-for-not-never", "[distributed][admission][oom]")
{
    // A held budget says nothing of the configured one, so it may yet hold the compile: not `neverFits`.
    Fixture f;
    f.placement.byFingerprint["fp"] = { WorkerOn("w") };
    auto held = Report(2 * GiB, 4);
    held.standing = BudgetStanding::HeldByAllowance;
    f.queue.ReportMachine("w", held);
    f.queue.ReportMachine("m", Report(2 * GiB, 4));
    f.estimates.RecordKilled(EstimateKey { .cacheKey = "huge", .sourcePath = "big.cpp", .compilerIdentity = "fp" });

    f.Ask(AdmitKind::Dispatchable, "m", "huge", AskOrigin::AfterKill);
    CHECK(f.queue.Pump().empty());

    f.queue.ReportMachine("w", Report(8 * GiB, 4)); // the allowance ramped back
    auto const later = f.queue.Pump();
    REQUIRE(later.size() == 1);
    REQUIRE(later[0].placement.has_value());
    CHECK(Unwrap(later[0].placement).machine == "w");
    CHECK_FALSE(later[0].neverFits);
}
