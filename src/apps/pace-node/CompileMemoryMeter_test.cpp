// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "CompileMemoryMeter.hpp"
#include "LiveNodeConfig.hpp"
#include "NodeReload.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/NodePolicy.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <system_error>
#include <vector>

#include <OomVictimExec.hpp>
#include <OomVictimTestSupport.hpp>
#include <core/Ranges.hpp>
#include <tests/CgroupFakes.hpp>
#include <tests/OwnCgroup.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::FakeCgroupHost;
using Pace::Testing::Unwrap;

TEST_CASE("compile-memory-unmeasured is raised with the reason, logged, and cleared when the meter works",
          "[node][memory-meter]")
{
    namespace Wire = CompileCacheWire;
    NodeConditions conditions;
    CapturingLogger logger;

    SECTION("a meter that works answers the row clear and logs nothing")
    {
        ReportCompileMemoryMeter(MeterReport {}, conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUnmeasured) == Wire::ConditionState::Clear);
        CHECK(logger.Snapshot().empty());
    }

    SECTION("one that cannot raises it with the reason, and says the same at Warn")
    {
        ReportCompileMemoryMeter(
            MeterReport { .state = MeterState::Unmeasured, .reason = "the cgroup is not delegated" }, conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUnmeasured) == Wire::ConditionState::Raised);
        auto const records = logger.Snapshot();
        REQUIRE(records.size() == 1);
        CHECK(records.front().level == LogLevel::Warn);
        CHECK(records.front().message.contains("the cgroup is not delegated"));
    }

    SECTION("a node systemd-oomd may still choose is said once at Warn, and the meter still counts as working")
    {
        ReportCompileMemoryMeter(MeterReport { .state = MeterState::Measured,
                                               .reason = {},
                                               .notApplied = {},
                                               .oomdOmitRefused = "/sys/fs/cgroup/x/node: Operation not supported" },
                                 conditions,
                                 logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUnmeasured) == Wire::ConditionState::Clear);
        auto const records = logger.Snapshot();
        REQUIRE(records.size() == 1);
        CHECK(records.front().level == LogLevel::Warn);
        CHECK(records.front().message.contains("user.oomd_omit"));
        CHECK(records.front().message.contains("/sys/fs/cgroup/x/node"));
    }

    SECTION("a platform with nothing to measure with raises nothing: the remedy would not apply there")
    {
        ReportCompileMemoryMeter(
            MeterReport { .state = MeterState::NotApplicable, .reason = "no meter here" }, conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUnmeasured) == Wire::ConditionState::NotEvaluated);
        auto const records = logger.Snapshot();
        REQUIRE(records.size() == 1);
        CHECK(records.front().level == LogLevel::Info);
    }
}

TEST_CASE("Each platform measures a compile the way its kernel allows", "[node][memory-meter]")
{
    CHECK(MeterPlatformTable[static_cast<std::size_t>(MeterPlatform::Windows)].strategy == MeterStrategy::PlatformMeter);
    CHECK(MeterPlatformTable[static_cast<std::size_t>(MeterPlatform::MacOs)].strategy == MeterStrategy::PlatformMeter);
    // The one where the spawner's resident set is folded into the child's: a cgroup, or nothing.
    CHECK(MeterPlatformTable[static_cast<std::size_t>(MeterPlatform::Linux)].strategy == MeterStrategy::JobCgroup);
    CHECK(MeterPlatformTable[static_cast<std::size_t>(MeterPlatform::OtherPosix)].strategy == MeterStrategy::NotMeasurable);
}

#if !defined(_WIN32)

namespace
{

/// A fake cgroup tree of a node that was not given its own cgroup (the shipped unit's case):
/// the cgroup filesystem is read-only to it.
[[nodiscard]] std::shared_ptr<FakeCgroupHost> UndelegatedHost()
{
    auto host = std::make_shared<FakeCgroupHost>();
    host->files["/proc/self/cgroup"] = "0::/system.slice/pace-node.service\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.controllers"] = "memory\n";
    host->unwritable.insert("/sys/fs/cgroup/system.slice/pace-node.service/cgroup.subtree_control");
    return host;
}

} // namespace

