// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Distributed/FleetView.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Pace::Cli
{

/// @file FleetFocus.hpp
/// The fleet view's keyboard focus as data: which zone is highlighted, which entry in it, and what
/// an arrow key does to that.
///
/// Pure: counts in, a focus out, so every rule below is a case over literals and the screen code
/// only renders what this decides. The two zones are walked as one list -- the machine lanes, then
/// the table rows that matched the filter -- with no wrap at either end.

/// Which part of the fleet view holds the highlight.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class FocusZone : std::uint8_t
{
    Lanes, ///< The per-machine chart lanes.
    Rows,  ///< The rows of the table the view shows, after filtering.
};

/// One arrow key's direction.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class FocusStep : std::uint8_t
{
    Up,   ///< Towards the first lane.
    Down, ///< Towards the last row.
};

/// Where the fleet view's highlight is.
///
/// Inactive until the first Up or Down, so a view nobody has navigated highlights nothing rather
/// than claiming a selection the operator never made.
struct FleetFocus
{
    FocusZone zone { FocusZone::Lanes }; ///< The zone holding the highlight.
    std::size_t index { 0 };             ///< The lane index, or the index into the MATCHED rows.
    bool active { false };               ///< False until the first Up/Down: nothing highlighted.

    /// Member-wise equality.
    /// @param other The focus to compare with.
    /// @return Whether every member is equal.
    [[nodiscard]] bool operator==(FleetFocus const& other) const = default;
};

/// Which column of a section's table names the machine one of its rows describes.
struct MachineColumnRow
{
    Distributed::FleetSection section;         ///< The section this row describes.
    std::optional<std::string_view> column {}; ///< That column, as the leader's renderer names it; none for none.
};

/// The machine column of every section, one row per enumerator, in enumerator order.
///
/// **Named here and written there**: a test requires each column to be one the leader's renderer
/// writes (`FleetColumnNames`), so a column renamed in `FleetView.cpp` fails that case rather than
/// leaving a section whose rows never resolve to a machine. `Members` names a machine by where its
/// `0xFC` port answers, the other machine-grained sections by `endpoint`; `Kpi` and `Series` are not
/// per-machine, and a revoked key's row describes a key that may no longer belong to any machine.
inline constexpr EnumTable<Distributed::FleetSection, MachineColumnRow> MachineColumnTable { {
    { .section = Distributed::FleetSection::Kpi, .column = std::nullopt },
    { .section = Distributed::FleetSection::Machines, .column = "endpoint" },
    { .section = Distributed::FleetSection::Workers, .column = "endpoint" },
    { .section = Distributed::FleetSection::Leases, .column = "endpoint" },
    { .section = Distributed::FleetSection::Members, .column = "scheduler-endpoint" },
    { .section = Distributed::FleetSection::Revoked, .column = std::nullopt },
    { .section = Distributed::FleetSection::Conditions, .column = "endpoint" },
    { .section = Distributed::FleetSection::Tiers, .column = "endpoint" },
    // Named by display name, not by where a machine answers: a row cannot resolve to a lane.
    { .section = Distributed::FleetSection::Clients, .column = std::nullopt },
    { .section = Distributed::FleetSection::Series, .column = std::nullopt },
} };

static_assert(RowsInEnumeratorOrder(MachineColumnTable, &MachineColumnRow::section),
              "MachineColumnTable must hold one row per FleetSection, in enumerator order");

/// The column of @p section naming the machine a row describes, or nullopt for none.
/// @param section The section.
/// @return Its machine column, or nullopt when its rows are not per machine.
[[nodiscard]] std::optional<std::string_view> MachineColumnOf(Distributed::FleetSection section) noexcept;

/// @p focus after @p step, given how many lanes and matched rows there are now.
///
/// An inactive focus becomes active on the first lane (the first row when there are no lanes) and
/// stays inactive when there is nothing at all. Down walks the lanes, then the rows, and stops on
/// the last row; Up walks back and stops on the first lane. Neither direction wraps.
/// @param focus The focus before the key.
/// @param step The key.
/// @param lanes How many lanes the view draws.
/// @param rows How many table rows matched the filter.
/// @return The focus after the key.
[[nodiscard]] FleetFocus StepFocus(FleetFocus focus, FocusStep step, std::size_t lanes, std::size_t rows) noexcept;

/// @p focus clamped to @p lanes / @p rows (the counts changed under it).
///
/// A focus past the end of its zone moves to that zone's last entry; a zone that emptied hands the
/// focus to the nearest entry of the other one -- the last lane for the rows, the first row for the
/// lanes -- and a view with neither drops the highlight. An inactive focus stays inactive.
/// @param focus The focus to clamp.
/// @param lanes How many lanes the view draws now.
/// @param rows How many table rows match the filter now.
/// @return The clamped focus.
[[nodiscard]] FleetFocus ClampFocus(FleetFocus focus, std::size_t lanes, std::size_t rows) noexcept;

} // namespace Pace::Cli
