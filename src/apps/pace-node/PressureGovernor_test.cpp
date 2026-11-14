// SPDX-License-Identifier: Apache-2.0
//
// The pressure governor: whose stall it is, when to yield and shed, and how the allowance comes back.
// Every case scripts a sequence of PSI samples against a `ManualClock`, so dwell and ramp are exact.
#include "PressureGovernor.hpp"

#include <Pace/Platform/MemoryPressure.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

#include <core/platform/Clock.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace std::chrono_literals;
using Node::GovernorDecision;
using Node::PressureGovernor;
using Node::PressureLevel;
using Node::PressureThresholds;
using Pace::Testing::Unwrap;

namespace
{

constexpr std::uint64_t GiB = std::uint64_t { 1 } << 30U;

/// The configured allowance every case starts from: a 96 GiB machine's 75%.
constexpr std::uint64_t Configured = 72 * GiB;

/// The least a yield lowers the allowance to: the default compile estimate, as production configures it.
constexpr std::uint64_t Floor = 2 * GiB;

/// One ramp step at the default 5%, derived the way the rule states it.
constexpr std::uint64_t RampStep = Configured * 5 / 100;

/// What a sample says, in PSI avg10 percentages.
struct Script
{
    double some = 0.0;                     ///< System `some`.
    std::optional<double> full = 0.0;      ///< System `full`; disengaged as on a kernel without the line.
    std::optional<double> compiles = 0.0;  ///< Compiles' `some`; disengaged when there is no compiles reading.
    std::optional<double> compilesFull {}; ///< Compiles' `full`; disengaged as on a file without the line.
    std::optional<bool> atLimit = false;   ///< The compiles stand at their own ceiling; disengaged for unreadable.
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
        if (script.compilesFull)
            compiles.full = Platform::PsiLine { .avg10 = *script.compilesFull, .avg60 = 0.0, .totalMicros = 0 };
        sample.compiles = compiles;
    }
    sample.compilesAtLimit = script.atLimit;
    return sample;
}

/// A governor at the default thresholds over its own manual clock.
struct Rig
{
    core::platform::ManualClock clock;
    PressureGovernor governor { PressureThresholds {}, Configured, Floor, clock };

    /// Advance the clock by @p elapsed, then step one sample. Milliseconds, so a case can step at a real loop's
    /// cadence -- a poll waited AFTER each step lands every sample a little past a whole second.
    GovernorDecision After(std::chrono::milliseconds elapsed, Script const& script, std::uint64_t hold)
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
        auto const decision = rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 19.0, .atLimit = true }, 30 * GiB);
        CHECK(decision.level == PressureLevel::Calm);
        CHECK(decision.allowanceBytes == Configured);
        CHECK_FALSE(decision.levelChanged);
    }
    SECTION("Control: compiles below 80% of the system's stall is someone else's")
    {
        Rig rig;
        CHECK(rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 15.0, .atLimit = true }, 30 * GiB).level
              == PressureLevel::Yield);
    }
    SECTION("A freezing desktop sheds whoever stalls")
    {
        Rig rig;
        CHECK(rig.After(0s, { .some = 20.0, .full = 8.0, .compiles = 19.0, .atLimit = true }, 30 * GiB).level
              == PressureLevel::Shed);
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

TEST_CASE("A stall with no compiles running yields with allowance at the floor and asks no kill", "[node][pressure]")
{
    // Held to what compiles held -- nothing -- the machine would report a budget no compile fits, and the queue would
    // refuse every compile there until the ramp came back: one default estimate is the least it is lowered to.
    Rig rig;
    auto const yield = rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = std::nullopt }, 0);
    CHECK(yield.level == PressureLevel::Yield);
    CHECK(yield.allowanceBytes == Floor);
    CHECK_FALSE(yield.killYoungest);

    auto const shed = rig.After(1s, Freezing, 0);
    CHECK(shed.level == PressureLevel::Shed);
    CHECK_FALSE(shed.killYoungest);
    CHECK(shed.allowanceBytes == Floor);

    // Nothing was killed, so the first compile to hold memory under a freeze is shed at once.
    CHECK(rig.After(1s, Freezing, 1 * GiB).killYoungest);
}

