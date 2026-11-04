// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LiveNodeConfig.hpp"
#include "NodeConditions.hpp"
#include "NodeReload.hpp"
#include "SharedCacheHost.hpp"

#include <Pace/Cache/StorageTier.hpp>
#include <Pace/Cluster/BudgetOverride.hpp>
#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/Logger.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/// @file BudgetOverrideApplier.hpp
/// Where this node applies the cache budget overrides its cluster records for it -- live, and so that **the later
/// change wins without comparing clocks** (the spec's Part 4).
///
/// An override is recorded by the leader and applied here, because only this machine knows what it configured. It
/// starts with no baseline; the first time this node applies it, it reports its own configured value
/// (MEMBER-REPORT `RecordBaseline`), and from then on every start, accepted reload and applied state compares the
/// configuration IN FORCE with that baseline. Unchanged, the override stands. Changed, the local edit is the later
/// change: the node says so at Warn naming both values, raises `cache-budget-override-dropped`, asks the leader to drop
/// exactly that override (MEMBER-REPORT `DropOverride`), and runs with its own value. Nothing here reads a clock --
/// which change is later is decided by whether the configuration moved since the baseline, never by two machines'
/// timestamps.

namespace Pace::Node
{

class CacheTier;
class IMcpUpstream;

/// What the applier does with one tier's override. **PRIVATE**: persisted and transmitted nowhere.
enum class OverrideAction : std::uint8_t
{
    None,                   ///< The override is already in force, or there is none.
    ReportBaselineAndApply, ///< No baseline is recorded: report this node's configured value, then apply.
    Apply,                  ///< The baseline equals what is configured and the override is not yet in force.
    DropLocalNewer,         ///< The configuration moved since the baseline: the local edit is later and wins.
    Last,                   ///< Not an action.
};

/// Decide what one tier's override asks of this node: a pure function of the recorded override, the configuration in
/// force and what is applied -- no clock, no state of its own.
///
/// - No override: `None` (the applier keeps the configured value).
/// - Baseline unset: `ReportBaselineAndApply`, whether or not applied -- a report is idempotent at the leader.
/// - Baseline equal to @p configured: `Apply`, or `None` when @p alreadyApplied.
/// - Baseline different from @p configured: `DropLocalNewer`.
/// @param recorded The override the cluster records for the tier, or nothing.
/// @param configured The tier's budget in the configuration in force now; zero is *unbounded*, a real value.
/// @param alreadyApplied Whether exactly this override is in force on the tier now.
/// @return What to do.
[[nodiscard]] OverrideAction DecideOverride(std::optional<Cluster::BudgetOverride> const& recorded,
                                            std::uint64_t configured,
                                            bool alreadyApplied) noexcept;

/// Where a tier's budget in force came from. **PRIVATE**: what travels is `BudgetSourceName`'s word.
enum class BudgetSource : std::uint8_t
{
    Configured, ///< The machine's own flag, in the configuration in force.
    Cluster,    ///< An override the cluster records for this member.
    Last,       ///< Not a source.
};

/// One row per `BudgetSource`: its word on every surface.
struct BudgetSourceRow
{
    BudgetSource source;   ///< The enumerator this row describes.
    std::string_view name; ///< What `pacectl node`, `node_status` and the dashboard print.
};

/// Every source, in enumerator order.
inline constexpr EnumTable<BudgetSource, BudgetSourceRow> BudgetSourceTable { {
    { .source = BudgetSource::Configured, .name = "configured" },
    { .source = BudgetSource::Cluster, .name = "cluster" },
} };
static_assert(RowsInEnumeratorOrder(BudgetSourceTable, &BudgetSourceRow::source),
              "BudgetSourceTable must hold one row per BudgetSource, in enumerator order");

/// @param source A source.
/// @return Its word.
[[nodiscard]] constexpr std::string_view BudgetSourceName(BudgetSource source) noexcept
{
    return BudgetSourceTable[static_cast<std::size_t>(source)].name;
}

/// One tier as the applier last left it: what is in force, where it came from, and what the cluster records.
struct TierBudgetStatus
{
    StorageTier tier {};                                ///< The tier.
    std::uint64_t configured { 0 };                     ///< Its budget in the configuration in force; zero is unbounded.
    std::optional<std::uint64_t> effective {};          ///< The budget in force on the tier; absent when it has no tier.
    BudgetSource source { BudgetSource::Configured };   ///< Where `effective` came from.
    std::optional<Cluster::BudgetOverride> recorded {}; ///< The override the cluster records, whether or not in force.
    /// Why the recorded override is not in force, or why the budget asked for could not be applied; empty when
    /// everything recorded is in force.
    std::string note {};