TEST_CASE("A node that was given no cgroup reports its compiles' peak unknown, and runs them all the same",
          "[node][memory-meter]")
{
    auto const host = UndelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };

    auto metered = meter.MakeRunner(metrics, logger);

    CHECK(metered.report.state == MeterState::Unmeasured);
    // A build that cannot spawn into a cgroup says that first, and never asks the host at all.
    CHECK(metered.report.reason.contains(Cc::CanSpawnIntoCgroups() ? "not delegated" : "cannot start a process"));
    // Nothing was touched on the way to saying no.
    CHECK(host->trail.empty());

    auto const run = metered.runner->RunCaptureSplit({ { "/bin/sh", "-c", "echo compiled" } });
    CHECK(run.exitCode == 0);
    CHECK(run.out == "compiled\n");
    // The peak that would have been reported otherwise is the node's own resident set, plus the child's.
    CHECK_FALSE(run.peakMemoryBytes.has_value());
}

TEST_CASE("A platform with no meter reports nothing and asks for nothing", "[node][memory-meter]")
{
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { UndelegatedHost(), MeterPlatform::OtherPosix };

    auto metered = meter.MakeRunner(metrics, logger);

    CHECK(metered.report.state == MeterState::NotApplicable);
    auto const run = metered.runner->RunCaptureSplit({ { "/bin/sh", "-c", "echo compiled" } });
    CHECK(run.exitCode == 0);
    CHECK_FALSE(run.peakMemoryBytes.has_value());
}

TEST_CASE("A platform whose child is measured alone uses the runner's own meter", "[node][memory-meter]")
{
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { UndelegatedHost(), MeterPlatform::MacOs };

    auto metered = meter.MakeRunner(metrics, logger);

    CHECK(metered.report.state == MeterState::Measured);
    auto const run = metered.runner->RunCaptureSplit({ { "/bin/sh", "-c", "echo compiled" } });
    CHECK(run.exitCode == 0);
    CHECK(run.peakMemoryBytes.has_value());
}

namespace
{

/// A fake tree in which everything works: a delegated cgroup offering memory, holding only the node.
[[nodiscard]] std::shared_ptr<FakeCgroupHost> DelegatedHost()
{
    auto host = std::make_shared<FakeCgroupHost>();
    host->files["/proc/self/cgroup"] = "0::/system.slice/pace-node.service\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.controllers"] = "memory\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.procs"] = "4242\n";
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/node/memory.peak"] = "1\n";
    return host;
}

/// How many times the trail made @p directory.
[[nodiscard]] std::ptrdiff_t Made(FakeCgroupHost const& host, std::string const& directory)
{
    return std::ranges::count(host.trail, "mkdir " + directory);
}

} // namespace

TEST_CASE("A formation reform rebuilds the worker around a meter that prepared once", "[node][memory-meter]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares one");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    auto const leaf = std::string { "/sys/fs/cgroup/system.slice/pace-node.service/node" };

    auto first = meter.MakeRunner(metrics, logger);
    // The process is now INSIDE the leaf, as after a real preparation: a second preparation would read
    // `.../node` as the node's cgroup and nest `.../node/node`.
    host->files["/proc/self/cgroup"] = "0::/system.slice/pace-node.service/node\n";
    auto second = meter.MakeRunner(metrics, logger);
    auto third = meter.MakeRunner(metrics, logger);

    CHECK(first.report.state == MeterState::Measured);
    CHECK(second.report.state == MeterState::Measured);
    CHECK(third.report.state == MeterState::Measured);
    CHECK(Made(*host, leaf) == 1);
    CHECK_FALSE(host->directories.contains(leaf + "/node"));
    CHECK(std::ranges::count(host->trail, "write " + leaf + "/cgroup.procs 4242") == 1);
}

TEST_CASE("A compile that cannot be placed in its cgroup is counted, and said once", "[node][memory-meter]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so no spawn is ever refused one");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    auto metered = meter.MakeRunner(metrics, logger);
    REQUIRE(metered.report.state == MeterState::Measured);

    // The fake hands out a descriptor that is no cgroup, so the system refuses to start the child in it.
    for (auto const index: std::views::iota(0, 3))
    {
        static_cast<void>(index);
        auto const run = metered.runner->RunCaptureSplit({ { "/bin/sh", "-c", "echo compiled" } });
        CHECK(run.exitCode == 0);
        CHECK_FALSE(run.peakMemoryBytes.has_value());
    }

    CHECK(metrics.Read(IMetricsSink::Counter::WorkerCompilesUnmeasured) == 3);
    auto const warnings = std::ranges::count(logger.Snapshot(), LogLevel::Warn, &CapturingLogger::Record::level);
    CHECK(warnings == 1);
}

