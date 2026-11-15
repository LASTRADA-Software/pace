// SPDX-License-Identifier: Apache-2.0
//
// A launcher's own compiles under the compile-OOM defense (#1770): where the node names its ceiling, where the
// launcher looks for it, and -- the decision this file exists for -- whether a compile joined the ceiling or ran
// under the OOM-victim shim alone, and why. Over fakes for every decision; one case places a real child in a real
// cgroup, where this host lets the test process make one.
#include "LocalCeiling.hpp"

#include <Pace/Platform/CgroupV2.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Cc;
using Pace::Testing::Unwrap;

namespace
{

/// A cgroup this fake source made: it holds nothing and measures nothing.
class FakeCgroup final: public Platform::IJobCgroup
{
  public:
    [[nodiscard]] int DirectoryDescriptor() const noexcept override
    {
        return -1;
    }
    [[nodiscard]] std::optional<std::uint64_t> PeakBytes() const override
    {
        return std::uint64_t { 4'096 };
    }
    [[nodiscard]] bool OomKilled() const override
    {
        return false;
    }
    [[nodiscard]] bool Shed() const noexcept override
    {
        return false;
    }
};

/// A source that makes a cgroup, or refuses to.
class FakeSource final: public Platform::IJobCgroupSource
{
  public:
    explicit FakeSource(bool refuses):
        _refuses { refuses }
    {
    }
    [[nodiscard]] std::expected<std::unique_ptr<Platform::IJobCgroup>, std::error_code> Create() override
    {
        if (_refuses)
            return std::unexpected { std::make_error_code(std::errc::permission_denied) };
        return std::make_unique<FakeCgroup>();
    }

  private:
    bool _refuses;
};

/// A runner standing in for `posix_spawn`: given a source it asks for a cgroup, and places the child in it unless
/// the system "refuses" -- in which case it tells the source, as the real runner does, and runs it outside.
class FakeRunner final: public IProcessRunner
{
  public:
    FakeRunner(std::shared_ptr<Platform::IJobCgroupSource> cgroups, bool spawnRefused):
        _cgroups { std::move(cgroups) },
        _spawnRefused { spawnRefused }
    {
    }
    [[nodiscard]] CompileRun RunCaptureCombined(std::span<std::string const> /*argv*/) override
    {
        return CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }
    [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> /*argv*/) override
    {
        CompileRun run { .exitCode = 0, .out = {}, .err = {} };
        if (_cgroups == nullptr)
            return run;
        auto const job = _cgroups->Create();
        if (!job.has_value())
            return run;
        if (_spawnRefused)
        {
            _cgroups->NoteUnmeasured(Platform::IJobCgroupSource::Unmeasured::SpawnRefused);
            return run;
        }
        run.inCgroup = true;
        return run;
    }

  private:
    std::shared_ptr<Platform::IJobCgroupSource> _cgroups;
    bool _spawnRefused;
};

/// A locator answering from a script.
class FakeLocator final: public ICeilingLocator
{
  public:
    explicit FakeLocator(std::expected<std::filesystem::path, CeilingMiss> answer):
        _answer { std::move(answer) }
    {
    }
    [[nodiscard]] std::expected<std::filesystem::path, CeilingMiss> Locate() const override
    {
        return _answer;
    }

  private:
    std::expected<std::filesystem::path, CeilingMiss> _answer;
};

/// The world one local compile runs in.
struct World
{
    std::expected<std::filesystem::path, CeilingMiss> located { std::filesystem::path { "/sys/fs/cgroup/n/compiles" } };
    bool cgroupRefused {};
    bool spawnRefused {};
    bool canSpawnIntoCgroups { true };
    ShimState shim { ShimState::Shimmed };
    std::vector<std::filesystem::path> sourcesMade; ///< Every `compiles/` a source was made for.