    [[nodiscard]] friend bool operator==(TierBudgetStatus const&, TierBudgetStatus const&) = default;
};

/// What `NODE-STATUS` reads the applier through.
class IBudgetOverrideStatusSource
{
  public:
    IBudgetOverrideStatusSource() = default;
    IBudgetOverrideStatusSource(IBudgetOverrideStatusSource const&) = delete;
    IBudgetOverrideStatusSource(IBudgetOverrideStatusSource&&) = delete;
    IBudgetOverrideStatusSource& operator=(IBudgetOverrideStatusSource const&) = delete;
    IBudgetOverrideStatusSource& operator=(IBudgetOverrideStatusSource&&) = delete;
    virtual ~IBudgetOverrideStatusSource() = default;

    /// @return One row per tier this node has or the cluster records an override for, in `StorageTier` order. Any
    ///         thread.
    [[nodiscard]] virtual std::vector<TierBudgetStatus> BudgetStatus() const = 0;
};

/// The tiers an override is applied to: `CacheTier`'s budget API as a seam, so the decision is testable without a store.
class ITierBudgets
{
  public:
    ITierBudgets() = default;
    ITierBudgets(ITierBudgets const&) = delete;
    ITierBudgets(ITierBudgets&&) = delete;
    ITierBudgets& operator=(ITierBudgets const&) = delete;
    ITierBudgets& operator=(ITierBudgets&&) = delete;
    virtual ~ITierBudgets() = default;

    /// @param tier A tier.
    /// @return Whether this node's cache has it.
    [[nodiscard]] virtual bool HasTier(StorageTier tier) const = 0;

    /// Give @p tier an override's budget: never zero (`CacheTier::ApplyTierBudget`).
    /// @param tier The tier.
    /// @param bytes The budget.
    /// @return Nothing, or why it was not applied.
    [[nodiscard]] virtual std::expected<void, std::string> ApplyOverride(StorageTier tier, std::uint64_t bytes) = 0;

    /// Give @p tier back its configured budget, zero meaning what it meant at startup (`CacheTier::RestoreTierBudget`).
    /// @param tier The tier.
    /// @param bytes The configured budget.
    /// @return Nothing, or why it was not applied.
    [[nodiscard]] virtual std::expected<void, std::string> RestoreConfigured(StorageTier tier, std::uint64_t bytes) = 0;
};

/// Production's `ITierBudgets`: the node's cache tier, or none.
class CacheTierBudgets final: public ITierBudgets
{
  public:
    /// @param tier The node's cache tier, or null on a node with none; must outlive this.
    explicit CacheTierBudgets(CacheTier* tier) noexcept:
        _tier { tier }
    {
    }

    /// @copydoc ITierBudgets::HasTier
    [[nodiscard]] bool HasTier(StorageTier tier) const override;

    /// @copydoc ITierBudgets::ApplyOverride
    [[nodiscard]] std::expected<void, std::string> ApplyOverride(StorageTier tier, std::uint64_t bytes) override;

    /// @copydoc ITierBudgets::RestoreConfigured
    [[nodiscard]] std::expected<void, std::string> RestoreConfigured(StorageTier tier, std::uint64_t bytes) override;

