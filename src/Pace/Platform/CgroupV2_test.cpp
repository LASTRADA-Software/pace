// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/CgroupV2.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <tests/CgroupFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace::Platform;
using Pace::Testing::FakeCgroupHost;
using Pace::Testing::Unwrap;

namespace
{

/// Make a job cgroup, or fail the case.
/// @param source Where it comes from.
/// @return The job.
[[nodiscard]] std::unique_ptr<IJobCgroup> NewJob(CgroupJobSource& source)
{
    return source.Create().value();
}

constexpr auto OwnCgroup = std::string_view { "/system.slice/pace-node.service" };
constexpr auto Delegated = std::string_view { "/sys/fs/cgroup/system.slice/pace-node.service" };

/// A host in which everything works: a delegated cgroup offering memory.
[[nodiscard]] std::unique_ptr<FakeCgroupHost> DelegatedHost()
{
    auto host = std::make_unique<FakeCgroupHost>();
    host->files["/proc/self/cgroup"] = "0::" + std::string { OwnCgroup } + "\n";
    host->files[std::string { Delegated } + "/cgroup.controllers"] = "cpu memory pids\n";
    host->files[std::string { Delegated } + "/cgroup.procs"] = "4242\n";
    host->files[std::string { Delegated } + "/node/memory.peak"] = "1048576\n";
    return host;
}

} // namespace

TEST_CASE("The unified hierarchy line of /proc/self/cgroup names the cgroup", "[platform][cgroup]")
{
    SECTION("a v2-only host")
    {
        CHECK(Unwrap(ParseUnifiedCgroupPath("0::/user.slice/session-3.scope\n")) == "/user.slice/session-3.scope");
    }
    SECTION("a hybrid host lists v1 controllers first and the unified line among them")
    {
        CHECK(Unwrap(ParseUnifiedCgroupPath("12:memory:/legacy\n1:name=systemd:/legacy\n0::/unified\n")) == "/unified");
    }
    SECTION("the root of a cgroup namespace is a path like any other")
    {
        CHECK(Unwrap(ParseUnifiedCgroupPath("0::/\n")) == "/");
    }
    SECTION("a cgroup removed under a live process loses the kernel's suffix")
    {
        CHECK(Unwrap(ParseUnifiedCgroupPath("0::/gone (deleted)\n")) == "/gone");
    }
}

TEST_CASE("A /proc/self/cgroup with no usable unified line is a stated cause", "[platform][cgroup]")
{
    for (auto const document: { std::string_view {},
                                std::string_view { "1:name=systemd:/x\n" },
                                std::string_view { "0::\n" },
                                std::string_view { "0::relative\n" },
                                std::string_view { "0::/a/../../b\n" } })
    {
        auto const parsed = ParseUnifiedCgroupPath(document);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().cause == CgroupUnavailable::NoUnifiedHierarchy);
    }
}

TEST_CASE("A delegated cgroup is prepared: leaf, move, memory, in that order", "[platform][cgroup]")
{
    auto const host = DelegatedHost();

    auto const layout = Unwrap(PrepareJobCgroups(*host));

    CHECK(layout.delegated == Delegated);
    CHECK(layout.leaf == std::string { Delegated } + "/node");
    // The process leaves the parent BEFORE the controller is enabled for its children: the other order is
    // refused by the kernel's no-internal-processes rule.
    REQUIRE(host->trail.size() == 3);
    CHECK(host->trail[0] == "mkdir " + std::string { Delegated } + "/node");
    CHECK(host->trail[1] == "write " + std::string { Delegated } + "/node/cgroup.procs 4242");
    CHECK(host->trail[2] == "write " + std::string { Delegated } + "/cgroup.subtree_control +memory");
    // The probe's descriptor is not left open.
    CHECK(host->closed.size() == 1);
}

