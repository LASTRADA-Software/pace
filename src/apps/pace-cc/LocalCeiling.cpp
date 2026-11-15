// SPDX-License-Identifier: Apache-2.0
#include "LocalCeiling.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include <core/Ranges.hpp>

namespace Pace::Cc
{

namespace
{

    /// Wraps the source a compile's cgroup comes from, to learn why a compile did not land in one.
    ///
    /// The runner keeps both failures to itself -- a cgroup it could not make is spawned without, and a spawn the
    /// system refused is retried without one -- and only the SOURCE hears of them. A launcher runs one compile at
    /// a time through this, so the last answer is that compile's; the lock keeps the interface's promise anyway.
    class RecordingSource final: public Platform::IJobCgroupSource
    {
      public:
        explicit RecordingSource(std::shared_ptr<Platform::IJobCgroupSource> inner):
            _inner { std::move(inner) }
        {
        }

        [[nodiscard]] std::expected<std::unique_ptr<Platform::IJobCgroup>, std::error_code> Create() override
        {
            auto made = _inner->Create();
            if (!made.has_value())
                Remember(CeilingMiss::CgroupNotMade);
            return made;
        }

        void NoteUnmeasured(Unmeasured why) noexcept override
        {
            if (why == Unmeasured::SpawnRefused)
                Remember(CeilingMiss::SpawnRefused);
            _inner->NoteUnmeasured(why);
        }

        /// @return Why the last compile did not land in a cgroup, as far as this source heard.
        [[nodiscard]] CeilingMiss Missed() const
        {
            auto const guard = std::scoped_lock { _mutex };
            return _missed;
        }

      private:
        void Remember(CeilingMiss miss) noexcept
        {
            auto const guard = std::scoped_lock { _mutex };
            _missed = miss;
        }

        std::shared_ptr<Platform::IJobCgroupSource> _inner;
        mutable std::mutex _mutex;
        // A compile outside its cgroup with neither failure heard is one the system placed elsewhere: the spawn
        // refusal is the one way that happens which the runner does not report before the retry succeeds.
        CeilingMiss _missed { CeilingMiss::SpawnRefused };
    };

