// SPDX-License-Identifier: Apache-2.0
//
// The machine's compile budget and the report every node sends of it (#1728, R6).
//
// Two halves, and the second is the one that decides whether the first matters: `MachineBudgetOf` is a
// pure function over the configuration, and `SendMachineBudgetOnce` is what puts it on the wire from
// EVERY node -- one running no worker included, which today registers nothing and would otherwise
// leave the leader with no row to charge.
#include "EndpointDialerTestUtils.hpp"
#include "HeldCharges.hpp"
#include "MachineBudget.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "SchedulerReachability.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/NodePolicy.hpp>
#include <Pace/Platform/HostMemory.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::Unwrap;

namespace
{

namespace Wire = Pace::CompileCacheWire;

constexpr std::uint64_t GiB = 1ULL << 30;
constexpr std::string_view Scheduler = "scheduler.example:6676";

/// A machine with @p cores threads, @p memory bytes and @p reserved held back for its own cache tier.
/// @param cores Hardware threads.
/// @param memory Physical memory.
/// @param reserved What the node reserves for itself.
/// @return The capacity `NodeCapacityOf` would have made of it.
[[nodiscard]] Distributed::NodeCapacity MachineOf(std::uint32_t cores, std::uint64_t memory, std::uint64_t reserved = 0)
{
    return Distributed::NodeCapacity { .logicalCores = cores, .totalMemoryBytes = memory, .reservedMemoryBytes = reserved };
}

/// Parse @p args the way `main` does.
/// @param args The flags.
/// @return The configuration; a parse error fails the case.
[[nodiscard]] NodeConfig Parsed(std::vector<char const*> const& args)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg);
    REQUIRE(flow.has_value());
    return cfg;
}

/// @param seed Distinguishes the keys.
/// @return A peak whose three key fields are made from @p seed.
[[nodiscard]] MeasuredPeak PeakOf(std::size_t seed, std::uint64_t bytes)
{
    return MeasuredPeak { Wire::EstimateKeyFields { .cacheKey = std::format("key-{}", seed),
                                                    .sourcePath = std::format("<SRCROOT>/src/{}.cpp", seed),
                                                    .compilerIdentity = "gcc-14" },
                          bytes };
}

/// A holder with a fixed answer.
class FixedHeld final: public IHeldCharges
{
  public:
    /// @param held What to report.
    explicit FixedHeld(HeldCharges held) noexcept:
        _held { held }
    {
    }

    [[nodiscard]] HeldCharges Held() const override
    {
        return _held;
    }

  private:
    HeldCharges _held;
};

/// Everything one machine-budget round needs, over a node registering with one scheduler.
struct BudgetFixture
{
    NodeConfig cfg;
    Distributed::NodeCapacity capacity { MachineOf(16, 64 * GiB, 8 * GiB) };
    FixedHeld held { HeldCharges { .slots = 3, .bytes = 6 * GiB } };
    MeasuredPeaks peaks;
    CapturingLogger logger;
    NodeConditions conditions;
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock, &conditions };

    BudgetFixture()
    {
        cfg = Testing::LearnerRegisteringWith(std::move(cfg), { std::string { Scheduler } });
    }

    /// Report once through @p dialer.
    /// @param dialer How the round reaches a scheduler.
    /// @return Whether a scheduler recorded it.
    [[nodiscard]] bool SendThrough(Testing::ScriptedDialer& dialer)
    {
        auto link = Unwrap(SchedulerLink::For(SchedulersOf(cfg, AsConfigured)));
        return SendMachineBudgetOnce(MachineBudgetRound { .cfg = cfg,
                                                          .capacity = capacity,
                                                          .held = held,
                                                          .peaks = peaks,
                                                          .logger = logger,
                                                          // Nothing proves: the scripted fleet serves no handshake.
                                                          .prover = nullptr,
                                                          .reachability = reachability },
                                     link,
                                     dialer);
    }

    /// @param level A severity.
    /// @return How many records at exactly that severity mention a budget.
    [[nodiscard]] std::size_t BudgetLinesAt(LogLevel level) const
    {
        auto const records = logger.Snapshot();
        return static_cast<std::size_t>(std::ranges::count_if(records, [level](CapturingLogger::Record const& record) {
            return record.level == level && record.message.contains("compile budget");
        }));
    }
};