  private:
    CacheTier* _tier;
};

/// How this node tells its leader about its own overrides: MEMBER-REPORT, under this node's own proof.
class IMemberReportClient
{
  public:
    IMemberReportClient() = default;
    IMemberReportClient(IMemberReportClient const&) = delete;
    IMemberReportClient(IMemberReportClient&&) = delete;
    IMemberReportClient& operator=(IMemberReportClient const&) = delete;
    IMemberReportClient& operator=(IMemberReportClient&&) = delete;
    virtual ~IMemberReportClient() = default;

    /// Report what this node had configured for @p tier when it applied override @p overrideId.
    /// @param tier The tier.
    /// @param baseline The configured budget; zero is unbounded.
    /// @param overrideId The override it was measured against.
    /// @return Nothing once the leader accepted it, or why not.
    [[nodiscard]] virtual std::expected<void, std::string> RecordBaseline(StorageTier tier,
                                                                          std::uint64_t baseline,
                                                                          std::uint64_t overrideId) = 0;

    /// Ask the leader to drop exactly override @p overrideId of @p tier: a newer one set since stands.
    /// @param tier The tier.
    /// @param overrideId The override this node's later configuration outranks.
    /// @return Nothing once the leader accepted it, or why not.
    [[nodiscard]] virtual std::expected<void, std::string> DropOverride(StorageTier tier, std::uint64_t overrideId) = 0;
};

/// Production's `IMemberReportClient`: one MEMBER-REPORT frame put to `UpstreamTarget::Leader`.
///
/// **No route of its own, and none is needed** (Task 18's re-review): on the leader the upstream answers in-process
/// through `FrameRouterAnswerer`, which presents this node's own proof, and on any other node it dials the leader under
/// that proof (`ProvenSchedulerDialer`) and follows `NotLeader` -- so the report passes MEMBER-REPORT's
/// `ProvenNodeOnly` door either way, and the leader attributes it to this machine. It never dials itself.
class UpstreamMemberReportClient final: public IMemberReportClient
{
  public:
    /// @param upstream The MCP surface's way to the fleet; must outlive this.
    explicit UpstreamMemberReportClient(IMcpUpstream& upstream) noexcept:
        _upstream { upstream }
    {
    }

    /// @copydoc IMemberReportClient::RecordBaseline
    [[nodiscard]] std::expected<void, std::string> RecordBaseline(StorageTier tier,
                                                                  std::uint64_t baseline,
                                                                  std::uint64_t overrideId) override;

    /// @copydoc IMemberReportClient::DropOverride
    [[nodiscard]] std::expected<void, std::string> DropOverride(StorageTier tier, std::uint64_t overrideId) override;

  private:
    IMcpUpstream& _upstream;
};

/// Whether this PROCESS has dropped a cluster override for a later local edit, and what was said: the latch behind
/// `cache-budget-override-dropped`.
///
/// **Owned by `main`, across every body**, beside `LeaseCheckInForce`: a reform builds a new body -- a new condition
/// registry and a new applier -- and a latched row is the claim that nothing this process does clears it, a reform
/// included. Each applier raises the row from here at construction, so a drop an earlier body made stays raised.
class BudgetDropLatch
{
  public:
    /// Record a drop, the newest detail kept. Any thread.
    /// @param detail What was said about it.
    void Record(std::string detail);

    /// @return The newest drop's detail, or nothing while this process has dropped none. Any thread.
    [[nodiscard]] std::optional<std::string> Detail() const;

