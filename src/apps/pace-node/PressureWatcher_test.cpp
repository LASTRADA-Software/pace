// SPDX-License-Identifier: Apache-2.0
//
// The pressure watcher: what a step DOES with the governor's answer -- the ceiling, the kill, the presence round,
// the published level, the condition, the counters and the log. Every case drives `StepOnce()` by hand over fakes
// and a manual clock; no thread runs.
#include "CacheTier.hpp"
#include "CompileMemoryMeter.hpp"
#include "HeldCharges.hpp"
#include "LiveNodeConfig.hpp"
#include "MachineBudget.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodePresenceTier.hpp"
#include "NodeReload.hpp"
#include "PressureWatcher.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/CgroupV2.hpp>
#include <Pace/Platform/MemoryPressure.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <IProcessRunner.hpp>
#include <core/platform/Clock.hpp>
#include <tests/CgroupFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using namespace std::chrono_literals;
using Pace::Testing::Unwrap;

namespace
{

namespace Wire = CompileCacheWire;

constexpr std::uint64_t GiB = std::uint64_t { 1 } << 30U;
constexpr std::uint64_t Configured = 64 * GiB;
constexpr std::uint64_t Holding = 20 * GiB;

/// A source a case scripts: each `Sample` answers `next`.
class ScriptedPressure final: public Platform::IMemoryPressureSource
{
  public:
    std::expected<Platform::PressureSample, std::string> next { Platform::PressureSample {} };

    [[nodiscard]] std::expected<Platform::PressureSample, std::string> Sample() override
    {
        return next;
    }
};

/// Settings a case moves.
class FixedSettings final: public IPressureSettings
{
  public:
    PressureSettings settings { .thresholds = PressureThresholds {}, .configuredBytes = Configured };

    [[nodiscard]] PressureSettings Current() const override
    {
        return settings;
    }
};

/// A ceiling that records what it is told to hold compiles under.
class RecordingCeiling final: public ICompileCeiling
{
  public:
    std::vector<std::optional<std::uint64_t>> applied;
    std::optional<std::uint64_t> held { Holding };

    void Apply(std::optional<std::uint64_t> allowanceBytes) override
    {
        applied.push_back(allowanceBytes);
    }

    [[nodiscard]] std::optional<std::uint64_t> HeldBytes() const override
    {
        return held;
    }
};

/// A killer that counts, and answers what a case says.
class CountingKiller final: public Platform::IYoungestKiller
{
  public:
    int asked = 0;
    bool answer = true;

    [[nodiscard]] bool KillYoungest() override
    {
        ++asked;
        return answer;
    }
};

/// What compiles were charged, fixed.
class FixedHeld final: public IHeldCharges
{
  public:
    HeldCharges held { .slots = 3, .bytes = 12 * GiB };

    [[nodiscard]] HeldCharges Held() const override
    {
        return held;
    }
};

/// PSI percentages.
struct Psi
{
    double some = 0.0;
    double full = 0.0;
    std::optional<double> compiles = 0.0;
};

/// @return The sample @p psi describes.
[[nodiscard]] Platform::PressureSample SampleOf(Psi const& psi)
{
    auto sample = Platform::PressureSample {};
    sample.system.some.avg10 = psi.some;
    sample.system.full = Platform::PsiLine { .avg10 = psi.full, .avg60 = 0.0, .totalMicros = 0 };
    if (psi.compiles.has_value())
    {
        auto compiles = Platform::PsiReading {};
        compiles.some.avg10 = *psi.compiles;
        sample.compiles = compiles;
    }
    return sample;
}

constexpr Psi Quiet {};
constexpr Psi GameStall { .some = 30.0, .full = 0.0, .compiles = 0.0 };
constexpr Psi Freeze { .some = 60.0, .full = 20.0, .compiles = 0.0 };

/// Whether a presence round is owed now, consuming it.
[[nodiscard]] bool RoundOwed(PresenceWake& wake, PresenceWakeReason reason = PresenceWakeReason::Pressure)
{
    auto const stop = std::stop_source {};
    return wake.WaitOut(stop.get_token(), 0ms) == reason;
}

/// The watcher and everything it borrows, built as `main` builds it: with or without a cgroup.
struct Rig
{
    core::platform::ManualClock clock;
    FixedSettings settings;
    RecordingCeiling ceiling;
    CountingKiller killer;
    FixedHeld held;
    PublishedPressure published;
    PresenceWake wake;
    NodeConditions conditions;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    ScriptedPressure* source = nullptr;
    std::unique_ptr<PressureWatcher> watcher;

