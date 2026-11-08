// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <system_error>

/// @file JobCgroups.hpp
/// What a process runner needs of a per-job cgroup, and nothing of how one is made.
///
/// **Interfaces only, header-only and standard-library-only, and that is load-bearing**: the runner
/// that consumes them (`apps/pace-cc/ProcessRunner.cpp`) is compiled into the launcher, which does not
/// link `Pace`. The Linux implementation, and the startup rules that decide whether one can exist at
/// all, are `Platform/CgroupV2.hpp` -- linked by the node alone.
///
/// Why a compile gets a cgroup of its own: on Linux the `ru_maxrss` that `wait4` hands back for a
/// spawned child INCLUDES the resident set of the process that spawned it, because the kernel folds the
/// old address space's high-water mark into the child at `exec`. A launcher is small, so there it is
/// harmless; a node holding a multi-gigabyte cache reports that figure as the peak of every compile it
/// runs, and a memory budget charged by it admits a handful of jobs on a machine that could run thirty.
/// A cgroup's `memory.peak` counts only the tasks that were ever IN it.
namespace Pace::Platform
{

/// One job's cgroup: a directory the job is spawned into and whose `memory.peak` is its peak.
///
/// **Destroying it ends the job's tree**: every task still in the cgroup is killed (`cgroup.kill`) and
/// the directory is removed, so a descendant the compiler left behind cannot outlive the compile that
/// started it or leak its memory into the next job's measurement.
class IJobCgroup
{
  public:
    IJobCgroup() = default;
    IJobCgroup(IJobCgroup const&) = delete;
    IJobCgroup(IJobCgroup&&) = delete;
    IJobCgroup& operator=(IJobCgroup const&) = delete;
    IJobCgroup& operator=(IJobCgroup&&) = delete;
    virtual ~IJobCgroup() = default;

    /// The cgroup's directory, open, for `posix_spawnattr_setcgroup_np`.
    /// @return A descriptor valid for as long as this object lives.
    [[nodiscard]] virtual int DirectoryDescriptor() const noexcept = 0;

    /// The most memory the job's whole tree held at once, in bytes.
    ///
    /// Read after the job's direct child has been reaped and before this object is destroyed.
    /// @return The peak; disengaged when the kernel would not say (never 0 for "unknown").
    [[nodiscard]] virtual std::optional<std::uint64_t> PeakBytes() const = 0;
};

/// Makes a fresh cgroup per job.
///
/// **Implementations must tolerate concurrent calls**, as `Cc::IProcessRunner` does: a node runs
/// several compiles at once through the one runner it was handed.
class IJobCgroupSource
{
  public:
    IJobCgroupSource() = default;
    IJobCgroupSource(IJobCgroupSource const&) = delete;
    IJobCgroupSource(IJobCgroupSource&&) = delete;
    IJobCgroupSource& operator=(IJobCgroupSource const&) = delete;
    IJobCgroupSource& operator=(IJobCgroupSource&&) = delete;
    virtual ~IJobCgroupSource() = default;

    /// Make a cgroup for one job.
    /// @return The cgroup, or why none could be made -- in which case the caller spawns without one
    ///         and reports the job's peak as unknown.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IJobCgroup>, std::error_code> Create() = 0;

    /// Why a spawn that asked for a cgroup ran without a usable measurement.
    enum class Unmeasured : std::uint8_t
    {
        SpawnRefused = 0, ///< The system would not start the child inside its cgroup; it ran outside it.
        PeakUnreadable,   ///< The child ran in its cgroup, but the kernel would not give its `memory.peak`.
    };

    /// The runner reports a compile it could not measure, so the source can count and say so. The
    /// failure to MAKE a cgroup is the source's own and needs no report. Called from any thread.
    /// @param why What went wrong.
    virtual void NoteUnmeasured(Unmeasured why) noexcept
    {
        static_cast<void>(why);
    }
};

} // namespace Pace::Platform
