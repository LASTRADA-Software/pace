// SPDX-License-Identifier: Apache-2.0
//
// Tests for the OOM-victim shim (layer A of the compile-OOM defence): its decision against a fake writer, and
// the property itself against real processes -- a compiler spawned through the shim, and a child that compiler
// forks at once, both read 1000 from their own `oom_score_adj`.

#include "IProcessRunner.hpp"
#include "OomVictimExec.hpp"
#include "OomVictimTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <tests/Unwrap.hpp>

using Pace::Testing::Unwrap;
using namespace Pace::Cc::OomVictimTesting;

using namespace Pace::Cc;

namespace
{

/// Records what the shim writes, and answers as it is told.
class RecordingOomScoreWriter final: public IOomScoreWriter
{
  public:
    /// @param refuse Whether every write fails.
    explicit RecordingOomScoreWriter(bool refuse = false):
        _refuse { refuse }
    {
    }

    [[nodiscard]] std::expected<void, std::error_code> Write(std::string_view score) override
    {
        _written.emplace_back(score);
        if (_refuse)
            return std::unexpected { std::make_error_code(std::errc::no_such_file_or_directory) };
        return {};
    }

    /// @return Every score asked for, in order.
    [[nodiscard]] std::vector<std::string> const& Written() const noexcept
    {
        return _written;
    }

  private:
    bool _refuse;
    std::vector<std::string> _written;
};

/// A program no `PATH` holds, so the shim's `exec` of it fails and the shim returns rather than replacing the test.
constexpr auto MissingCompiler = "/nonexistent/pace-oom-victim-test/cc";

} // namespace

TEST_CASE("RunOomVictimExecIfInvoked is not the shim under any other name and writes nothing", "[cc][oom]")
{
    RecordingOomScoreWriter writer;
    auto const argv = std::array<char const*, 3> { "/usr/bin/pace-node", MissingCompiler, "--version" };

    CHECK_FALSE(RunOomVictimExecIfInvoked(argv, writer).has_value());
    CHECK(writer.Written().empty());
}

TEST_CASE("RunOomVictimExecIfInvoked as pace-oom-victim-exec writes 1000 before it runs the compiler", "[cc][oom]")
{
#if !defined(__linux__)
    SKIP("the OOM-victim shim exists on Linux only");
#else
    RecordingOomScoreWriter writer;
    auto const argv = std::array<char const*, 3> { "/usr/bin/pace-oom-victim-exec", MissingCompiler, "-c" };

    auto const failed = RunOomVictimExecIfInvoked(argv, writer);

    // Returned at all only because the compiler could not be run; the score was written first all the same.
    REQUIRE(failed.has_value());
    CHECK(Unwrap(failed) == OomVictimExecFailed);
    CHECK(writer.Written() == std::vector<std::string> { "1000" });
#endif
}

TEST_CASE("RunOomVictimExecIfInvoked still runs the compiler when the OOM score cannot be written", "[cc][oom]")
{
#if !defined(__linux__)
    SKIP("the OOM-victim shim exists on Linux only");
#else
    RecordingOomScoreWriter writer { true };
    auto const argv = std::array<char const*, 2> { "pace-oom-victim-exec", MissingCompiler };

    // It reached the `exec`: the only way back is the compiler failing to run, which is 127 rather than a refusal.
    CHECK(RunOomVictimExecIfInvoked(argv, writer) == std::optional { OomVictimExecFailed });
    CHECK(writer.Written().size() == 1);
#endif
}

TEST_CASE("RunOomVictimExecIfInvoked with no compiler to run fails without writing", "[cc][oom]")
{
#if !defined(__linux__)
    SKIP("the OOM-victim shim exists on Linux only");
#else
    RecordingOomScoreWriter writer;
    auto const argv = std::array<char const*, 1> { "pace-oom-victim-exec" };

    CHECK(RunOomVictimExecIfInvoked(argv, writer) == std::optional { OomVictimExecFailed });
    CHECK(writer.Written().empty());
#endif
}

TEST_CASE("A compiler spawned through the shim and its immediate children run at oom_score_adj 1000", "[cc][oom]")
{
#if !defined(__linux__)
    SKIP("oom_score_adj is Linux's");
#else
    if (OwnScore() == OomVictimScore)
        SKIP("this test process already runs at oom_score_adj 1000, so a child inheriting it proves nothing");
    auto const runner = MakeProcessRunner(PeakSource::Unmeasured, OomVictim { .self = ShimPath() });

    auto const run = runner->RunCaptureSplit(std::vector<std::string> { "/bin/sh", "-c", ReadScoreTwice });

    INFO(run.err);
    REQUIRE(run.exitCode == 0);
    CHECK(LinesOf(run.out) == std::vector<std::string> { "1000", "1000" });
#endif
}

TEST_CASE("A compiler spawned without the shim runs at the oom_score_adj it inherits", "[cc][oom]")
{
#if !defined(__linux__)
    SKIP("oom_score_adj is Linux's");
#else
    // The control: the same command through a runner with no shim reads the spawner's own score, so the case
    // above reads 1000 because of the shim and not because of anything the runner or the host does anyway.
    auto const own = OwnScore();
    REQUIRE_FALSE(own.empty());
    if (own == OomVictimScore)
        SKIP("this test process already runs at oom_score_adj 1000, so the control cannot tell the shim's work apart");
    auto const runner = MakeProcessRunner(PeakSource::Unmeasured, std::nullopt);

    auto const run = runner->RunCaptureSplit(std::vector<std::string> { "/bin/sh", "-c", ReadScoreTwice });

    REQUIRE(run.exitCode == 0);
    CHECK(LinesOf(run.out) == std::vector<std::string> { own, own });
#endif
}

TEST_CASE("A compiler the shim could not run is NotSpawned as it is without the shim", "[cc][oom]")
{
#if !defined(__linux__)
    SKIP("the OOM-victim shim exists on Linux only");
#else
    // The shim itself always spawns, so without the runner's own check a missing compiler would read as one that
    // ran and exited 127 -- and a toolchain probe that never ran must not read as one that answered.
    auto const shimmed = MakeProcessRunner(PeakSource::Unmeasured, OomVictim { .self = ShimPath() });
    auto const direct = MakeProcessRunner(PeakSource::Unmeasured, std::nullopt);

    for (auto const* const program: { MissingCompiler, "pace-oom-victim-test-no-such-compiler" })
    {
        CAPTURE(program);
        CHECK(shimmed->RunCaptureSplit(std::vector<std::string> { program, "--version" }).exitCode == NotSpawned);
        CHECK(direct->RunCaptureSplit(std::vector<std::string> { program, "--version" }).exitCode == NotSpawned);
    }
    // And a compiler found on `PATH` by its bare name runs through the shim as it does directly.
    CHECK(shimmed->RunCaptureSplit(std::vector<std::string> { "sh", "-c", "exit 3" }).exitCode == 3);
#endif
}

TEST_CASE("The shim names the running image through /proc/self/exe", "[cc][oom]")
{
#if !defined(__linux__)
    CHECK_FALSE(ThisProcessOomVictim().has_value());
#else
    auto const victim = ThisProcessOomVictim();
    REQUIRE(victim.has_value());
    CHECK(Unwrap(victim).self == std::filesystem::path { "/proc/self/exe" });
#endif
}