  private:
    mutable std::mutex _mutex;
    std::optional<std::string> _detail; ///< Guarded by `_mutex`.
};

/// How long a report the leader did not take waits before the applier asks again with nothing else changing.
///
/// A retry, not a correctness bound: a report also goes again at the next applied state or accepted reload. It is what
/// lets an override set while the leader was unreachable from here take effect without an operator's nudge -- and it
/// is never a hot loop: a failed apply is retried only beside a pending report or on a trigger.
inline constexpr std::chrono::seconds BudgetReportRetryInterval { 30 };

/// Applies this member's cache budget overrides to its tiers, live.
///
/// **Off the consensus apply path and off every reactor.** `Applied` records the latest state and wakes the
/// applier's own thread; that thread calls `ApplyTierBudget` (a memory shrink holds the shard lock for its one-shot
/// eviction) and sends MEMBER-REPORT, which blocks for a round trip. On the apply callback either would be a voter
/// missing heartbeats; on a reactor, every `0xFC` surface stalling. A burst of applied states is COALESCED: only the
/// newest is kept, and one pass reconciles to it.
///
/// **The configuration in force, never the start's** (`INodeConfigSource`): a reload that changed `--cache-memory`
/// is what the next pass compares the baseline with.
///
/// **Shutdown is `Stop()`, called by `WorkerBody` as soon as its serve loop ends** -- before any surface is torn down.
/// Destruction order alone is not enough: on the leader a report is answered in-process through the node's own `0xFC`
/// router (`FrameRouterAnswerer`), and that router is torn down BEFORE this object is destroyed, so a pass run in that
/// window would post onto a freed responder or wait out `McpUpstream::CallDeadline` on a loop nobody turns. After
/// `Stop()` no pass starts and `OnState`/`ReloadAccepted` do nothing; the destructor calls it again, harmlessly.
class BudgetOverrideApplier final: public IBudgetOverrideStatusSource, public IAcceptedReloadListener
{
  public:
    /// @param tiers The tiers overrides are applied to; must outlive this.
    /// @param reports How this node reports to its leader; must outlive this.
    /// @param config The configuration in force, read at every pass; must outlive this.
    /// @param logger Where a dropped override is said at Warn, and a failure once per change.
    /// @param conditions Where `cache-budget-override-dropped` is raised, or null on a node running no consensus,
    ///        whose conditions answer the row from its scope.
    /// @param dropped The process's latch for that row, which outlives every body; must outlive this.
    /// @param selfId This node's member id: whose record the overrides are read from.
    /// @param on Where reconciliation runs: the applier's own thread, or only a caller of `Reconcile` (a test).
    BudgetOverrideApplier(ITierBudgets& tiers,
                          IMemberReportClient& reports,
                          INodeConfigSource const& config,
                          ILogger& logger,
                          NodeConditions* conditions,
                          BudgetDropLatch& dropped,
                          std::string selfId,
                          ReconcileOn on);

    BudgetOverrideApplier(BudgetOverrideApplier const&) = delete;
    BudgetOverrideApplier& operator=(BudgetOverrideApplier const&) = delete;
    BudgetOverrideApplier(BudgetOverrideApplier&&) = delete;
    BudgetOverrideApplier& operator=(BudgetOverrideApplier&&) = delete;

    /// `Stop()`.
    ~BudgetOverrideApplier() override;

    /// Stop for good: no pass starts after this returns, and `OnState`/`ReloadAccepted` are no-ops. Joins the
    /// applier's thread, so a pass already running finishes the tier it is on first. Idempotent; any thread but the
    /// applier's own.
    void Stop();

    /// Record @p state as the newest the cluster applied and wake the applier. Any thread; never applies or reports.
    /// @param state The state the cluster just applied.
    void OnState(Cluster::ClusterState const& state);

    /// An accepted reload changed the configuration in force: wake the applier to compare it. Any thread.
    void ReloadAccepted() override;

    /// Reconcile once against the newest state and the configuration in force now. On the applier's thread, or on a
    /// test's.
    void Reconcile();

    /// @copydoc IBudgetOverrideStatusSource::BudgetStatus
    [[nodiscard]] std::vector<TierBudgetStatus> BudgetStatus() const override;

  private:
    /// What one tier runs with now.
    struct InForce
    {
        std::uint64_t bytes { 0 };                        ///< The budget in force.
        BudgetSource source { BudgetSource::Configured }; ///< Where it came from.
        std::uint64_t overrideId { 0 };                   ///< The override in force, when `source` is `Cluster`.
    };

    /// @return Whether `Stop()` was called.
    [[nodiscard]] bool Stopped() const;

    /// The thread's body.
    /// @param stop Requested by the destructor.
    void Run(std::stop_token const& stop);

