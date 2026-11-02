// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"
#include "LocalCache.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeRefusal.hpp"
#include "Responders.hpp"
#include "SharedCacheDirectory.hpp"
#include "TierStatsSource.hpp"

#include <Pace/Cache/IStorage.hpp>
#include <Pace/Cache/InMemoryLruStorage.hpp>
#include <Pace/Cache/StorageTier.hpp>
#include <Pace/Core/Compression.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/NodePolicy.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Platform/HostInfo.hpp>
#include <Pace/Platform/LocalAddresses.hpp>
#include <Pace/Platform/ProcessExit.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <core/platform/Clock.hpp>

namespace Pace::Node
{

class NodeIoLoop;
class NodeProofClient;
class SharedCacheHost;

/// Which upstream a node's private tier reads through to. **PRIVATE: in-process only**, never
/// transmitted or persisted, so it carries no values.
enum class UpstreamKind : std::uint8_t
{
    None,             ///< No `--upstream`, and no identity to prove to the fleet's shared cache with.
    Daemon,           ///< `--upstream`: a pace-kv, or anything speaking its verbs. Presents `--requirepass`.
    FleetSharedCache, ///< The fleet's `shared-cache` setting, whichever machine it names -- this one included.
    InProcessShared,  ///< This machine's own shared tier, read in process: the other half of the setting.
    Last,             ///< Not a kind: the length of a table keyed by one.
};

/// What `MakeCacheUpstream` chooses between and builds from. Borrowed throughout: every member
/// outlives the upstream built from it.
///
/// **One loop, and nothing that could name a second.** The shared cache's dialer connects on
/// `io`'s connector and its session pool keeps sessions on `io`'s reactor; a record that also
/// carried a connector would let the two be different loops, which no type could then catch
/// (`SharedSessionPool`'s own note says what that breaks).
struct UpstreamParts
{
    std::string_view upstream;                 ///< `--upstream`, or empty.
    ICredentialSource const& credential;       ///< Presented on the `--upstream` leg ONLY.
    ISharedCacheTargetSource const& directory; ///< What the fleet's setting resolves to now.
    NodeProofClient const* prover;             ///< This machine's identity, or null when it holds none.
    NodeIoLoop& io;                            ///< Connector, reactor and resolver: ONE loop.
    core::platform::IClock& clock;             ///< The upstream's refreshes and idle ages.
    IMetricsSink& metrics;                     ///< Where each upstream counts.
    NodeConditions* conditions;                ///< Where `shared-cache-unproven` is answered; may be null.
    ILogger& logger;                           ///< Where each upstream reports.
    SharedCacheHost const* host;               ///< This machine's shared tier, for the in-process half; may be null.
};

/// Builds one kind's upstream. Never returns null.
using UpstreamBuilder = std::unique_ptr<ICacheUpstream> (*)(UpstreamParts const& parts);

/// One row per kind: which verb pair it speaks, how the startup line names it, and how it is built.
struct UpstreamKindRow
{
    UpstreamKind kind;                                 ///< The kind this row describes.
    std::optional<CompileCacheWire::CacheVerbs> verbs; ///< Absent for the two that send nothing.
    std::string_view described;                        ///< What the startup line says after `upstream`.
    bool namesEndpoint;                                ///< Whether that is preceded by `--upstream`'s value.
    UpstreamBuilder build;                             ///< Never null.
};

/// @param parts Unused.
/// @return `NoUpstream`.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildNoUpstream(UpstreamParts const& parts);

/// @param parts `upstream`, `credential`, `io`, `clock` and `logger`.
/// @return A `RemoteUpstream` to `--upstream`, presenting `--requirepass`.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildRemoteUpstream(UpstreamParts const& parts);

/// The fleet's shared cache as the setting names it: a proven session to another machine and,
/// where there is a host, this machine's own tier in process -- chosen per operation by the
/// directory (`SwitchingSharedUpstream`, in `CacheTier.cpp`).
/// @param parts `prover` must not be null.
/// @return The upstream.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildSharedCacheUpstream(UpstreamParts const& parts);

/// @param parts `host`; a null host builds `NoUpstream`, a machine that cannot serve the tier.
/// @return An `InProcessSharedUpstream` over the host.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildInProcessSharedUpstream(UpstreamParts const& parts);

/// One row per `UpstreamKind`, in enumerator order.
inline constexpr EnumTable<UpstreamKind, UpstreamKindRow> UpstreamKindTable { {
    { .kind = UpstreamKind::None,
      .verbs = std::nullopt,
      .described = "none",
      .namesEndpoint = false,
      .build = &BuildNoUpstream },
    { .kind = UpstreamKind::Daemon,
      .verbs = CompileCacheWire::DaemonCacheVerbs,
      .described = "(override, pace-kv verbs)",
      .namesEndpoint = true,
      .build = &BuildRemoteUpstream },
    { .kind = UpstreamKind::FleetSharedCache,
      .verbs = CompileCacheWire::FleetSharedCacheVerbs,
      .described = "shared-cache (the fleet setting; resolved at every apply)",
      .namesEndpoint = false,
      .build = &BuildSharedCacheUpstream },
    { .kind = UpstreamKind::InProcessShared,
      .verbs = std::nullopt,
      .described = "shared-cache (this machine's own tier, in process)",
      .namesEndpoint = false,
      .build = &BuildInProcessSharedUpstream },
} };
static_assert(RowsInEnumeratorOrder(UpstreamKindTable, &UpstreamKindRow::kind),
              "UpstreamKindTable must hold one row per UpstreamKind, in enumerator order");

/// The kind this node's private tier reads through to: the ONE decision, which `MakeCacheUpstream`
/// only looks up.
///
/// `--upstream` wins -- the operator typed that address on this machine, and it is fixed for the
/// process (`Reloadable::No`) -- then the fleet's setting for a node with an identity to prove,
/// then nothing. Never `InProcessShared`: that half is built beside the fleet's, and which of the
/// two answers is the directory's to say per operation, since the setting can name this machine
/// or another at runtime.
/// @param parts What the node has.
/// @return The kind.
[[nodiscard]] UpstreamKind UpstreamKindOf(UpstreamParts const& parts) noexcept;

/// The private tier's upstream, as the kind's row builds it.
/// @param parts What the node has.
/// @return The upstream; never null.
[[nodiscard]] std::unique_ptr<ICacheUpstream> MakeCacheUpstream(UpstreamParts const& parts);

/// Told after a tier's budget moved, so whatever was DERIVED from the tiers is derived again.
///
/// The cache tier cannot recompute those figures itself -- `NodeCapacityOf` needs the
/// configuration and the machine, which the tier does not hold -- and must not leave them to
/// whoever asked for the resize, who would then be one more caller able to forget.
class ITierBudgetListener
{
  public:
    virtual ~ITierBudgetListener() = default;

