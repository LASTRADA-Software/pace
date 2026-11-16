// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"
#include "NodeConfig.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/NodePolicy.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/CgroupV2.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <IProcessRunner.hpp>
#include <LocalCeiling.hpp>

/// @file CompileMemoryMeter.hpp
/// How a node measures the peak memory of the compiles it runs -- and says so when it cannot.
///
/// A worker reports each compile's peak to the fleet's admission estimates. On Linux the figure `wait4`
/// hands back (`ru_maxrss`) INCLUDES the resident set of the spawner (see `Cc::MakeProcessRunner()`), and
/// a node holding a multi-gigabyte cache would report all of it as every compile's peak: the budget
/// charged thirty slots at 13 GB each admits four of them. So a Linux node never feeds that figure to an
/// estimate. It measures in a cgroup per compile where the machine allows it, and where it does not
/// it reports the peak UNKNOWN -- and raises `compile-memory-unmeasured` to say why, since an
/// estimate that never learns is otherwise a slowdown nobody can trace.
namespace Pace::Node
{

class INodeCapacitySource;
class INodeConfigSource;

/// The platforms a meter is chosen for.
enum class MeterPlatform : std::uint8_t
{
    Windows = 0, ///< Each compile runs in a job object, which measures it alone.
    MacOs,       ///< `posix_spawn` makes a fresh task, so `ru_maxrss` is the child's own.
    Linux,       ///< `ru_maxrss` includes the spawner's resident set: a cgroup per compile, or nothing.
    OtherPosix,  ///< Nothing known to measure a child alone.
    Last,        ///< Not a platform.
};

/// What a platform measures a compile with.
enum class MeterStrategy : std::uint8_t
{
    PlatformMeter = 0, ///< The runner's own (job object, or `ru_maxrss` where it is the child's alone).
    JobCgroup,         ///< A cgroup per compile, when the machine delegates one.
    NotMeasurable,     ///< No meter here, and no remedy to offer.
};

/// One platform's strategy.
struct MeterPlatformRow
{
    MeterPlatform platform; ///< The enumerator this row describes.
    MeterStrategy strategy; ///< How its compiles are measured.
};

/// Every platform, in enumerator order.
inline constexpr EnumTable<MeterPlatform, MeterPlatformRow> MeterPlatformTable { {
    { .platform = MeterPlatform::Windows, .strategy = MeterStrategy::PlatformMeter },
    { .platform = MeterPlatform::MacOs, .strategy = MeterStrategy::PlatformMeter },
    { .platform = MeterPlatform::Linux, .strategy = MeterStrategy::JobCgroup },
    { .platform = MeterPlatform::OtherPosix, .strategy = MeterStrategy::NotMeasurable },
} };
static_assert(RowsInEnumeratorOrder(MeterPlatformTable, &MeterPlatformRow::platform),
              "MeterPlatformTable must hold one row per MeterPlatform, in enumerator order");

/// The platform this binary was built for.
#if defined(_WIN32)
inline constexpr MeterPlatform ThisMeterPlatform = MeterPlatform::Windows;
#elif defined(__APPLE__)
inline constexpr MeterPlatform ThisMeterPlatform = MeterPlatform::MacOs;
#elif defined(__linux__)
inline constexpr MeterPlatform ThisMeterPlatform = MeterPlatform::Linux;
#else
inline constexpr MeterPlatform ThisMeterPlatform = MeterPlatform::OtherPosix;
#endif

/// Whether compile peaks are measured, as `compile-memory-unmeasured` needs to hear it.
enum class MeterState : std::uint8_t
{
    Measured = 0,  ///< Each compile's peak is reported (also a test double with nothing to measure).
    Unmeasured,    ///< It could be, and is not: an operator can fix it. Raises the condition.
    NotApplicable, ///< It cannot be on this platform: no remedy exists, so the row is not evaluated.
};

/// What the meter chose, and why.
struct MeterReport
{
    MeterState state { MeterState::Measured }; ///< The answer.
    std::string reason {};                     ///< Why, for `Unmeasured` and `NotApplicable`.
    /// The settings of `compiles/` this kernel lacks (`CgroupLayout::notApplied`), said once at startup.
    std::vector<std::string> notApplied {};
    /// Why the node's own cgroup could not be taken out of systemd-oomd's choice (`CgroupLayout::oomdOmitRefused`),
    /// said once at startup; disengaged when it was, or when there is no cgroup to mark.
    std::optional<std::string> oomdOmitRefused {};
};

/// Whether the compiles this worker runs are held under one memory ceiling, as `compile-memory-uncapped`
/// needs to hear it.
///
/// Private: transmitted nowhere. What an operator reads is the condition row.
enum class CeilingState : std::uint8_t
{
    Capped,        ///< `compiles/memory.max` holds the ceiling.
    Uncapped,      ///< It could, and does not: an operator can fix it. Raises the condition.
    NotApplicable, ///< It cannot on this platform (no cgroups): no remedy, so the row is not evaluated.
};

/// What applying the ceiling came to.
struct CeilingReport
{
    CeilingState state { CeilingState::Capped }; ///< The answer.
    std::string reason {};                       ///< Why, for `Uncapped` and `NotApplicable`.
};

/// The runner a node spawns compilers with, and what it says about measuring them.
struct MeteredRunner
{
    std::unique_ptr<Cc::IProcessRunner> runner; ///< Spawns every compiler this node runs.
    MeterReport report {};                      ///< Whether `runner` reports a peak, and if not why.
};

/// The node's compile-memory meter: prepared ONCE per process, whatever the number of formation
/// reforms that rebuild the worker around it.
///
/// **Why once**: preparing moves the process into a leaf cgroup, so a second preparation from there
/// would nest `node/node` beside the first one's jobs. The meter is therefore built where the process
/// is (the serving parts), prepares lazily on the first `MakeRunner` -- a node that runs no worker never
/// rearranges its cgroups -- and every later runner shares the one job source.
class CompileMemoryMeter
{
  public:
    /// @param host The machine's cgroup tree; consulted only on Linux.
    /// @param platform What to measure like; the build's own except in a test.
    /// @param oomVictim The shim every runner spawns its compilers through, so each starts as the kernel's first
    ///        OOM victim (`Cc::ThisProcessOomVictim`, resolved once by `main`); disengaged to spawn them directly.
    /// @param rendezvous Where `compiles/` is named for the compiles `pace-cc` runs itself on this machine, so they
    ///        join the ceiling too (#1770); null names it nowhere. Process-lifetime, as the meter is.
    explicit CompileMemoryMeter(std::shared_ptr<Platform::ICgroupHost const> host,
                                MeterPlatform platform = ThisMeterPlatform,
                                std::optional<Cc::OomVictim> oomVictim = std::nullopt,
                                std::unique_ptr<Cc::ICeilingRendezvous> rendezvous = nullptr);

