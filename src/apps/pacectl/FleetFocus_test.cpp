// SPDX-License-Identifier: Apache-2.0
#include "FleetFocus.hpp"

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Cli;
using Distributed::FleetSection;
using Pace::Testing::Unwrap;

TEST_CASE("the first arrow key highlights the first lane", "[cli][dashboard][fleet][focus]")
{
    auto const first = FleetFocus { .zone = FocusZone::Lanes, .index = 0, .active = true };
    CHECK(StepFocus({}, FocusStep::Down, 3, 5) == first);
    CHECK(StepFocus({}, FocusStep::Up, 3, 5) == first);
}

TEST_CASE("down walks the lanes and then into the rows and stops at the last row", "[cli][dashboard][fleet][focus]")
{
    auto focus = FleetFocus { .zone = FocusZone::Lanes, .index = 2, .active = true };
    focus = StepFocus(focus, FocusStep::Down, 3, 2);
    CHECK(focus == FleetFocus { .zone = FocusZone::Rows, .index = 0, .active = true });
    focus = StepFocus(focus, FocusStep::Down, 3, 2);
    CHECK(focus == FleetFocus { .zone = FocusZone::Rows, .index = 1, .active = true });
    focus = StepFocus(focus, FocusStep::Down, 3, 2);
    CHECK(focus == FleetFocus { .zone = FocusZone::Rows, .index = 1, .active = true });

    // Inside the lanes, Down advances one lane at a time.
    CHECK(StepFocus({ .zone = FocusZone::Lanes, .index = 0, .active = true }, FocusStep::Down, 3, 2)
          == FleetFocus { .zone = FocusZone::Lanes, .index = 1, .active = true });
    // From the last lane with no rows below it there is nowhere to go.
    CHECK(StepFocus({ .zone = FocusZone::Lanes, .index = 2, .active = true }, FocusStep::Down, 3, 0)
          == FleetFocus { .zone = FocusZone::Lanes, .index = 2, .active = true });
}

TEST_CASE("up from the first row returns to the last lane", "[cli][dashboard][fleet][focus]")
{
    CHECK(StepFocus({ .zone = FocusZone::Rows, .index = 0, .active = true }, FocusStep::Up, 3, 4)
          == FleetFocus { .zone = FocusZone::Lanes, .index = 2, .active = true });
    // Up inside the rows walks back one row, and Up on the first lane stays there.
    CHECK(StepFocus({ .zone = FocusZone::Rows, .index = 3, .active = true }, FocusStep::Up, 3, 4)
          == FleetFocus { .zone = FocusZone::Rows, .index = 2, .active = true });
    CHECK(StepFocus({ .zone = FocusZone::Lanes, .index = 0, .active = true }, FocusStep::Up, 3, 4)
          == FleetFocus { .zone = FocusZone::Lanes, .index = 0, .active = true });
}

TEST_CASE("with no lanes the rows are the only zone", "[cli][dashboard][fleet][focus]")
{
    CHECK(StepFocus({}, FocusStep::Down, 0, 2) == FleetFocus { .zone = FocusZone::Rows, .index = 0, .active = true });
    CHECK(StepFocus({ .zone = FocusZone::Rows, .index = 0, .active = true }, FocusStep::Up, 0, 2)
          == FleetFocus { .zone = FocusZone::Rows, .index = 0, .active = true });
}

TEST_CASE("nothing to focus leaves the focus inactive", "[cli][dashboard][fleet][focus]")
{
    CHECK_FALSE(StepFocus({}, FocusStep::Down, 0, 0).active);
    CHECK_FALSE(StepFocus({}, FocusStep::Up, 0, 0).active);
}

TEST_CASE("a focus past the end is clamped when rows or lanes go away", "[cli][dashboard][fleet][focus]")
{
    CHECK(ClampFocus({ .zone = FocusZone::Rows, .index = 7, .active = true }, 3, 4)
          == FleetFocus { .zone = FocusZone::Rows, .index = 3, .active = true });
    CHECK(ClampFocus({ .zone = FocusZone::Rows, .index = 0, .active = true }, 3, 0)
          == FleetFocus { .zone = FocusZone::Lanes, .index = 2, .active = true });
    CHECK(ClampFocus({ .zone = FocusZone::Lanes, .index = 5, .active = true }, 2, 0)
          == FleetFocus { .zone = FocusZone::Lanes, .index = 1, .active = true });
    CHECK_FALSE(ClampFocus({ .zone = FocusZone::Lanes, .index = 0, .active = true }, 0, 0).active);

    // The lanes went away under a lane focus: the rows take it.
    CHECK(ClampFocus({ .zone = FocusZone::Lanes, .index = 1, .active = true }, 0, 3)
          == FleetFocus { .zone = FocusZone::Rows, .index = 0, .active = true });
    // An inactive focus stays inactive: clamping highlights nothing.
    CHECK(ClampFocus({}, 3, 4) == FleetFocus {});
    // A focus inside the bounds is left alone.
    CHECK(ClampFocus({ .zone = FocusZone::Rows, .index = 2, .active = true }, 3, 4)
          == FleetFocus { .zone = FocusZone::Rows, .index = 2, .active = true });
}

TEST_CASE("every section says which column names its machine", "[cli][dashboard][fleet][focus]")
{
    CHECK(MachineColumnOf(FleetSection::Machines) == std::string_view { "endpoint" });
    CHECK(MachineColumnOf(FleetSection::Workers) == std::string_view { "endpoint" });
    CHECK(MachineColumnOf(FleetSection::Leases) == std::string_view { "endpoint" });
    CHECK(MachineColumnOf(FleetSection::Conditions) == std::string_view { "endpoint" });
    CHECK(MachineColumnOf(FleetSection::Tiers) == std::string_view { "endpoint" });
    CHECK(MachineColumnOf(FleetSection::Members) == std::string_view { "scheduler-endpoint" });
    CHECK_FALSE(MachineColumnOf(FleetSection::Revoked));
    CHECK_FALSE(MachineColumnOf(FleetSection::Kpi));
    CHECK_FALSE(MachineColumnOf(FleetSection::Series));
}

TEST_CASE("every machine column is one the leader's renderer writes", "[cli][dashboard][fleet][focus]")
{
    // The column is named here, the table is written there: a column renamed in the leader's
    // renderer must fail HERE rather than leave a section whose rows never resolve to a machine.
    auto const snapshot = Distributed::FleetSnapshot {};
    for (auto const section: Enumerators<FleetSection>())
    {
        auto const column = MachineColumnOf(section);
        if (!column.has_value())
            continue;
        auto const names = Distributed::FleetColumnNames(section, snapshot);
        INFO(std::string { Unwrap(column) });
        CHECK(std::ranges::find(names, std::string { Unwrap(column) }) != names.end());
    }
}
