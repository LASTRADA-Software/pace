// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdmitSession.hpp"
#include "CacheProtocol.hpp"
#include "IProcessRunner.hpp"

#include <Pace/Protocol/CompileCacheWire.hpp>

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

/// What `RunMiss` runs. Each member is one step of the miss path as `main.cpp` performs it, behind
/// a callable so the whole sequence can be exercised against fakes -- `main.cpp` is in no test
/// target, and this is the logic that decides where a compile runs and when the queue is told.
///
/// Every member is required.
struct MissSeams
{
    /// Open an admission on the endpoint the queue is asked at. Null means none is to be asked:
    /// nothing is configured to ask, or this invocation already learned the node does not answer.
    std::function<std::unique_ptr<IAdmitSession>()> open;
    /// FETCH the key again after a `Stored` grant, and serve the hit if there is one.
    /// @return The exit code when a hit was served; disengaged on a miss.
    std::function<std::optional<int>()> serveStored;
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
    int code {};                                  ///< The exit code.
    std::optional<std::uint64_t> queuedMs;        ///< How long the queue held this compile; empty when none did.
    std::optional<std::uint64_t> peakMemoryBytes; ///< What the compile measured, when it did.
};

/// The most times one request goes back into the queue after workers refuse it.
///
/// The LEADER bounds this too (R7: after N refusals it waits for the next capacity report); this is
/// the launcher's own stop, so a leader that never converges costs one compile run in the open
/// rather than a launcher that asks forever.
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

/// What a tree is called when its name cannot be sent: not text the leader would accept.
inline constexpr std::string_view UnnamedTreeLabel = "(unnamed tree)";

/// A person's name for the build tree a compile runs in (R14): the working directory relative to the
/// source root.
///
/// Display only, so it can never cost a compile: a working directory that is not UTF-8 -- text a peer
/// chose, which the leader refuses where it enters -- or that does not fit the wire's bound is named
/// `UnnamedTreeLabel` rather than refused, truncated or repaired. A directory outside the source root is
/// spelled whole.
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
///   admission once more, and a second fails open;
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