    /// @param cgroups Whether compiles run in a cgroup of the node's own.
    /// @param hasPsi Whether the platform has PSI at all.
    explicit Rig(bool cgroups = true, bool hasPsi = true)
    {
        auto scripted = std::make_unique<ScriptedPressure>();
        source = scripted.get();
        watcher = std::make_unique<PressureWatcher>(PressureWatcherParts { .source = std::move(scripted),
                                                                           .settings = settings,
                                                                           .ceiling = cgroups ? &ceiling : nullptr,
                                                                           .killer = cgroups ? &killer : nullptr,
                                                                           .held = held,
                                                                           .published = published,
                                                                           .wake = wake,
                                                                           .conditions = conditions,
                                                                           .metrics = metrics,
                                                                           .logger = logger,
                                                                           .clock = clock,
                                                                           .poll = 1s,
                                                                           .platformHasPsi = hasPsi });
    }

    /// Start the watcher without its thread, as a case steps it by hand.
    void Started() const
    {
        REQUIRE(watcher->Start(false).has_value());
    }

    /// Advance the clock by @p elapsed, then take one step on @p psi.
    void Step(std::chrono::milliseconds elapsed, Psi const& psi)
    {
        clock.advance(elapsed);
        source->next = SampleOf(psi);
        watcher->StepOnce();
    }

    /// @return What is published now.
    [[nodiscard]] MachinePressure Published() const
    {
        auto const now = published.Current();
        REQUIRE(now.has_value());
        return Unwrap(now);
    }

    /// @return How many lines at @p level were logged.
    [[nodiscard]] std::size_t Lines(LogLevel level) const
    {
        return static_cast<std::size_t>(std::ranges::count(logger.Snapshot(), level, &CapturingLogger::Record::level));
    }
};

} // namespace

TEST_CASE("Entering yield holds compiles under the allowance, asks one presence round and raises the condition",
          "[node][pressure]")
{
    Rig rig;
    rig.Started();
    CHECK(rig.conditions.StateOf(NodeCondition::MemoryPressureShedding) == Wire::ConditionState::Clear);
    CHECK(rig.Published().level == Wire::PressureLevel::Calm);
    // An allowance at the configuration lowers nothing, so none is stated.
    CHECK_FALSE(rig.Published().allowanceBytes.has_value());

    rig.Step(1s, Quiet);
    // Calm at the configured figure: nothing to hand the ceiling, and no round is owed.
    CHECK(rig.ceiling.applied.empty());
    CHECK_FALSE(RoundOwed(rig.wake));

    rig.Step(1s, GameStall);
    // The allowance is what compiles hold NOW, written through the ceiling.
    CHECK(rig.ceiling.applied == std::vector<std::optional<std::uint64_t>> { Holding });
    CHECK(rig.Published().level == Wire::PressureLevel::Yield);
    CHECK(rig.Published().allowanceBytes == Holding);
    CHECK(RoundOwed(rig.wake));
    CHECK(rig.conditions.StateOf(NodeCondition::MemoryPressureShedding) == Wire::ConditionState::Raised);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureYieldEntries) == 1);

    // Held: nothing more is written and no further round is asked for while the level stands.
    rig.Step(1s, GameStall);
    CHECK(rig.ceiling.applied.size() == 1);
    CHECK_FALSE(RoundOwed(rig.wake));

    // Back to calm once the stall eases and the dwell is spent: a round, and the row clears.
    rig.Step(10s, Quiet);
    CHECK(rig.Published().level == Wire::PressureLevel::Calm);
    CHECK(RoundOwed(rig.wake));
    CHECK(rig.conditions.StateOf(NodeCondition::MemoryPressureShedding) == Wire::ConditionState::Clear);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureYieldEntries) == 1);
    // Info both ways: a yield is the node doing its job.
    CHECK(rig.Lines(LogLevel::Warn) == 0);
}