    /// A runner over this process's meter, preparing it on the first call.
    /// @param metrics Where compiles that go unmeasured at run time are counted; outlives the runner.
    /// @param logger Where the first of them is said; outlives the runner.
    /// @return The runner and what it can measure.
    [[nodiscard]] MeteredRunner MakeRunner(IMetricsSink& metrics, ILogger& logger);

    /// Hold every compile under @p bytes together: write `compiles/memory.max`.
    ///
    /// **Writes every time**, at startup and at every accepted reload, never only when the figure
    /// changed: a `compiles/` an earlier run left behind keeps that run's `memory.max`, and a lowered
    /// ceiling must not wait for a restart. Never prepares: a meter no worker has asked for a runner has
    /// rearranged nothing, and answers `Uncapped` saying so.
    /// @param bytes The ceiling.
    /// @return `Capped` once written; `Uncapped` with the preparation's failure, or the write's, when no
    ///         ceiling holds; `NotApplicable` on a platform with no cgroups.
    [[nodiscard]] CeilingReport ApplyCeiling(std::uint64_t bytes);

    /// Where every compile this worker runs lives: the cgroup whose `memory.pressure` says whether a stall is the
    /// compiles' own.
    /// @return The `compiles/` directory; disengaged before a worker prepared it, or where compiles get no cgroup.
    [[nodiscard]] std::optional<std::filesystem::path> CompilesCgroup() const;

    /// What every compile this worker runs holds now: `compiles/memory.current`.
    /// @return The bytes; disengaged before a worker prepared `compiles/`, without one, or when the kernel would not
    ///         say.
    [[nodiscard]] std::optional<std::uint64_t> CompilesHeldBytes() const;

    /// What kills the youngest running compile when the node sheds.
    /// @return The process's job source; null before a worker prepared it, or where compiles get no cgroup.
    [[nodiscard]] Platform::IYoungestKiller* YoungestKiller() const noexcept;

