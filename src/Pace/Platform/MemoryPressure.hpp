// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Platform/CgroupV2.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

/// Memory pressure stall information (PSI): how much of the last seconds the machine, or the compiles'
/// cgroup, spent stalled waiting for memory. Linux only; read through `ICgroupHost`, so it runs against
/// a fake in a unit test. On a kernel without PSI the source reports an error, never a calm reading.
namespace Pace::Platform
{

/// The system-wide PSI file.
inline constexpr std::string_view PsiSystemPath = "/proc/pressure/memory";

/// The per-cgroup PSI file, relative to a cgroup directory.
inline constexpr std::string_view PsiCgroupFileName = "memory.pressure";

/// How close to its `memory.max` a cgroup's `memory.current` counts as AT it, as a divisor of the ceiling: within a
/// twentieth (5%). A cgroup reclaiming against its ceiling hovers just under it; one well below it is not stalling
/// against it, whatever its stall says.
inline constexpr std::uint64_t CeilingNearDivisor = 20;

/// One `some`/`full` line of a PSI file.
struct PsiLine
{
    double avg10 = 0.0;            ///< Percent of the last 10 s stalled.
    double avg60 = 0.0;            ///< Percent of the last 60 s stalled.
    std::uint64_t totalMicros = 0; ///< Cumulative stall, microseconds.
};

/// A whole PSI file.
struct PsiReading
{
    PsiLine some {};                ///< Some tasks stalled.
    std::optional<PsiLine> full {}; ///< All tasks stalled; absent in some files on older kernels.
};

/// Parse a PSI document.
/// @param document The file's text.
/// @return The reading, or a message naming the line that could not be read.
[[nodiscard]] std::expected<PsiReading, std::string> ParsePsi(std::string_view document);

/// A cgroup outside this node's own that carries the machine's stall and has a memory limit of its own
/// (`memory.max` or `memory.high` a number): a stall freeing machine memory cannot relieve when it is AT that limit.
struct ConfinedStall
{
    std::filesystem::path cgroup; ///< Its directory.
    PsiReading psi {};            ///< Its own `memory.pressure`.
    bool limited {};              ///< Whether it sets a numeric `memory.max` or `memory.high` of its own.
    /// Whether it stands at its own limit now: a limit's count in its own `memory.events.local` (the hierarchical
    /// `memory.events` where the kernel has no local file) rose since the previous look. Standing NEAR a limit is not
    /// enough: a cgroup whose page cache fills up to its limit and stops there stalls on nothing of its own.
    bool atLimit {};
};

/// One look at the machine's memory pressure.
struct PressureSample
{
    PsiReading system {};                  ///< The whole machine.
    std::optional<PsiReading> compiles {}; ///< The compiles' cgroup, when there is one.

    /// Why `compiles` is absent although a compiles cgroup was configured. Disengaged when no compiles
    /// path was configured or the read succeeded, so "not configured" and "unreadable" stay distinct.
    std::optional<std::string> compilesFault {};

    /// Whether the compiles' cgroup is at its own ceiling now: its `memory.events` `max` count rose since the
    /// previous sample, or its `memory.current` stands within `1 / CeilingNearDivisor` of a numeric `memory.max`.
    ///
    /// The evidence that a stall the compiles carry is the CEILING working rather than the machine running short:
    /// compiles far below their limit stall only because the machine reclaims from everything, a game's shortage
    /// included. False where no ceiling is set (`max`) and nothing rose. DISENGAGED where no compiles cgroup is
    /// configured, or where none of it can be read (`compilesLimitFault` says why) -- not-at-limit and could-not-tell
    /// are two answers, though a reader counts both as nobody's own stall, the safe direction (a yield).
    std::optional<bool> compilesAtLimit {};

    /// Why `compilesAtLimit` is disengaged although a compiles cgroup was configured: neither its `memory.events` nor
    /// its `memory.current` and `memory.max` could be read. Disengaged when no compiles path was configured or a
    /// reading was had.
    std::optional<std::string> compilesLimitFault {};

