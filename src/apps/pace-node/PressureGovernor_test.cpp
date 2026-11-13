// SPDX-License-Identifier: Apache-2.0
//
// The pressure governor: whose stall it is, when to yield and shed, and how the allowance comes back.
// Every case scripts a sequence of PSI samples against a `ManualClock`, so dwell and ramp are exact.
#include "PressureGovernor.hpp"

#include <Pace/Platform/MemoryPressure.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>

#include <core/platform/Clock.hpp>

using namespace Pace;
using namespace std::chrono_literals;
using Node::GovernorDecision;
using Node::PressureGovernor;
using Node::PressureLevel;
using Node::PressureThresholds;

namespace
{

constexpr std::uint64_t GiB = std::uint64_t { 1 } << 30U;

/// The configured allowance every case starts from: a 96 GiB machine's 75%.
constexpr std::uint64_t Configured = 72 * GiB;

/// One ramp step at the default 5%, derived the way the rule states it.
constexpr std::uint64_t RampStep = Configured * 5 / 100;

/// What a sample says, in PSI avg10 percentages.
struct Script
{
    double some = 0.0;                    ///< System `some`.
    std::optional<double> full = 0.0;     ///< System `full`; disengaged as on a kernel without the line.
    std::optional<double> compiles = 0.0; ///< Compiles' `some`; disengaged when there is no compiles reading.
};

/// @return The sample @p script describes.
Platform::PressureSample SampleOf(Script const& script)
{
    auto sample = Platform::PressureSample {};
    sample.system.some.avg10 = script.some;
    if (script.full)
        sample.system.full = Platform::PsiLine { .avg10 = *script.full, .avg60 = 0.0, .totalMicros = 0 };
    if (script.compiles)
    {
        auto compiles = Platform::PsiReading {};
        compiles.some.avg10 = *script.compiles;
        sample.compiles = compiles;
    }
    return sample;
}

/// A governor at the default thresholds over its own manual clock.
struct Rig
{
    core::platform::ManualClock clock;
    PressureGovernor governor { PressureThresholds {}, Configured, clock };

    /// Advance the clock by @p elapsed, then step one sample.
    GovernorDecision After(std::chrono::seconds elapsed, Script const& script, std::uint64_t hold)
    {
        clock.advance(elapsed);
        return governor.Step(SampleOf(script), hold);
    }
};

/// The desktop is fine.
constexpr Script Quiet { .some = 0.0, .full = 0.0, .compiles = 0.0 };

/// A game needs memory: the system stalls and the compiles do not.
constexpr Script GameStall { .some = 20.0, .full = 0.0, .compiles = 2.0 };

/// Every task stalls at once: the desktop freezes.
constexpr Script Freezing { .some = 40.0, .full = 8.0, .compiles = 2.0 };

} // namespace

TEST_CASE("A game's stall yields and keeps the allowance at what compiles held", "[node][pressure]")
{
    Rig rig;
    auto const decision = rig.After(0s, GameStall, 30 * GiB);
    CHECK(decision.level == PressureLevel::Yield);
    CHECK(decision.allowanceBytes == 30 * GiB);
    CHECK_FALSE(decision.killYoungest);
    CHECK(decision.levelChanged);
}

TEST_CASE("Compiles stalling under their own ceiling is not a yield", "[node][pressure]")
{
    SECTION("Compiles carry the stall: the ceiling is working")
    {
        Rig rig;
        auto const decision = rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 19.0 }, 30 * GiB);
        CHECK(decision.level == PressureLevel::Calm);
        CHECK(decision.allowanceBytes == Configured);
        CHECK_FALSE(decision.levelChanged);
    }
    SECTION("Control: compiles below 80% of the system's stall is someone else's")
    {
        Rig rig;
        CHECK(rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 15.0 }, 30 * GiB).level == PressureLevel::Yield);
    }
    SECTION("A freezing desktop sheds whoever stalls")
    {
        Rig rig;
        CHECK(rig.After(0s, { .some = 20.0, .full = 8.0, .compiles = 19.0 }, 30 * GiB).level == PressureLevel::Shed);
    }
}

TEST_CASE("A freezing desktop sheds one compile per dwell", "[node][pressure]")
{
    Rig rig;
    auto const first = rig.After(0s, Freezing, 30 * GiB);
    CHECK(first.level == PressureLevel::Shed);
    CHECK(first.killYoungest);
    CHECK(first.levelChanged);
    CHECK(first.allowanceBytes == 30 * GiB);

    auto const within = rig.After(9s, Freezing, 20 * GiB);
    CHECK(within.level == PressureLevel::Shed);
    CHECK_FALSE(within.killYoungest);
    CHECK_FALSE(within.levelChanged);

    auto const after = rig.After(1s, Freezing, 20 * GiB);
    CHECK(after.level == PressureLevel::Shed);
    CHECK(after.killYoungest);
}