TEST_CASE("A shed kills the youngest once per dwell, and a shed with nothing to kill does nothing else", "[node][pressure]")
{
    Rig rig;
    rig.Started();
    rig.Step(1s, Freeze);
    CHECK(rig.Published().level == Wire::PressureLevel::Shed);
    CHECK(rig.killer.asked == 1);
    CHECK(RoundOwed(rig.wake));
    // Entering a shed is said at Warn: compiles are being killed.
    CHECK(rig.Lines(LogLevel::Warn) == 1);

    // Within the dwell: no second kill.
    rig.Step(5s, Freeze);
    CHECK(rig.killer.asked == 1);
    rig.Step(5s, Freeze);
    CHECK(rig.killer.asked == 2);

    // Nothing left running: the killer answers false, and nothing else moves -- no round, no line, no write.
    rig.killer.answer = false;
    auto const lines = rig.logger.Snapshot().size();
    auto const writes = rig.ceiling.applied.size();
    rig.Step(10s, Freeze);
    CHECK(rig.killer.asked == 3);
    CHECK(rig.Published().level == Wire::PressureLevel::Shed);
    CHECK_FALSE(RoundOwed(rig.wake));
    CHECK(rig.logger.Snapshot().size() == lines);
    CHECK(rig.ceiling.applied.size() == writes);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureShedEntries) == 1);
}

TEST_CASE("Without a cgroup the watcher reports the level and allowance and writes no ceiling", "[node][pressure]")
{
    Rig rig { false };
    rig.Started();
    // Its start line says what it cannot do.
    CHECK(std::ranges::any_of(rig.logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Info && record.message.contains("changes only what this machine reports");
    }));

    rig.Step(1s, GameStall);
    CHECK(rig.Published().level == Wire::PressureLevel::Yield);
    // No kernel figure to read: what compiles were charged is the allowance.
    CHECK(rig.Published().allowanceBytes == rig.held.held.bytes);
    CHECK(RoundOwed(rig.wake));
    rig.Step(1s, Freeze);
    CHECK(rig.Published().level == Wire::PressureLevel::Shed);

    // Neither fake was reached: there is nothing to write and nothing to kill.
    CHECK(rig.ceiling.applied.empty());
    CHECK(rig.killer.asked == 0);
}

TEST_CASE("Without PSI the watcher does not start, says why once at Info and raises nothing", "[node][pressure]")
{
    SECTION("a kernel that offers none")
    {
        Rig rig;
        rig.source->next = std::unexpected { std::string { "/proc/pressure/memory: No such file or directory" } };

        auto const started = rig.watcher->Start(false);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().contains("/proc/pressure/memory"));
        CHECK(rig.conditions.StateOf(NodeCondition::MemoryPressureShedding) == Wire::ConditionState::NotEvaluated);
        // M4: a stated no-op, not a fault -- once, at Info.
        CHECK(rig.Lines(LogLevel::Warn) == 0);
        CHECK(rig.Lines(LogLevel::Info) == 1);
        // Nothing published: absent, which a reader must not take for calm.
        CHECK_FALSE(rig.published.Current().has_value());
    }
    SECTION("a platform that has none: never sampled, and said with the platform's reason")
    {
        Rig rig { true, false };
        // Would succeed if asked: the platform answer comes first.
        auto const started = rig.watcher->Start(false);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().contains("this platform has no memory pressure stall information"));
        CHECK(rig.conditions.StateOf(NodeCondition::MemoryPressureShedding) == Wire::ConditionState::NotEvaluated);
        CHECK(rig.Lines(LogLevel::Warn) == 0);
        CHECK(rig.Lines(LogLevel::Info) == 1);
        CHECK_FALSE(rig.published.Current().has_value());
    }
}

