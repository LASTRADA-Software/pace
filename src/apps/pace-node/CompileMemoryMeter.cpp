// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "CompileMemoryMeter.hpp"
#include "LiveNodeConfig.hpp"
#include "MachineBudget.hpp"

#include <Pace/Platform/JobCgroups.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
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

CompileMemoryMeter::CompileMemoryMeter(std::shared_ptr<Platform::ICgroupHost const> host,
                                       MeterPlatform platform,
                                       std::optional<Cc::OomVictim> oomVictim,
                                       std::unique_ptr<Cc::ICeilingRendezvous> rendezvous):
    _host { std::move(host) },
    _platform { platform },
    _oomVictim { std::move(oomVictim) },
    _rendezvous { std::move(rendezvous) }
{
}

Cc::ICeilingRendezvous* CompileMemoryMeter::Rendezvous() const noexcept
{
    return _rendezvous.get();
}

void CompileMemoryMeter::Prepare()
{
    Decide();
    _decided.store(true, std::memory_order_release);
}

void CompileMemoryMeter::Decide()
{
    switch (MeterPlatformTable[static_cast<std::size_t>(_platform)].strategy)
    {
        case MeterStrategy::PlatformMeter:
            return;
        case MeterStrategy::NotMeasurable:
            _report = MeterReport { .state = MeterState::NotApplicable,
                                    .reason = "this platform has no meter that measures a compile apart from the node",
                                    .notApplied = {},
                                    .oomdOmitRefused = std::nullopt };
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
        _report = MeterReport { .state = MeterState::Unmeasured,
                                .reason = layout.error().Describe(),
                                .notApplied = {},
                                .oomdOmitRefused = std::nullopt };
        return;
    }
    _report.notApplied = layout->notApplied;
    _report.oomdOmitRefused = layout->oomdOmitRefused;
    _layout = *layout;
    _source = std::make_shared<Platform::CgroupJobSource>(_host, *std::move(layout));
}

CeilingReport CompileMemoryMeter::ApplyCeiling(std::uint64_t bytes)
{
    // Asked of the platform first, so a platform with no cgroups answers the same before and after a worker.
    if (MeterPlatformTable[static_cast<std::size_t>(_platform)].strategy != MeterStrategy::JobCgroup)
        return CeilingReport { .state = CeilingState::NotApplicable,
                               .reason = "the compile memory ceiling is a Linux cgroup, and this platform has none" };
    if (!_decided.load(std::memory_order_acquire))
        return CeilingReport { .state = CeilingState::Uncapped,
                               .reason = "no worker has prepared this node's compile cgroups yet" };
    if (!_layout.has_value())
        // The preparation's own failure: the same cause `compile-memory-unmeasured` names.
        return CeilingReport { .state = CeilingState::Uncapped, .reason = _report.reason };
    if (auto const written = Platform::SetCompileCeiling(*_host, *_layout, bytes); !written.has_value())
        return CeilingReport { .state = CeilingState::Uncapped,
                               .reason = std::format("the ceiling could not be written to {}: {}",
                                                     (_layout->compiles / "memory.max").string(),
                                                     written.error().message()) };
    return CeilingReport { .state = CeilingState::Capped, .reason = {} };
}

std::optional<std::filesystem::path> CompileMemoryMeter::CompilesCgroup() const
{
    if (!_decided.load(std::memory_order_acquire) || !_layout.has_value())
        return std::nullopt;
    return _layout->compiles;
}

std::optional<std::uint64_t> CompileMemoryMeter::CompilesHeldBytes() const
{
    if (!_decided.load(std::memory_order_acquire) || !_layout.has_value())
        return std::nullopt;
    auto const text = _host->ReadFile(_layout->compiles / "memory.current");
    if (!text.has_value())
        return std::nullopt;
    auto const digits = std::string_view { *text }.substr(0, text->find_last_not_of("\n") + 1);
    auto value = std::uint64_t { 0 };
    auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc {} || end != digits.data() + digits.size() || digits.empty())
        return std::nullopt;
    return value;
}

Platform::IYoungestKiller* CompileMemoryMeter::YoungestKiller() const noexcept
{
    if (!_decided.load(std::memory_order_acquire))
        return nullptr;
    return _source.get();
}

MeteredRunner CompileMemoryMeter::MakeRunner(IMetricsSink& metrics, ILogger& logger)
{
    std::call_once(_prepared, [this] { Prepare(); });

    if (_source != nullptr)
        return MeteredRunner { .runner = Cc::MakeProcessRunner(
                                   std::make_shared<ReportingJobSource>(_source, metrics, logger), _oomVictim),
                               .report = _report };
    auto const strategy = MeterPlatformTable[static_cast<std::size_t>(_platform)].strategy;
    auto const peak = strategy == MeterStrategy::PlatformMeter ? Cc::PeakSource::ResourceUsage : Cc::PeakSource::Unmeasured;
    return MeteredRunner { .runner = Cc::MakeProcessRunner(peak, _oomVictim), .report = _report };
}

