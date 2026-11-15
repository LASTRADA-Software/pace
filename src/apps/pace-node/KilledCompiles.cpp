// SPDX-License-Identifier: Apache-2.0
#include "KilledCompiles.hpp"

#include <format>

#include <CompileJob.hpp>

namespace Pace::Node
{

KilledCompilesLog::KilledCompilesLog(Cc::IJobRefusalObserver& next, ILogger& logger) noexcept:
    _next { next },
    _logger { logger }
{
}

void KilledCompilesLog::OnJobRefused(Cc::JobError const& error)
{
    // The detail is the line: it names the cause word and the source, and it is this worker's own
    // text built from `SafeSourceName`'s output, so nothing a client sent reaches the log verbatim.
    if (error.reason == Cc::JobRefusal::CompileKilled)
        _logger.Log(LogLevel::Info, std::format("worker: {}; its client asks the queue for it once more", error.detail));
    _next.OnJobRefused(error);
}

} // namespace Pace::Node