TEST_CASE("Pressure that dips briefly does not leave yield before dwell", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).level == PressureLevel::Yield);

    auto const dip = rig.After(3s, Quiet, 30 * GiB);
    CHECK(dip.level == PressureLevel::Yield);
    CHECK_FALSE(dip.levelChanged);
    CHECK(rig.After(3s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    CHECK(rig.After(3s, Quiet, 30 * GiB).level == PressureLevel::Yield);

    SECTION("Between the thresholds is not below the exit, whatever the dwell")
    {
        CHECK(rig.After(10s, { .some = 7.0, .full = 0.0, .compiles = 0.0 }, 30 * GiB).level == PressureLevel::Yield);
    }
    SECTION("Below the exit once the dwell is spent leaves")
    {
        auto const calm = rig.After(1s, Quiet, 30 * GiB);
        CHECK(calm.level == PressureLevel::Calm);
        CHECK(calm.levelChanged);
        CHECK(calm.allowanceBytes == 30 * GiB);
    }
}

TEST_CASE("Calm ramps the allowance back by 5% per 30 s and never past configured", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).allowanceBytes == 30 * GiB);
    REQUIRE(rig.After(10s, Quiet, 30 * GiB).level == PressureLevel::Calm);

    CHECK(rig.After(29s, Quiet, 30 * GiB).allowanceBytes == 30 * GiB);
    CHECK(rig.After(1s, Quiet, 30 * GiB).allowanceBytes == (30 * GiB) + RampStep);
    CHECK(rig.After(60s, Quiet, 30 * GiB).allowanceBytes == (30 * GiB) + (3 * RampStep));

    // Twelve more steps would carry it to 84 GiB: the cap holds on the step that would cross it, and after.
    auto lastAllowance = std::uint64_t {};
    for ([[maybe_unused]] auto const step: std::views::iota(0, 12))
    {
        lastAllowance = rig.After(30s, Quiet, 30 * GiB).allowanceBytes;
        CHECK(lastAllowance <= Configured);
    }
    CHECK(lastAllowance == Configured);
}

TEST_CASE("A yield during the ramp resets the allowance to current holding", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    REQUIRE(rig.After(10s, Quiet, 30 * GiB).level == PressureLevel::Calm);
    REQUIRE(rig.After(30s, Quiet, 30 * GiB).allowanceBytes == (30 * GiB) + RampStep);

    auto const again = rig.After(5s, GameStall, 20 * GiB);
    CHECK(again.level == PressureLevel::Yield);
    CHECK(again.levelChanged);
    CHECK(again.allowanceBytes == 20 * GiB);

    SECTION("Holding more than the allowance does not raise it")
    {
        REQUIRE(rig.After(10s, Quiet, 20 * GiB).level == PressureLevel::Calm);
        CHECK(rig.After(1s, GameStall, 50 * GiB).allowanceBytes == 20 * GiB);
    }
}

TEST_CASE("A stall with no compiles running yields with allowance 0 and asks no kill", "[node][pressure]")
{
    Rig rig;
    auto const yield = rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = std::nullopt }, 0);
    CHECK(yield.level == PressureLevel::Yield);
    CHECK(yield.allowanceBytes == 0);
    CHECK_FALSE(yield.killYoungest);

    auto const shed = rig.After(1s, Freezing, 0);
    CHECK(shed.level == PressureLevel::Shed);
    CHECK_FALSE(shed.killYoungest);
    CHECK(shed.allowanceBytes == 0);

    // Nothing was killed, so the first compile to hold memory under a freeze is shed at once.
    CHECK(rig.After(1s, Freezing, 1 * GiB).killYoungest);
}

TEST_CASE("Reconfigure lowers the cap and clamps the allowance", "[node][pressure]")
{
    SECTION("A calm allowance at the old cap drops to the new one")
    {
        Rig rig;
        rig.governor.Reconfigure(40 * GiB);
        CHECK(rig.After(0s, Quiet, 0).allowanceBytes == 40 * GiB);
    }
    SECTION("An allowance under the new cap is kept")
    {
        Rig rig;
        REQUIRE(rig.After(0s, GameStall, 30 * GiB).allowanceBytes == 30 * GiB);
        rig.governor.Reconfigure(40 * GiB);
        CHECK(rig.After(1s, GameStall, 30 * GiB).allowanceBytes == 30 * GiB);
        rig.governor.Reconfigure(20 * GiB);
        CHECK(rig.After(1s, GameStall, 30 * GiB).allowanceBytes == 20 * GiB);
    }
    SECTION("Raising the cap does not raise the allowance; the ramp climbs toward it")
    {
        Rig rig;
        rig.governor.Reconfigure(100 * GiB);
        CHECK(rig.After(0s, Quiet, 0).allowanceBytes == Configured);
        CHECK(rig.After(30s, Quiet, 0).allowanceBytes == Configured + (100 * GiB * 5 / 100));
    }
}