    /// Reconcile one tier; on the reconciling thread.
    /// @param tier The tier.
    /// @param recorded The override the newest state records for it.
    /// @param configured Its budget in the configuration in force.
    /// @return Its status, and whether a report is still owed.
    [[nodiscard]] std::pair<TierBudgetStatus, bool> ReconcileTier(StorageTier tier,
                                                                  std::optional<Cluster::BudgetOverride> const& recorded,
                                                                  std::uint64_t configured);

    /// Drop @p recorded because the configuration moved since its baseline: said once per override, raised, reported.
    /// @param tier The tier.
    /// @param recorded The override.
    /// @param baseline The baseline it records.
    /// @param configured The configured value that outranks it.
    /// @return Nothing once the leader took the drop, or why not.
    [[nodiscard]] std::expected<void, std::string> DropLocalNewer(StorageTier tier,
                                                                  Cluster::BudgetOverride const& recorded,
                                                                  std::uint64_t baseline,
                                                                  std::uint64_t configured);

    /// Put @p bytes from @p source in force on @p tier, unless it already is.
    /// @param tier The tier.
    /// @param bytes The budget.
    /// @param source Where it comes from.
    /// @param overrideId The override, for `BudgetSource::Cluster`.
    /// @return Nothing, or why the tier refused it.
    [[nodiscard]] std::expected<void, std::string> Enforce(StorageTier tier,
                                                           std::uint64_t bytes,
                                                           BudgetSource source,
                                                           std::uint64_t overrideId);

    ITierBudgets& _tiers;
    IMemberReportClient& _reports;
    INodeConfigSource const& _config;
    ILogger& _logger;
    NodeConditions* _conditions;
    BudgetDropLatch& _dropped;
    std::string _selfId;

    mutable std::mutex _mutex;                        ///< Guards `_pending`, `_dirty`, `_retry` and `_stopped`.
    std::condition_variable_any _wake;                ///< States, reloads and stops.
    std::optional<Cluster::ClusterState> _pending {}; ///< The newest state applied; guarded by `_mutex`.
    bool _dirty { true };                             ///< A pass is owed; guarded by `_mutex`.
    bool _retry { false };                            ///< A report is still owed; guarded by `_mutex`.
    bool _stopped { false };                          ///< `Stop()` was called; guarded by `_mutex`.

    /// Reconciler-only: one pass at a time (`Reconcile` may also be called by a test).
    std::mutex _reconcileMutex;
    EnumTable<StorageTier, InForce> _inForce {}; ///< Guarded by `_reconcileMutex`.
    std::set<std::uint64_t> _droppedSaid {};     ///< Overrides already said dropped; by `_reconcileMutex`.
    std::set<std::pair<StorageTier, std::string>> _failuresSaid {}; ///< Failures already logged; by `_reconcileMutex`.

    mutable std::mutex _statusMutex;          ///< Guards `_status`.
    std::vector<TierBudgetStatus> _status {}; ///< The last pass's rows; guarded by `_statusMutex`.

    /// Last, so it stops and joins before anything above it goes.
    std::jthread _thread;
};

/// Stops a `BudgetOverrideApplier` when it goes out of scope: declared after whatever the applier's reports reach, so on
/// every way out of the owner's scope -- an early return included -- the applier stops before that is destroyed.
class StopBeforeTeardown
{
  public:
    /// @param applier Stopped by the destructor; must outlive this.
    explicit StopBeforeTeardown(BudgetOverrideApplier& applier) noexcept:
        _applier { applier }
    {
    }

    StopBeforeTeardown(StopBeforeTeardown const&) = delete;
    StopBeforeTeardown& operator=(StopBeforeTeardown const&) = delete;
    StopBeforeTeardown(StopBeforeTeardown&&) = delete;
    StopBeforeTeardown& operator=(StopBeforeTeardown&&) = delete;

    /// `BudgetOverrideApplier::Stop()`.
    ~StopBeforeTeardown()
    {
        _applier.Stop();
    }

  private:
    BudgetOverrideApplier& _applier;
};

} // namespace Pace::Node