TEST_CASE("The compiles' pressure going unreadable is said on its transitions, never per sample", "[node][pressure]")
{
    Rig rig;
    rig.Started();
    auto faulted = SampleOf(Quiet);
    faulted.compiles.reset();
    faulted.compilesFault = "compiles/memory.pressure: Permission denied";

    auto const warns = [&rig] {
        return rig.Lines(LogLevel::Warn);
    };
    for ([[maybe_unused]] auto const sample: std::views::iota(0, 3))
    {
        rig.clock.advance(1s);
        rig.source->next = faulted;
        rig.watcher->StepOnce();
    }
    CHECK(warns() == 1);

    auto const infos = rig.Lines(LogLevel::Info);
    rig.Step(1s, Quiet);
    rig.Step(1s, Quiet);
    CHECK(rig.Lines(LogLevel::Info) == infos + 1);

    // And again on the next fault: a transition, not a first time.
    rig.clock.advance(1s);
    rig.source->next = faulted;
    rig.watcher->StepOnce();
    CHECK(warns() == 2);
}

TEST_CASE("The time in each acting level is counted in whole seconds against the level held", "[node][pressure]")
{
    Rig rig;
    rig.Started();
    rig.Step(3s, Quiet);
    rig.Step(1s, GameStall); // four seconds of calm, counted nowhere: calm is the rest
    rig.Step(12s, Freeze);   // twelve of yield, ending in a shed
    rig.Step(2500ms, Freeze);
    rig.Step(1s, Freeze); // three and a half of shed: three counted, the half carried

    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureYieldSeconds) == 12);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureShedSeconds) == 3);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureYieldEntries) == 1);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeMemoryPressureShedEntries) == 1);
}

TEST_CASE("A reload moving a threshold or the ceiling reaches the next step", "[node][pressure][reload]")
{
    Rig rig;
    rig.Started();
    constexpr auto Mild = Psi { .some = 7.0, .full = 0.0, .compiles = 0.0 };
    rig.Step(1s, Mild);
    CHECK(rig.Published().level == Wire::PressureLevel::Calm);

    rig.settings.settings.thresholds.yieldEnter = 6.0;
    rig.settings.settings.thresholds.yieldExit = 3.0;
    rig.Step(1s, Mild);
    CHECK(rig.Published().level == Wire::PressureLevel::Yield);

    CHECK(rig.ceiling.applied.back() == Holding);

    // A lowered ceiling clamps the allowance at once: down to the configuration, where it lowers nothing more, so
    // the ceiling is handed none and writes the configured 8 GiB.
    rig.settings.settings.configuredBytes = 8 * GiB;
    rig.Step(1s, Mild);
    CHECK_FALSE(rig.Published().allowanceBytes.has_value());
    CHECK(rig.ceiling.applied.back() == std::nullopt);
}

TEST_CASE("A raise reaches a never-pressured watcher at once, and one after a yield ramps and is said",
          "[node][pressure][reload]")
{
    // I2. An allowance pressure never lowered IS the configuration: a raise moves the ceiling and the budget at once.
    Rig rig;
    rig.Started();
    rig.Step(1s, Quiet);
    rig.settings.settings.configuredBytes = 96 * GiB;
    rig.Step(1s, Quiet);
    CHECK_FALSE(rig.Published().allowanceBytes.has_value());
    CHECK(rig.ceiling.applied.empty());
    CHECK(rig.Lines(LogLevel::Info) == 1); // the start line alone: nothing was held back

    // After a yield, the lowered allowance stays and ramps: said once at Info.
    rig.Step(1s, GameStall);
    rig.Step(10s, Quiet);
    REQUIRE(rig.Published().level == Wire::PressureLevel::Calm);
    REQUIRE(rig.Published().allowanceBytes == Holding);
    rig.settings.settings.configuredBytes = 128 * GiB;
    rig.Step(1s, Quiet);
    CHECK(rig.Published().allowanceBytes == Holding);
    CHECK(std::ranges::any_of(rig.logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Info && record.message.contains("compile ceiling rose to");
    }));
    rig.Step(30s, Quiet);
    CHECK(rig.Published().allowanceBytes == Holding + (128 * GiB * 5 / 100));
}

