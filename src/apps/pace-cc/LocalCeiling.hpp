// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AtomicFile.hpp"
#include "IProcessRunner.hpp"

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Platform/JobCgroups.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

/// @file LocalCeiling.hpp
/// A launcher's OWN compiles under the compile-OOM defense (#1770).
///
/// A node holds every compile it runs under one memory ceiling, its `compiles/` cgroup. A compile `pace-cc` runs
/// itself -- no fleet, a miss that is not dispatched, the fallback after a kill -- runs in the build's own process
/// tree, outside it. This places such a compile INSIDE the local node's ceiling whenever this launcher can: the node
/// names the cgroup in its runtime directory (the RENDEZVOUS), and the launcher makes a cgroup of its own below it
/// and starts the compiler there. Where it cannot -- no node, a node under the system unit's account, a build tree
/// whose cgroup shares no writable ancestor with the node's, a host with no cgroups -- the compiler still starts as
/// the kernel's first OOM victim (`OomVictimExec.hpp`). Which of the two applied is a NAMED outcome per compile
/// (`LocalDefense`, `CeilingMiss`), recorded in the invocation log: a defense that is silently off is an anti-goal.
///
/// Header-only dependencies and the standard library: the launcher compiles this without linking `Pace`; the node
/// compiles it too, as the side that writes the rendezvous.
namespace Pace::Cc
{

/// The file a node names its compile ceiling in, in its runtime directory: one line, the absolute path of its
/// `compiles/` cgroup.
inline constexpr std::string_view CeilingRendezvousName = "compile-ceiling";

/// The runtime directory the shipped USER unit gives its node (`RuntimeDirectory=pace-node`), under
/// `$XDG_RUNTIME_DIR`: where a launcher of the same user looks for the rendezvous.
inline constexpr std::string_view NodeRuntimeDirectoryName = "pace-node";

/// Where a node writes its rendezvous: `$RUNTIME_DIRECTORY`, which systemd sets for a unit naming
/// `RuntimeDirectory=` -- its first entry when it lists several.
///
/// **Only there, never derived from `$XDG_RUNTIME_DIR`.** A node started by hand (a test fixture, a developer's
/// shell) gets no runtime directory from anybody, and one that derived the user unit's would overwrite -- or, on
/// its way down, remove -- the rendezvous of the node the user actually runs. And systemd removes the directory
/// when the unit stops, so a stopped node leaves nothing behind for a launcher to follow.
/// @param runtimeDirectory `$RUNTIME_DIRECTORY`; disengaged when unset.
/// @return The file to write; disengaged when there is nowhere to write it.
[[nodiscard]] std::optional<std::filesystem::path> RendezvousPublishPath(std::optional<std::string> const& runtimeDirectory);

/// Where a launcher reads the rendezvous: `$XDG_RUNTIME_DIR/pace-node/compile-ceiling`, the user unit's.
///
/// Under the SYSTEM unit the node's runtime directory is `/run/pace-node`, mode 0700 and owned by its account, so a
/// launcher of another user could not read it -- nor write the node's cgroup if it could. That launcher looks only
/// at its own user's, finds nothing, and says so (`CeilingMiss::NoNodeCeiling`).
/// @param xdgRuntimeDirectory `$XDG_RUNTIME_DIR`; disengaged when unset.
/// @return The file to read; disengaged when the variable is unset or empty.
[[nodiscard]] std::optional<std::filesystem::path> RendezvousLookupPath(
    std::optional<std::string> const& xdgRuntimeDirectory);

/// The rendezvous text for @p compiles.
/// @param compiles The node's `compiles/` cgroup.
/// @return One line, newline-terminated.
[[nodiscard]] std::string RenderRendezvous(std::filesystem::path const& compiles);

/// Read a rendezvous: its first line, which must be an absolute path.
/// @param text The file's text.
/// @return The cgroup; disengaged when the first line names no absolute path.
[[nodiscard]] std::optional<std::filesystem::path> ParseRendezvous(std::string_view text);

/// Where a compile this launcher runs itself is asked to run. Private: never stored or sent.
enum class LocalPlacement : std::uint8_t
{
    /// Inside the local node's compile ceiling where this launcher can place it there, and under the OOM-victim
    /// shim alone where it cannot -- `CompileRun::inCgroup` says which happened.
    UnderCeiling,
    /// Under the OOM-victim shim alone, never the ceiling: the compile was killed there once already, or no
    /// machine's budget can hold it (`LocalGrantReason::NeverFits`), so the ceiling would only kill it again.
    OutsideCeiling,
};

/// What defended a compile this launcher ran itself against memory. PERSISTED, as its row's token in the invocation
/// log, so the values are spelled out; `Last` is the count.
enum class LocalDefense : std::uint8_t
{
    /// Inside the local node's compile ceiling, and under the OOM-victim shim.
    Ceiling = 0,
    /// Under the OOM-victim shim alone: the kernel kills it first, and nothing bounds it.
    ShimOnly = 1,
    /// Neither: no shim on this host (not Linux, or no `/proc`).
    Unprotected = 2,
    Last = 3
};

/// Why a compile this launcher ran itself did not join the node's ceiling. Private: what the log records is the
/// row's `id`; `Last` is the count.
enum class CeilingMiss : std::uint8_t
{
    None,               ///< It joined.
    NotThisPlatform,    ///< No cgroups to join: not Linux, or a build that cannot start a process in one.
    NoRuntimeDirectory, ///< `$XDG_RUNTIME_DIR` is unset: nowhere to look for a node.
    NoNodeCeiling,      ///< No rendezvous: no node of this user holds a ceiling (none runs, or the system unit's).
    Unreadable,         ///< A rendezvous that names no absolute path.
    CgroupNotMade,      ///< The node's `compiles/` refused this user a cgroup of its own.
    SpawnRefused,       ///< The system would not start the compiler there: no writable common ancestor.
    KeptOutside,        ///< Asked to run outside: killed there once already, or nothing's budget can hold it.
    Last
};

/// One `CeilingMiss`: a stable id the log and the report use, and the sentence a person reads.
struct CeilingMissRow
{
    CeilingMiss miss;          ///< The reason this row describes.
    std::string_view id;       ///< Stable, for the log; never reworded.
    std::string_view sentence; ///< For the verbose line and the report.
};

/// One row per `CeilingMiss`.
inline constexpr EnumTable<CeilingMiss, CeilingMissRow> CeilingMissTable { {
    { .miss = CeilingMiss::None, .id = "joined", .sentence = "joined the local node's compile ceiling" },
    { .miss = CeilingMiss::NotThisPlatform,
      .id = "no-cgroups",
      .sentence = "this host or build cannot start a compiler inside a cgroup" },
    { .miss = CeilingMiss::NoRuntimeDirectory,
      .id = "no-runtime-dir",
      .sentence = "XDG_RUNTIME_DIR is unset, so no node's ceiling can be found" },
    { .miss = CeilingMiss::NoNodeCeiling,
      .id = "no-node-ceiling",
      .sentence = "no pace-node of this user publishes a compile ceiling (none runs, or it is the system unit's)" },
    { .miss = CeilingMiss::Unreadable, .id = "unreadable", .sentence = "the node's compile-ceiling file names no cgroup" },
    { .miss = CeilingMiss::CgroupNotMade,
      .id = "cgroup-refused",
      .sentence = "the node's compile ceiling would not take a cgroup from this user" },
    { .miss = CeilingMiss::SpawnRefused,
      .id = "spawn-refused",
      .sentence = "the system would not start the compiler inside the node's ceiling (no writable common cgroup)" },
    { .miss = CeilingMiss::KeptOutside,
      .id = "kept-outside",
      .sentence = "kept outside the ceiling, which killed it once or cannot hold it" },
} };

static_assert(RowsInEnumeratorOrder(CeilingMissTable, &CeilingMissRow::miss),
              "CeilingMissTable must hold one row per CeilingMiss, in enumerator order");

/// One `LocalDefense`: its log token.
struct LocalDefenseRow
{
    LocalDefense defense;   ///< The defense this row describes.
    std::string_view token; ///< What the log records, before `:` and the miss's id.
};

/// One row per `LocalDefense`.
inline constexpr EnumTable<LocalDefense, LocalDefenseRow> LocalDefenseTable { {
    { .defense = LocalDefense::Ceiling, .token = "ceiling" },
    { .defense = LocalDefense::ShimOnly, .token = "shim" },
    { .defense = LocalDefense::Unprotected, .token = "none" },
} };

static_assert(RowsInEnumeratorOrder(LocalDefenseTable, &LocalDefenseRow::defense),
              "LocalDefenseTable must hold one row per LocalDefense, in enumerator order");

/// What one compile this launcher ran itself ran under.
struct LocalDefenseReport
{
    LocalDefense defense { LocalDefense::Unprotected }; ///< What defended it.
    CeilingMiss miss { CeilingMiss::None };             ///< Why not the ceiling; `None` exactly when it joined.

