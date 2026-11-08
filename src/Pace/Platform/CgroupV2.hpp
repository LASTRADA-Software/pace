// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Platform/JobCgroups.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// @file CgroupV2.hpp
/// The Linux cgroup v2 facility behind `IJobCgroupSource`: the startup rules that decide whether a node
/// may hand each compile a cgroup of its own, and the source that does.
///
/// **The facility is all or nothing, and "nothing" is a stated answer.** A node that cannot get a
/// delegated subtree (the shipped unit does not delegate one -- `ProtectControlGroups=yes`) does not
/// guess a peak from `ru_maxrss`, which is the very figure this exists to replace: it reports the
/// peak UNKNOWN and says why (`CgroupFailure`), so the estimator falls back to its default and an
/// operator can read the remedy off `pacectl node`.
///
/// Every decision here -- the parse of `/proc/self/cgroup`, the layout, which failure maps to which
/// reason -- is pure over `ICgroupHost`, so the whole of it runs against a fake in a unit test.
namespace Pace::Platform
{

/// The ambient resources the facility touches: the cgroup filesystem, this process's identity and the
/// one pause it makes.
///
/// One seam rather than five because they are one fact -- *the machine's cgroup tree* -- and a fake of
/// it is a whole scripted tree.
class ICgroupHost
{
  public:
    ICgroupHost() = default;
    ICgroupHost(ICgroupHost const&) = delete;
    ICgroupHost(ICgroupHost&&) = delete;
    ICgroupHost& operator=(ICgroupHost const&) = delete;
    ICgroupHost& operator=(ICgroupHost&&) = delete;
    virtual ~ICgroupHost() = default;

    /// Read a whole (pseudo-)file as text.
    /// @param path The file.
    /// @return Its contents, or the platform's refusal.
    [[nodiscard]] virtual std::expected<std::string, std::error_code> ReadFile(std::filesystem::path const& path) const = 0;

    /// Write @p text to an existing (pseudo-)file in one `write`, as a cgroup control file requires.
    /// @param path The file.
    /// @param text What to write.
    /// @return Nothing, or the platform's refusal.
    [[nodiscard]] virtual std::expected<void, std::error_code> WriteFile(std::filesystem::path const& path,
                                                                         std::string_view text) const = 0;

    /// Make one directory (a cgroup).
    /// @param path The directory; its parent exists.
    /// @return Nothing, or the platform's refusal -- `file_exists` when it is already there.
    [[nodiscard]] virtual std::expected<void, std::error_code> MakeDirectory(std::filesystem::path const& path) const = 0;

    /// Remove one empty directory (a cgroup).
    /// @param path The directory.
    /// @return Nothing, or the platform's refusal -- `device_or_resource_busy` while tasks remain.
    [[nodiscard]] virtual std::expected<void, std::error_code> RemoveCgroupDirectory(
        std::filesystem::path const& path) const = 0;

    /// The names of the entries of a directory (not the paths).
    /// @param path The directory.
    /// @return The names, in no particular order, or the platform's refusal.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, std::error_code> ListDirectory(
        std::filesystem::path const& path) const = 0;

    /// Open a directory read-only and close-on-exec, for `posix_spawnattr_setcgroup_np`.
    /// @param path The directory.
    /// @return The descriptor, or the platform's refusal.
    [[nodiscard]] virtual std::expected<int, std::error_code> OpenDirectory(std::filesystem::path const& path) const = 0;

    /// Close a descriptor `OpenDirectory` returned.
    /// @param descriptor The descriptor.
    virtual void CloseDescriptor(int descriptor) const noexcept = 0;

    /// Whether this process may write @p path (`access(W_OK)`; false on a read-only mount).
    /// @param path The file.
    /// @return True when it may.
    [[nodiscard]] virtual bool IsWritable(std::filesystem::path const& path) const = 0;