namespace
{

constexpr std::uint64_t CeilingGiB = std::uint64_t { 1 } << 30;

/// The `memory.max` writes of @p host's trail, in order.
[[nodiscard]] std::vector<std::string> CeilingWrites(FakeCgroupHost const& host)
{
    auto const prefix = std::string { "write /sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.max " };
    auto writes = std::vector<std::string> {};
    for (auto const& entry: host.trail)
        if (entry.starts_with(prefix))
            writes.push_back(entry.substr(prefix.size()));
    return writes;
}

/// The detail @p conditions carries for @p condition.
[[nodiscard]] std::string DetailOf(NodeConditions const& conditions, NodeCondition condition)
{
    auto const rows = conditions.Snapshot();
    auto const row = std::ranges::find(rows, RowFor(condition).id, &CompileCacheWire::NodeConditionFields::id);
    REQUIRE(row != rows.end());
    return row->detail;
}

/// The configuration in force, as the reloader's snapshot is: a case changes it under the keeper.
struct InForceConfig final: INodeConfigSource
{
    NodeConfig cfg;
    [[nodiscard]] NodeConfig Current() const override
    {
        return cfg;
    }
};

/// What this machine is, as a live cache-budget override moves it.
struct MovableMachine final: INodeCapacitySource
{
    explicit MovableMachine(Distributed::NodeCapacity initial):
        machine { initial }
    {
    }
    Distributed::NodeCapacity machine;
    [[nodiscard]] Distributed::NodeCapacity Current() const override
    {
        return machine;
    }
};

/// A 96 GiB machine with nothing reserved.
[[nodiscard]] Distributed::NodeCapacity CeilingMachine()
{
    return Distributed::NodeCapacity { .logicalCores = 16, .totalMemoryBytes = 96 * CeilingGiB, .reservedMemoryBytes = 0 };
}

} // namespace

TEST_CASE("CompileMemoryMeter.unavailable-cgroups-raise-uncapped", "[node][meter][memory-meter]")
{
    namespace Wire = CompileCacheWire;
    auto const host = UndelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };

    auto const metered = meter.MakeRunner(metrics, logger);
    ReportCompileMemoryMeter(metered.report, conditions, logger);
    PutCompileCeilingInForce(meter, NodeConfig {}, CeilingMachine(), conditions, logger);

    // Two questions with one cause today: nothing measures a compile, and nothing caps them.
    CHECK(conditions.StateOf(NodeCondition::CompileMemoryUnmeasured) == Wire::ConditionState::Raised);
    CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == Wire::ConditionState::Raised);
    // The detail is the preparation's own failure, as `compile-memory-unmeasured` carries it.
    CHECK(DetailOf(conditions, NodeCondition::CompileMemoryUncapped) == metered.report.reason);
    // And no ceiling was written anywhere: there is no compiles/ to write it to.
    CHECK(CeilingWrites(*host).empty());
    CHECK(std::ranges::count(logger.Snapshot(), LogLevel::Warn, &CapturingLogger::Record::level) == 2);
}

TEST_CASE("CompileMemoryMeter.reload-rewrites-the-ceiling", "[node][meter][memory-meter]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);

    CHECK(meter.ApplyCeiling(16 * CeilingGiB).state == CeilingState::Capped);
    CHECK(meter.ApplyCeiling(8 * CeilingGiB).state == CeilingState::Capped);

    // Every application WRITES: a lowered ceiling must not wait for a restart.
    CHECK(CeilingWrites(*host) == std::vector<std::string> { "17179869184", "8589934592" });
}

TEST_CASE("CompileMemoryMeter.accepted-reload-rewrites-the-ceiling-from-the-config-in-force", "[node][meter][memory-meter]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);

    /// The configuration in force, as the reloader's snapshot is: changed under the listener.
    struct SwappableConfig final: INodeConfigSource
    {
        NodeConfig cfg;
        [[nodiscard]] NodeConfig Current() const override
        {
            return cfg;
        }
    } inForce;
    inForce.cfg.compileMemoryLimitBytes = 16 * CeilingGiB;
    FixedNodeCapacity const capacity { CeilingMachine() };
    CompileCeilingKeeper keeper { meter, inForce, capacity, conditions, logger };
    CompileCeilingOnReload ceiling { keeper };

    // Told through the fan-out `main` hands `ApplyReloadRequest`, as the budget applier is beside it.
    struct CountingListener final: IAcceptedReloadListener
    {
        int told = 0;
        void ReloadAccepted() override
        {
            ++told;
        }
    } other;
    AcceptedReloadListeners listeners { { &other, &ceiling } };

    listeners.ReloadAccepted();
    inForce.cfg.compileMemoryLimitBytes = 8 * CeilingGiB;
    listeners.ReloadAccepted();

    CHECK(other.told == 2);
    CHECK(CeilingWrites(*host) == std::vector<std::string> { "17179869184", "8589934592" });
    CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == CompileCacheWire::ConditionState::Clear);
}