    /// @return True when both say the same.
    [[nodiscard]] bool operator==(LocalDefenseReport const&) const = default;
};

/// The log's spelling of @p report: `ceiling`, or `<defense>:<miss id>`.
/// @param report What a compile ran under.
/// @return The token.
[[nodiscard]] std::string LocalDefenseToken(LocalDefenseReport const& report);

/// Read a token `LocalDefenseToken` wrote.
/// @param token The log's text.
/// @return The report; disengaged for an empty or unknown token -- a line from before the column, or a later build.
[[nodiscard]] std::optional<LocalDefenseReport> ParseLocalDefenseToken(std::string_view token);

/// Where the local node's compile ceiling is, as this launcher can find it.
class ICeilingLocator
{
  public:
    ICeilingLocator() = default;
    ICeilingLocator(ICeilingLocator const&) = delete;
    ICeilingLocator(ICeilingLocator&&) = delete;
    ICeilingLocator& operator=(ICeilingLocator const&) = delete;
    ICeilingLocator& operator=(ICeilingLocator&&) = delete;
    virtual ~ICeilingLocator() = default;

    /// @return The node's `compiles/` cgroup, or why there is none to join.
    [[nodiscard]] virtual std::expected<std::filesystem::path, CeilingMiss> Locate() const = 0;
};

/// The production locator: `$XDG_RUNTIME_DIR` and the rendezvous file, read through the two callables it is given.
class RendezvousLocator final: public ICeilingLocator
{
  public:
    /// Reads one environment variable; disengaged when unset.
    using EnvironmentReader = std::function<std::optional<std::string>(std::string_view name)>;
    /// Reads one file whole; disengaged when it cannot be read.
    using FileReader = std::function<std::optional<std::string>(std::filesystem::path const& path)>;