    /// Where this meter's `compiles/` is named for this machine's launchers (#1770).
    /// @return The rendezvous; null when the meter names it nowhere.
    [[nodiscard]] Cc::ICeilingRendezvous* Rendezvous() const noexcept;

  private:
    /// Decide the meter, and publish that it has; runs once.
    void Prepare();

    /// What `Prepare` decides.
    void Decide();

    std::shared_ptr<Platform::ICgroupHost const> _host;
    MeterPlatform _platform;
    std::optional<Cc::OomVictim> _oomVictim;             ///< Handed to every runner this meter makes.
    std::unique_ptr<Cc::ICeilingRendezvous> _rendezvous; ///< Names `compiles/` for launchers; may be null.
    std::once_flag _prepared;
    /// Set, with release, once `Prepare` has decided: `ApplyCeiling` reads `_layout` and `_report` only after.
    std::atomic<bool> _decided { false };
    std::shared_ptr<Platform::CgroupJobSource> _source; ///< Null unless the strategy is `JobCgroup` and it is available.
    std::optional<Platform::CgroupLayout> _layout;      ///< Where `compiles/` is; engaged exactly when `_source` is.
    MeterReport _report;
};

/// Say what the meter chose: the condition, and in words.
/// @param report `MeteredRunner::report`.
/// @param conditions Where `compile-memory-unmeasured` is answered.
/// @param logger Where the reason is logged.
void ReportCompileMemoryMeter(MeterReport const& report, NodeConditions& conditions, ILogger& logger);

/// Put the compile memory ceiling in force, and say so: `compile memory ceiling: <bytes> (<derived|typed>)`
/// at Info and the row clear once written, the row raised (and a Warn) when it cannot be, and the row not
/// evaluated on a platform with no cgroups.
///
/// **The ONE answer for `compile-memory-uncapped`**: `ReportCompileMemoryMeter` leaves it to this, so a
/// worker whose ceiling nobody put in force reads `undecided` -- the wiring defect `NodeConditions::Settle`
/// names -- rather than a `clear` nobody earned. Called by `main` once a worker exists, and again after every
/// accepted reload with the configuration then in force.
/// @param meter The process's meter, prepared by the worker's runner.
/// @param cfg The configuration in force.
/// @param capacity What this machine is now.
/// @param conditions Where `compile-memory-uncapped` is answered.
/// @param logger Where the ceiling, or why there is none, is said.
/// @param allowanceBytes The pressure governor's allowance, which lowers the ceiling when it is below the configured
///        figure (said at Debug then: the governor's own line names the allowance when its level changes, and a
///        ramp moves it every interval); disengaged for none.
/// @param rendezvous Where the ceiling is named for the compiles `pace-cc` runs itself on this machine (#1770):
///        published while it holds, withdrawn while it does not, a failure said at Info. Null names it nowhere.
void PutCompileCeilingInForce(CompileMemoryMeter& meter,
                              NodeConfig const& cfg,
                              Distributed::NodeCapacity const& capacity,
                              NodeConditions& conditions,
                              ILogger& logger,
                              std::optional<std::uint64_t> allowanceBytes = std::nullopt,
                              Cc::ICeilingRendezvous* rendezvous = nullptr);

/// The configuration and capacity one write of the ceiling was derived from, read together: the budget a presence
/// round reports is taken from the same pair, so the two describe one moment.
struct CeilingBasis
{
    NodeConfig cfg;                     ///< The configuration in force.
    Distributed::NodeCapacity capacity; ///< What this machine is.
};

/// Where the pressure watcher holds every compile under its allowance: `compiles/memory.max`, through the ONE writer
/// of that file.
///
/// Absent on a node whose compiles run in no cgroup: the watcher then changes only what the machine reports.
class ICompileCeiling
{
  public:
    ICompileCeiling() = default;
    ICompileCeiling(ICompileCeiling const&) = delete;
    ICompileCeiling(ICompileCeiling&&) = delete;
    ICompileCeiling& operator=(ICompileCeiling const&) = delete;
    ICompileCeiling& operator=(ICompileCeiling&&) = delete;
    virtual ~ICompileCeiling() = default;