TEST_CASE("CompileMemoryMeter.keeper-writes-on-change-and-always-when-told", "[node][meter][memory-meter]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);
    InForceConfig const inForce;
    MovableMachine capacity { CeilingMachine() };
    CompileCeilingKeeper keeper { meter, inForce, capacity, conditions, logger };
    auto shrunk = CeilingMachine();
    shrunk.reservedMemoryBytes = 32 * CeilingGiB;

    static_cast<void>(keeper.Keep(CeilingWrite::Always));   // startup
    static_cast<void>(keeper.Keep(CeilingWrite::OnChange)); // a round: unchanged, nothing written
    capacity.machine = shrunk;
    static_cast<void>(keeper.Keep(CeilingWrite::OnChange)); // a round after the reserve moved
    static_cast<void>(keeper.Keep(CeilingWrite::Always));   // a reload: written whatever it was

    CHECK(CeilingWrites(*host)
          == std::vector<std::string> {
              std::to_string(72 * CeilingGiB), std::to_string(48 * CeilingGiB), std::to_string(48 * CeilingGiB) });
}

TEST_CASE("CompileMemoryMeter.ceiling-in-force-is-said-and-clears-the-row", "[node][meter][memory-meter]")
{
    namespace Wire = CompileCacheWire;
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);

    SECTION("derived: the machine budget")
    {
        PutCompileCeilingInForce(meter, NodeConfig {}, CeilingMachine(), conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == Wire::ConditionState::Clear);
        CHECK(CeilingWrites(*host) == std::vector<std::string> { std::to_string(72 * CeilingGiB) });
        CHECK(std::ranges::contains(logger.Snapshot(),
                                    std::format("compile memory ceiling: {} (derived)", 72 * CeilingGiB),
                                    &CapturingLogger::Record::message));
    }
    SECTION("typed: the flag, as given")
    {
        auto cfg = NodeConfig {};
        cfg.compileMemoryLimitBytes = 24 * CeilingGiB;
        PutCompileCeilingInForce(meter, cfg, CeilingMachine(), conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == Wire::ConditionState::Clear);
        CHECK(CeilingWrites(*host) == std::vector<std::string> { std::to_string(24 * CeilingGiB) });
        CHECK(std::ranges::contains(logger.Snapshot(),
                                    std::format("compile memory ceiling: {} (typed)", 24 * CeilingGiB),
                                    &CapturingLogger::Record::message));
    }
    SECTION("a write the kernel refuses raises the row, naming the file")
    {
        host->writeRefusals["/sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.max"] =
            std::errc::permission_denied;
        PutCompileCeilingInForce(meter, NodeConfig {}, CeilingMachine(), conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == Wire::ConditionState::Raised);
        CHECK(DetailOf(conditions, NodeCondition::CompileMemoryUncapped).contains("memory.max"));

        // And the next write that lands clears it, so the row says it is LIVE: one that clears is not latched.
        host->writeRefusals.clear();
        PutCompileCeilingInForce(meter, NodeConfig {}, CeilingMachine(), conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == Wire::ConditionState::Clear);
        CHECK(RowFor(NodeCondition::CompileMemoryUncapped).persistence == Wire::ConditionPersistence::Live);
    }
}

TEST_CASE("CompileMemoryMeter.ceiling-not-evaluated-off-linux", "[node][meter][memory-meter]")
{
    namespace Wire = CompileCacheWire;
    auto const host = UndelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::MacOs };
    static_cast<void>(meter.MakeRunner(metrics, logger));

    PutCompileCeilingInForce(meter, NodeConfig {}, CeilingMachine(), conditions, logger);

    // No cgroup facility, so no remedy: the row says so rather than asking for one.
    CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == Wire::ConditionState::NotEvaluated);
    CHECK(host->trail.empty());
}

TEST_CASE("CompileMemoryMeter.unapplied-compiles-settings-are-said-once", "[node][meter][memory-meter]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    auto const host = DelegatedHost();
    host->writeRefusals["/sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.swap.max"] =
        std::errc::no_such_file_or_directory;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };

    auto const metered = meter.MakeRunner(metrics, logger);
    REQUIRE(metered.report.state == MeterState::Measured);
    ReportCompileMemoryMeter(metered.report, conditions, logger);

    auto const records = logger.Snapshot();
    REQUIRE(std::ranges::count(records, LogLevel::Warn, &CapturingLogger::Record::level) == 1);
    auto const warning = std::ranges::find(records, LogLevel::Warn, &CapturingLogger::Record::level);
    CHECK(warning->message.contains("compiles may swap"));
    CHECK(warning->message.contains("memory.swap.max"));
}