    /// @param environment Reads `XDG_RUNTIME_DIR`. @param files Reads the rendezvous.
    RendezvousLocator(EnvironmentReader environment, FileReader files);

    /// @copydoc ICeilingLocator::Locate
    [[nodiscard]] std::expected<std::filesystem::path, CeilingMiss> Locate() const override;

  private:
    EnvironmentReader _environment;
    FileReader _files;
};

/// Whether this launcher's compilers start under the OOM-victim shim. Private.
enum class ShimState : std::uint8_t
{
    Shimmed, ///< Every compiler starts as the kernel's first OOM victim.
    NoShim,  ///< None does: no shim on this host.
};

/// What a local compile ran, with what defended it.
struct LocalRun
{
    CompileRun run;             ///< The compile.
    LocalDefenseReport defense; ///< What it ran under.
};

/// Runs the compiles this launcher runs itself: inside the local node's ceiling where it can, under the OOM-victim
/// shim wherever there is one, and says which.
///
/// The decisions are here and every effect is injected, so each outcome is a case over fakes.
class LocalCompileRunner
{
  public:
    /// Makes a runner; @p cgroups null for one that starts every child where the launcher is.
    using RunnerFactory =
        std::function<std::unique_ptr<IProcessRunner>(std::shared_ptr<Platform::IJobCgroupSource> cgroups)>;
    /// Makes the source of this launcher's cgroups below the node's `compiles/`.
    using SourceFactory = std::function<std::shared_ptr<Platform::IJobCgroupSource>(std::filesystem::path const& compiles)>;