    /// A tier's budget has just moved; re-read whatever is derived from what the tiers hold.
    /// Called under the tier's budget lock, after the resize, from the resizing thread.
    virtual void TierBudgetApplied() = 0;

  protected:
    ITierBudgetListener() = default;
    ITierBudgetListener(ITierBudgetListener const&) = default;
    ITierBudgetListener& operator=(ITierBudgetListener const&) = default;
    ITierBudgetListener(ITierBudgetListener&&) = default;
    ITierBudgetListener& operator=(ITierBudgetListener&&) = default;
};

/// How the eviction driver paces itself.
struct EvictionPacing
{
    /// How long one run of back-to-back slices may last before the driver pauses. Measured on
    /// the injected clock, so a manual clock that never moves runs until the store fits.
    std::chrono::milliseconds runBudget;
    /// How long the driver pauses between runs, so traffic on the store's lock is never shut
    /// out by a run that has used its budget; ended early only by a stop.
    std::chrono::milliseconds pause;
    /// What the driver does between two slices of one run, with the store's lock RELEASED:
    /// a yield in production, so a thread parked on that lock gets it before the next slice.
    /// A test that must prove the interleaving waits here for traffic to complete. Handed the
    /// driver's stop token, so anything that waits here ends when the driver is stopped.
    std::function<void(std::stop_token const&)> betweenSlices;
};

/// The pacing a node runs with: runs of at most 200 ms, each followed by a 50 ms pause.
///
/// A slice is `CowTreeStorage::EvictSliceRecords` records, one batched flush, so a run is many
/// short holds of the shard lock with a yield between each; the pause bounds the driver to
/// four fifths of wall time even against a disk that makes every slice slow. At a few
/// milliseconds per slice that drains hundreds of thousands of records a minute, so halving a
/// store of millions converges in minutes where a once-a-minute slice took hours.
[[nodiscard]] EvictionPacing DefaultEvictionPacing();

/// Carries a store over its budget down to it, slice by slice, on a thread of its own.
///
/// **Its own thread, never a reactor's, and never the sampler's.** A reactor serves every
/// `0xFC` surface on the node, and a slice is commits; the sampler's minute is a history
/// cadence, and a driver that waits a minute between slices is the hours-long convergence
/// this replaces. So a stoppable thread, idle on a condition variable until `Kick`ed: by an
/// applied budget, at startup (a store reopened over its budget), and by the sampler's minute
/// as a fallback.
///
/// **The store's lock is released between slices.** Each `IStorage::EvictStep` takes and
/// releases the shard mutex itself; between two calls the driver runs
/// `EvictionPacing::betweenSlices`, and after `runBudget` it pauses. A run ends when the store
/// fits, or when a step evicted nothing while still over -- a budget below what the store can
/// shrink to, or a disk refusing every erase -- so a driver that cannot make progress waits
/// for the next kick instead of spinning.
///
/// **Stopped and joined by its destructor**, which the owner runs before the store goes: a
/// stop lands at the next slice boundary, so it waits at most one slice.
///
/// **Never ends the process.** A run that throws -- an allocation failure, a storage fault
/// surfaced as an exception -- is caught at the thread's loop, said once at Warn with how far it
/// got, counted (`FailedRuns`), and the driver waits for the next kick.
class TierEvictionDriver
{
  public:
    /// Start the thread, idle until kicked.
    /// @param storage The store to step; must outlive this.
    /// @param clock What a run's budget is measured on; must outlive this.
    /// @param logger Where a run that failed is reported; must outlive this.
    /// @param converged Called on the driver's thread when a run that found the store over its
    ///        budget brought it to fit; empty for nothing.
    /// @param pacing How the driver paces itself.
    TierEvictionDriver(IStorage& storage,
                       core::platform::IClock& clock,
                       ILogger& logger,
                       std::function<void()> converged,
                       EvictionPacing pacing);