    /// The cgroups elsewhere on the machine that carry its stall under a memory limit of their own
    /// (`ConfinedStallScan`). Engaged and empty when the tree was read and none does; DISENGAGED where no scan is
    /// configured, or where the tree cannot be read (`confinedFault` says why) -- a reader counts both as no
    /// confined stall, the safe direction (the stall is the machine's).
    std::optional<std::vector<ConfinedStall>> confined {};

    /// Why `confined` is disengaged although a scan is configured: the cgroup tree's root could not be listed.
    std::optional<std::string> confinedFault {};
};

/// Where `ConfinedStallScan` looks, what it leaves out, and how much it may spend.
///
/// **Cost.** A walk lists a cgroup directory and reads one `memory.pressure` per child cgroup, and it descends only
/// into a child that carries at least `share` of the machine's stall -- a cgroup's PSI covers its whole subtree, so
/// no cgroup below one that does not can either. It runs only while the machine stalls at all, at most once per
/// `walkInterval`, and stops at `maxVisited` cgroups and `maxDepth` levels. A walk keeps the carriers that set a limit
/// of their own, at most `maxCandidates`; between walks a sample re-reads only those: four small files each.
///
/// **Staleness, both directions.** A cgroup that reaches its limit after a walk is found by the next: until then its
/// stall counts as the machine's, which yields when it need not -- admissions lost, never a freeze missed. A candidate
/// is re-judged at every sample, at its limit and by its share, so one that leaves its limit, or a shortage the rest
/// of the machine starts to stall on, counts at once -- except that the governor holds at-limit evidence for two
/// dwells, as it does the compiles', so a shed after a cgroup leaves its limit can come that much late.
struct ConfinedScanBounds
{
    std::filesystem::path root { DefaultCgroupMount }; ///< The cgroup v2 mount.
    /// Cgroups never counted as confined, with their ancestors and descendants: this node's own and its compiles'. A
    /// stall there is one the node's own yield relieves, which the compiles' own rule already judges.
    std::vector<std::filesystem::path> excluded {};
    double share { 0.8 };                     ///< Of the machine's stall, for a cgroup to carry it.
    std::chrono::seconds walkInterval { 10 }; ///< Between two walks of the tree.
    std::uint32_t maxVisited { 64 };          ///< Cgroups one walk reads at most.
    std::uint32_t maxDepth { 8 };             ///< Levels below the root one walk descends at most.
    std::uint32_t maxCandidates { 4 };        ///< Cgroups a walk keeps for the samples until the next.
};

/// The bounds a node scans with: the mount, and this process's own cgroup (`/proc/self/cgroup`) and @p compiles left
/// out. A process whose own cgroup cannot be read leaves out only @p compiles -- its own stall may then be discounted
/// where its own yield would relieve it, which the compiles' rule still catches for the compiles themselves.
/// @param host The filesystem seam.
/// @param compiles The compiles' cgroup directory, when one exists.
/// @param share Of the machine's stall, for a cgroup to carry it: the governor's `ownShare`.
/// @return The bounds.
[[nodiscard]] ConfinedScanBounds ConfinedScanBoundsFor(ICgroupHost const& host,
                                                       std::optional<std::filesystem::path> const& compiles,
                                                       double share);

/// Finds the cgroups that carry the machine's memory stall at a memory limit of their own -- one stalling at its own
/// `memory.max` raises the machine's PSI as surely as a shortage does, though freeing memory cannot relieve it.
/// Reads through `ICgroupHost` only, so a case scripts the tree. Not thread-safe; one watcher thread samples it.
class ConfinedStallScan
{
  public:
    /// @param host The filesystem seam.
    /// @param bounds Where it looks and what it may spend.
    /// @param clock Paces the walks.
    ConfinedStallScan(std::shared_ptr<ICgroupHost const> host,
                      ConfinedScanBounds bounds,
                      core::platform::IClock const& clock);

