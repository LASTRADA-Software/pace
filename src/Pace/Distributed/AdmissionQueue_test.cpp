// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/AdmissionQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>

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
    PeakEstimates estimates { 3 * GiB, 64 };
    FakePlacement placement;
    AdmissionQueue queue { estimates, placement };

    /// Enqueues a request of `bytes`, the figure being taught to the estimator under its own key.
    Ticket Add(AdmitKind kind, std::string caller, std::uint64_t bytes, std::string fingerprint = {})
    {
        auto key = EstimateKey { "key-" + std::to_string(++_serial), "a.cpp", fingerprint };
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

  private:
    int _serial {};
};

MachineReport Report(std::uint64_t budget, std::uint32_t slots, std::uint32_t heldSlots = 0, std::uint64_t heldBytes = 0)
{
    return MachineReport { budget, slots, heldSlots, heldBytes };
}

std::vector<Ticket> Tickets(std::vector<Decision> const& decisions)
{
    std::vector<Ticket> result;
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
    for (int i = 0; i < 4; ++i)
        f.Local(3 * GiB);
    CHECK(f.queue.Pump().size() == 2);
    CHECK(f.queue.Depth() == 2);
}

TEST_CASE("AdmissionQueue.slots-bound-admission", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(64 * GiB, 2));
    for (int i = 0; i < 3; ++i)
        f.Local(3 * GiB);
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
    static_cast<void>(head);
}

TEST_CASE("AdmissionQueue.head-reservation-prevents-starvation", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(8 * GiB, 32));
    auto const holder = f.Local(4 * GiB);
    REQUIRE(f.queue.Pump().size() == 1);
    auto const head = f.Local(6 * GiB);
    std::vector<Ticket> smalls;
    for (int i = 0; i < 3; ++i)
        smalls.push_back(f.Local(1 * GiB));
    REQUIRE(f.queue.Pump().size() == 3);

    REQUIRE(f.queue.Release(holder)); // 3 GiB in use, 5 free, head needs 6
    auto const late = f.Local(1 * GiB);
    CHECK(f.queue.Pump().empty()); // the free 5 GiB is the head's, not the latecomer's

    for (auto const small: smalls)
        REQUIRE(f.queue.Release(small));
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

TEST_CASE("AdmissionQueue.fifo-among-equals", "[distributed][admission]")
{
    Fixture f;
    f.queue.ReportMachine("m", Report(3 * GiB, 4));
    auto const first = f.Local(3 * GiB);
    auto const second = f.Local(3 * GiB);
    auto const third = f.Local(3 * GiB);

    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { first });
    REQUIRE(f.queue.Release(first));
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { second });
    REQUIRE(f.queue.Release(second));
    CHECK(Tickets(f.queue.Pump()) == std::vector<Ticket> { third });
}

TEST_CASE("AdmissionQueue.dispatchable-uses-placement", "[distributed][admission]")
{
    Fixture f;
    Placement const worker { "worker-1", "w", "10.0.0.2:6680" };
    f.placement.byFingerprint["fp"] = { worker };
    f.queue.ReportMachine("w", Report(8 * GiB, 4));
    f.queue.ReportMachine("c", Report(8 * GiB, 4));

    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "fp");
    auto const granted = f.queue.Pump();
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].kind == Pace::CompileCacheWire::GrantKind::Worker);
    REQUIRE(granted[0].placement.has_value());
    CHECK(granted[0].placement->workerId == worker.workerId);
    CHECK(granted[0].placement->machine == "w");
    CHECK(granted[0].placement->endpoint == worker.endpoint);

    // Machine w is charged: a second 5 GiB request does not fit there.
    f.Add(AdmitKind::Dispatchable, "c", 5 * GiB, "fp");
    CHECK(f.queue.Pump().empty());
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
