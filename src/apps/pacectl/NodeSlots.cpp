// SPDX-License-Identifier: Apache-2.0
#include "NodeSlots.hpp"

#include <Pace/Platform/HostLoad.hpp>

#include <algorithm>
#include <limits>

namespace Pace::Cli
{

namespace
{
    /// @p value narrowed to the `u32` the scheduler's arithmetic takes, saturating.
    [[nodiscard]] std::uint32_t Saturated(std::uint64_t value) noexcept
    {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
    }

    /// The CPU busy share between @p previous and @p now, and whether one was owed.
    struct CpuShare
    {
        std::optional<std::uint32_t> permille; ///< The share, when it could be taken.
        bool owed { false };                   ///< The platform reports CPU, so a share was expected.
    };

    [[nodiscard]] CpuShare CpuShareOf(StatsReading const* previous, HostLoadReading const& now) noexcept
    {
        if (!now.cpu.has_value())
            return CpuShare { .permille = std::nullopt, .owed = false };
        auto const* earlier =
            previous != nullptr && previous->snapshot.hostLoad.has_value() && previous->snapshot.hostLoad->cpu.has_value()
                ? &*previous->snapshot.hostLoad->cpu
                : nullptr;
        if (earlier == nullptr)
            return CpuShare { .permille = std::nullopt, .owed = true };
        return CpuShare { .permille = CpuBusyPermille(*earlier, *now.cpu), .owed = true };
    }
    /// The pressure level a host reading carries.
    /// @param byte `HostCapacity::memoryPressure`.
    /// @return The level; disengaged for a byte this build has no level for.
    [[nodiscard]] std::optional<Distributed::PressureLevel> PressureOf(std::size_t byte) noexcept
    {
        if (byte >= static_cast<std::size_t>(Distributed::PressureLevel::Last))
            return std::nullopt;
        return static_cast<Distributed::PressureLevel>(byte);
    }
} // namespace

std::optional<NodeSlots> NodeSlotsOf(StatsReading const* previous, StatsReading const& now) noexcept
{
    if (!now.snapshot.host.has_value())
        return std::nullopt;
    auto const& host = *now.snapshot.host;

    auto slots = NodeSlots { .inFlight = Saturated(host.busySlots),
                             .registered = Saturated(host.configuredSlots),
                             .ceilings = std::nullopt,
                             .cpuBusyPermille = std::nullopt,
                             .availableMemoryBytes = std::nullopt };

    // A cordon is KNOWN from one reading, whatever the CPU says, and it binds whatever the
    // resources say (#1303) -- so the two early returns below, which exist because a limit
    // cannot be named yet, do not apply to it. Stating nothing for a cordoned node would tell
    // an operator nothing holds it back while the scheduler gives it no work at all.
    auto const cordoned = host.cordoned != 0;
    // The pressure level, as the node's watcher read it: like the cordon, KNOWN from one reading, and it withdraws
    // every slot whatever the resources say. A byte this build has no level for reads as no statement.
    auto const pressure = PressureOf(host.memoryPressure);
    auto const pressed = pressure.has_value() && *pressure != Distributed::PressureLevel::Calm;
    auto const knownOnly = [&] {
        return Distributed::SlotCeilingsFor(Distributed::NodeCapacity { .logicalCores = Saturated(host.logicalCores) },
                                            slots.registered,
                                            Distributed::NodeLoad { .inFlight = slots.inFlight,
                                                                    .cpuBusyPermille = std::nullopt,
                                                                    .availableMemoryBytes = std::nullopt,
                                                                    .freeScratchBytes = host.diskFreeBytes,
                                                                    .cache = {},
                                                                    .cordoned = cordoned,
                                                                    .pressure = pressure });
    };

    // A reading carrying no load says nothing the ceilings are made of, which is not the same as
    // a platform that will not say: the scheduler has figures here and the panel has none.
    if (!now.snapshot.hostLoad.has_value())
    {
        if (cordoned || pressed)
            slots.ceilings = knownOnly();
        return slots;
    }
    auto const& load = *now.snapshot.hostLoad;

    auto const cpu = CpuShareOf(previous, load);
    slots.cpuBusyPermille = cpu.permille;
    slots.availableMemoryBytes = load.availableMemoryBytes;
    if (cpu.owed && !cpu.permille.has_value())
    {
        if (cordoned || pressed)
            slots.ceilings = knownOnly();
        return slots;
    }

    // Exactly the scheduler's inputs: the cores the CPU ceiling scales by, the slots the node
    // registered with, and the load a heartbeat would carry. Free scratch is the host's figure,
    // which the node reads on its scratch filesystem.
    slots.ceilings = Distributed::SlotCeilingsFor(Distributed::NodeCapacity { .logicalCores = Saturated(host.logicalCores) },
                                                  slots.registered,
                                                  Distributed::NodeLoad { .inFlight = slots.inFlight,
                                                                          .cpuBusyPermille = cpu.permille,
                                                                          .availableMemoryBytes = load.availableMemoryBytes,
                                                                          .freeScratchBytes = host.diskFreeBytes,
                                                                          .cache = {},
                                                                          .cordoned = cordoned,
                                                                          .pressure = pressure });
    return slots;
}

} // namespace Pace::Cli