namespace
{

/// Bytes of this process's memory made resident and kept so.
class ResidentBallast
{
  public:
    explicit ResidentBallast(std::size_t bytes):
        _block { std::make_unique_for_overwrite<unsigned char[]>(bytes) },
        _bytes { bytes }
    {
        constexpr std::size_t PageBytes = 4096;
        for (auto const offset: std::views::iota(std::size_t { 0 }, bytes) | core::ranges::Stride(PageBytes))
            _block[offset] = static_cast<unsigned char>(offset / PageBytes);
    }

    /// @return A byte of the ballast, so the stores cannot be elided.
    [[nodiscard]] unsigned char Sample() const
    {
        return _block[_bytes / 2];
    }

  private:
    std::unique_ptr<unsigned char[]> _block;
    std::size_t _bytes;
};

/// The number of `job-*` cgroups left under @p parent.
[[nodiscard]] std::size_t JobCgroupsUnder(std::filesystem::path const& parent)
{
    std::size_t count = 0;
    for (auto const& entry: std::filesystem::directory_iterator { parent })
        if (entry.path().filename().string().starts_with("job-"))
            ++count;
    return count;
}

constexpr std::uint64_t GiB = std::uint64_t { 1 } << 30;
constexpr std::uint64_t MiB = std::uint64_t { 1 } << 20;

} // namespace

TEST_CASE("A node holding a gigabyte measures a small compile as small, in a cgroup of its own",
          "[node][memory-meter][cgroup]")
{
    // **Opt-in, because the preparation MOVES THIS PROCESS into a cgroup of its own**: harmless in a
    // delegated scope made for the purpose, and an uninvited reshuffle of a developer's session
    // otherwise. Run it as
    //
    //     systemd-run --user --scope -p Delegate=yes env PACE_TEST_CGROUP_DELEGATED=1 pace-node-tests "[cgroup]"
    //
    // With the variable SET, a machine that cannot measure is a FAILURE, not a skip: CI sets it where it
    // provides a delegated cgroup, and a case that skipped there would stay green while the real-kernel
    // path rotted.
    if (!ReadEnvironmentVariable("PACE_TEST_CGROUP_DELEGATED").has_value())
        SKIP("PACE_TEST_CGROUP_DELEGATED is not set: this case needs a delegated, writable cgroup (see the comment)");

    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { std::make_shared<Platform::SystemCgroupHost>() };
    auto metered = meter.MakeRunner(metrics, logger);
    INFO("the meter said: " << metered.report.reason);
    REQUIRE(metered.report.state == MeterState::Measured);

    // The parent whose resident set `ru_maxrss` used to charge to every child.
    ResidentBallast const ballast { GiB + (GiB / 2) };
    REQUIRE(ballast.Sample() != 255);

    auto const trivial = std::vector<std::string> { "/bin/sh", "-c", "echo compiled" };

    // The fix: the child's peak, not the parent's. A gigabyte and a half of the parent in the answer
    // would be the bug this exists to fix.
    auto const small = metered.runner->RunCaptureSplit(trivial);
    REQUIRE(small.exitCode == 0);
    REQUIRE(small.peakMemoryBytes.has_value());
    CHECK(Unwrap(small.peakMemoryBytes) < 128 * MiB);

    // The bug, for the record: the launcher's meter (`wait4`'s `ru_maxrss`) charges the same trivial
    // child with everything its spawner holds, because the kernel folds the old address space's
    // high-water mark into the child at `exec`. If a kernel stops doing that, this fails and the
    // comment on `Cc::MakeProcessRunner()` is the thing to correct.
    auto const legacy = Cc::MakeProcessRunner()->RunCaptureSplit(trivial);
    REQUIRE(legacy.exitCode == 0);
    REQUIRE(legacy.peakMemoryBytes.has_value());
    CHECK(Unwrap(legacy.peakMemoryBytes) >= GiB);

    // A descendant's memory still counts: the shell stays small and a process it starts holds ~120 MB.
    auto const heavy = metered.runner->RunCaptureSplit(
        { { "/bin/sh", "-c", "x=$(head -c 120000000 /dev/zero | tr '\\0' a); echo ${#x}" } });
    REQUIRE(heavy.exitCode == 0);
    REQUIRE(heavy.peakMemoryBytes.has_value());
    CHECK(Unwrap(heavy.peakMemoryBytes) >= 100 * MiB);
    CHECK(Unwrap(heavy.peakMemoryBytes) < GiB);

    // A descendant the compiler leaves running dies with the compile, and the job's cgroup goes with it:
    // `rmdir` only succeeds on an empty cgroup, so the directory being gone is the proof.
    auto const own = Unwrap(Platform::SystemCgroupHost {}.ReadFile("/proc/self/cgroup"));
    auto const ownPath = Unwrap(Platform::ParseUnifiedCgroupPath(own));
    auto const jobsParent = (std::filesystem::path { Platform::DefaultCgroupMount } / ownPath.substr(1)).parent_path();
    auto const straggler =
        metered.runner->RunCaptureSplit({ { "/bin/sh", "-c", "sleep 30 >/dev/null 2>&1 & echo started" } });
    CHECK(straggler.exitCode == 0);
    CHECK(JobCgroupsUnder(jobsParent) == 0);
}