void ReportCompileMemoryMeter(MeterReport const& report, NodeConditions& conditions, ILogger& logger)
{
    // One line for every setting of `compiles/` this kernel lacks: tolerated, and said once.
    if (!report.notApplied.empty())
    {
        auto said = std::string {};
        for (auto const& sentence: report.notApplied)
            said += (said.empty() ? "" : "; ") + sentence;
        logger.Logf(LogLevel::Warn, "the compiles cgroup runs without some of its settings: {}", said);
    }
    // Said once, never fatal: oomd remains free to pick the node, as it was before the node marked anything.
    if (report.oomdOmitRefused.has_value())
        logger.Logf(LogLevel::Warn,
                    "systemd-oomd may choose this node, its cache with it, as a victim: {} could not be set ({})",
                    Platform::OomdOmitAttribute,
                    *report.oomdOmitRefused);

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

void PutCompileCeilingInForce(CompileMemoryMeter& meter,
                              NodeConfig const& cfg,
                              Distributed::NodeCapacity const& capacity,
                              NodeConditions& conditions,
                              ILogger& logger,
                              std::optional<std::uint64_t> allowanceBytes,
                              Cc::ICeilingRendezvous* rendezvous)
{
    auto const configured = CompileCeilingOf(cfg, capacity);
    auto const bytes = std::min(configured, allowanceBytes.value_or(configured));
    auto const applied = meter.ApplyCeiling(bytes);
    switch (applied.state)
    {
        case CeilingState::Capped:
            if (bytes < configured)
                logger.Logf(LogLevel::Debug,
                            "compile memory ceiling: {} (the memory-pressure allowance, below the {} {})",
                            bytes,
                            configured,
                            cfg.compileMemoryLimitBytes.has_value() ? "typed" : "derived");
            else
                logger.Logf(LogLevel::Info,
                            "compile memory ceiling: {} ({})",
                            bytes,
                            cfg.compileMemoryLimitBytes.has_value() ? "typed" : "derived");
            conditions.Clear(NodeCondition::CompileMemoryUncapped);
            // Named for this machine's launchers, so the compiles they run themselves join it (#1770).
            if (auto const compiles = meter.CompilesCgroup(); rendezvous != nullptr && compiles.has_value())
                if (auto const published = rendezvous->Publish(*compiles); !published.has_value())
                    logger.Logf(LogLevel::Info, "compile memory ceiling: {}", published.error());
            return;
        case CeilingState::Uncapped:
            if (rendezvous != nullptr)
                rendezvous->Withdraw();
            logger.Logf(LogLevel::Warn,
                        "compiles run with no memory ceiling over them, so one that grows can take memory from "
                        "everything else on this machine: {}",
                        applied.reason);
            conditions.Raise(NodeCondition::CompileMemoryUncapped, applied.reason);
            return;
        case CeilingState::NotApplicable:
            if (rendezvous != nullptr)
                rendezvous->Withdraw();
            conditions.NotEvaluated(NodeCondition::CompileMemoryUncapped, applied.reason);
            return;
    }
}

CompileCeilingKeeper::CompileCeilingKeeper(CompileMemoryMeter& meter,
                                           INodeConfigSource const& inForce,
                                           INodeCapacitySource const& capacity,
                                           NodeConditions& conditions,
                                           ILogger& logger,
                                           Cc::ICeilingRendezvous* rendezvous) noexcept:
    _meter { meter },
    _inForce { inForce },
    _capacity { capacity },
    _conditions { conditions },
    _logger { logger },
    _rendezvous { rendezvous }
{
}

CeilingBasis CompileCeilingKeeper::Keep(CeilingWrite when)
{
    auto const guard = std::scoped_lock { _mutex };
    return KeepLocked(when);
}

void CompileCeilingKeeper::Apply(std::optional<std::uint64_t> allowanceBytes)
{
    auto const guard = std::scoped_lock { _mutex };
    _allowance = allowanceBytes;
    static_cast<void>(KeepLocked(CeilingWrite::OnChange));
}

std::optional<std::uint64_t> CompileCeilingKeeper::HeldBytes() const
{
    return _meter.CompilesHeldBytes();
}

CeilingBasis CompileCeilingKeeper::KeepLocked(CeilingWrite when)
{
    // Read HERE, under the lock, never handed in: a pair read before another writer took the lock is older than
    // what that writer wrote, and writing from it would put a superseded -- possibly higher -- ceiling back.
    auto basis = CeilingBasis { .cfg = _inForce.Current(), .capacity = _capacity.Current() };
    auto const configured = CompileCeilingOf(basis.cfg, basis.capacity);
    auto const bytes = std::min(configured, _allowance.value_or(configured));
    if (when == CeilingWrite::OnChange && _written == bytes)
        return basis;
    _written = bytes;
    PutCompileCeilingInForce(_meter, basis.cfg, basis.capacity, _conditions, _logger, _allowance, _rendezvous);
    return basis;
}

} // namespace Pace::Node