    /// Whether the system lets a process be created directly inside a cgroup at all.
    ///
    /// Asked of the system rather than assumed, because a service's seccomp filter can refuse the
    /// `clone3` call that does it -- and does, under `RestrictNamespaces=` -- with nothing in the
    /// cgroup tree to show for it. The answer is about the CALL, so any cgroup serves: an answer from
    /// the kernel's own checks (the cgroup is not a leaf: busy) counts as the call being allowed.
    /// @param directory A descriptor `OpenDirectory` returned.
    /// @return Nothing when the call is allowed, or the refusal.
    [[nodiscard]] virtual std::expected<void, std::error_code> ProbeSpawnInto(int directory) const = 0;

    /// This process's id, to move into its leaf cgroup.
    /// @return The pid.
    [[nodiscard]] virtual std::uint32_t OwnProcessId() const noexcept = 0;

    /// Wait @p duration. The ONE clock the facility reads: a fake records it and returns.
    /// @param duration How long.
    virtual void Pause(std::chrono::microseconds duration) const = 0;
};

/// The real cgroup tree: `/proc`, `/sys/fs/cgroup`, `posix_spawn`. Linux alone; on every other
/// platform every call fails with `operation_not_supported`.
class SystemCgroupHost final: public ICgroupHost
{
  public:
    /// @copydoc ICgroupHost::ReadFile
    [[nodiscard]] std::expected<std::string, std::error_code> ReadFile(std::filesystem::path const& path) const override;
    /// @copydoc ICgroupHost::WriteFile
    [[nodiscard]] std::expected<void, std::error_code> WriteFile(std::filesystem::path const& path,
                                                                 std::string_view text) const override;
    /// @copydoc ICgroupHost::MakeDirectory
    [[nodiscard]] std::expected<void, std::error_code> MakeDirectory(std::filesystem::path const& path) const override;
    /// @copydoc ICgroupHost::RemoveCgroupDirectory
    [[nodiscard]] std::expected<void, std::error_code> RemoveCgroupDirectory(
        std::filesystem::path const& path) const override;
    /// @copydoc ICgroupHost::ListDirectory
    [[nodiscard]] std::expected<std::vector<std::string>, std::error_code> ListDirectory(
        std::filesystem::path const& path) const override;
    /// @copydoc ICgroupHost::OpenDirectory
    [[nodiscard]] std::expected<int, std::error_code> OpenDirectory(std::filesystem::path const& path) const override;
    /// @copydoc ICgroupHost::CloseDescriptor
    void CloseDescriptor(int descriptor) const noexcept override;
    /// @copydoc ICgroupHost::IsWritable
    [[nodiscard]] bool IsWritable(std::filesystem::path const& path) const override;
    /// @copydoc ICgroupHost::ProbeSpawnInto
    [[nodiscard]] std::expected<void, std::error_code> ProbeSpawnInto(int directory) const override;
    /// @copydoc ICgroupHost::OwnProcessId
    [[nodiscard]] std::uint32_t OwnProcessId() const noexcept override;
    /// @copydoc ICgroupHost::Pause
    void Pause(std::chrono::microseconds duration) const override;
};

/// Why the facility is unavailable.
///
/// Private: transmitted nowhere. What an operator reads is the row's text.
enum class CgroupUnavailable : std::uint8_t
{
    SpawnIntoUnsupported = 0, ///< This build or platform cannot spawn directly into a cgroup.
    NoUnifiedHierarchy,       ///< This process is not in a cgroup v2 tree the node can read.
    NoMemoryController,       ///< The node's cgroup is not offered the memory controller.
    NotDelegated,             ///< The node's cgroup is not writable by it.
    SharedCgroup,             ///< Other processes live in the node's cgroup, so it cannot delegate to children.
    StaleJobCgroups,          ///< Cgroups an earlier node left behind could not be removed.
    LeafNotCreated,           ///< The node's own leaf cgroup could not be made.
    ProcessNotMoved,          ///< The node could not move itself into its leaf.
    MemoryNotEnabled,         ///< The memory controller could not be enabled for the node's children.
    PeakNotReadable,          ///< The kernel offers no `memory.peak` (older than 5.19).
    SpawnIntoRefused,         ///< The system refuses `clone3` into a cgroup (a seccomp filter).
    Last,                     ///< Not a cause.
};

