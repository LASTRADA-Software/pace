// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/Logger.hpp>

#include <WorkerProtocol.hpp>

/// @file KilledCompiles.hpp
/// Says, in this node's log, each compile its machine killed.

namespace Pace::Node
{

/// Logs one Info line per killed compile -- its cause and its source -- and hands every refusal on.
///
/// A kill is counted per cause (`pace_worker_compiles_killed_*_total`), and the count says how
/// often, never which translation unit. The line is what lets an operator see that one TU is killed
/// every build, which a rate cannot show. Info rather than Warn: a kill is the memory defence doing
/// its job, and the client compiles locally, correctly. Not rate-limited: a kill is bounded by the
/// compiles this worker admits, each of which the defence chose to end.
///
/// A decorator rather than a second observer list, so `WorkerProtocol` keeps its one required
/// observer and `RefusedArgumentsReport` keeps its one job. Thread-safe as far as @p next and the
/// logger are: `OnJobRefused` runs on whichever pool thread ran the job.
class KilledCompilesLog final: public Cc::IJobRefusalObserver
{
  public:
    /// @param next Told every refusal, killed or not; must outlive this.
    /// @param logger Where the kill lines go; must outlive this.
    KilledCompilesLog(Cc::IJobRefusalObserver& next, ILogger& logger) noexcept;

    KilledCompilesLog(KilledCompilesLog const&) = delete;
    KilledCompilesLog& operator=(KilledCompilesLog const&) = delete;
    KilledCompilesLog(KilledCompilesLog&&) = delete;
    KilledCompilesLog& operator=(KilledCompilesLog&&) = delete;
    ~KilledCompilesLog() override = default;

    /// Log the refusal when it is a killed compile, then hand it on.
    /// @param error Why the job was refused.
    void OnJobRefused(Cc::JobError const& error) override;

  private:
    Cc::IJobRefusalObserver& _next;
    ILogger& _logger;
};

} // namespace Pace::Node