TEST_CASE("Without a full line, a yield held for twice the dwell sheds", "[node][pressure]")
{
    Rig rig;
    auto const noFull = Script { .some = 20.0, .full = std::nullopt, .compiles = 2.0 };
    REQUIRE(rig.After(0s, noFull, 30 * GiB).level == PressureLevel::Yield);
    CHECK(rig.After(19s, noFull, 30 * GiB).level == PressureLevel::Yield);

    auto const shed = rig.After(1s, noFull, 30 * GiB);
    CHECK(shed.level == PressureLevel::Shed);
    CHECK(shed.killYoungest);

    SECTION("Shed ends once the dwell is spent and the stall is gone")
    {
        CHECK(rig.After(9s, { .some = 2.0, .full = std::nullopt, .compiles = 0.0 }, 20 * GiB).level == PressureLevel::Shed);
        auto const calm = rig.After(1s, { .some = 2.0, .full = std::nullopt, .compiles = 0.0 }, 20 * GiB);
        CHECK(calm.level == PressureLevel::Calm);
        CHECK(calm.levelChanged);
    }
    SECTION("Shed steps down to yield while the system still stalls above the yield exit")
    {
        auto const yield = rig.After(10s, { .some = 7.0, .full = std::nullopt, .compiles = 0.0 }, 20 * GiB);
        CHECK(yield.level == PressureLevel::Yield);
        CHECK_FALSE(yield.killYoungest);
    }
    SECTION("Shed holds while the stall that raised it persists")
    {
        CHECK(rig.After(10s, noFull, 20 * GiB).level == PressureLevel::Shed);
    }
}

TEST_CASE("A yield whose stall eases below its entry is not escalated", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    CHECK(rig.After(25s, { .some = 7.0, .full = 0.0, .compiles = 0.0 }, 30 * GiB).level == PressureLevel::Yield);
}

TEST_CASE("Compiles stalling under the ceiling a yield set are neither escalated nor held", "[node][pressure]")
{
    // The likely path: a yield clamps the allowance to what compiles hold, the compiles keep growing into
    // that ceiling and stall against it, and the game that caused the yield exits. The system's stall is
    // then the compiles' own.
    Rig rig;
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    auto const ownStall = Script { .some = 20.0, .full = 0.0, .compiles = 19.0 };

    auto const held = rig.After(9s, ownStall, 30 * GiB);
    CHECK(held.level == PressureLevel::Yield);

    // Past the dwell the own stall is no reason to stay; past twice the dwell it is no reason to shed.
    auto const left = rig.After(1s, ownStall, 30 * GiB);
    CHECK(left.level == PressureLevel::Calm);
    CHECK(left.levelChanged);
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 6))
    {
        auto const decision = rig.After(5s, ownStall, 30 * GiB);
        CHECK(decision.level == PressureLevel::Calm);
        CHECK_FALSE(decision.killYoungest);
    }
}

TEST_CASE("Compiles stalling under their own ceiling do not hold a shed", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, Freezing, 30 * GiB).level == PressureLevel::Shed);
    auto const ownStall = Script { .some = 20.0, .full = 0.0, .compiles = 19.0 };
    CHECK(rig.After(9s, ownStall, 20 * GiB).level == PressureLevel::Shed);
    auto const left = rig.After(1s, ownStall, 20 * GiB);
    CHECK(left.level == PressureLevel::Calm);
    CHECK_FALSE(left.killYoungest);
}

TEST_CASE("Only a SUSTAINED foreign stall escalates a yield to shed", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    auto const eased = Script { .some = 7.0, .full = 0.0, .compiles = 0.0 };
    auto const back = Script { .some = 11.0, .full = 0.0, .compiles = 0.0 };
    CHECK(rig.After(1s, eased, 30 * GiB).level == PressureLevel::Yield);
    CHECK(rig.After(18s, eased, 30 * GiB).level == PressureLevel::Yield);

    // The stall returns at t = 20 s: its 2 × dwell starts now, not at the yield's entry.
    auto const returned = rig.After(1s, back, 30 * GiB);
    CHECK(returned.level == PressureLevel::Yield);
    CHECK_FALSE(returned.killYoungest);
    CHECK(rig.After(19s, back, 30 * GiB).level == PressureLevel::Yield);

    auto const shed = rig.After(1s, back, 30 * GiB);
    CHECK(shed.level == PressureLevel::Shed);
    CHECK(shed.killYoungest);
}

TEST_CASE("A machine with no stall at all stays calm, with or without a compiles reading", "[node][pressure]")
{
    Rig rig;
    auto const zero = rig.After(0s, Quiet, 30 * GiB);
    CHECK(zero.level == PressureLevel::Calm);
    CHECK(zero.allowanceBytes == Configured);
    CHECK(rig.After(1s, { .some = 0.0, .full = std::nullopt, .compiles = std::nullopt }, 30 * GiB).level
          == PressureLevel::Calm);
    CHECK(rig.After(1s, { .some = 10.0, .full = 4.9, .compiles = std::nullopt }, 30 * GiB).level == PressureLevel::Calm);
}

TEST_CASE("The pressure level's bytes are pinned", "[node][pressure][wire]")
{
    CHECK(static_cast<std::uint8_t>(CompileCacheWire::PressureLevel::Calm) == 0);
    CHECK(static_cast<std::uint8_t>(CompileCacheWire::PressureLevel::Yield) == 1);
    CHECK(static_cast<std::uint8_t>(CompileCacheWire::PressureLevel::Shed) == 2);
    CHECK(static_cast<std::uint8_t>(CompileCacheWire::PressureLevel::Last) == 3);
}