/// One cause: a stable id and what it means.
struct CgroupUnavailableRow
{
    CgroupUnavailable cause; ///< The enumerator this row describes.
    std::string_view id;     ///< Stable and kebab-case; for logs and scripts.
    std::string_view reason; ///< A sentence an operator can act on, ending without a full stop (a detail follows).
};

/// Every cause, in enumerator order.
inline constexpr EnumTable<CgroupUnavailable, CgroupUnavailableRow> CgroupUnavailableTable { {
    { .cause = CgroupUnavailable::SpawnIntoUnsupported,
      .id = "spawn-into-unsupported",
      .reason = "this build cannot start a process directly inside a cgroup (it takes Linux and glibc 2.39 or newer)" },
    { .cause = CgroupUnavailable::NoUnifiedHierarchy,
      .id = "no-unified-hierarchy",
      .reason = "this process is not in a cgroup v2 hierarchy it can read" },
    { .cause = CgroupUnavailable::NoMemoryController,
      .id = "no-memory-controller",
      .reason = "the cgroup this node runs in is not offered the memory controller" },
    { .cause = CgroupUnavailable::NotDelegated,
      .id = "not-delegated",
      .reason = "the cgroup this node runs in is not delegated to it (not writable)" },
    { .cause = CgroupUnavailable::SharedCgroup,
      .id = "shared-cgroup",
      .reason = "other processes live in the cgroup this node runs in (it is not a cgroup of the node's own, as a "
                "service's is), so memory accounting cannot be enabled for its children" },
    { .cause = CgroupUnavailable::StaleJobCgroups,
      .id = "stale-job-cgroups",
      .reason = "job cgroups an earlier node left behind could not be removed" },
    { .cause = CgroupUnavailable::LeafNotCreated,
      .id = "leaf-not-created",
      .reason = "the node's own leaf cgroup could not be created" },
    { .cause = CgroupUnavailable::ProcessNotMoved,
      .id = "process-not-moved",
      .reason = "the node could not move itself into its leaf cgroup" },
    { .cause = CgroupUnavailable::MemoryNotEnabled,
      .id = "memory-not-enabled",
      .reason = "the memory controller could not be enabled for the node's child cgroups" },
    { .cause = CgroupUnavailable::PeakNotReadable,
      .id = "peak-not-readable",
      .reason = "the kernel offers no memory.peak (it needs Linux 5.19 or newer)" },
    { .cause = CgroupUnavailable::SpawnIntoRefused,
      .id = "spawn-into-refused",
      .reason = "the system refuses to start a process inside a cgroup (a seccomp filter such as "
                "RestrictNamespaces= blocks clone3)" },
} };
static_assert(RowsInEnumeratorOrder(CgroupUnavailableTable, &CgroupUnavailableRow::cause),
              "CgroupUnavailableTable must hold one row per CgroupUnavailable, in enumerator order");

/// The row describing @p cause.
/// @param cause The cause.
/// @return Its row.
[[nodiscard]] constexpr CgroupUnavailableRow const& RowFor(CgroupUnavailable cause) noexcept
{
    return CgroupUnavailableTable[static_cast<std::size_t>(cause)];
}

/// Why the facility is unavailable, with what was observed.
struct CgroupFailure
{
    CgroupUnavailable cause {};
    std::string detail; ///< What was observed (a path, an errno); empty when the row says it all.

    /// The failure as one sentence.
    /// @return `<reason>[: <detail>]`.
    [[nodiscard]] std::string Describe() const;
};

/// Where a node's compile cgroups live.
struct CgroupLayout
{
    std::filesystem::path delegated; ///< The node's own cgroup, whose children are the jobs.
    std::filesystem::path leaf;      ///< The leaf the node moved itself into.
};

/// The cgroup v2 mount point on every systemd and container host.
inline constexpr std::string_view DefaultCgroupMount = "/sys/fs/cgroup";

/// The name of the leaf the node moves itself into: cgroup v2 forbids a cgroup from holding processes
/// AND delegating a controller to its children, so the node steps aside from the jobs' parent.
inline constexpr std::string_view NodeLeafName = "node";