/// @return A framed Ok reply with no payload.
[[nodiscard]] std::vector<std::byte> Accepted()
{
    return Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {});
}

/// The whole frames in @p sent.
/// @param sent Everything written on one connection.
/// @return `(op, payload)` per frame.
[[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> FramesIn(std::span<std::byte const> sent)
{
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> frames;
    while (sent.size() >= Wire::RequestHeaderSize)
    {
        auto const header = Wire::DecodeRequestHeader(sent);
        if (!header.has_value())
            break;
        auto const whole = Wire::RequestHeaderSize + std::size_t { header->payloadLength };
        if (sent.size() < whole)
            break;
        auto const payload = sent.subspan(Wire::RequestHeaderSize, header->payloadLength);
        frames.emplace_back(header->opRaw, std::vector<std::byte> { payload.begin(), payload.end() });
        sent = sent.subspan(whole);
    }
    return frames;
}

} // namespace

TEST_CASE("MachineBudget.default-is-75-percent-minus-cache", "[node][machinebudget]")
{
    // 64 GiB with an 8 GiB resident cache reserve: (64 - 8) * 3/4 = 42 GiB. The reserve is what the
    // tier BUILT (`NodeCapacity::reservedMemoryBytes`), never what a flag asked for.
    auto const report = MachineBudgetOf(NodeConfig {}, MachineOf(16, 64 * GiB, 8 * GiB), HeldCharges {});
    CHECK(report.memoryBudgetBytes == 42 * GiB);

    // The control that the reserve is what moves it: no reserve, 75% of the whole.
    CHECK(MachineBudgetOf(NodeConfig {}, MachineOf(16, 64 * GiB), HeldCharges {}).memoryBudgetBytes == 48 * GiB);

    // A reserve past the machine is clamped, so the subtraction cannot wrap into a huge budget.
    CHECK(MachineBudgetOf(NodeConfig {}, MachineOf(16, 4 * GiB, 9 * GiB), HeldCharges {}).memoryBudgetBytes
          == DefaultCompileEstimateBytes);
}

TEST_CASE("MachineBudget.floored-at-default-estimate", "[node][machinebudget]")
{
    // 2 GiB of RAM is 1.5 GiB at 75%: below what ONE unknown compile is charged, so every compile
    // would queue for ever. Floored at the default estimate, 2 GiB.
    CHECK(MachineBudgetOf(NodeConfig {}, MachineOf(2, 2 * GiB), HeldCharges {}).memoryBudgetBytes == 2 * GiB);

    // The floor is the CONFIGURED estimate, so raising the estimate raises the floor with it.
    auto cfg = Parsed({ "--compile-estimate-default=6g" });
    CHECK(MachineBudgetOf(cfg, MachineOf(2, 2 * GiB), HeldCharges {}).memoryBudgetBytes == 6 * GiB);

    // And the floor binds only a DERIVED budget: a typed figure is the operator's answer.
    auto typed = Parsed({ "--machine-memory-budget=1g" });
    CHECK(MachineBudgetOf(typed, MachineOf(2, 2 * GiB), HeldCharges {}).memoryBudgetBytes == 1 * GiB);
}

TEST_CASE("MachineBudget.explicit-percent", "[node][machinebudget]")
{
    // `N%` is of HOST RAM, as `--cache-memory`'s is; resolved when the flag is parsed, so the report
    // states bytes. The capacity passed is deliberately unrelated: the flag wins over the derivation.
    auto const half = Parsed({ "--machine-memory-budget=50%" });
    auto const quarter = Parsed({ "--machine-memory-budget=25%" });
    REQUIRE(half.machineMemoryBudgetBytes.has_value());
    REQUIRE(quarter.machineMemoryBudgetBytes.has_value());
    CHECK(*half.machineMemoryBudgetBytes > *quarter.machineMemoryBudgetBytes);
    CHECK(*half.machineMemoryBudgetBytes <= QueryHostTotalMemoryBytes());

    auto const report = MachineBudgetOf(half, MachineOf(4, 1 * GiB), HeldCharges {});
    CHECK(report.memoryBudgetBytes == *half.machineMemoryBudgetBytes);
}

TEST_CASE("MachineBudget.explicit-size", "[node][machinebudget]")
{
    auto const cfg = Parsed({ "--machine-memory-budget=12g" });
    CHECK(MachineBudgetOf(cfg, MachineOf(16, 64 * GiB, 8 * GiB), HeldCharges {}).memoryBudgetBytes == 12 * GiB);
    CHECK(MachineBudgetOf(cfg, MachineOf(2, 2 * GiB), HeldCharges {}).memoryBudgetBytes == 12 * GiB);
}

TEST_CASE("MachineBudget.slots-default-cores", "[node][machinebudget]")
{
    CHECK(MachineBudgetOf(NodeConfig {}, MachineOf(24, 64 * GiB), HeldCharges {}).slots == 24);

    // Zero cores is "did not say", which is one core and never a machine that can run nothing.
    CHECK(MachineBudgetOf(NodeConfig {}, MachineOf(0, 64 * GiB), HeldCharges {}).slots == 1);
}

TEST_CASE("MachineBudget.slots-explicit", "[node][machinebudget]")
{
    auto const cfg = Parsed({ "--machine-slots=6" });
    CHECK(MachineBudgetOf(cfg, MachineOf(24, 64 * GiB), HeldCharges {}).slots == 6);

    // Not `--slots`: that sizes the WORKER and is a different number on the same machine.
    auto both = Parsed({ "--machine-slots=6", "--slots=20" });
    CHECK(MachineBudgetOf(both, MachineOf(24, 64 * GiB), HeldCharges {}).slots == 6);
}

TEST_CASE("MachineBudget.held-passes-through", "[node][machinebudget]")
{
    auto const report =
        MachineBudgetOf(NodeConfig {}, MachineOf(8, 32 * GiB), HeldCharges { .slots = 5, .bytes = 9 * GiB + 7 });
    CHECK(report.heldSlots == 5);
    CHECK(report.heldBytes == 9 * GiB + 7);
}

TEST_CASE("MachineBudget.reported-with-no-worker", "[node][machinebudget]")
{
    // R6. `--slots=0` is "no worker", and it must not be "no machine": the leader charges a machine
    // whose launchers it admitted, and a node that reports nothing is a machine it cannot charge.
    auto const cfg = Parsed({ "--slots=0", "--machine-slots=12" });
    REQUIRE_FALSE(RunsWorker(cfg));
    auto const report = MachineBudgetOf(cfg, MachineOf(24, 64 * GiB), HeldCharges {});
    CHECK(report.slots == 12);
    CHECK(report.memoryBudgetBytes > 0);

    // And without naming the machine's slots, it is the machine's cores -- not the worker's zero.
    auto const bare = Parsed({ "--slots=0" });
    REQUIRE_FALSE(RunsWorker(bare));
    CHECK(MachineBudgetOf(bare, MachineOf(24, 64 * GiB), HeldCharges {}).slots == 24);
}

TEST_CASE("MachineBudget.report-goes-on-the-wire-from-a-node-with-no-worker", "[node][machinebudget]")
{
    BudgetFixture fixture;
    fixture.cfg.slots = 0;
    REQUIRE_FALSE(RunsWorker(fixture.cfg));
    fixture.peaks.OnPeak(PeakOf(1, 3 * GiB).first, 3 * GiB);

    Testing::ScriptedDialer dialer { { Accepted() } };
    REQUIRE(fixture.SendThrough(dialer));

    auto const frames = FramesIn(dialer.SentOn(0));
    REQUIRE(frames.size() == 1);
    // The verb, by its byte as well as its name.
    CHECK(frames.front().first == 0x24);
    CHECK(frames.front().first == static_cast<std::uint8_t>(Wire::Op::MachineBudget));

    auto const sent = Unwrap(Wire::DecodeMachineBudgetPayload(frames.front().second));
    CHECK(sent.memoryBudgetBytes == 42 * GiB);
    CHECK(sent.slots == 16);
    CHECK(sent.heldSlots == 3);
    CHECK(sent.heldBytes == 6 * GiB);
    REQUIRE(sent.peaks.size() == 1);
    CHECK(sent.peaks.front().first.cacheKey == "key-1");
    CHECK(sent.peaks.front().second == 3 * GiB);

    // Delivered, so not offered again.
    CHECK(fixture.peaks.Take().empty());
}

TEST_CASE("MachineBudget.refused-report-keeps-its-peaks-and-is-said-once", "[node][machinebudget]")
{
    // A leader that serves no queue yet answers the verb `DispatchNotPermitted`: an ordinary refusal.
    // It must not crash the loop, lose the measurements, or say the same thing every round.
    BudgetFixture fixture;
    fixture.peaks.OnPeak(PeakOf(1, 3 * GiB).first, 3 * GiB);
    auto const refused = Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted, "no queue here");

    Testing::ScriptedDialer first { { refused } };
    CHECK_FALSE(fixture.SendThrough(first));
    // Put back, for the next round to offer again.
    auto const back = fixture.peaks.Take();
    REQUIRE(back.size() == 1);
    CHECK(back.front().second == 3 * GiB);
    fixture.peaks.Restore(back);

    CHECK(fixture.BudgetLinesAt(LogLevel::Warn) == 1);

    // The same refusal again, a round later: a repeat, which is Debug and never another Warn.
    Testing::ScriptedDialer second { { refused } };
    CHECK_FALSE(fixture.SendThrough(second));
    CHECK(fixture.BudgetLinesAt(LogLevel::Warn) == 1);
    CHECK(fixture.BudgetLinesAt(LogLevel::Debug) >= 1);

    // And when the leader takes it, the recovery is said, and the peaks go.
    Testing::ScriptedDialer third { { Accepted() } };
    CHECK(fixture.SendThrough(third));
    CHECK(fixture.BudgetLinesAt(LogLevel::Warn) == 2);
    CHECK(fixture.peaks.Take().empty());
}