TEST_CASE("Reconfigure lowers the cap and clamps the allowance", "[node][pressure]")
{
    SECTION("A calm allowance at the old cap drops to the new one")
    {
        Rig rig;
        rig.governor.Reconfigure(40 * GiB, Floor);
        CHECK(rig.After(0s, Quiet, 0).allowanceBytes == 40 * GiB);
    }
    SECTION("An allowance under the new cap is kept")
    {
        Rig rig;
        REQUIRE(rig.After(0s, GameStall, 30 * GiB).allowanceBytes == 30 * GiB);
        rig.governor.Reconfigure(40 * GiB, Floor);
        CHECK(rig.After(1s, GameStall, 30 * GiB).allowanceBytes == 30 * GiB);
        rig.governor.Reconfigure(20 * GiB, Floor);
        CHECK(rig.After(1s, GameStall, 30 * GiB).allowanceBytes == 20 * GiB);
    }
    SECTION("Raising the cap raises an allowance pressure never lowered, at once")
    {
        Rig rig;
        rig.governor.Reconfigure(100 * GiB, Floor);
        CHECK(rig.After(0s, Quiet, 0).allowanceBytes == 100 * GiB);
        CHECK(rig.governor.AllowanceBytes() == 100 * GiB);
    }
    SECTION("Raising the cap does not raise an allowance a yield lowered; the ramp climbs toward it")
    {
        Rig rig;
        REQUIRE(rig.After(0s, GameStall, 30 * GiB).allowanceBytes == 30 * GiB);
        REQUIRE(rig.After(10s, Quiet, 0).level == PressureLevel::Calm);
        rig.governor.Reconfigure(100 * GiB, Floor);
        CHECK(rig.After(0s, Quiet, 0).allowanceBytes == 30 * GiB);
        CHECK(rig.After(30s, Quiet, 0).allowanceBytes == (30 * GiB) + (100 * GiB * 5 / 100));
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
    auto const ownStall = Script { .some = 20.0, .full = 0.0, .compiles = 19.0, .atLimit = true };

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
    auto const ownStall = Script { .some = 20.0, .full = 0.0, .compiles = 19.0, .atLimit = true };
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

TEST_CASE("A full stall the compiles carry does not hold a shed", "[node][pressure]")
{
    Rig rig;
    REQUIRE(rig.After(0s, Freezing, 30 * GiB).level == PressureLevel::Shed);

    SECTION("The compiles' own full stall: the shed ends at its dwell and kills no more")
    {
        auto const own = Script { .some = 3.0, .full = 3.0, .compiles = 0.0, .compilesFull = 2.9, .atLimit = true };
        CHECK_FALSE(rig.After(5s, own, 20 * GiB).killYoungest);
        auto const left = rig.After(5s, own, 20 * GiB);
        CHECK(left.level == PressureLevel::Calm);
        CHECK(left.levelChanged);
        CHECK_FALSE(left.killYoungest);
        for ([[maybe_unused]] auto const tick: std::views::iota(0, 4))
        {
            auto const decision = rig.After(5s, own, 20 * GiB);
            CHECK(decision.level == PressureLevel::Calm);
            CHECK_FALSE(decision.killYoungest);
        }
    }
    SECTION("Control: a full stall the compiles do not carry holds the shed")
    {
        auto const foreign = Script { .some = 3.0, .full = 3.0, .compiles = 0.0, .compilesFull = 0.5, .atLimit = true };
        CHECK(rig.After(5s, foreign, 20 * GiB).level == PressureLevel::Shed);
        auto const held = rig.After(5s, foreign, 20 * GiB);
        CHECK(held.level == PressureLevel::Shed);
        CHECK(held.killYoungest);
    }
    SECTION("Control: no compiles full line is never an own stall")
    {
        auto const unknown =
            Script { .some = 3.0, .full = 3.0, .compiles = 0.0, .compilesFull = std::nullopt, .atLimit = true };
        CHECK(rig.After(10s, unknown, 20 * GiB).level == PressureLevel::Shed);
    }
}

TEST_CASE("An own-stall sample restarts the foreign stall's escalation clock", "[node][pressure]")
{
    // Foreign from 0 s to 9 s, the compiles' own at 9 s, foreign again from 10 s: the 2 x dwell runs from
    // 10 s, so the shed comes at 30 s and not at 20 s.
    Rig rig;
    auto const ownStall = Script { .some = 20.0, .full = 0.0, .compiles = 19.0, .atLimit = true };
    REQUIRE(rig.After(0s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    REQUIRE(rig.After(9s, ownStall, 30 * GiB).level == PressureLevel::Yield);
    CHECK(rig.After(1s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    auto const at20 = rig.After(10s, GameStall, 30 * GiB);
    CHECK(at20.level == PressureLevel::Yield);
    CHECK_FALSE(at20.killYoungest);
    CHECK(rig.After(9s, GameStall, 30 * GiB).level == PressureLevel::Yield);
    auto const at30 = rig.After(1s, GameStall, 30 * GiB);
    CHECK(at30.level == PressureLevel::Shed);
    CHECK(at30.killYoungest);
}

TEST_CASE("A level whose threshold is disabled is never entered, escalation included", "[node][pressure]")
{
    constexpr auto DeepFreeze = Script { .some = 60.0, .full = 40.0, .compiles = 0.0 };

    SECTION("yield disabled: a foreign stall stays calm, and a freeze still sheds")
    {
        core::platform::ManualClock clock;
        auto thresholds = PressureThresholds {};
        thresholds.yieldEnter = Node::DisabledPressureThreshold;
        PressureGovernor governor { thresholds, Configured, Floor, clock };
        CHECK(governor.Step(SampleOf(Script { .some = 60.0, .full = 0.0, .compiles = 0.0 }), 8 * GiB).level
              == PressureLevel::Calm);
        CHECK(governor.Step(SampleOf(DeepFreeze), 8 * GiB).level == PressureLevel::Shed);
    }
    SECTION("shed disabled: a freeze yields, and a yield held past twice the dwell is not escalated")
    {
        core::platform::ManualClock clock;
        auto thresholds = PressureThresholds {};
        thresholds.shedEnter = Node::DisabledPressureThreshold;
        PressureGovernor governor { thresholds, Configured, Floor, clock };
        CHECK(governor.Step(SampleOf(DeepFreeze), 8 * GiB).level == PressureLevel::Yield);
        for ([[maybe_unused]] auto const tick: std::views::iota(0, 6))
        {
            clock.advance(10s);
            auto const decision = governor.Step(SampleOf(DeepFreeze), 8 * GiB);
            CHECK(decision.level == PressureLevel::Yield);
            CHECK_FALSE(decision.killYoungest);
        }
    }
}

TEST_CASE("Retune moves the thresholds of a running governor", "[node][pressure]")
{
    Rig rig;
    constexpr auto Mild = Script { .some = 7.0, .full = 0.0, .compiles = 0.0 };
    CHECK(rig.After(1s, Mild, 8 * GiB).level == PressureLevel::Calm);
    auto lower = PressureThresholds {};
    lower.yieldEnter = 6.0;
    lower.yieldExit = 3.0;
    rig.governor.Retune(lower);
    CHECK(rig.After(1s, Mild, 8 * GiB).level == PressureLevel::Yield);
}

TEST_CASE("The allowance floor is never above the configured bytes", "[node][pressure]")
{
    SECTION("A configuration below the floor holds the allowance to it, not to the floor")
    {
        core::platform::ManualClock clock;
        PressureGovernor governor { PressureThresholds {}, 1 * GiB, Floor, clock };
        CHECK(governor.Step(SampleOf(GameStall), 0).allowanceBytes == 1 * GiB);
    }
    SECTION("Reconfigured below the floor, a lowered allowance is clamped to the new configured bytes")
    {
        Rig rig;
        REQUIRE(rig.After(0s, GameStall, 0).allowanceBytes == Floor);
        rig.governor.Reconfigure(1 * GiB, Floor);
        CHECK(rig.governor.AllowanceBytes() == 1 * GiB);
    }
}

TEST_CASE("A stall the compiles carry far below their ceiling is a yield", "[node][pressure]")
{
    // Compiles in direct reclaim stall with everything else when a game takes the machine's memory: their share of the
    // stall is no evidence the stall is theirs unless they stand at their own ceiling.
    SECTION("Compiles' some close to the system's, far below their limit: yield")
    {
        Rig rig;
        auto const decision = rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 19.5, .atLimit = false }, 30 * GiB);
        CHECK(decision.level == PressureLevel::Yield);
        CHECK(decision.allowanceBytes == 30 * GiB);
    }
    SECTION("Control: the same stall at their limit is the ceiling working")
    {
        Rig rig;
        CHECK(rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 19.5, .atLimit = true }, 30 * GiB).level
              == PressureLevel::Calm);
    }
    SECTION("A full stall they carry below their limit holds a shed")
    {
        Rig rig;
        REQUIRE(rig.After(0s, Freezing, 30 * GiB).level == PressureLevel::Shed);
        auto const below = Script { .some = 3.0, .full = 3.0, .compiles = 0.0, .compilesFull = 2.9, .atLimit = false };
        CHECK(rig.After(10s, below, 20 * GiB).level == PressureLevel::Shed);
    }
}

TEST_CASE("A shed left on the compiles' own full stall is not re-entered on it", "[node][pressure]")
{
    // An own full stall at or above the shed threshold, held for a minute: entering sheds (full is "regardless"),
    // leaving at the dwell counts it as eased -- and it must stay left, not flap back each dwell and kill again.
    auto const own = Script { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = true };
    Rig rig;
    auto kills = 0;
    auto changes = 0;
    auto const tally = [&](GovernorDecision const& decision) {
        kills += decision.killYoungest ? 1 : 0;
        changes += decision.levelChanged ? 1 : 0;
        return decision;
    };
    REQUIRE(tally(rig.After(0s, own, 30 * GiB)).level == PressureLevel::Shed);
    for ([[maybe_unused]] auto const second: std::views::iota(0, 60))
        static_cast<void>(tally(rig.After(1s, own, 30 * GiB)));
    CHECK(kills <= 1);
    CHECK(changes == 2); // into Shed, and out of it once
    CHECK(rig.After(1s, own, 30 * GiB).level == PressureLevel::Calm);

    SECTION("The latch lifts once the full stall turns foreign: that sheds again")
    {
        auto const foreign = Script { .some = 8.0, .full = 6.0, .compiles = 1.0, .compilesFull = 0.5, .atLimit = true };
        auto const decision = rig.After(1s, foreign, 30 * GiB);
        CHECK(decision.level == PressureLevel::Shed);
        CHECK(decision.killYoungest);
    }
    SECTION("The latch lifts once the full stall falls below the exit: the next own freeze sheds again")
    {
        REQUIRE(rig.After(1s, Quiet, 30 * GiB).level == PressureLevel::Calm);
        CHECK(rig.After(1s, own, 30 * GiB).level == PressureLevel::Shed);
    }
}

TEST_CASE("At-limit evidence is sticky for one dwell: a shed's own kill does not make the stall foreign", "[node][pressure]")
{
    // The evidence describes the last second, PSI's avg10 the last ten. A shed kills the youngest; the survivors sit
    // below the ceiling at once while avg10 still carries the full stall they caused. Read instantly, that stall
    // looks foreign at the dwell, the shed holds, and a second compile dies.
    SECTION("the evidence drops right after the kill while avg10 decays: one kill, no re-shed, in a minute")
    {
        Rig rig;
        auto kills = 0;
        auto sheds = 0;
        auto const tally = [&](GovernorDecision const& decision) {
            kills += decision.killYoungest ? 1 : 0;
            sheds += decision.levelChanged && decision.level == PressureLevel::Shed ? 1 : 0;
        };
        tally(rig.After(0s, { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = true }, 30 * GiB));
        REQUIRE(rig.governor.AllowanceBytes() == 30 * GiB);
        for (auto const second: std::views::iota(1, 61))
        {
            // avg10 decays with a ten-second time constant: 6% * e^(-t/10), ~2.2% at t = 10 -- above shedExit.
            auto const full = 6.0 * std::exp(-static_cast<double>(second) / 10.0);
            tally(rig.After(
                1s,
                { .some = full * 1.3, .full = full, .compiles = full * 1.3, .compilesFull = full, .atLimit = false },
                20 * GiB));
        }
        CHECK(kills == 1);
        CHECK(sheds == 1);
    }
    SECTION("an own freeze whose evidence flickers stays latched: one kill, no re-shed, in a minute")
    {
        Rig rig;
        auto kills = 0;
        auto sheds = 0;
        for (auto const second: std::views::iota(0, 61))
        {
            // At the ceiling two seconds in three; full stays at 6%, at or above shedEnter, the whole minute.
            auto const decision =
                rig.After(second == 0 ? 0s : 1s,
                          { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = second % 3 != 2 },
                          30 * GiB);
            kills += decision.killYoungest ? 1 : 0;
            sheds += decision.levelChanged && decision.level == PressureLevel::Shed ? 1 : 0;
        }
        CHECK(kills == 1);
        CHECK(sheds == 1);
    }
    SECTION("control: evidence absent for longer than the window lets the full stall count as foreign again")
    {
        Rig rig;
        REQUIRE(rig.After(0s, { .some = 3.0, .full = 3.0, .compiles = 2.9, .compilesFull = 2.9, .atLimit = true }, 30 * GiB)
                    .level
                == PressureLevel::Calm);
        auto const stale = Script { .some = 3.0, .full = 6.0, .compiles = 2.9, .compilesFull = 5.9, .atLimit = false };
        REQUIRE(rig.After(1s, { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = true }, 30 * GiB)
                    .level
                == PressureLevel::Shed);
        // Own and sticky: the shed is left at its dwell and latched.
        CHECK(rig.After(10s, stale, 30 * GiB).level == PressureLevel::Calm);
        // Twenty seconds after the evidence was last seen the window has closed: the stall is nobody's own any more,
        // the latch lifts, and the freeze sheds. The window ends the hold; it only delays it.
        CHECK(rig.After(10s, stale, 30 * GiB).level == PressureLevel::Shed);
    }
    SECTION("unreadable evidence is none: an at-limit sample does not carry over a reading that failed")
    {
        Rig rig;
        REQUIRE(rig.After(0s, { .some = 20.0, .full = 0.0, .compiles = 19.0, .atLimit = true }, 30 * GiB).level
                == PressureLevel::Calm);
        CHECK(rig.After(1s, { .some = 20.0, .full = 0.0, .compiles = 19.0, .atLimit = std::nullopt }, 30 * GiB).level
              == PressureLevel::Yield);
    }
}

namespace
{

/// Section 1's scenario at a cadence: an own freeze shed at t = 0, the evidence gone with the kill, full avg10
/// decaying from 6% with a ten-second time constant.
/// @param step The time between samples.
/// @return How many compiles were killed and how many times `Shed` was entered, over sixty seconds.
[[nodiscard]] std::pair<int, int> OwnFreezeAfterKill(std::chrono::milliseconds step)
{
    Rig rig;
    auto kills = 0;
    auto sheds = 0;
    auto const tally = [&](GovernorDecision const& decision) {
        kills += decision.killYoungest ? 1 : 0;
        sheds += decision.levelChanged && decision.level == PressureLevel::Shed ? 1 : 0;
    };
    tally(rig.After(0s, { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = true }, 30 * GiB));
    auto elapsed = std::chrono::milliseconds {};
    while (elapsed < 60s)
    {
        elapsed += step;
        auto const full = 6.0 * std::exp(-std::chrono::duration<double>(elapsed).count() / 10.0);
        tally(rig.After(step,
                        { .some = full * 1.3, .full = full, .compiles = full * 1.3, .compilesFull = full, .atLimit = false },
                        20 * GiB));
    }
    return { kills, sheds };
}

} // namespace

TEST_CASE("At-limit evidence outlives the shed's first exit check at a real loop's cadence", "[node][pressure]")
{
    // A real loop steps, then waits one poll, so every sample lands a little past a whole second and the shed's first
    // exit check a little past its dwell. Evidence held for exactly one dwell expired a hair before that check.
    SECTION("1001 ms: one kill, no re-shed, in a minute")
    {
        auto const [kills, sheds] = OwnFreezeAfterKill(std::chrono::milliseconds { 1001 });
        CHECK(kills == 1);
        CHECK(sheds == 1);
    }
    SECTION("1010 ms: one kill, no re-shed, in a minute")
    {
        auto const [kills, sheds] = OwnFreezeAfterKill(std::chrono::milliseconds { 1010 });
        CHECK(kills == 1);
        CHECK(sheds == 1);
    }
    SECTION("control: whole seconds still give one kill")
    {
        auto const [kills, sheds] = OwnFreezeAfterKill(1s);
        CHECK(kills == 1);
        CHECK(sheds == 1);
    }
    SECTION("a shed entered on full from a sample with no evidence, the evidence one poll older: one kill")
    {
        // The freeze is entered on `full` regardless of whose it is, from a sample that carries no evidence; the
        // evidence was last seen one poll earlier, so at the first exit check it is a dwell AND a poll old.
        Rig rig;
        auto kills = 0;
        auto sheds = 0;
        auto const tally = [&](GovernorDecision const& decision) {
            kills += decision.killYoungest ? 1 : 0;
            sheds += decision.levelChanged && decision.level == PressureLevel::Shed ? 1 : 0;
            return decision;
        };
        auto const before = Script { .some = 3.0, .full = 3.0, .compiles = 2.9, .compilesFull = 2.9, .atLimit = true };
        REQUIRE(tally(rig.After(0s, before, 30 * GiB)).level == PressureLevel::Calm);
        auto const own = Script { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = false };
        REQUIRE(tally(rig.After(1s, own, 30 * GiB)).level == PressureLevel::Shed);
        for ([[maybe_unused]] auto const second: std::views::iota(0, 10))
            static_cast<void>(tally(rig.After(1s, own, 30 * GiB)));
        CHECK(kills == 1);
        CHECK(sheds == 1);
    }
    SECTION("section 3's leave-and-latch at the loop's cadence: left at the first exit check")
    {
        Rig rig;
        auto const step = std::chrono::milliseconds { 1001 };
        REQUIRE(rig.After(0s, { .some = 3.0, .full = 3.0, .compiles = 2.9, .compilesFull = 2.9, .atLimit = true }, 30 * GiB)
                    .level
                == PressureLevel::Calm);
        REQUIRE(
            rig.After(step, { .some = 8.0, .full = 6.0, .compiles = 7.9, .compilesFull = 5.9, .atLimit = true }, 30 * GiB)
                .level
            == PressureLevel::Shed);
        auto const stale = Script { .some = 3.0, .full = 6.0, .compiles = 2.9, .compilesFull = 5.9, .atLimit = false };
        auto left = std::optional<GovernorDecision> {};
        auto steps = 0;
        while (!left.has_value() && steps < 15)
        {
            ++steps;
            auto const decision = rig.After(step, stale, 30 * GiB);
            CHECK_FALSE(decision.killYoungest);
            if (decision.level != PressureLevel::Shed)
                left = decision;
        }
        REQUIRE(left.has_value());
        CHECK(steps == 10); // the first sample at least one dwell after entering
        CHECK(Unwrap(left).level == PressureLevel::Calm);
    }
}
