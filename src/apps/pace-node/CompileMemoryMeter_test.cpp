// SPDX-License-Identifier: Apache-2.0
#include "CompileMemoryMeter.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <core/Ranges.hpp>
#include <tests/CgroupFakes.hpp>
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

#endif