    /// @param locator Where the ceiling is. @param sources Makes this launcher's cgroups below it.
    /// @param runners Makes the runner a compile spawns through. @param shim Whether that runner starts it under the shim.
    /// @param canSpawnIntoCgroups Whether this build can start a process inside a cgroup at all (`CanSpawnIntoCgroups`).
    LocalCompileRunner(ICeilingLocator const& locator,
                       SourceFactory sources,
                       RunnerFactory runners,
                       ShimState shim,
                       bool canSpawnIntoCgroups);

    /// Run @p argv here, capturing its two streams.
    /// @param argv The compiler's command line. @param placement Whether it may join the ceiling.
    /// @return The compile and what defended it.
    [[nodiscard]] LocalRun Run(std::span<std::string const> argv, LocalPlacement placement);

  private:
    ICeilingLocator const& _locator;
    SourceFactory _sources;
    RunnerFactory _runners;
    ShimState _shim;
    bool _canSpawnIntoCgroups;
};

/// Where a node keeps its rendezvous: written once its ceiling holds, withdrawn when it does not.
class ICeilingRendezvous
{
  public:
    ICeilingRendezvous() = default;
    ICeilingRendezvous(ICeilingRendezvous const&) = delete;
    ICeilingRendezvous(ICeilingRendezvous&&) = delete;
    ICeilingRendezvous& operator=(ICeilingRendezvous const&) = delete;
    ICeilingRendezvous& operator=(ICeilingRendezvous&&) = delete;
    virtual ~ICeilingRendezvous() = default;

    /// Name @p compiles as the ceiling a launcher of this user may join.
    /// @param compiles The node's `compiles/` cgroup.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] virtual std::expected<void, std::string> Publish(std::filesystem::path const& compiles) = 0;

    /// Name none: the ceiling does not hold, so no launcher should go looking for it.
    virtual void Withdraw() noexcept = 0;
};

/// The production rendezvous: one file in the node's runtime directory (`RendezvousPublishPath`), replaced
/// atomically so a launcher never reads half of it.
///
/// **Quiet on repetition**: a publish of the path already published -- or already refused -- writes nothing and
/// reports nothing, so a caller that re-applies its ceiling at every change of the pressure allowance logs a
/// failure once rather than per change.
class RendezvousFile final: public ICeilingRendezvous
{
  public:
    /// @param target Where the file goes; disengaged on a node with no runtime directory, whose launchers cannot
    ///        find it -- the first publish says so.
    /// @param files The filesystem; outlives this.
    /// @param writerId What distinguishes this writer's temporary file: the process id.
    RendezvousFile(std::optional<std::filesystem::path> target, IAtomicWriteFiles& files, std::uint64_t writerId);

    /// @copydoc ICeilingRendezvous::Publish
    [[nodiscard]] std::expected<void, std::string> Publish(std::filesystem::path const& compiles) override;

    /// @copydoc ICeilingRendezvous::Withdraw
    void Withdraw() noexcept override;

  private:
    std::optional<std::filesystem::path> _target;
    IAtomicWriteFiles& _files;
    std::uint64_t _writerId;
    std::optional<std::filesystem::path> _attempted; ///< The path last published or refused; disengaged after a withdraw.
    bool _written {};                                ///< Whether the file is there to withdraw.
};

} // namespace Pace::Cc