    /// Stops the thread and joins it.
    ~TierEvictionDriver();

    TierEvictionDriver(TierEvictionDriver const&) = delete;
    TierEvictionDriver& operator=(TierEvictionDriver const&) = delete;
    TierEvictionDriver(TierEvictionDriver&&) = delete;
    TierEvictionDriver& operator=(TierEvictionDriver&&) = delete;

    /// Ask for a run. Cheap and safe from any thread; kicks while a run is going are folded
    /// into one more look once it ends.
    void Kick();

    /// @return How many slices the driver has run, ever. Any thread.
    [[nodiscard]] std::uint64_t Slices() const noexcept
    {
        return _slices.load(std::memory_order_acquire);
    }

    /// @return How many runs have brought an over-budget store to fit. Any thread.
    [[nodiscard]] std::uint64_t Convergences() const noexcept
    {
        return _convergences.load(std::memory_order_acquire);
    }

    /// @return How many runs ended in an exception, caught at the thread's loop. Any thread.
    [[nodiscard]] std::uint64_t FailedRuns() const noexcept
    {
        return _failedRuns.load(std::memory_order_acquire);
    }

  private:
    /// The thread's body: wait for a kick, run, repeat until stopped.
    /// @param stop Requested by the destructor.
    void Loop(std::stop_token const& stop);

    /// One run: slices back to back until the store fits, stops progressing, or is stopped.
    /// @param stop Requested by the destructor.
    void Run(std::stop_token const& stop);

    /// Say, once, that a run ended in an exception, and count it. Never throws: it runs where an
    /// exception would end the process.
    /// @param why What the exception said.
    /// @param slices How many slices the run had taken before it failed.
    void ReportFailedRun(std::string_view why, std::uint64_t slices) noexcept;