TEST_CASE("CompileMemoryMeter.round-after-a-reload-never-writes-the-older-ceiling-back",
          "[node][meter][memory-meter][reload]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    // C2. A presence round that read the configuration BEFORE a reload, and wrote AFTER the reload's own write, put
    // the old, higher ceiling back for an interval -- a limit above its configured value. Every writer now reads the
    // pair under the lock it writes under, so the round that comes second writes what is in force then, or nothing.
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);
    InForceConfig inForce;
    inForce.cfg.compileMemoryLimitBytes = 16 * CeilingGiB;
    MovableMachine const capacity { CeilingMachine() };
    CompileCeilingKeeper keeper { meter, inForce, capacity, conditions, logger };
    CompileCeilingOnReload reload { keeper };

    static_cast<void>(keeper.Keep(CeilingWrite::Always)); // startup: 16g
    // The round begins: under the old code it read its pair HERE, before the reload, and held it.
    auto const roundStartedWith = inForce.Current();
    REQUIRE(roundStartedWith.compileMemoryLimitBytes == 16 * CeilingGiB);
    // The reload lands and writes 8g.
    inForce.cfg.compileMemoryLimitBytes = 8 * CeilingGiB;
    reload.ReloadAccepted();
    // The round now keeps the ceiling: nothing older than 8g may be written, and the budget it reports is 8g's.
    auto const basis = keeper.Keep(CeilingWrite::OnChange);

    CHECK(CeilingWrites(*host) == std::vector<std::string> { "17179869184", "8589934592" });
    CHECK(basis.cfg.compileMemoryLimitBytes == 8 * CeilingGiB);
}

TEST_CASE("CompileMemoryMeter.allowance-lowers-the-ceiling-and-never-raises-it", "[node][meter][memory-meter][pressure]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    // The pressure watcher is the third writer: what is written is min(configured ceiling, allowance).
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConditions conditions;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);
    InForceConfig inForce;
    inForce.cfg.compileMemoryLimitBytes = 16 * CeilingGiB;
    MovableMachine const capacity { CeilingMachine() };
    CompileCeilingKeeper keeper { meter, inForce, capacity, conditions, logger };

    static_cast<void>(keeper.Keep(CeilingWrite::Always));
    keeper.Apply(6 * CeilingGiB);  // a yield: what compiles held
    keeper.Apply(6 * CeilingGiB);  // unchanged: nothing written
    keeper.Apply(40 * CeilingGiB); // an allowance above the configuration writes the configuration
    // A reload lowering the limit below the allowance wins, and the allowance still binds below it.
    inForce.cfg.compileMemoryLimitBytes = 12 * CeilingGiB;
    static_cast<void>(keeper.Keep(CeilingWrite::OnChange));
    keeper.Apply(10 * CeilingGiB);

    CHECK(CeilingWrites(*host)
          == std::vector<std::string> { std::to_string(16 * CeilingGiB),
                                        std::to_string(6 * CeilingGiB),
                                        std::to_string(16 * CeilingGiB),
                                        std::to_string(12 * CeilingGiB),
                                        std::to_string(10 * CeilingGiB) });
    // A ceiling the allowance lowered is said at Debug: the watcher's own line names it when the level changes.
    CHECK(std::ranges::any_of(logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Debug && record.message.contains("memory-pressure allowance");
    }));
    CHECK(conditions.StateOf(NodeCondition::CompileMemoryUncapped) == CompileCacheWire::ConditionState::Clear);
}