TEST_CASE("MachineBudget.unreachable-scheduler-keeps-the-peaks", "[node][machinebudget]")
{
    BudgetFixture fixture;
    fixture.peaks.OnPeak(PeakOf(1, 3 * GiB).first, 3 * GiB);

    // An empty entry is a dial the script fails on purpose.
    Testing::ScriptedDialer dialer { { std::vector<std::byte> {} } };
    CHECK_FALSE(fixture.SendThrough(dialer));
    CHECK(fixture.peaks.Take().size() == 1);
}

TEST_CASE("MachineBudget.measured-peaks-keep-the-newest-per-key", "[node][machinebudget]")
{
    MeasuredPeaks peaks;
    peaks.OnPeak(PeakOf(1, 100).first, 100);
    peaks.OnPeak(PeakOf(2, 200).first, 200);
    peaks.OnPeak(PeakOf(1, 300).first, 300);

    auto const taken = peaks.Take();
    REQUIRE(taken.size() == 2);
    // One per key, the newest figure, at the newest position.
    CHECK(taken[0].first.cacheKey == "key-2");
    CHECK(taken[1].first.cacheKey == "key-1");
    CHECK(taken[1].second == 300);
    CHECK(peaks.Take().empty());
}

TEST_CASE("MachineBudget.measured-peaks-are-bounded-oldest-out", "[node][machinebudget]")
{
    MeasuredPeaks peaks;
    auto const extra = std::size_t { 5 };
    for (auto const seed: std::views::iota(std::size_t { 0 }, Wire::MaxMachineBudgetPeaks + extra))
        peaks.OnPeak(PeakOf(seed, seed).first, seed);

    auto const taken = peaks.Take();
    REQUIRE(taken.size() == Wire::MaxMachineBudgetPeaks);
    // The oldest went, and the newest stayed.
    CHECK(taken.front().first.cacheKey == std::format("key-{}", extra));
    CHECK(taken.back().first.cacheKey == std::format("key-{}", Wire::MaxMachineBudgetPeaks + extra - 1));

    // A report built from a full sample is one the receiver accepts.
    auto const frame = Wire::EncodeMachineBudget(MachineBudgetToWire(Distributed::MachineReport {}, taken));
    auto const frames = FramesIn(frame);
    REQUIRE(frames.size() == 1);
    CHECK(Wire::DecodeMachineBudgetPayload(frames.front().second).has_value());
}

