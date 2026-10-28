// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdmitSession.hpp"
#include "CacheProtocol.hpp"
#include "Dispatch.hpp"
#include "IProcessRunner.hpp"
#include "ReachabilityMemo.hpp"
#include "Stats.hpp"

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Pace::Cc
{

/// How an admitted dispatch to a worker went.
struct AdmittedDispatch
{
    /// The compile a worker ran, as the launcher continues with it; disengaged when none did.
    std::optional<CompileRun> run;
    /// The worker declined the job for a reason the queue answers (full, cordoned, stopping, out of
    /// memory budget): the request goes back into the queue instead of compiling anywhere unbounded.
    bool workerRefused {};
};

/// How the FETCH a `Stored` grant sends again ended. Private: never stored or sent. `Last` is the count.
enum class StoredFetchKind : std::uint8_t
{
    Served,   ///< The other compile's object was there, and was served: no compile.
    Declined, ///< The cache settled the invocation as a plain compile with no STORE (`Warn`'s answer).
    Missed,   ///< Not there after all: ask the queue again.
    Last
};

/// What the FETCH a `Stored` grant sends again answered.
struct StoredFetch
{
    StoredFetchKind kind {}; ///< How it ended.
    int code {};             ///< The exit code of a `Served` hit; meaningless otherwise.
};

/// What `RunMiss` runs. Each member is one step of the miss path as `main.cpp` performs it, behind
/// a callable so the whole sequence can be exercised against fakes -- `main.cpp` is in no test
/// target, and this is the logic that decides where a compile runs and when the queue is told.
///
/// Every member is required, except `failedOpen`, which says it is optional.
struct MissSeams
{
    /// Open an admission on the endpoint the queue is asked at. Null means none is to be asked:
    /// nothing is configured to ask, or this invocation already learned the node does not answer.
    /// @param failedWorker Disengaged for the request as the compile was first described. Engaged
    ///        after the worker a grant named FAILED the compile in a way no queue answers: the request
    ///        is then `LocalOnly`, with that worker added to its exclusions, so the compile this machine
    ///        now runs is charged here rather than to a worker that is not running it.
    std::function<std::unique_ptr<IAdmitSession>(std::optional<std::string_view> failedWorker)> open;
    /// FETCH the key again after a `Stored` grant, and serve the hit if there is one.
    /// @return How the FETCH ended.
    std::function<StoredFetch()> serveStored;
    /// Told of EVERY grant, as it arrives and before anything runs under it. The moment a queue has
    /// answered at all, which is when a refusal this launcher made before asking becomes true to record
    /// (Ruling 12); and for a `Local` grant of a dispatchable request, the fleet's reason for placing
    /// it nowhere (`AdmitTicket::localReason`, Ruling 11). Never called for an admission that failed open.
    std::function<void(AdmitTicket const& ticket)> granted;
    /// Dispatch to the worker an admission granted.
    std::function<AdmittedDispatch(AdmitTicket const&)> dispatchAdmitted;
    /// Today's dispatch with no admission: a `LEASE` at the scheduler, when one is configured.
    /// @return The compile, or disengaged to compile here.
    std::function<std::optional<CompileRun>()> dispatchUnadmitted;
    /// Run the compiler here.
    std::function<CompileRun()> compileLocal;
    /// Everything after the compile: replay its output, decide the exit code and STORE.
    /// @param run The compile. @param stored Set to true when the object reached the cache.
    /// @return The exit code.
    std::function<int(CompileRun& run, bool& stored)> finish;
    /// Told why an admission failed open -- a session that never answered, or one that failed while
    /// requeueing -- so the launcher can say so and remember a node that is not there. Optional.
    /// @param reason The session's own words.
    std::function<void(std::string const& reason)> failedOpen;
};

/// What `RunMiss` leaves for the statistics.
struct MissReport
{
    /// The exit code; disengaged when the cache, asked again after a `Stored` grant, settled the
    /// invocation as a plain compile with no STORE -- the caller then takes exactly that path.
    std::optional<int> code;
    /// How long the queue held this compile, every admission of this invocation summed; empty when
    /// none did.
    std::optional<std::uint64_t> queuedMs;
    std::optional<std::uint64_t> peakMemoryBytes; ///< What the compile measured, when it did.
};

/// The most times one request goes back into the queue after workers refuse it.
///
/// The LEADER bounds this too (R7: after N refusals it waits for the next capacity report); this is
/// the launcher's own stop, so a leader that never converges costs one compile run HERE -- asked for
/// local-only, with the last refusing worker excluded, and charged here like any other -- rather than a
/// launcher that asks forever.
inline constexpr std::size_t MaxWorkerRefusals = 16;

/// How a settled compile is told to the queue.
/// @param code The exit code. @param stored Whether the object was stored.
/// @return `Failed` for a failed compile, else `Stored` or `NotStored`.
[[nodiscard]] constexpr CompileCacheWire::SettleOutcome SettleOutcomeOf(int code, bool stored) noexcept
{
    if (code != 0)
        return CompileCacheWire::SettleOutcome::Failed;
    return stored ? CompileCacheWire::SettleOutcome::Stored : CompileCacheWire::SettleOutcome::NotStored;
}

/// Whether an invocation asks the fleet queue anything at all, and if not, why.
///
/// Explicit values are not needed: private, never stored or sent. `Last` is the count.
enum class AskVerdict : std::uint8_t
{
    Ask,            ///< Open an admission.
    NoEndpoint,     ///< Nothing is configured to ask.
    FetchNotServed, ///< This invocation's own FETCH to the same node was not served: it is not there.
    RememberedDown, ///< A launcher a moment ago could not reach it.
    Last
};

/// The two endpoints a launcher is configured with.
struct LauncherEndpoints
{
    std::string_view cache;     ///< `PACE_ADDR`: this machine's own node, which serves the cache.
    std::string_view scheduler; ///< `PACE_SCHEDULER`: where a `LEASE` goes; empty when none is named.
};

/// Where the fleet queue is asked: ALWAYS the cache's own node, `PACE_ADDR` (Ruling 16).
///
/// Never `PACE_SCHEDULER`. The leader charges a compile to the machine that ASKED, and names that
/// machine only by a key the asking connection proved or by loopback: a launcher's own node proves
/// its machine's key when it relays, while a launcher dialling a remote scheduler directly proves
/// nothing and would be charged to no machine at all. Taking both endpoints is what makes that a
/// case rather than a comment: a scheduler named or not, the answer is the same.
/// @param endpoints What the launcher is configured with.
/// @return The `host:port` to ask; empty when there is nothing to ask.
[[nodiscard]] constexpr std::string_view AdmissionEndpointOf(LauncherEndpoints const& endpoints) noexcept
{
    return endpoints.cache;
}

/// Where a dispatch plan could be spent that a launcher a moment ago could not reach -- or nothing,
/// when somewhere it could be spent may still answer.
///
/// A plan is spent first at the QUEUE (`AdmissionEndpointOf`) and, when no queue answers, at a named
/// scheduler's `LEASE`. So it is refused only when EVERY place it could be spent was not reached:
/// a stopped local node with a live scheduler named still dispatches exactly as before a queue
/// existed, and a dead scheduler behind a live local node still lets the queue place the compile.
/// @param endpoints What the launcher is configured with.
/// @param remembered Whether the reachability memo says an endpoint was not reached recently.
/// @return The endpoint to name in the refusal, or nothing when the plan stands.
[[nodiscard]] std::optional<std::string_view> PlanUnreachableAt(LauncherEndpoints const& endpoints,
                                                                std::function<bool(std::string_view)> const& remembered);

/// Decide whether to dial the queue, from what this invocation ALREADY knows.
///
/// **A node that is stopped must cost exactly what it did before a queue existed**, and a refused
/// loopback connect on Windows can take the whole connect budget: so an admission is never a fresh
/// question to a node the invocation has just learned is not answering. Pure over its inputs, so the
/// rule is a case rather than a comment.
/// @param endpoint Where the queue would be asked; empty when nowhere.
/// @param cacheAddress Where the cache is, which the FETCH went to.
/// @param fetched How this invocation's FETCH ended; disengaged when it made none.
/// @param rememberedUnreached Whether the reachability memo says @p endpoint was not reached recently.
/// @return Whether to ask, else the reason not to.
[[nodiscard]] constexpr AskVerdict DecideAsk(std::string_view endpoint,
                                             std::string_view cacheAddress,
                                             std::optional<CacheOutcomeKind> fetched,
                                             bool rememberedUnreached) noexcept
{
    if (endpoint.empty())
        return AskVerdict::NoEndpoint;
    if (fetched.has_value() && endpoint == cacheAddress && !CacheIsServing(*fetched))
        return AskVerdict::FetchNotServed;
    if (rememberedUnreached)
        return AskVerdict::RememberedDown;
    return AskVerdict::Ask;
}

/// The `DecideAsk` a launcher puts to its own evidence: its FETCH, when it made one, and what the
/// reachability memo says about @p endpoint now.
/// @param endpoint Where the queue would be asked; empty when nowhere.
/// @param cacheAddress Where the cache is, which the FETCH went to.
/// @param fetched How this invocation's FETCH ended; disengaged when it made none.
/// @param memo What recent launchers learned.
/// @param now The current wall-clock time.
/// @return Whether to ask, else the reason not to.
[[nodiscard]] AskVerdict DecideAskFrom(std::string_view endpoint,
                                       std::string_view cacheAddress,
                                       std::optional<CacheOutcomeKind> fetched,
                                       ReachabilityMemo const& memo,
                                       std::chrono::system_clock::time_point now);

/// Remember, for the launchers after this one, that this invocation's FETCH found no node at the
/// endpoint the queue is asked at.
///
/// **What keeps a stopped node costing each later invocation of a build nothing**: an uncacheable
/// compile makes no FETCH, so it has only the memo to ask, and without this entry it dials the queue
/// -- a whole connect budget on Windows -- to learn what a FETCH a moment ago already learned. Narrow
/// in exactly the memo's way: only `TransportFailure::Unreached`, a dial that made no connection, and
/// only when the FETCH went to the endpoint the queue would be asked at.
/// @param memo The memo to note it in.
/// @param admissionEndpoint Where the queue is asked; empty when nowhere.
/// @param cacheAddress Where the FETCH went.
/// @param fetched How it ended.
/// @param failure Which way, when it was a transport failure.
/// @param now The current wall-clock time.
/// @return Whether anything was noted.
bool RememberUnreachedFetch(ReachabilityMemo& memo,
                            std::string_view admissionEndpoint,
                            std::string_view cacheAddress,
                            CacheOutcomeKind fetched,
                            TransportFailure failure,
                            std::chrono::system_clock::time_point now);

/// How a `Local` grant's reason reads on the dispatch axis: what the LEASE path records for the
/// refusal the same fleet would have answered (Ruling 11), so `dist-compile-e2e`'s wording and the
/// `--show-stats` tally are one fact whether or not a queue placed the compile.
struct LocalGrantRow
{
    CompileCacheWire::LocalGrantReason reason; ///< The reason this row describes.
    /// Whether there is anything to record: false only for `Asked`, which no refusal stands for.
    bool records;
    DeclineCause cause;               ///< The decline the lease path records for the same refusal.
    CompileCacheWire::ErrorCode code; ///< The refusal it reads as, named in the line.
    std::string_view sentence;        ///< What the line says, `{}` standing for the toolchain.
};

/// One row per `LocalGrantReason`.
inline constexpr EnumTable<CompileCacheWire::LocalGrantReason, LocalGrantRow> LocalGrantTable { {
    { .reason = CompileCacheWire::LocalGrantReason::Asked,
      .records = false,
      .cause = DeclineCause::NoToolchain,
      .code = CompileCacheWire::ErrorCode::NoWorker,
      .sentence = {} },
    { .reason = CompileCacheWire::LocalGrantReason::NoWorker,
      .records = true,
      .cause = DeclineCause::NoToolchain,
      .code = CompileCacheWire::ErrorCode::NoWorker,
      .sentence = "no worker serves {}" },
    // The lease path's `Excluded` row: the same code as `NoWorker`, its own sentence.
    { .reason = CompileCacheWire::LocalGrantReason::AllExcluded,
      .records = true,
      .cause = DeclineCause::NoToolchain,
      .code = CompileCacheWire::ErrorCode::NoWorker,
      .sentence = "every worker serving {} is on this client's exclusion list" },
    { .reason = CompileCacheWire::LocalGrantReason::NoRoom,
      .records = true,
      .cause = DeclineCause::NoCapacity,
      .code = CompileCacheWire::ErrorCode::NoCapacity,
      .sentence = "no worker serving {} had room for it, so the queue ran it here" },
} };

static_assert(RowsInEnumeratorOrder(LocalGrantTable, &LocalGrantRow::reason),
              "LocalGrantTable must hold one row per LocalGrantReason, in enumerator order");

/// What to record for a `Local` grant, and the line to say.
struct LocalGrantNote
{
    DispatchRecording recording; ///< The dispatch axis's state and fixed tally reason.
    std::string line;            ///< `not dispatched (rejected (<code>): ...); compiling locally`.
};

/// Explain a `Local` grant on the dispatch axis.
/// @param reason Why the leader kept it here.
/// @param toolchainLabel What a person calls the compiler; may be empty.
/// @return What to record, or nothing for a request that asked to run here or a reason past the table.
[[nodiscard]] std::optional<LocalGrantNote> ExplainLocalGrant(CompileCacheWire::LocalGrantReason reason,
                                                              std::string_view toolchainLabel);

/// What a tree is called when its name cannot be sent: not text the leader would accept.
inline constexpr std::string_view UnnamedTreeLabel = "(unnamed tree)";

/// A person's name for the build tree a compile runs in (R14): the working directory relative to the
/// source root.
///
/// Display only, so it can never cost a compile: a working directory that is not UTF-8 -- text a peer
/// chose, which the leader refuses where it enters -- or that does not fit the wire's bound is named
/// `UnnamedTreeLabel` rather than refused, truncated or repaired. A directory outside the source root is
/// spelled whole, and so is one whose source root is not text: that root names nothing to be relative
/// to, while the directory is still a name a person can read.
/// @param workingDirectory Where the compile runs.
/// @param sourceRoot The source root; may be empty.
/// @return The label; never empty.
[[nodiscard]] std::string TreeLabelOf(std::string_view workingDirectory, std::string_view sourceRoot);

/// Decide where a cache miss compiles and carry it through to the STORE.
///
/// Asks the queue first (`open`), and:
/// - **fails open** -- no session, no grant, a session that fails while waiting -- into exactly what
///   this launcher did before a queue existed (`dispatchUnadmitted`, else `compileLocal`), told to
///   nobody;
/// - on `Local` compiles here, on `Worker` dispatches to the granted worker (a refusal is
///   `Requeue`d on the SAME connection), and on `Stored` fetches: a hit is served, one miss opens the
///   admission once more, a second fails open, and a FETCH the cache settles as a plain compile ends
///   with no code, for the caller to take exactly that path;
/// - when the granted worker fails the compile in a way no queue answers (unreachable, a failed
///   dispatch preprocess, a result discarded), closes that admission -- the leader abandons it and the
///   worker's charge is freed -- and asks again `LocalOnly` with that worker excluded, so the compile
///   runs here under a grant of its own; only when none can be had does it compile here unadmitted;
/// - holds the admission connection from the grant until the compile AND its STORE are done -- the
///   connection is the lease -- and then sends `SETTLE`, so the STORE is always visible before the
///   queue wakes the compiles waiting on it (R1).
///
/// **An exception ends the connection and sends no `SETTLE`**: the leader reads that as an
/// abandonment, which is what a launcher that did not finish is.
/// @param seams What to run.
/// @return The exit code and what the queue and the compile reported.
[[nodiscard]] MissReport RunMiss(MissSeams const& seams);

} // namespace Pace::Cc