/// The cgroup path of a `/proc/self/cgroup` document: the unified-hierarchy line (`0::<path>`).
/// @param document The file's text.
/// @return The path as the kernel spells it (absolute, `/` for the root), or why there is none.
[[nodiscard]] std::expected<std::string, CgroupFailure> ParseUnifiedCgroupPath(std::string_view document);

/// How long a cgroup is given to empty after `cgroup.kill` before its directory is left behind.
///
/// **The worst case is the bound on how long a finished compile's thread blocks in teardown**:
/// `attempts` x `pause`, which is 2000 x 500 us = one second of sleeping (plus the `rmdir`s), and only
/// for a cgroup whose tasks are slow to die. A cgroup that empties at once costs one `rmdir`.
struct CgroupTeardownPolicy
{
    std::uint32_t attempts { 2000 };         ///< `rmdir` tries while the kill takes effect.
    std::chrono::microseconds pause { 500 }; ///< Between tries.
};

/// Kill whatever is left in the cgroup @p directory and remove it, retrying while the kill takes effect.
/// @param host The cgroup tree.
/// @param directory The cgroup.
/// @param policy How patiently to retry.
/// @return Nothing when it is gone, or the last refusal.
[[nodiscard]] std::expected<void, std::error_code> RemoveCgroup(ICgroupHost const& host,
                                                                std::filesystem::path const& directory,
                                                                CgroupTeardownPolicy const& policy);

/// Get this node ready to hand each compile a cgroup of its own.
///
/// In order, and stopping at the first that fails: read where this process lives; check that its
/// cgroup is offered the memory controller, is writable, and holds no other process; ask the system
/// whether it lets a process be started inside a cgroup at all -- **all of which read and touch
/// nothing**; then create the leaf `node`, move this process into it, enable the memory controller for
/// the parent's children and read that back, confirm `memory.peak` exists there, and remove the `job-*`
/// cgroups an earlier node left behind.
///
/// **Once per process.** The node ends up INSIDE the leaf, so a second call would find itself in
/// `.../node` and nest `.../node/node` beside the first call's jobs; the owner of the facility
/// (`Node::CompileMemoryMeter`) prepares lazily and keeps the answer across a formation reform.
///
/// Delegation is not proved: systemd marks a delegated cgroup with an xattr (`trusted.delegate`, or
/// `user.delegate` from a user manager), but the first is unreadable to the unprivileged account a
/// service runs as and a user manager did not set the second on a delegated unit's cgroup (measured
/// on systemd 259), so requiring either would refuse every working setup. What is checked instead is
/// the effect: writable, unshared, `+memory` read back, and a probe at every job.
/// @param host The cgroup tree.
/// @param mount Where cgroup v2 is mounted.
/// @param teardown How patiently stale job cgroups are removed.
/// @return The layout, or why the facility is unavailable.
[[nodiscard]] std::expected<CgroupLayout, CgroupFailure> PrepareJobCgroups(
    ICgroupHost const& host,
    std::filesystem::path const& mount = std::filesystem::path { DefaultCgroupMount },
    CgroupTeardownPolicy const& teardown = {});

/// Makes `job-<n>` cgroups under the node's delegated cgroup.
class CgroupJobSource final: public IJobCgroupSource
{
  public:
    /// @param host The cgroup tree; shared with every job this source makes, so a job finishing late
    ///        never reaches a host that is gone.
    /// @param layout What `PrepareJobCgroups` returned.
    /// @param teardown How patiently a finished job's cgroup is removed.
    CgroupJobSource(std::shared_ptr<ICgroupHost const> host, CgroupLayout layout, CgroupTeardownPolicy teardown = {});

    /// @copydoc IJobCgroupSource::Create
    [[nodiscard]] std::expected<std::unique_ptr<IJobCgroup>, std::error_code> Create() override;

  private:
    std::shared_ptr<ICgroupHost const> _host;
    CgroupLayout _layout;
    CgroupTeardownPolicy _teardown;
    std::atomic<std::uint64_t> _next { 1 };
};

} // namespace Pace::Platform
