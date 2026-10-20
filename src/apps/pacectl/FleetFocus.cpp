// SPDX-License-Identifier: Apache-2.0
#include "FleetFocus.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

namespace Pace::Cli
{

namespace
{
    /// Where @p focus sits in the one list the two zones make: the lanes, then the rows.
    /// @param focus An active focus inside its zone.
    /// @param lanes How many lanes there are.
    /// @return Its position in that list.
    [[nodiscard]] std::size_t PositionOf(FleetFocus const& focus, std::size_t lanes) noexcept
    {
        return focus.zone == FocusZone::Lanes ? focus.index : lanes + focus.index;
    }

    /// The active focus at @p position of the list the lanes and the rows make.
    /// @param position A position inside that list.
    /// @param lanes How many lanes there are.
    /// @return That focus.
    [[nodiscard]] FleetFocus FocusAt(std::size_t position, std::size_t lanes) noexcept
    {
        if (position < lanes)
            return FleetFocus { .zone = FocusZone::Lanes, .index = position, .active = true };
        return FleetFocus { .zone = FocusZone::Rows, .index = position - lanes, .active = true };
    }
} // namespace

std::optional<std::string_view> MachineColumnOf(Distributed::FleetSection section) noexcept
{
    return MachineColumnTable[static_cast<std::size_t>(section)].column;
}

FleetFocus StepFocus(FleetFocus focus, FocusStep step, std::size_t lanes, std::size_t rows) noexcept
{
    auto const total = lanes + rows;
    if (total == 0)
        return FleetFocus {};
    // The first key only lights the highlight, on the first entry there is, whichever key it was.
    if (!focus.active)
        return FocusAt(0, lanes);
    auto const position = PositionOf(ClampFocus(focus, lanes, rows), lanes);
    // No wrap at either end: the first lane and the last row are where the keys stop.
    auto const next =
        step == FocusStep::Down ? std::min(position + 1, total - 1) : std::max(position, std::size_t { 1 }) - 1;
    return FocusAt(next, lanes);
}

FleetFocus ClampFocus(FleetFocus focus, std::size_t lanes, std::size_t rows) noexcept
{
    if (!focus.active)
        return focus;
    if (lanes + rows == 0)
        return FleetFocus {};
    auto const ownCount = focus.zone == FocusZone::Lanes ? lanes : rows;
    if (ownCount > 0)
        return FleetFocus { .zone = focus.zone, .index = std::min(focus.index, ownCount - 1), .active = true };
    // The zone emptied under the focus: the nearest entry of the other zone takes it -- the last
    // lane, which sits just above the rows, or the first row, which sits just below the lanes.
    return focus.zone == FocusZone::Rows ? FocusAt(lanes - 1, lanes) : FocusAt(lanes, lanes);
}

} // namespace Pace::Cli