    /// Hold every compile under @p allowanceBytes, or under the configured ceiling where that is lower: the figure
    /// written is never above what the configuration in force asks for.
    /// @param allowanceBytes The pressure governor's allowance where pressure lowered it below the configured
    ///        ceiling; disengaged where it stands at the configuration, which a reload then moves at once.
    virtual void Apply(std::optional<std::uint64_t> allowanceBytes) = 0;

    /// What every compile holds now, as the kernel counts it.
    /// @return The bytes; disengaged when the kernel would not say.
    [[nodiscard]] virtual std::optional<std::uint64_t> HeldBytes() const = 0;
};

/// When `CompileCeilingKeeper::Keep` writes the ceiling.
///
/// Private: transmitted nowhere.
enum class CeilingWrite : std::uint8_t
{
    Always,   ///< At startup and after an accepted reload: whatever `compiles/` holds is rewritten.
    OnChange, ///< At every presence round: only when the derived figure moved since the last write.
};

/// The ONE writer of `compiles/memory.max`, and the ONE place the compile ceiling and the budget this machine reports
/// are taken from the same derivation.
///
/// `MachineBudgetOf` and `CompileCeilingOf` both read the configuration in force and the capacity; either moves
/// under a running node -- a reload changes `--compile-memory-limit`, a cache-budget override moves the reserve the
/// derived figure comes from. So the presence round keeps the ceiling here and reports the budget from the pair
/// `Keep` hands back: the queue is never told of room the kernel would kill a compile for, nor the ceiling left
/// below a budget that rose.
///
/// **Three writers, one figure.** The presence round (`OnChange`), an accepted reload (`Always`) and the pressure
/// watcher (`Apply`, its allowance) all write through here, and each READS the configuration and the capacity under
/// the lock it writes under. A writer never carries a snapshot in: a presence round that read the old configuration
/// before a reload wrote the new ceiling would otherwise write the old, higher figure back over it -- a limit raised
/// above its configured value until the next round. What is written is `min(configured ceiling, allowance)`, so the
/// governor can lower the ceiling and never raise it past the configuration.
///
/// Thread-safe: `main` (startup, reload), the presence loop and the pressure watcher call it.
class CompileCeilingKeeper final: public ICompileCeiling
{
  public:
    /// @param meter The process's meter, prepared by the worker's runner; outlives this.
    /// @param inForce The configuration in force, read at every write; outlives this.
    /// @param capacity What this machine is, read at every write; outlives this.
    /// @param conditions Where `compile-memory-uncapped` is answered; outlives this.
    /// @param logger Where the ceiling is said; outlives this.
    /// @param rendezvous Where the ceiling is named for this machine's launchers (#1770); null for nowhere. Outlives this.
    CompileCeilingKeeper(CompileMemoryMeter& meter,
                         INodeConfigSource const& inForce,
                         INodeCapacitySource const& capacity,
                         NodeConditions& conditions,
                         ILogger& logger,
                         Cc::ICeilingRendezvous* rendezvous = nullptr) noexcept;

    /// Put the ceiling the configuration in force and the capacity derive in force (`PutCompileCeilingInForce`),
    /// lowered to the allowance last applied, when @p when says so. A figure that was attempted and refused is not
    /// retried per round: the row is raised, and a reload retries.
    /// @param when Whether an unchanged figure is written again.
    /// @return The pair the figure was derived from, for the budget the caller reports beside it.
    [[nodiscard]] CeilingBasis Keep(CeilingWrite when);

    /// @copydoc ICompileCeiling::Apply
    void Apply(std::optional<std::uint64_t> allowanceBytes) override;

    /// @copydoc ICompileCeiling::HeldBytes
    [[nodiscard]] std::optional<std::uint64_t> HeldBytes() const override;

  private:
    /// Read the pair and write the figure it and the allowance give, when @p when says so. `_mutex` held.
    /// @param when Whether an unchanged figure is written again.
    /// @return The pair read.
    CeilingBasis KeepLocked(CeilingWrite when);

    CompileMemoryMeter& _meter;
    INodeConfigSource const& _inForce;
    INodeCapacitySource const& _capacity;
    NodeConditions& _conditions;
    ILogger& _logger;
    Cc::ICeilingRendezvous* _rendezvous;
    std::mutex _mutex;                          ///< Serialises writes and guards the two below.
    std::optional<std::uint64_t> _written {};   ///< The figure last attempted.
    std::optional<std::uint64_t> _allowance {}; ///< The governor's lowered allowance; disengaged when none binds.
};

} // namespace Pace::Node