    /// @return What the compile ran under.
    [[nodiscard]] LocalDefenseReport Run(LocalPlacement placement)
    {
        FakeLocator const locator { located };
        LocalCompileRunner runner {
            locator,
            [this](std::filesystem::path const& compiles) -> std::shared_ptr<Platform::IJobCgroupSource> {
                sourcesMade.push_back(compiles);
                return std::make_shared<FakeSource>(cgroupRefused);
            },
            [this](std::shared_ptr<Platform::IJobCgroupSource> cgroups) -> std::unique_ptr<IProcessRunner> {
                return std::make_unique<FakeRunner>(std::move(cgroups), spawnRefused);
            },
            shim,
            canSpawnIntoCgroups,
        };
        std::vector<std::string> const argv { "cc", "-c", "tu.c" };
        return runner.Run(argv, placement).defense;
    }
};

} // namespace

TEST_CASE("LocalCeiling.a-compile-placed-in-the-node-ceiling-says-so", "[launcher][local-ceiling]")
{
    World world;
    auto const defense = world.Run(LocalPlacement::UnderCeiling);
    CHECK(defense == LocalDefenseReport { .defense = LocalDefense::Ceiling, .miss = CeilingMiss::None });
    // Below the cgroup the rendezvous named, and nowhere else.
    REQUIRE(world.sourcesMade.size() == 1);
    CHECK(world.sourcesMade.front() == std::filesystem::path { "/sys/fs/cgroup/n/compiles" });
}

TEST_CASE("LocalCeiling.each-way-of-not-joining-is-named", "[launcher][local-ceiling]")
{
    // Every one of these ran the compile -- under the shim alone -- and none may read as the ceiling.
    World world;
    auto expected = CeilingMiss::None;
    SECTION("no runtime directory to look in")
    {
        world.located = std::unexpected { CeilingMiss::NoRuntimeDirectory };
        expected = CeilingMiss::NoRuntimeDirectory;
    }
    SECTION("no node of this user names a ceiling")
    {
        world.located = std::unexpected { CeilingMiss::NoNodeCeiling };
        expected = CeilingMiss::NoNodeCeiling;
    }
    SECTION("the node's compiles/ refuses this user a cgroup")
    {
        world.cgroupRefused = true;
        expected = CeilingMiss::CgroupNotMade;
    }
    SECTION("the system will not start the compiler there")
    {
        world.spawnRefused = true;
        expected = CeilingMiss::SpawnRefused;
    }
    SECTION("this build cannot start a process in a cgroup at all")
    {
        world.canSpawnIntoCgroups = false;
        expected = CeilingMiss::NotThisPlatform;
    }

    auto const defense = world.Run(LocalPlacement::UnderCeiling);
    CHECK(defense == LocalDefenseReport { .defense = LocalDefense::ShimOnly, .miss = expected });
}

TEST_CASE("LocalCeiling.kept-outside-never-asks-for-a-cgroup", "[launcher][local-ceiling]")
{
    // Killed in the ceiling once, or nothing can hold it: the ceiling would only kill it again, so it is not asked.
    World world;
    auto const defense = world.Run(LocalPlacement::OutsideCeiling);
    CHECK(defense == LocalDefenseReport { .defense = LocalDefense::ShimOnly, .miss = CeilingMiss::KeptOutside });
    CHECK(world.sourcesMade.empty());
}

TEST_CASE("LocalCeiling.no-shim-is-no-defense-not-the-shim", "[launcher][local-ceiling]")
{
    World world;
    world.shim = ShimState::NoShim;
    world.located = std::unexpected { CeilingMiss::NoNodeCeiling };
    CHECK(world.Run(LocalPlacement::UnderCeiling)
          == LocalDefenseReport { .defense = LocalDefense::Unprotected, .miss = CeilingMiss::NoNodeCeiling });
}

TEST_CASE("LocalCeiling.the-node-writes-where-its-unit-is-and-the-launcher-reads-the-user-unit", "[launcher][local-ceiling]")
{
    // Only a unit's own runtime directory: a node started by hand writes nothing, so it can never overwrite -- or
    // withdraw -- the rendezvous of the node the user runs.
    CHECK_FALSE(RendezvousPublishPath(std::nullopt).has_value());
    CHECK_FALSE(RendezvousPublishPath(std::string {}).has_value());
    CHECK(RendezvousPublishPath(std::string { "/run/user/1000/pace-node" })
          == std::filesystem::path { "/run/user/1000/pace-node/compile-ceiling" });
    CHECK(RendezvousPublishPath(std::string { "/run/user/1000/pace-node:/run/user/1000/other" })
          == std::filesystem::path { "/run/user/1000/pace-node/compile-ceiling" });
    // The two meet exactly under the user unit: `%t/pace-node` is `$XDG_RUNTIME_DIR/pace-node`.
    CHECK(RendezvousLookupPath(std::string { "/run/user/1000" })
          == RendezvousPublishPath(std::string { "/run/user/1000/pace-node" }));
    CHECK_FALSE(RendezvousLookupPath(std::nullopt).has_value());
    CHECK_FALSE(RendezvousLookupPath(std::string {}).has_value());
}

TEST_CASE("LocalCeiling.the-rendezvous-names-one-absolute-cgroup", "[launcher][local-ceiling]")
{
    auto const compiles = std::filesystem::path { "/sys/fs/cgroup/user.slice/pace-node.service/compiles" };
    CHECK(ParseRendezvous(RenderRendezvous(compiles)) == compiles);
    CHECK_FALSE(ParseRendezvous("").has_value());
    CHECK_FALSE(ParseRendezvous("compiles\n").has_value());
    CHECK_FALSE(ParseRendezvous("\n/sys/fs/cgroup/x\n").has_value());
}

TEST_CASE("LocalCeiling.the-locator-says-why-it-found-nothing", "[launcher][local-ceiling]")
{
    std::map<std::string, std::string> environment;
    std::map<std::filesystem::path, std::string> files;
    RendezvousLocator const locator {
        [&](std::string_view name) -> std::optional<std::string> {
            auto const found = environment.find(std::string { name });
            return found == environment.end() ? std::nullopt : std::optional<std::string> { found->second };
        },
        [&](std::filesystem::path const& path) -> std::optional<std::string> {
            auto const found = files.find(path);
            return found == files.end() ? std::nullopt : std::optional<std::string> { found->second };
        },
    };

    CHECK(locator.Locate().error() == CeilingMiss::NoRuntimeDirectory);
    environment["XDG_RUNTIME_DIR"] = "/run/user/1000";
    CHECK(locator.Locate().error() == CeilingMiss::NoNodeCeiling);
    files[std::filesystem::path { "/run/user/1000/pace-node/compile-ceiling" }] = "not a path\n";
    CHECK(locator.Locate().error() == CeilingMiss::Unreadable);
    files[std::filesystem::path { "/run/user/1000/pace-node/compile-ceiling" }] = "/sys/fs/cgroup/n/compiles\n";
    CHECK(Unwrap(locator.Locate()) == std::filesystem::path { "/sys/fs/cgroup/n/compiles" });
}

TEST_CASE("LocalCeiling.a-defense-token-round-trips-and-a-contradiction-is-refused", "[launcher][local-ceiling]")
{
    for (auto const& row: CeilingMissTable)
    {
        auto const report = row.miss == CeilingMiss::None
                                ? LocalDefenseReport { .defense = LocalDefense::Ceiling, .miss = CeilingMiss::None }
                                : LocalDefenseReport { .defense = LocalDefense::ShimOnly, .miss = row.miss };
        CAPTURE(row.id);
        CHECK(ParseLocalDefenseToken(LocalDefenseToken(report)) == report);
    }
    CHECK(LocalDefenseToken({ .defense = LocalDefense::Ceiling, .miss = CeilingMiss::None }) == "ceiling");
    CHECK(LocalDefenseToken({ .defense = LocalDefense::ShimOnly, .miss = CeilingMiss::NoNodeCeiling })
          == "shim:no-node-ceiling");
    // A line from before the column, a later build's word, and a claim that contradicts itself are no reading.
    CHECK_FALSE(ParseLocalDefenseToken("").has_value());
    CHECK_FALSE(ParseLocalDefenseToken("cgroup").has_value());
    CHECK_FALSE(ParseLocalDefenseToken("shim:sideways").has_value());
    CHECK_FALSE(ParseLocalDefenseToken("ceiling:kept-outside").has_value());
    CHECK_FALSE(ParseLocalDefenseToken("shim").has_value());
}

namespace
{

/// Files a rendezvous is written through, in memory.
class MemoryFiles final: public IAtomicWriteFiles
{
  public:
    std::map<std::filesystem::path, std::string> files;
    std::size_t creates {};