TEST_CASE("CompileMemoryMeter.reads-what-compiles-hold-and-hands-out-its-killer", "[node][meter][memory-meter][pressure]")
{
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so the meter never prepares compiles/");
    auto const host = DelegatedHost();
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    CompileMemoryMeter meter { host, MeterPlatform::Linux };
    // Nothing prepared yet: nothing to read and nothing to kill with.
    CHECK_FALSE(meter.CompilesHeldBytes().has_value());
    CHECK(meter.YoungestKiller() == nullptr);

    REQUIRE(meter.MakeRunner(metrics, logger).report.state == MeterState::Measured);
    CHECK(meter.YoungestKiller() != nullptr);
    CHECK_FALSE(meter.CompilesHeldBytes().has_value()); // the kernel said nothing yet
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.current"] = "123456789\n";
    CHECK(meter.CompilesHeldBytes() == std::optional<std::uint64_t> { 123456789 });
    host->files["/sys/fs/cgroup/system.slice/pace-node.service/compiles/memory.current"] = "max\n";
    CHECK_FALSE(meter.CompilesHeldBytes().has_value());
}

#endif

#if defined(__linux__)

namespace
{

/// A cgroup tree held in memory -- except that every job directory OPENS as this process's own cgroup, so the
/// runner's child really runs inside a cgroup and the runner keeps the job, while nothing on the machine is made,
/// written or removed.
class OwnCgroupBackedHost final: public Platform::ICgroupHost
{
  public:
    explicit OwnCgroupBackedHost(int ownDescriptor):
        own { ownDescriptor }
    {
    }
    FakeCgroupHost tree; ///< Everything but the descriptor.
    int own;             ///< This process's own cgroup, borrowed.

    [[nodiscard]] std::expected<std::string, std::error_code> ReadFile(std::filesystem::path const& path) const override
    {
        return tree.ReadFile(path);
    }
    [[nodiscard]] std::expected<void, std::error_code> WriteFile(std::filesystem::path const& path,
                                                                 std::string_view text) const override
    {
        return tree.WriteFile(path, text);
    }
    [[nodiscard]] std::expected<void, std::error_code> MakeDirectory(std::filesystem::path const& path) const override
    {
        return tree.MakeDirectory(path);
    }
    [[nodiscard]] std::expected<void, std::error_code> RemoveCgroupDirectory(
        std::filesystem::path const& path) const override
    {
        return tree.RemoveCgroupDirectory(path);
    }
    [[nodiscard]] std::expected<std::vector<std::string>, std::error_code> ListDirectory(
        std::filesystem::path const& path) const override
    {
        return tree.ListDirectory(path);
    }
    [[nodiscard]] std::expected<int, std::error_code> OpenDirectory(std::filesystem::path const& /*path*/) const override
    {
        return own;
    }
    void CloseDescriptor(int /*descriptor*/) const noexcept override
    {
        // Borrowed from `OwnCgroupDirectory`, which closes it.
    }
    [[nodiscard]] bool IsWritable(std::filesystem::path const& path) const override
    {
        return tree.IsWritable(path);
    }
    [[nodiscard]] std::expected<void, std::error_code> ProbeSpawnInto(int directory) const override
    {
        return tree.ProbeSpawnInto(directory);
    }
    [[nodiscard]] std::uint32_t OwnProcessId() const noexcept override
    {
        return tree.OwnProcessId();
    }
    void Pause(std::chrono::microseconds duration) const override
    {
        tree.Pause(duration);
    }
    [[nodiscard]] std::expected<void, std::error_code> SetExtendedAttribute(std::filesystem::path const& path,
                                                                            std::string_view name,
                                                                            std::string_view value) const override
    {
        return tree.SetExtendedAttribute(path, name, value);
    }
};

/// The node's job source, as the governor's kill reaches it: each job is chosen as the youngest the moment it is
/// made -- before its compiler starts -- and the spawns the system refused to place are counted.
class SheddingJobSource final: public Platform::IJobCgroupSource
{
  public:
    explicit SheddingJobSource(std::shared_ptr<Platform::CgroupJobSource> source):
        inner { std::move(source) }
    {
    }
    [[nodiscard]] std::expected<std::unique_ptr<Platform::IJobCgroup>, std::error_code> Create() override
    {
        auto made = inner->Create();
        if (made.has_value())
            killed = inner->KillYoungest();
        return made;
    }
    void NoteUnmeasured(Unmeasured why) noexcept override
    {
        if (why == Unmeasured::SpawnRefused)
            ++refused;
    }
    std::shared_ptr<Platform::CgroupJobSource> inner; ///< The node's own source.
    bool killed = false;                              ///< Whether `KillYoungest` chose the job.
    std::atomic<int> refused { 0 };                   ///< Spawns that ran outside the cgroup.
};

} // namespace