TEST_CASE("MachineBudget.measured-peaks-drop-what-cannot-travel", "[node][machinebudget]")
{
    // A peak filed under half a key estimates nothing, so a field over its wire cap is dropped and never
    // truncated -- and a key with no object key is no key.
    MeasuredPeaks peaks;
    auto longKey = PeakOf(1, 1).first;
    longKey.cacheKey.assign(Wire::MaxLeaseKeyBytes + 1, 'k');
    peaks.OnPeak(longKey, 1);
    auto longPath = PeakOf(2, 1).first;
    longPath.sourcePath.assign(Wire::MaxAdmitPathBytes + 1, 'p');
    peaks.OnPeak(longPath, 1);
    auto longIdentity = PeakOf(3, 1).first;
    longIdentity.compilerIdentity.assign(Wire::MaxToolchainFingerprintBytes + 1, 'c');
    peaks.OnPeak(longIdentity, 1);
    auto noKey = PeakOf(4, 1).first;
    noKey.cacheKey.clear();
    peaks.OnPeak(noKey, 1);
    CHECK(peaks.Take().empty());

    // The control: every field AT its cap is kept.
    auto atCap = PeakOf(5, 1).first;
    atCap.cacheKey.assign(Wire::MaxLeaseKeyBytes, 'k');
    atCap.sourcePath.assign(Wire::MaxAdmitPathBytes, 'p');
    atCap.compilerIdentity.assign(Wire::MaxToolchainFingerprintBytes, 'c');
    peaks.OnPeak(atCap, 1);
    CHECK(peaks.Take().size() == 1);
}