    /// Read the candidates, walking the tree first when one is due.
    /// @param system The machine's stall now, which a cgroup is measured against.
    /// @return The candidates, or why the tree cannot be read.
    [[nodiscard]] std::expected<std::vector<ConfinedStall>, std::string> Read(PsiReading const& system);

    /// @return How many walks it has made; a case asserts the bound with it.
    [[nodiscard]] std::uint32_t Walks() const noexcept;

  private:
    /// Walk the tree from the root, keeping the cgroups that carry @p system's stall under a limit of their own.
    /// @return What it kept, as it looked at them -- this sample's reading -- or why the root cannot be listed.
    [[nodiscard]] std::expected<std::vector<ConfinedStall>, std::string> Walk(PsiReading const& system);

    /// @return @p cgroup's reading, when it is a cgroup with a memory controller; refreshes its event counts.
    [[nodiscard]] std::optional<ConfinedStall> Look(std::filesystem::path const& cgroup);

    /// @return Whether @p cgroup is excluded, or an ancestor or descendant of one that is.
    [[nodiscard]] bool Excluded(std::filesystem::path const& cgroup) const;

    /// @return Whether @p cgroup is excluded or a descendant of one that is: a walk does not descend there.
    [[nodiscard]] bool InsideExcluded(std::filesystem::path const& cgroup) const;

    std::shared_ptr<ICgroupHost const> _host;
    ConfinedScanBounds _bounds;
    core::platform::IClock const& _clock;
    std::optional<core::platform::SteadyTimePoint> _walkedAt {};
    std::vector<std::filesystem::path> _candidates {};
    /// Each limit's `memory.events` count at the previous look, keyed `<cgroup>|<key>`; only rises count.
    std::map<std::string, std::uint64_t> _eventsSeen {};
    std::uint32_t _walks {};
};

/// Where pressure samples come from.
class IMemoryPressureSource
{
  public:
    IMemoryPressureSource() = default;
    IMemoryPressureSource(IMemoryPressureSource const&) = delete;
    IMemoryPressureSource(IMemoryPressureSource&&) = delete;
    IMemoryPressureSource& operator=(IMemoryPressureSource const&) = delete;
    IMemoryPressureSource& operator=(IMemoryPressureSource&&) = delete;
    virtual ~IMemoryPressureSource() = default;

    /// Take a sample.
    /// @return The sample, or why it could not be taken.
    [[nodiscard]] virtual std::expected<PressureSample, std::string> Sample() = 0;
};

/// A source reading `/proc/pressure/memory` and `<compiles>/memory.pressure`, and whether the compiles stand at
/// their ceiling from `<compiles>/memory.events`, `memory.current` and `memory.max`.
class PsiPressureSource final: public IMemoryPressureSource
{
  public:
    /// @param host The filesystem seam.
    /// @param compilesCgroup The compiles' cgroup directory, when one exists.
    /// @param confined The scan for stalls confined to a cgroup's own limit; null for none.
    PsiPressureSource(std::shared_ptr<ICgroupHost const> host,
                      std::optional<std::filesystem::path> compilesCgroup,
                      std::unique_ptr<ConfinedStallScan> confined = nullptr);

    [[nodiscard]] std::expected<PressureSample, std::string> Sample() override;

  private:
    /// @param compiles The compiles' cgroup directory.
    /// @return Whether the compiles stand at their ceiling now, or why that cannot be read; updates the `max` count.
    [[nodiscard]] std::expected<bool, std::string> CompilesAtLimit(std::filesystem::path const& compiles);

    std::shared_ptr<ICgroupHost const> _host;
    std::optional<std::filesystem::path> _compiles;
    std::unique_ptr<ConfinedStallScan> _confined;
    /// The compiles' `memory.events` `max` count at the previous sample; disengaged until one was read, so the
    /// first sample never reads as a rise.
    std::optional<std::uint64_t> _maxEventsSeen {};
};

} // namespace Pace::Platform