    IStorage& _storage;
    core::platform::IClock& _clock;
    ILogger& _logger;
    std::function<void()> _converged;
    EvictionPacing _pacing;
    std::mutex _mutex;                 ///< Guards `_kicked`.
    std::condition_variable_any _wake; ///< Kicks and stops.
    bool _kicked { false };
    std::atomic<std::uint64_t> _slices { 0 };
    std::atomic<std::uint64_t> _convergences { 0 };
    std::atomic<std::uint64_t> _failedRuns { 0 };
    std::atomic<std::uint64_t> _unreportedFailures { 0 }; ///< Failed runs whose report itself failed.
    /// Last, so it stops and joins before anything above it goes.
    std::jthread _thread;
};

/// The node's cache surface: storage, upstream, read-through logic, protocol and
/// listener, owned as one thing.
///
/// A class rather than six locals in `WorkerBody`, for two reasons and the second is
/// the one that matters. They form a *reference chain* -- the endpoint holds the
/// responder, which holds the proxy, which holds the cache, which holds the storage
/// and the upstream -- so their declaration order in a function body is load-bearing
/// and silently so; getting it wrong is a dangling reference rather than a compile
/// error. Here the order is the member order, which is checked by the language.
///
/// And `WorkerBody` reached a cognitive complexity of 67 against clang-tidy's limit
/// of 60 when this was inline. That number is a symptom worth listening to rather
/// than a rule to argue with: it went over because a coherent decision with one
/// answer -- "does this node cache, and where" -- had been spread across a
/// function that already had several.
///
/// It is the `ITierStatsSource` `node-status` reads its tiers through.
class CacheTier final: public ITierStatsSource
{
  public:
    /// Build the tier and start serving it.
    ///
    /// The error is a diagnostic string rather than one of the project's error enums,
    /// the same departure `AdminEndpoint::Start` documents and for the same reason:
    /// this fails in ways belonging to two taxonomies and the caller's response is
    /// identical either way.
    /// The storage arrives already built, rather than being assembled in here, and
    /// that is what keeps two unequal failures apart. A store that will not open is
    /// always fatal; a port that will not bind is fatal only when the operator typed
    /// the address. Both reduce to a diagnostic string, so building the store inside
    /// this function would leave `StartCacheTierOrExplain` unable to tell which it
    /// was holding — and a bad `--cache-dir` on a node using the default cache port
    /// would be logged as a warning and stepped over.
    /// @param cfg The parsed configuration.
    /// @param storage Where this tier keeps objects; already opened.
    /// @param upstream What the tier's upstream is built from, by `MakeCacheUpstream`; the startup
    ///        line names the same kind, so the two cannot describe different choices. Every part
    ///        must outlive the tier.
    /// @param locality Decides whether a caller is on this machine -- the whole of
    ///        who may read this tier, since #287; must outlive it.
    /// @param clock Time source for the tier's expiry; must outlive the tier.
    /// @param metrics Where hits, misses, upstream outcomes and locality refusals
    ///        are counted.
    /// @param logger Where to announce what the tier holds.
    /// @return The tier, or why it could not be built.
    ///
    /// **Binds nothing since #290.** The cache verbs are answered on the node's one
    /// `0xFC` listener, beside the scheduler's, so the tier is a component this
    /// process owns rather than a surface it opens -- and what opens that listener is
    /// `StartNodeSurfaceOrExplain`, which is also where a bind failure is judged.
    [[nodiscard]] static std::expected<std::unique_ptr<CacheTier>, std::string> Start(NodeConfig const& cfg,
                                                                                      std::unique_ptr<IStorage> storage,
                                                                                      UpstreamParts const& upstream,
                                                                                      ILocalityOracle const& locality,
                                                                                      core::platform::IClock& clock,
                                                                                      IMetricsSink& metrics,
                                                                                      ILogger& logger);

    ~CacheTier() override = default;

    CacheTier(CacheTier const&) = delete;
    CacheTier& operator=(CacheTier const&) = delete;
    CacheTier(CacheTier&&) = delete;
    CacheTier& operator=(CacheTier&&) = delete;

    /// What answers the cache verbs on this node's `0xFC` listener.
    ///
    /// Handed to `MergedResponder`, which routes each frame to the component owning
    /// its verb family. The tier owns no listener of its own (#290).
    /// @return This tier's responder; outlives no longer than the tier.
    [[nodiscard]] IFrameResponder& Responder() noexcept
    {
        return _responder;
    }

    /// What this node's cache holds, as one figure.
    ///
    /// Safe from any thread, which is the whole reason the storage below is behind
    /// a `ShardedStorage`: the tier is mutated on the reactor thread while the
    /// heartbeat thread and the `/metrics` scrape read it.
    /// @return The cache's own statistics.
    [[nodiscard]] StorageStats Snapshot() const noexcept
    {
        return _storage->Snapshot();
    }

    /// The same, kept apart by the tier holding each number.
    ///
    /// What `Snapshot()` cannot say when both halves are configured: the composite
    /// reports the on-disk store's item count, bytes and budget, so the in-memory
    /// half above it leaves no trace. See `IStorage::SnapshotTiers` for why these
    /// are per-tier answers rather than a total waiting to be summed.
    /// @return One entry per tier this node configured.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override
    {
        return _storage->SnapshotTiers();
    }