TEST_CASE("A compile KillYoungest chose is reported shed by the runner that reaps it", "[node][memory-meter][KillYoungest]")
{
    // C1, the production half: the mark the runner reads (`IJobCgroup::Shed`) is the one `KillYoungest` sets, on
    // the job object the runner holds. The kill itself is the child's own `kill -9`: the fake tree swallows the
    // `cgroup.kill` write, so nothing is killed on this machine but the child the case started.
    if (!Cc::CanSpawnIntoCgroups())
        SKIP("this build cannot spawn into a cgroup, so its runner never keeps a job to read");
    Testing::OwnCgroupDirectory const own;
    if (own.Descriptor() < 0)
        SKIP("this process's own cgroup v2 directory cannot be opened");
    auto host = std::make_shared<OwnCgroupBackedHost>(own.Descriptor());
    host->tree.files["/proc/self/cgroup"] = "0::/system.slice/pace-node.service\n";
    host->tree.files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.controllers"] = "memory\n";
    host->tree.files["/sys/fs/cgroup/system.slice/pace-node.service/cgroup.procs"] = "4242\n";
    host->tree.files["/sys/fs/cgroup/system.slice/pace-node.service/node/memory.peak"] = "1\n";
    // The kernel says populated once a compiler runs in the job; the fake tree says it from the start, since this
    // case chooses the victim between `Create` and the spawn -- a moment the real kernel would answer empty.
    host->tree.files["/sys/fs/cgroup/system.slice/pace-node.service/compiles/job-1/cgroup.events"] = "populated 1\n";
    auto const layout = Platform::PrepareJobCgroups(*host);
    REQUIRE(layout.has_value());
    auto const source = std::make_shared<SheddingJobSource>(std::make_shared<Platform::CgroupJobSource>(host, *layout));
    auto const runner = Cc::MakeProcessRunner(source, std::nullopt);

    auto const run = runner->RunCaptureSplit({ { "/bin/sh", "-c", "kill -9 $$" } });
    if (source->refused != 0)
        SKIP("the system refused to start a child inside this process's own cgroup");

    CHECK(source->killed);
    // Twice: the shed's kill, then the teardown's every finished job gets.
    CHECK(std::ranges::count_if(host->tree.trail,
                                [](std::string const& entry) { return entry.ends_with("/job-1/cgroup.kill 1"); })
          == 2);
    CHECK(run.exitCode == 137);
    REQUIRE(run.killed.has_value());
    CHECK(Unwrap(run.killed) == Platform::CompileKill::Shed);
}

TEST_CASE("Every runner a meter given the OOM-victim shim makes runs its compilers at oom_score_adj 1000",
          "[node][memory-meter][oom]")
{
    // Layer A's production wiring: `main` hands the shim to the meter, and the meter to BOTH runner forms it can
    // pick -- the cgroup form on a delegated node, the plain form on one that was given no cgroup. Dropping it from
    // either branch turns Layer A off on those nodes and nothing else notices.
    using namespace Cc::OomVictimTesting;
    if (OwnScore() == Cc::OomVictimScore)
        SKIP("this test process already runs at oom_score_adj 1000, so a child inheriting it proves nothing");
    auto const victim = Cc::OomVictim { .self = ShimPath() };

    struct Form
    {
        std::shared_ptr<FakeCgroupHost> host;
        MeterState state; ///< Which form the meter picked: `Measured` is the cgroup form, `Unmeasured` the plain one.
    };
    auto forms = std::vector<Form> { { .host = UndelegatedHost(), .state = MeterState::Unmeasured } };
    if (Cc::CanSpawnIntoCgroups())
        forms.push_back({ .host = DelegatedHost(), .state = MeterState::Measured });

    for (auto const& form: forms)
    {
        AtomicMetricsSink metrics;
        CapturingLogger logger;
        CompileMemoryMeter meter { form.host, MeterPlatform::Linux, victim };
        auto metered = meter.MakeRunner(metrics, logger);
        CAPTURE(metered.report.reason);
        REQUIRE(metered.report.state == form.state);

        auto const run = metered.runner->RunCaptureSplit({ { "/bin/sh", "-c", ReadScoreTwice } });

        INFO(run.err);
        REQUIRE(run.exitCode == 0);
        CHECK(LinesOf(run.out) == std::vector<std::string> { "1000", "1000" });
    }
}

#endif
