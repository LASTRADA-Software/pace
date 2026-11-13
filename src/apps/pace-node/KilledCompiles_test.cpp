// SPDX-License-Identifier: Apache-2.0
#include "KilledCompiles.hpp"

#include <Pace/Core/Logger.hpp>
#include <Pace/Platform/JobCgroups.hpp>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <optional>
#include <vector>

#include <CompileJob.hpp>
#include <WorkerProtocol.hpp>

using namespace Pace;
using namespace Pace::Node;

namespace
{

/// Remembers every refusal it is handed.
class RecordingRefusals final: public Cc::IJobRefusalObserver
{
  public:
    void OnJobRefused(Cc::JobError const& error) override
    {
        auto const guard = std::scoped_lock { _mutex };
        _seen.push_back(error);
    }

    /// @return What was handed on so far.
    [[nodiscard]] std::vector<Cc::JobError> Seen() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _seen;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<Cc::JobError> _seen;
};

} // namespace

TEST_CASE("CompileKilled: a killed compile is logged once at Info naming its cause and source", "[node][compile-killed]")
{
    RecordingRefusals next;
    CapturingLogger logger;
    KilledCompilesLog log { next, logger };

    log.OnJobRefused(Cc::JobError::KilledCompile(Platform::CompileKill::Shed, "widget.cpp"));

    auto const lines = logger.Snapshot();
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().level == LogLevel::Info);
    CHECK(lines.front().message.contains("shed"));
    CHECK(lines.front().message.contains("widget.cpp"));
    // Handed on, whole: the report behind this one still sees every refusal.
    REQUIRE(next.Seen().size() == 1);
    CHECK(next.Seen().front().killed == std::optional { Platform::CompileKill::Shed });
}

TEST_CASE("CompileKilled: a refusal that is not a kill is handed on and logged nowhere", "[node][compile-killed]")
{
    RecordingRefusals next;
    CapturingLogger logger;
    KilledCompilesLog log { next, logger };

    log.OnJobRefused(
        Cc::JobError { .reason = Cc::JobRefusal::SpawnFailed, .detail = {}, .subjects = {}, .killed = std::nullopt });

    CHECK(logger.Snapshot().empty());
    REQUIRE(next.Seen().size() == 1);
    CHECK(next.Seen().front() == Cc::JobRefusal::SpawnFailed);
}