    /// Give one tier of the running store a new budget.
    ///
    /// The API a cluster budget override is applied through. The memory tier evicts to the
    /// new budget before this returns (in memory, no I/O); the disk tier adopts the budget and
    /// evicts NOTHING here, and this tier's eviction driver is kicked to carry it down slice
    /// by slice on its own thread, so neither this call nor any reactor holds the store's
    /// lock for a drain. What is still over is visible as `IStorage::BytesOverBudget` of that
    /// tier's `SnapshotTiers()` entry, the one derivation every surface reports it through. A
    /// grow evicts nothing.
    ///
    /// **Locking.** Applies are serialised by this tier's budget mutex, so a resize and the
    /// recomputation it triggers are one step against other applies. The budget itself and
    /// the eviction cursor live in the store, under the `ShardedStorage` shard mutex that
    /// every cache verb, every `Snapshot` and the driver's `EvictStep` also take -- never
    /// across more than one slice of commits, and never across a tree walk (a slice gathers
    /// its victims under a read snapshot and erases after it). Lock order: budget mutex, then
    /// shard mutex, then the listener's own, then the driver's; nothing takes them the other
    /// way round, and the driver takes the budget mutex (to tell the listener it converged)
    /// holding nothing else.
    ///
    /// After the resize the listener (`SetBudgetListener`) is told, which is what re-derives
    /// the machine's capacity -- the reserve held back from compiles and the budget the
    /// node reports -- from what the tiers now HOLD rather than from the number asked for.
    /// It is told again when the driver brings the store to fit, since the key index the
    /// reserve counts shrinks with it.
    /// @param tier Which tier.
    /// @param bytes The new budget. Zero is refused (`TierOverrideCannotRemoveTier`), as is
    ///        anything `CheckTierBudget` refuses.
    /// @return Nothing, or why the budget was not applied -- a zero, or a tier this node
    ///         does not have ("no disk tier on this node"), in which case nothing changed.
    [[nodiscard]] std::expected<void, std::string> ApplyTierBudget(StorageTier tier, std::size_t bytes);

    /// Ask the eviction driver for a run: the sampler's minute calls this as a fallback, so a
    /// store left over its budget by anything (a reopen, a run that could not progress) is
    /// looked at again. Cheap when nothing is over.
    void KickEviction();

    /// @return The tier's eviction driver, for what it has done. Any thread.
    [[nodiscard]] TierEvictionDriver const& Eviction() const noexcept
    {
        return _eviction;
    }

    /// Who is told when `ApplyTierBudget` moved a budget.
    ///
    /// Taken under the budget mutex, so clearing it (as `LiveNodeCapacity`'s destructor
    /// does) cannot race an apply that is about to call it. A listener being SET is told once
    /// at once, under that lock, so a change it was not yet registered for is not lost.
    /// @param listener The listener, or nullptr for none; must outlive its registration.
    void SetBudgetListener(ITierBudgetListener* listener);

    /// Whether this tier reads through to a shared cache.
    ///
    /// Read off the upstream that was actually BUILT rather than off the
    /// configuration that asked for one, the same way `StorageTiersOf` reads the
    /// storage: those are the two things that can disagree, and the scrape must
    /// describe what is running.
    /// @return True when a shared cache is configured.
    [[nodiscard]] bool HasUpstream() const noexcept
    {
        return _upstream->Configured();
    }

    /// The upstream this tier reads through to, for whoever tells it the cluster applied a state.
    /// @return The upstream; outlives no longer than the tier.
    [[nodiscard]] ICacheUpstream& Upstream() noexcept
    {
        return *_upstream;
    }

    /// @return How reaching the fleet's shared cache went, where this tier reads through to it;
    ///         null otherwise. See `ICacheUpstream::SharedCacheStatus`.
    [[nodiscard]] ISharedCacheStatusSource const* SharedCacheStatus() const noexcept
    {
        return _upstream->SharedCacheStatus();
    }

  private:
    CacheTier(std::unique_ptr<IStorage> storage,
              std::unique_ptr<ICacheUpstream> upstream,
              ILocalityOracle const& locality,
              core::platform::IClock& clock,
              IMetricsSink& metrics,
              ILogger& logger);

    /// Tell the listener the tiers moved, under the budget mutex. The driver's convergence.
    void NotifyBudgetListener();

    // Declaration order IS construction order, and every one of these is referenced
    // by the one below it. Reordering them is a dangling reference, which is why
    // they live here rather than as locals somebody has to keep in the right order.
    std::unique_ptr<IStorage> _storage;
    std::unique_ptr<ICacheUpstream> _upstream;
    LocalCache _cache;
    CacheProxy _proxy;
    CacheResponder _responder;