    [[nodiscard]] std::unique_ptr<IFileSink> Create(std::filesystem::path const& path) override
    {
        ++creates;
        files[path].clear();
        return std::make_unique<Sink>(files[path]);
    }
    [[nodiscard]] bool Replace(std::filesystem::path const& from, std::filesystem::path const& to) override
    {
        files[to] = std::move(files[from]);
        files.erase(from);
        return true;
    }
    void Remove(std::filesystem::path const& path) noexcept override
    {
        files.erase(path);
    }

  private:
    class Sink final: public IFileSink
    {
      public:
        explicit Sink(std::string& into):
            _into { into }
        {
        }
        [[nodiscard]] bool Write(std::span<std::byte const> bytes) override
        {
            for (auto const byte: bytes)
                _into.push_back(static_cast<char>(byte));
            return true;
        }
        [[nodiscard]] bool Close() override
        {
            return true;
        }

      private:
        std::string& _into;
    };
};

} // namespace

TEST_CASE("LocalCeiling.a-node-publishes-while-capped-and-withdraws-after", "[node][local-ceiling]")
{
    MemoryFiles files;
    auto const target = std::filesystem::path { "/run/user/1000/pace-node/compile-ceiling" };
    RendezvousFile rendezvous { target, files, 42 };
    auto const compiles = std::filesystem::path { "/sys/fs/cgroup/n/compiles" };

    REQUIRE(rendezvous.Publish(compiles).has_value());
    REQUIRE(files.files.contains(target));
    CHECK(ParseRendezvous(files.files.at(target)) == compiles);
    // The same ceiling again writes nothing: a ceiling re-applied at every allowance step costs no file.
    auto const creates = files.creates;
    REQUIRE(rendezvous.Publish(compiles).has_value());
    CHECK(files.creates == creates);

    rendezvous.Withdraw();
    CHECK_FALSE(files.files.contains(target));
    // And after a withdraw, capped again is published again.
    REQUIRE(rendezvous.Publish(compiles).has_value());
    CHECK(files.files.contains(target));
}

TEST_CASE("LocalCeiling.a-node-with-no-runtime-directory-says-so-once", "[node][local-ceiling]")
{
    MemoryFiles files;
    RendezvousFile rendezvous { std::nullopt, files, 42 };
    auto const compiles = std::filesystem::path { "/sys/fs/cgroup/n/compiles" };
    CHECK_FALSE(rendezvous.Publish(compiles).has_value());
    CHECK(rendezvous.Publish(compiles).has_value());
    CHECK(files.files.empty());
    rendezvous.Withdraw(); // nothing written, nothing to remove
    CHECK(files.files.empty());
}

TEST_CASE("LocalCeiling.a-real-child-lands-in-a-real-ceiling", "[launcher][local-ceiling][cgroup]")
{
    // The one end-to-end placement: a cgroup made below this test process's own, named as a node would name its
    // `compiles/`, and a real child started through the real runner. Skipped where this host does not let the test
    // make a child cgroup or start a process inside one -- which is itself the `CeilingMiss` the launcher reports.
    if (!CanSpawnIntoCgroups())
        SKIP("this build cannot start a process inside a cgroup");
    auto const host = std::make_shared<Platform::SystemCgroupHost>();
    auto const self = host->ReadFile("/proc/self/cgroup");
    if (!self.has_value())
        SKIP("no /proc/self/cgroup");
    auto const own = Platform::ParseUnifiedCgroupPath(*self);
    if (!own.has_value())
        SKIP("no unified cgroup hierarchy");
    auto const compiles = std::filesystem::path { Platform::DefaultCgroupMount }
                          / std::filesystem::path { *own }.relative_path()
                          / std::format("pace-local-ceiling-test-{}", host->OwnProcessId());
    if (auto const made = host->MakeDirectory(compiles); !made.has_value())
        SKIP(std::format("this process may not make a cgroup below its own: {}", made.error().message()));

    FakeLocator const locator { compiles };
    LocalCompileRunner runner {
        locator,
        [&](std::filesystem::path const& at) -> std::shared_ptr<Platform::IJobCgroupSource> {
            return std::make_shared<Platform::CgroupJobSource>(
                host,
                Platform::CgroupLayout {
                    .delegated = {}, .leaf = {}, .compiles = at, .notApplied = {}, .oomdOmitRefused = std::nullopt },
                Platform::CgroupTeardownPolicy {},
                "pace-cc-test-");
        },
        [](std::shared_ptr<Platform::IJobCgroupSource> cgroups) {
            return cgroups != nullptr ? MakeProcessRunner(std::move(cgroups), std::nullopt)
                                      : MakeProcessRunner(PeakSource::ResourceUsage, std::nullopt);
        },
        ShimState::NoShim,
        true,
    };
    std::vector<std::string> const argv { "true" };
    auto const local = runner.Run(argv, LocalPlacement::UnderCeiling);
    static_cast<void>(host->RemoveCgroupDirectory(compiles));

    CHECK(local.run.exitCode == 0);
    if (!local.run.inCgroup)
        SKIP(std::format("the system would not start a child inside it: {}",
                         CeilingMissTable[static_cast<std::size_t>(local.defense.miss)].sentence));
    CHECK(local.defense == LocalDefenseReport { .defense = LocalDefense::Ceiling, .miss = CeilingMiss::None });
}