TEST_CASE("A cgroup namespace root is mounted where the mount is", "[platform][cgroup]")
{
    auto const host = std::make_unique<FakeCgroupHost>();
    host->files["/proc/self/cgroup"] = "0::/\n";
    host->files["/sys/fs/cgroup/cgroup.controllers"] = "memory\n";
    host->files["/sys/fs/cgroup/cgroup.procs"] = "4242\n";
    host->files["/sys/fs/cgroup/node/memory.peak"] = "1\n";

    auto const layout = Unwrap(PrepareJobCgroups(*host));

    CHECK(layout.delegated == "/sys/fs/cgroup");
    CHECK(layout.leaf == "/sys/fs/cgroup/node");
}

TEST_CASE("A leaf left by an earlier start is reused", "[platform][cgroup]")
{
    auto const host = DelegatedHost();
    host->directories.insert(std::string { Delegated } + "/node");

    CHECK(PrepareJobCgroups(*host).has_value());
}

namespace
{
struct FailureCase
{
    std::string_view name;
    void (*script)(FakeCgroupHost&);
    CgroupUnavailable expected;
};

constexpr auto Node = std::string_view { "/sys/fs/cgroup/system.slice/pace-node.service" };
} // namespace

TEST_CASE("Every way the facility cannot start is its own stated cause, and touches nothing it should not",
          "[platform][cgroup]")
{
    auto const failures = std::vector<FailureCase> {
        { .name = "no /proc/self/cgroup",
          .script = [](FakeCgroupHost& host) { host.files.erase("/proc/self/cgroup"); },
          .expected = CgroupUnavailable::NoUnifiedHierarchy },
        { .name = "a v1 host: the mount has no controllers file",
          .script = [](FakeCgroupHost& host) { host.files.erase(std::string { Node } + "/cgroup.controllers"); },
          .expected = CgroupUnavailable::NoUnifiedHierarchy },
        { .name = "memory is not offered",
          .script = [](FakeCgroupHost& host) { host.files[std::string { Node } + "/cgroup.controllers"] = "cpu pids\n"; },
          .expected = CgroupUnavailable::NoMemoryController },
        { .name = "a controller named memory-like is not memory",
          .script = [](FakeCgroupHost& host) { host.files[std::string { Node } + "/cgroup.controllers"] = "memoryx\n"; },
          .expected = CgroupUnavailable::NoMemoryController },
        { .name = "not delegated: the control file is not writable (ProtectControlGroups=yes)",
          .script = [](FakeCgroupHost& host) { host.unwritable.insert(std::string { Node } + "/cgroup.subtree_control"); },
          .expected = CgroupUnavailable::NotDelegated },
        { .name = "a terminal's scope: writable, but a shell lives in it",
          .script = [](FakeCgroupHost& host) { host.files[std::string { Node } + "/cgroup.procs"] = "4242\n9001\n"; },
          .expected = CgroupUnavailable::SharedCgroup },
        { .name = "the leaf cannot be made",
          .script = [](FakeCgroupHost& host) { host.unmakeable.insert(std::string { Node } + "/node"); },
          .expected = CgroupUnavailable::LeafNotCreated },
    };
    for (auto const& failure: failures)
    {
        DYNAMIC_SECTION(failure.name)
        {
            auto const host = DelegatedHost();
            failure.script(*host);

            auto const layout = PrepareJobCgroups(*host);

            REQUIRE_FALSE(layout.has_value());
            CHECK(layout.error().cause == failure.expected);
            // A host that cannot do it is left exactly as it was: nothing created, nothing moved.
            CHECK(host->trail.empty());
        }
    }
}