    /// Serialises `ApplyTierBudget` and `SetBudgetListener`. Guards no figure: the budgets
    /// live in the store, under its shard mutex.
    std::mutex _budgetMutex;
    /// Told after every applied budget; guarded by `_budgetMutex`.
    ITierBudgetListener* _budgetListener { nullptr };
    /// Carries an over-budget store down to its budget. LAST, so its thread is stopped and
    /// joined before the storage, the cache and the budget mutex it reaches go.
    TierEvictionDriver _eviction;
};

/// Kick the node's cache tier's eviction driver, if it has a tier.
///
/// What the node's sampler runs after every sample: the fallback look at a store left over its
/// budget when nothing else kicked the driver.
/// @param tier The node's cache tier, or null when it has none.
void KickTierEviction(CacheTier* tier);

/// What this machine is, read when it is needed rather than captured at startup.
///
/// The capacity a node advertises is derived from what its cache tier HOLDS (#167), and a
/// live budget override moves that, so every reader that outlives one round -- the presence
/// loop's machine budget and NODE-ANNOUNCE, the worker's slot ceilings -- reads through this.
class INodeCapacitySource
{
  public:
    virtual ~INodeCapacitySource() = default;

    /// @return What this machine is now. Safe from any thread.
    [[nodiscard]] virtual Distributed::NodeCapacity Current() const = 0;

  protected:
    INodeCapacitySource() = default;
    INodeCapacitySource(INodeCapacitySource const&) = default;
    INodeCapacitySource& operator=(INodeCapacitySource const&) = default;
    INodeCapacitySource(INodeCapacitySource&&) = default;
    INodeCapacitySource& operator=(INodeCapacitySource&&) = default;
};

/// A capacity that never moves: a node with no tier to resize, and every test that is not
/// about a resize.
class FixedNodeCapacity final: public INodeCapacitySource
{
  public:
    /// @param capacity What this machine is, for good.
    explicit FixedNodeCapacity(Distributed::NodeCapacity capacity) noexcept:
        _capacity { capacity }
    {
    }

    /// @return The capacity this was built with.
    [[nodiscard]] Distributed::NodeCapacity Current() const override
    {
        return _capacity;
    }

  private:
    Distributed::NodeCapacity _capacity;
};

/// The machine's capacity, re-derived whenever the cache tier's budget moves.
///
/// Derived by `NodeCapacityOf` over `CacheCapacityOf` and `IndexReserveBytesOf` -- the same
/// three calls `WorkerBody` makes at startup, so a recomputation and the start can never
/// derive differently -- and from the tier that was BUILT, read again, never from the number
/// an override asked for. Registers itself as the tier's budget listener and unregisters in
/// its destructor.
///
/// **What does not follow** a live budget: the worker's own slot count and its thread pool,
/// and the capacity record its REGISTER carries, are fixed at startup (`WorkerTier`). A shrink
/// leaves them conservative; a grow is gated by the machine budget, which does follow.
class LiveNodeCapacity final: public INodeCapacitySource, public ITierBudgetListener
{
  public:
    /// Derive the capacity now and listen for budget changes.
    /// @param cfg The parsed configuration; must outlive this.
    /// @param host What the machine says about itself; must outlive this.
    /// @param tier The node's cache tier, or null when it has none; must outlive this.
    LiveNodeCapacity(NodeConfig const& cfg, IHostFactsSource const& host, CacheTier* tier);

    /// Stops listening, under the tier's budget mutex.
    ~LiveNodeCapacity() override;

    LiveNodeCapacity(LiveNodeCapacity const&) = delete;
    LiveNodeCapacity& operator=(LiveNodeCapacity const&) = delete;
    LiveNodeCapacity(LiveNodeCapacity&&) = delete;
    LiveNodeCapacity& operator=(LiveNodeCapacity&&) = delete;

    /// @return The capacity as last derived.
    [[nodiscard]] Distributed::NodeCapacity Current() const override;

    /// Derive the capacity again from what the tiers hold now.
    void TierBudgetApplied() override;

  private:
    /// @return `NodeCapacityOf` over the tier as it stands.
    [[nodiscard]] Distributed::NodeCapacity Derive() const;