TEST_CASE("The allowance a yield sets is never below what the machine's compiles are charged", "[node][pressure]")
{
    // M6. The budget is compared against the CHARGED bytes -- local leases included, which run outside compiles/ --
    // so an allowance taken from compiles/memory.current alone would leave the queue admitting nothing until the
    // ramp caught up. The larger of the two is taken; the ceiling never exceeds the configuration either way.
    Rig rig;
    rig.held.held.bytes = 30 * GiB; // more than compiles/ is resident in (Holding, 20 GiB)
    rig.Started();
    rig.Step(1s, GameStall);
    CHECK(rig.Published().allowanceBytes == 30 * GiB);
    CHECK(rig.ceiling.applied.back() == 30 * GiB);

    // And charged bytes past the configuration lower nothing: the allowance stays the configuration.
    Rig over;
    over.held.held.bytes = 2 * Configured;
    over.Started();
    over.Step(1s, GameStall);
    CHECK(over.Published().level == Wire::PressureLevel::Yield);
    CHECK_FALSE(over.Published().allowanceBytes.has_value());
    CHECK(over.ceiling.applied.empty());
}

TEST_CASE("The pressure flags map to the governor's thresholds, and zero turns a level off", "[node][pressure][config]")
{
    auto const defaults = MemoryPressureThresholdsOf(NodeConfig {});
    CHECK(defaults == PressureThresholds {});

    auto cfg = NodeConfig {};
    cfg.memoryPressureYieldPercent = 20;
    cfg.memoryPressureShedPercent = 10;
    auto const moved = MemoryPressureThresholdsOf(cfg);
    CHECK(moved.yieldEnter == 20.0);
    CHECK(moved.yieldExit == 10.0);
    CHECK(moved.shedEnter == 10.0);
    CHECK(moved.shedExit == 2.0);

    cfg.memoryPressureYieldPercent = 0;
    cfg.memoryPressureShedPercent = 0;
    auto const off = MemoryPressureThresholdsOf(cfg);
    CHECK(std::isinf(off.yieldEnter));
    CHECK(std::isinf(off.shedEnter));
}

namespace
{

/// A cgroup tree in which everything works: a delegated cgroup offering memory, holding only the node.
[[nodiscard]] std::shared_ptr<Testing::FakeCgroupHost> DelegatedCgroups()
{
    auto host = std::make_shared<Testing::FakeCgroupHost>();
    host->files["/proc/self/cgroup"] = "0::/system.slice/pace-node.service\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.controllers"] = "memory\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.procs"] = "4242\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/node/memory.peak"] = "1\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.current"] = "1073741824\n";
    return host;
}

/// The figures written to `compiles/memory.max`, in order.
[[nodiscard]] std::vector<std::uint64_t> CeilingsWritten(Testing::FakeCgroupHost const& host)
{
    auto const prefix = std::string { "write /sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.max " };
    auto written = std::vector<std::uint64_t> {};
    for (auto const& entry: host.trail)
        if (entry.starts_with(prefix))
            written.push_back(std::stoull(entry.substr(prefix.size())));
    return written;
}

/// The configuration in force, as a reload changes it.
struct InForce final: INodeConfigSource
{
    NodeConfig cfg;
    [[nodiscard]] NodeConfig Current() const override
    {
        return cfg;
    }
};

/// What this machine is, as a cache-budget override moves it.
struct Machine final: INodeCapacitySource
{
    Distributed::NodeCapacity capacity { .logicalCores = 8, .totalMemoryBytes = 32 * GiB, .reservedMemoryBytes = 0 };
    [[nodiscard]] Distributed::NodeCapacity Current() const override
    {
        return capacity;
    }
};

