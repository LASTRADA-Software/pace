// SPDX-License-Identifier: Apache-2.0
#include "CompileMemoryMeter.hpp"

#include <Pace/Platform/JobCgroups.hpp>

#include <atomic>
#include <format>
#include <utility>

namespace Pace::Node
{

namespace
{

    /// A job source that counts, and says once, every compile it could not measure.
    ///
    /// A cgroup that cannot be made, a spawn the system refuses and a `memory.peak` the kernel will not
    /// give all end the same way -- the compile runs and its peak is unknown -- and none of them is
    /// visible anywhere else once the node has started. Wraps the process's one source, per runner,
    /// because the sink and the logger belong to a body and the source outlives it.
    class ReportingJobSource final: public Platform::IJobCgroupSource
    {
      public:
        ReportingJobSource(std::shared_ptr<Platform::IJobCgroupSource> inner, IMetricsSink& metrics, ILogger& logger):
            _inner { std::move(inner) },
            _metrics { metrics },
            _logger { logger }
        {
        }

        /// @copydoc Platform::IJobCgroupSource::Create
        [[nodiscard]] std::expected<std::unique_ptr<Platform::IJobCgroup>, std::error_code> Create() override
        {
            auto made = _inner->Create();
            if (!made.has_value())
                Count(std::format("a job cgroup could not be made: {}", made.error().message()));
            return made;
        }

        /// @copydoc Platform::IJobCgroupSource::NoteUnmeasured
        void NoteUnmeasured(Unmeasured why) noexcept override
        {
            Count(why == Unmeasured::SpawnRefused ? "the system would not start the compiler inside its cgroup"
                                                  : "the kernel gave no memory.peak for the compile's cgroup");
        }

      private:
        /// Count one, and say it the first time only: the counter is the rate, the line is the onset.
        /// @param what What went wrong.
        void Count(std::string_view what) noexcept
        {
            _metrics.Increment(IMetricsSink::Counter::WorkerCompilesUnmeasured);
            if (!_said.exchange(true))
                _logger.Logf(LogLevel::Warn,
                             "a compile ran with its peak memory unmeasured ({}); this is said once, and every one is "
                             "counted in pace_worker_compiles_unmeasured_total",
                             what);
        }

        std::shared_ptr<Platform::IJobCgroupSource> _inner;
        IMetricsSink& _metrics;
        ILogger& _logger;
        std::atomic<bool> _said { false };
    };

} // namespace

CompileMemoryMeter::CompileMemoryMeter(std::shared_ptr<Platform::ICgroupHost const> host, MeterPlatform platform):
    _host { std::move(host) },
    _platform { platform }
{
}

void CompileMemoryMeter::Prepare()
{
    switch (MeterPlatformTable[static_cast<std::size_t>(_platform)].strategy)
    {
        case MeterStrategy::PlatformMeter:
            return;
        case MeterStrategy::NotMeasurable:
            _report = MeterReport { .state = MeterState::NotApplicable,
                                    .reason = "this platform has no meter that measures a compile apart from the node" };
            return;
        case MeterStrategy::JobCgroup:
            break;
    }

    auto const failure = Cc::CanSpawnIntoCgroups()
                             ? std::optional<Platform::CgroupFailure> {}
                             : std::optional { Platform::CgroupFailure {
                                   .cause = Platform::CgroupUnavailable::SpawnIntoUnsupported, .detail = {} } };
    auto layout = failure.has_value()
                      ? std::expected<Platform::CgroupLayout, Platform::CgroupFailure> { std::unexpected { *failure } }
                      : Platform::PrepareJobCgroups(*_host);
    if (!layout.has_value())
    {
        _report = MeterReport { .state = MeterState::Unmeasured, .reason = layout.error().Describe() };
        return;
    }
    _source = std::make_shared<Platform::CgroupJobSource>(_host, *std::move(layout));
}

MeteredRunner CompileMemoryMeter::MakeRunner(IMetricsSink& metrics, ILogger& logger)
{
    std::call_once(_prepared, [this] { Prepare(); });

    if (_source != nullptr)
        return MeteredRunner { .runner =
                                   Cc::MakeProcessRunner(std::make_shared<ReportingJobSource>(_source, metrics, logger)),
                               .report = _report };
    auto const strategy = MeterPlatformTable[static_cast<std::size_t>(_platform)].strategy;
    auto const peak = strategy == MeterStrategy::PlatformMeter ? Cc::PeakSource::ResourceUsage : Cc::PeakSource::Unmeasured;
    return MeteredRunner { .runner = Cc::MakeProcessRunner(peak), .report = _report };
}

void ReportCompileMemoryMeter(MeterReport const& report, NodeConditions& conditions, ILogger& logger)
{
    switch (report.state)
    {
        case MeterState::Measured:
            conditions.Clear(NodeCondition::CompileMemoryUnmeasured);
            return;
        case MeterState::NotApplicable:
            // No remedy applies, so no condition asks for one: the row says it was not evaluated, and why.
            logger.Logf(LogLevel::Info, "compile peak memory is not measured on this platform: {}", report.reason);
            conditions.NotEvaluated(NodeCondition::CompileMemoryUnmeasured, report.reason);
            return;
        case MeterState::Unmeasured:
            logger.Logf(LogLevel::Warn,
                        "compile peak memory is not measured, so the fleet's memory estimates cannot learn from this "
                        "worker's compiles: {}",
                        report.reason);
            conditions.Raise(NodeCondition::CompileMemoryUnmeasured, report.reason);
            return;
    }
}

} // namespace Pace::Node