TEST_CASE("MachineBudget.restore-keeps-what-was-measured-since", "[node][machinebudget]")
{
    MeasuredPeaks peaks;
    peaks.OnPeak(PeakOf(1, 100).first, 100);
    peaks.OnPeak(PeakOf(2, 200).first, 200);
    auto const taken = peaks.Take();

    // Key 1 is measured again while the send was failing: its newer figure outranks the undelivered one.
    peaks.OnPeak(PeakOf(1, 999).first, 999);
    peaks.Restore(taken);

    auto const all = peaks.Take();
    REQUIRE(all.size() == 2);
    // Undelivered first (older), then what was measured since.
    CHECK(all[0].first.cacheKey == "key-2");
    CHECK(all[0].second == 200);
    CHECK(all[1].first.cacheKey == "key-1");
    CHECK(all[1].second == 999);
}

TEST_CASE("MachineBudget.held-charge-registry-releases-on-every-path", "[node][machinebudget]")
{
    HeldChargeRegistry registry;
    CHECK(registry.Held() == HeldCharges {});
    {
        auto const first = registry.Hold(5 * GiB);
        auto second = registry.Hold(2 * GiB);
        CHECK(registry.Held() == HeldCharges { .slots = 2, .bytes = 7 * GiB });

        // A moved handle owns the charge, and the moved-from one releases nothing.
        auto const moved = std::move(second);
        CHECK(registry.Held() == HeldCharges { .slots = 2, .bytes = 7 * GiB });
    }
    CHECK(registry.Held() == HeldCharges {});
}

TEST_CASE("MachineBudget.held-sources-are-summed", "[node][machinebudget]")
{
    FixedHeld const worker { HeldCharges { .slots = 2, .bytes = 4 * GiB } };
    HeldChargeRegistry leases;
    auto const handle = leases.Hold(1 * GiB);
    CombinedHeldCharges const combined { { &worker, &leases } };
    CHECK(combined.Held() == HeldCharges { .slots = 3, .bytes = 5 * GiB });
}