/// The production stack, by hand: the meter over a fake cgroup tree, the keeper, and the watcher writing through
/// it -- the allowance in place, as `main` builds it.
struct KeptRig
{
    std::shared_ptr<Testing::FakeCgroupHost> host = DelegatedCgroups();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    InForce inForce;
    Machine machine;
    CompileCeilingKeeper keeper { meter, inForce, machine, conditions, logger };
    CompileCeilingOnReload reload { keeper };
    LivePressureSettings settings { inForce, machine };
    FixedHeld held;
    PublishedPressure published;
    PresenceWake wake;
    core::platform::ManualClock clock;
    ScriptedPressure* source = nullptr;
    std::unique_ptr<PressureWatcher> watcher;

    KeptRig()
    {
        REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);
        static_cast<void>(keeper.Keep(CeilingWrite::Always));
        auto scripted = std::make_unique<ScriptedPressure>();
        source = scripted.get();
        watcher = std::make_unique<PressureWatcher>(PressureWatcherParts { .source = std::move(scripted),
                                                                           .settings = settings,
                                                                           .ceiling = &keeper,
                                                                           .killer = meter.YoungestKiller(),
                                                                           .held = held,
                                                                           .published = published,
                                                                           .wake = wake,
                                                                           .conditions = conditions,
                                                                           .metrics = metrics,
                                                                           .logger = logger,
                                                                           .clock = clock,
                                                                           .poll = 1s });
        REQUIRE(watcher->Start(false).has_value());
    }

    void Step(Psi const& psi)
    {
        clock.advance(1s);
        source->next = SampleOf(psi);
        watcher->StepOnce();
    }
};

} // namespace

TEST_CASE("A reload raising the limit reaches memory.max at once on a never-pressured node", "[node][pressure][reload]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    KeptRig rig;
    rig.inForce.cfg.compileMemoryLimitBytes = 8 * GiB;
    rig.reload.ReloadAccepted();
    rig.Step(Quiet);
    rig.inForce.cfg.compileMemoryLimitBytes = 16 * GiB;
    rig.reload.ReloadAccepted(); // the raise: written by the reload itself, nothing held back
    rig.Step(Quiet);
    rig.Step(Quiet);

    auto const derived = CompileCeilingOf(NodeConfig {}, rig.machine.capacity);
    CHECK(CeilingsWritten(*rig.host) == std::vector<std::uint64_t> { derived, 8 * GiB, 16 * GiB });
}

TEST_CASE("A capacity that moves re-applies the derived ceiling with the pressure allowance in place",
          "[node][pressure][resize][meter]")
{
    // Task 4's capacity case, as production runs it: the watcher writes through the same keeper. Calm, both
    // directions follow at once; after a yield the allowance binds, and a rise waits on the ramp.
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    KeptRig rig;
    auto const at = [&rig](std::uint64_t reserved) {
        rig.machine.capacity.reservedMemoryBytes = reserved;
        static_cast<void>(rig.keeper.Keep(CeilingWrite::OnChange)); // the presence round
        rig.Step(Quiet);                                            // the watcher's next sample
    };
    rig.Step(Quiet);
    at(8 * GiB); // reserve up: derived budget down
    at(0);       // reserve back: up again, at once
    auto const calm = CeilingsWritten(*rig.host);
    CHECK(calm == std::vector<std::uint64_t> { 24 * GiB, 18 * GiB, 24 * GiB });

    // A yield: the allowance is what compiles are charged (12 GiB, above the 1 GiB resident), and a rise waits.
    rig.Step(GameStall);
    rig.machine.capacity.totalMemoryBytes = 64 * GiB;
    static_cast<void>(rig.keeper.Keep(CeilingWrite::OnChange));
    rig.Step(GameStall);
    auto const pressed = CeilingsWritten(*rig.host);
    CHECK(pressed == std::vector<std::uint64_t> { 24 * GiB, 18 * GiB, 24 * GiB, 12 * GiB });
}