TEST_CASE("A failure after the node moved is reported at the step that failed", "[platform][cgroup]")
{
    auto const leaf = std::string { Node } + "/node";
    SECTION("the node cannot move itself")
    {
        auto const host = DelegatedHost();
        host->writeRefusals[leaf + "/cgroup.procs"] = std::errc::permission_denied;
        auto const layout = PrepareJobCgroups(*host);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().cause == CgroupUnavailable::ProcessNotMoved);
    }
    SECTION("the memory controller is refused: other processes still live in the parent")
    {
        auto const host = DelegatedHost();
        host->writeRefusals[std::string { Node } + "/cgroup.subtree_control"] = std::errc::device_or_resource_busy;
        auto const layout = PrepareJobCgroups(*host);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().cause == CgroupUnavailable::MemoryNotEnabled);
    }
    SECTION("the controller was written and is not there: a service manager that does not delegate rewrote it")
    {
        auto const host = DelegatedHost();
        host->files[std::string { Node } + "/cgroup.subtree_control"] = "cpu pids\n";
        auto const layout = PrepareJobCgroups(*host);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().cause == CgroupUnavailable::MemoryNotEnabled);
    }
    SECTION("a kernel older than 5.19 has no memory.peak")
    {
        auto const host = DelegatedHost();
        host->files.erase(leaf + "/memory.peak");
        auto const layout = PrepareJobCgroups(*host);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().cause == CgroupUnavailable::PeakNotReadable);
    }
    SECTION("the system refuses clone3 into a cgroup")
    {
        auto const host = DelegatedHost();
        host->spawnProbe = std::errc::operation_not_supported;
        auto const layout = PrepareJobCgroups(*host);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().cause == CgroupUnavailable::SpawnIntoRefused);
        CHECK(host->closed.size() == 1);
        // Asked first: a refusal here leaves the tree untouched, nothing created and nobody moved.
        CHECK(host->trail.empty());
    }
}

TEST_CASE("Every cause has a distinct id and a reason, and a failure describes itself with its detail", "[platform][cgroup]")
{
    auto ids = std::set<std::string_view> {};
    for (auto const& row: CgroupUnavailableTable)
    {
        CHECK_FALSE(row.id.empty());
        CHECK_FALSE(row.reason.empty());
        CHECK(ids.insert(row.id).second);
    }
    CHECK(CgroupFailure { .cause = CgroupUnavailable::NotDelegated, .detail = "/x" }.Describe().ends_with(": /x"));
    CHECK(CgroupFailure { .cause = CgroupUnavailable::NotDelegated, .detail = {} }.Describe()
          == RowFor(CgroupUnavailable::NotDelegated).reason);
}

TEST_CASE("A job's cgroup is a fresh numbered directory, read for its peak, then killed and removed", "[platform][cgroup]")
{
    auto host = DelegatedHost();
    auto* const raw = host.get();
    auto const layout = Unwrap(PrepareJobCgroups(*host));
    raw->trail.clear();
    CgroupJobSource source { std::move(host), layout };

    {
        auto first = NewJob(source);
        auto second = NewJob(source);
        CHECK(first->DirectoryDescriptor() != second->DirectoryDescriptor());
        raw->files[layout.delegated.generic_string() + "/job-1/memory.peak"] = "123456\n";
        CHECK(Unwrap(first->PeakBytes()) == 123456);
        // No file, no answer: never a made-up figure.
        CHECK_FALSE(second->PeakBytes().has_value());
    }

    // Destroyed in reverse: job-2 first. The kill precedes the removal, for each.
    REQUIRE(raw->trail.size() == 6);
    CHECK(raw->trail[0] == "mkdir " + layout.delegated.generic_string() + "/job-1");
    CHECK(raw->trail[1] == "mkdir " + layout.delegated.generic_string() + "/job-2");
    CHECK(raw->trail[2] == "write " + layout.delegated.generic_string() + "/job-2/cgroup.kill 1");
    CHECK(raw->trail[3] == "rmdir " + layout.delegated.generic_string() + "/job-2");
    CHECK(raw->trail[4] == "write " + layout.delegated.generic_string() + "/job-1/cgroup.kill 1");
    CHECK(raw->trail[5] == "rmdir " + layout.delegated.generic_string() + "/job-1");
}