    NodeConfig const& _cfg;
    IHostFactsSource const& _host;
    CacheTier* _tier;
    mutable std::mutex _mutex;             ///< Guards `_current`.
    Distributed::NodeCapacity _current {}; ///< As last derived.
};

/// What the on-disk half's B+tree is called inside `--cache-dir`.
///
/// A file inside the directory rather than the directory itself, because the
/// operator named a place to keep a cache and this tier is entitled to put more than
/// one thing there later. It also keeps `--cache-dir` safe to point at a path that
/// does not exist yet, which is what an operator will do.
///
/// Named in the header rather than kept private to the tier because a test that
/// measures the store FILE has to open it: a second spelling of the name is a test
/// that survives a rename by failing with a message about a missing path rather than
/// about the rename.
constexpr std::string_view DiskStoreFileName = "objects.cow";

/// Where an on-disk store lives, how large it may grow and how it compresses.
///
/// One record for both tiers a node can open on disk -- its private tier under
/// `--cache-dir` and the fleet's shared tier under `--cluster-dir` -- so the two differ
/// by these values and by nothing a second copy of the opening code could get wrong.
struct DiskStoreSpec
{
    std::filesystem::path path; ///< The store file; its directory is created if missing.
    std::uint64_t maxBytes {};  ///< The store's ceiling; zero grows as needed.
    CompressionCodec codec {};  ///< The EFFECTIVE codec, from `Compression::EffectiveCodec`.
    int level {};               ///< The codec's effort.
    std::size_t minBytes {};    ///< Values below this are stored as they are.
    std::string_view inUse;     ///< Who may hold the store when it is claimed already, and what to do: the whole sentence.
};

/// Open an on-disk store, or explain why it will not open.
///
/// ONE process per store, and the store enforces it rather than asking to be trusted:
/// `CowTreeStorage::Open` claims the file for the life of the process, so a second opener
/// is refused with `StorageErrorCode::InUse`. That refusal is spelled out here, naming the
/// path it was given and the spec's remedy, because the flag that places a store is the
/// one most likely to be copied between two nodes on one machine. A filesystem that
/// cannot lock is reported rather than assumed: a guard that silently does nothing reads
/// exactly like one that works.
/// @param spec Where the store is and how it is kept.
/// @param logger Where an unenforceable claim is reported.
/// @return The store, or why it could not be opened.
[[nodiscard]] std::expected<std::unique_ptr<IStorage>, std::string> OpenDiskStore(DiskStoreSpec const& spec,
                                                                                  ILogger& logger);

/// Wrap a tier's storage for the threads that share it.
///
/// A single-shard `ShardedStorage`, and not for sharding: it is the lock. A tier is mutated
/// on a reactor and its statistics are read by the heartbeat thread and by whatever scrapes
/// `/metrics`, and `Snapshot()` on these backends writes a `mutable` member. Inside it a
/// `WriteErrorReportingStorage`, so a persistence failure -- a full disk, an I/O error, a
/// read-only store -- is counted and logged rather than silently not happening (#574), and
/// covered by the same lock.
/// @param storage The composed storage.
/// @param logger Where a write error is reported.
/// @return The wrapped storage.
[[nodiscard]] std::unique_ptr<IStorage> ShareStorage(std::unique_ptr<IStorage> storage, ILogger& logger);

/// The in-memory codec settings this configuration produces.
///
/// Its own function, and public, for two reasons. It is the ONE place a memory
/// tier's codec is decided, so a second tier cannot be built without it -- the
/// daemon learned that as `MakeL1`, whose comment says every L1 goes through it
/// "so the memory-compression settings cannot be applied to some shards and
/// silently missed on others". And it is a pure mapping from configuration to
/// options, so a test can assert the wiring without a filesystem, a socket or a
/// started tier -- which matters because a flag that reaches no tier is inert and
/// looks, from every other surface, exactly like one that works.
///
/// **`codec` is the EFFECTIVE one**, resolved through `Compression::EffectiveCodec`:
/// what this tier will store under, never what the configuration asked for. Being
/// the one place the decision is made is what lets the startup line read its report
/// from here instead of authoring a second answer that disagrees on a build without
/// `PACE_ENABLE_COMPRESSION`.
/// @param cfg The node configuration.
/// @return The options to hand `InMemoryLruStorage::SetCompression`.
[[nodiscard]] InMemoryLruStorage::CompressionOptions MemoryCompressionOf(NodeConfig const& cfg);

/// Resident bytes this node's caches would spend on their key indexes with the whole
/// store loaded (#175).
///
/// Passed to `NodeCapacityOf` as its own argument rather than added to
/// `NodeCacheCapacity`, which is ON THE WIRE: a new field there would be a frame-shape
/// change, and `MinSupportedVersion == CurrentVersion` makes that a flag day for the
/// fleet (#998). `reservedMemoryBytes` already travels, so the figure reaches the
/// leader through a field that exists.
/// @param tier This node's cache tier, or nullptr when it runs none.
/// @return Bytes to hold back, or 0 without a tier.
[[nodiscard]] std::uint64_t IndexReserveBytesOf(CacheTier const* tier);

/// What @p tier is configured to hold, in the vocabulary the fleet speaks.
///
/// Read off the tier that was actually built rather than off the configuration
/// that asked for it, and the difference is the point: `--listen-node` may have
/// been taken, `--cache-memory 0` may have turned the memory half off, and a
/// node that announced a budget it does not have would have the leader reporting
/// a cache that is not there.
///
/// Read off the tier each time it is asked, so a budget moved by `ApplyTierBudget` is what
/// the next caller sees -- `LiveNodeCapacity` is the caller that re-asks when one moves.
/// @param tier The node's cache, or null when it has none.
/// @return The per-tier budgets; every tier absent when @p tier is null.
[[nodiscard]] Distributed::NodeCacheCapacity CacheCapacityOf(CacheTier const* tier);

/// What @p tier holds right now, in the vocabulary the fleet speaks.
///
/// A heartbeat fact. The hit/miss split comes from the sink rather than from the
/// storage, and deliberately: `LocalCache` counts a hit when it serves from this
/// node's tier and a miss when it has to reach the shared cache, which is the
/// question an operator is asking. The storage's own `getHits` counts something
/// narrower -- reads of one tier -- and a lower tier is only consulted when the
/// one above it missed, so those do not add up to the node's.
/// @param tier The node's cache, or null when it has none.
/// @param metrics Where `LocalCache` counted its hits and misses.
/// @return What the cache holds; every tier absent when @p tier is null.
[[nodiscard]] Distributed::NodeCacheLoad CacheLoadOf(CacheTier const* tier, IMetricsSink const& metrics);

/// Start the node's cache tier, or explain why the node must not start at all.
///
/// Three outcomes, which is why the return type looks the way it does:
///
///   - **A tier** — it is serving.
///   - **Success carrying nothing** — there is deliberately no tier, either because
///     `--listen-node` was emptied or because there is nowhere to keep objects.
///     The node continues; a line has already been logged.
///   - **An error** — the store could not be opened, so startup must stop.
///
/// The third outcome used to include a bind failure the operator had asked for by
/// naming the address. That judgement moved to `StartNodeSurfaceOrExplain` with the
/// surfaces (#290): one listener now carries the cache verbs and the scheduler's, and
/// a failure to open it cannot be judged by one of the two components on it.
///
/// That middle state is the whole reason this is a function rather than four lines
/// in `WorkerBody`. Which of the three applies is a judgement worth stating once and
/// testing, and `main.cpp` is in no test target — the lesson `CacheProtocol.cpp`,
/// `RootReconciler.cpp` and `AdminEndpoint.cpp` were each extracted for. It also kept
/// `WorkerBody` under clang-tidy's cognitive-complexity limit, which is the symptom
/// that said so.
/// @param cfg The parsed configuration.
/// @param upstream What the tier's upstream is built from; built only when a tier is, so a node
///        that keeps no objects opens no session pool. Every part must outlive the tier.
/// @param locality Decides whether a caller is on this machine; must outlive it.
/// @param clock Time source for the tier's expiry; must outlive it.
/// @param metrics Where hits, misses and upstream outcomes are counted.
/// @param logger Where what the tier holds, or why there is none, is announced.
/// @return The tier, a null tier meaning "carry on without one", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<CacheTier>, NodeRefusal> StartCacheTierOrExplain(NodeConfig const& cfg,
                                                                                             UpstreamParts const& upstream,
                                                                                             ILocalityOracle const& locality,
                                                                                             core::platform::IClock& clock,
                                                                                             IMetricsSink& metrics,
                                                                                             ILogger& logger);

/// Convert the on-disk half of the tier to this build's record layout, and say
/// what happened.
///
/// Lives here rather than in `main.cpp` because WHERE the store is, is this
/// file's knowledge: the tier decides that `--cache-dir` names a directory and
/// the store is one file inside it. A conversion that reconstructed that path
/// for itself would be a second place to change it, and would convert nothing
/// on the day the first one moved.
/// @param cfg The parsed configuration; `cacheDir` must not be empty.
/// @return A line to print on success; otherwise the reason, and whether a re-run may get past
///         it (`CommandEnding`).
[[nodiscard]] std::expected<std::string, UnfinishedCommand> MigrateDiskTier(NodeConfig const& cfg);

} // namespace Pace::Node