    /// What defended a compile that did not join the ceiling.
    /// @param shim Whether it ran under the shim. @param miss Why not the ceiling.
    /// @return The report.
    [[nodiscard]] LocalDefenseReport Outside(ShimState shim, CeilingMiss miss) noexcept
    {
        return LocalDefenseReport { .defense =
                                        shim == ShimState::Shimmed ? LocalDefense::ShimOnly : LocalDefense::Unprotected,
                                    .miss = miss };
    }

} // namespace

std::optional<std::filesystem::path> RendezvousPublishPath(std::optional<std::string> const& runtimeDirectory)
{
    if (!runtimeDirectory.has_value())
        return std::nullopt;
    // systemd separates several `RuntimeDirectory=` entries with `:`; the first is the unit's own.
    auto const first = std::string_view { *runtimeDirectory }.substr(0, runtimeDirectory->find(':'));
    if (first.empty())
        return std::nullopt;
    return std::filesystem::path { first } / CeilingRendezvousName;
}

std::optional<std::filesystem::path> RendezvousLookupPath(std::optional<std::string> const& xdgRuntimeDirectory)
{
    if (!xdgRuntimeDirectory.has_value() || xdgRuntimeDirectory->empty())
        return std::nullopt;
    return std::filesystem::path { *xdgRuntimeDirectory } / NodeRuntimeDirectoryName / CeilingRendezvousName;
}

std::string RenderRendezvous(std::filesystem::path const& compiles)
{
    return compiles.generic_string() + '\n';
}

std::optional<std::filesystem::path> ParseRendezvous(std::string_view text)
{
    auto const line = text.substr(0, text.find('\n'));
    // A cgroup path is absolute: anything else is a file somebody else wrote, and joined it would be a directory
    // relative to wherever the launcher happens to run.
    if (line.empty() || !line.starts_with('/'))
        return std::nullopt;
    return std::filesystem::path { line };
}

std::string LocalDefenseToken(LocalDefenseReport const& report)
{
    auto const& defense = LocalDefenseTable[static_cast<std::size_t>(report.defense)];
    if (report.miss == CeilingMiss::None)
        return std::string { defense.token };
    return std::format("{}:{}", defense.token, CeilingMissTable[static_cast<std::size_t>(report.miss)].id);
}

std::optional<LocalDefenseReport> ParseLocalDefenseToken(std::string_view token)
{
    auto const colon = token.find(':');
    auto const head = token.substr(0, colon);
    auto const tail = colon == std::string_view::npos ? std::string_view {} : token.substr(colon + 1);
    auto const* const defense = core::findOrNull(LocalDefenseTable, head, &LocalDefenseRow::token);
    if (defense == nullptr)
        return std::nullopt;
    auto miss = CeilingMiss::None;
    if (colon != std::string_view::npos)
    {
        auto const* const row = core::findOrNull(CeilingMissTable, tail, &CeilingMissRow::id);
        if (row == nullptr || row->miss == CeilingMiss::None)
            return std::nullopt;
        miss = row->miss;
    }
    // `ceiling` and a miss are one claim each: joined with a reason it did not join is no token this build writes.
    if ((defense->defense == LocalDefense::Ceiling) != (miss == CeilingMiss::None))
        return std::nullopt;
    return LocalDefenseReport { .defense = defense->defense, .miss = miss };
}

RendezvousLocator::RendezvousLocator(EnvironmentReader environment, FileReader files):
    _environment { std::move(environment) },
    _files { std::move(files) }
{
}

std::expected<std::filesystem::path, CeilingMiss> RendezvousLocator::Locate() const
{
    auto const path = RendezvousLookupPath(_environment("XDG_RUNTIME_DIR"));
    if (!path.has_value())
        return std::unexpected { CeilingMiss::NoRuntimeDirectory };
    auto const text = _files(*path);
    if (!text.has_value())
        return std::unexpected { CeilingMiss::NoNodeCeiling };
    auto compiles = ParseRendezvous(*text);
    if (!compiles.has_value())
        return std::unexpected { CeilingMiss::Unreadable };
    return *std::move(compiles);
}

RendezvousFile::RendezvousFile(std::optional<std::filesystem::path> target,
                               IAtomicWriteFiles& files,
                               std::uint64_t writerId):
    _target { std::move(target) },
    _files { files },
    _writerId { writerId }
{
}

std::expected<void, std::string> RendezvousFile::Publish(std::filesystem::path const& compiles)
{
    if (_attempted == compiles)
        return {};
    _attempted = compiles;
    if (!_target.has_value())
        return std::unexpected { std::string {
            "no runtime directory (RUNTIME_DIRECTORY is unset), so the compiles pace-cc runs itself cannot find this "
            "node's compile ceiling" } };
    auto const text = RenderRendezvous(compiles);
    auto const written = WriteFileAtomically(*_target, std::as_bytes(std::span { text }), _writerId, _files);
    if (!written.has_value())
        return std::unexpected { std::format("{} could not be written, so the compiles pace-cc runs itself cannot find "
                                             "this node's compile ceiling",
                                             _target->string()) };
    _written = true;
    return {};
}

void RendezvousFile::Withdraw() noexcept
{
    _attempted.reset();
    if (!std::exchange(_written, false) || !_target.has_value())
        return;
    _files.Remove(*_target);
}

LocalCompileRunner::LocalCompileRunner(
    ICeilingLocator const& locator, SourceFactory sources, RunnerFactory runners, ShimState shim, bool canSpawnIntoCgroups):
    _locator { locator },
    _sources { std::move(sources) },
    _runners { std::move(runners) },
    _shim { shim },
    _canSpawnIntoCgroups { canSpawnIntoCgroups }
{
}

LocalRun LocalCompileRunner::Run(std::span<std::string const> argv, LocalPlacement placement)
{
    auto const outside = [&](CeilingMiss miss) {
        return LocalRun { .run = _runners(nullptr)->RunCaptureSplit(argv), .defense = Outside(_shim, miss) };
    };
    if (placement == LocalPlacement::OutsideCeiling)
        return outside(CeilingMiss::KeptOutside);
    if (!_canSpawnIntoCgroups)
        return outside(CeilingMiss::NotThisPlatform);
    auto const compiles = _locator.Locate();
    if (!compiles.has_value())
        return outside(compiles.error());

    auto const source = std::make_shared<RecordingSource>(_sources(*compiles));
    auto run = _runners(source)->RunCaptureSplit(argv);
    if (run.inCgroup)
        return LocalRun { .run = std::move(run),
                          .defense = LocalDefenseReport { .defense = LocalDefense::Ceiling, .miss = CeilingMiss::None } };
    return LocalRun { .run = std::move(run), .defense = Outside(_shim, source->Missed()) };
}

} // namespace Pace::Cc