TEST_CASE("A peak the kernel reports as zero or as text is not a measurement", "[platform][cgroup]")
{
    auto host = DelegatedHost();
    auto* const raw = host.get();
    auto const layout = Unwrap(PrepareJobCgroups(*host));
    CgroupJobSource source { std::move(host), layout };
    auto job = NewJob(source);

    for (auto const text:
         { std::string_view { "0\n" }, std::string_view { "max\n" }, std::string_view { "" }, std::string_view { "12x\n" } })
    {
        raw->files[layout.delegated.generic_string() + "/job-1/memory.peak"] = std::string { text };
        CHECK_FALSE(job->PeakBytes().has_value());
    }
}

TEST_CASE("A job's cgroup that is still draining is retried until it empties, and given up on after the policy",
          "[platform][cgroup]")
{
    auto host = DelegatedHost();
    auto* const raw = host.get();
    auto const layout = Unwrap(PrepareJobCgroups(*host));
    raw->busyRemovals.insert(layout.delegated.generic_string() + "/job-1");

    SECTION("it empties in time")
    {
        raw->busyFor = 3;
        CgroupJobSource source { std::move(host),
                                 layout,
                                 CgroupTeardownPolicy { .attempts = 10, .pause = std::chrono::microseconds { 1 } } };
        {
            auto const job = NewJob(source);
        }
        CHECK(raw->removalAttempts == 4);
        CHECK(raw->pauses == 3);
    }
    SECTION("it does not, and the directory is left for the service manager")
    {
        raw->busyFor = 1000;
        CgroupJobSource source { std::move(host),
                                 layout,
                                 CgroupTeardownPolicy { .attempts = 5, .pause = std::chrono::microseconds { 1 } } };
        {
            auto const job = NewJob(source);
        }
        CHECK(raw->removalAttempts == 5);
        CHECK(raw->pauses == 4);
    }
}

TEST_CASE("A name an earlier node left behind is skipped, never adopted", "[platform][cgroup]")
{
    auto host = DelegatedHost();
    auto* const raw = host.get();
    auto const layout = Unwrap(PrepareJobCgroups(*host));
    raw->directories.insert(layout.delegated.generic_string() + "/job-1");
    CgroupJobSource source { std::move(host), layout };

    auto const job = NewJob(source);

    CHECK(raw->directories.contains(layout.delegated.generic_string() + "/job-2"));
}

TEST_CASE("Job cgroups an earlier node left behind are swept at preparation, however many", "[platform][cgroup]")
{
    auto host = DelegatedHost();
    auto* const raw = host.get();
    for (auto const index: std::views::iota(1, 21))
        raw->directories.insert(std::string { Delegated } + "/job-" + std::to_string(index));
    raw->directories.insert(std::string { Delegated } + "/not-a-job");

    auto const layout = Unwrap(PrepareJobCgroups(*host));

    // Killed, then removed; anything that is not a job is left alone.
    for (auto const index: std::views::iota(1, 21))
    {
        CHECK_FALSE(raw->directories.contains(std::string { Delegated } + "/job-" + std::to_string(index)));
        CHECK(std::ranges::contains(
            raw->trail, "write " + std::string { Delegated } + "/job-" + std::to_string(index) + "/cgroup.kill 1"));
    }
    CHECK(raw->directories.contains(std::string { Delegated } + "/not-a-job"));

    CgroupJobSource source { std::move(host), layout };
    auto const job = NewJob(source);
    CHECK(raw->directories.contains(std::string { Delegated } + "/job-1"));
}

TEST_CASE("A stale job cgroup that will not empty makes the facility unavailable, naming it", "[platform][cgroup]")
{
    auto const host = DelegatedHost();
    host->directories.insert(std::string { Delegated } + "/job-7");
    host->busyRemovals.insert(std::string { Delegated } + "/job-7");
    host->busyFor = 1000000;

    auto const layout = PrepareJobCgroups(*host,
                                          std::filesystem::path { DefaultCgroupMount },
                                          CgroupTeardownPolicy { .attempts = 3, .pause = std::chrono::microseconds { 1 } });

    REQUIRE_FALSE(layout.has_value());
    CHECK(layout.error().cause == CgroupUnavailable::StaleJobCgroups);
    CHECK(layout.error().detail.contains("job-7"));
}
