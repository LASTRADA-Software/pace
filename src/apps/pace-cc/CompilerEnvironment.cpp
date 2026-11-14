// SPDX-License-Identifier: Apache-2.0
#include "CompilerEnvironment.hpp"

#include <array>
#include <format>

namespace Pace::Cc
{

namespace
{
    constexpr std::uint8_t MsvcDrivers = FlavorBit(Flavor::Cl) | FlavorBit(Flavor::ClangCl);
    constexpr std::uint8_t ClangDrivers = FlavorBit(Flavor::Clang) | FlavorBit(Flavor::ClangCl);
    constexpr std::uint8_t GnuDrivers = FlavorBit(Flavor::Gcc) | FlavorBit(Flavor::Clang);
    constexpr std::uint8_t EveryDriver = MsvcDrivers | GnuDrivers;

    // Over-inclusion is the safe direction for a `SearchesIncludes` row -- a driver that ignores
    // the variable costs a manifest miss when it changes, never a wrong hit -- so a row names
    // every driver that MAY read it. A `ChangesTheCompile` row is narrower on purpose: it costs
    // a whole compile's caching, so it names only the drivers documented to read it.
    constexpr std::array Rows {
        // cl prepends CL and appends _CL_ to its command line; clang-cl reads both the same way.
        CompilerEnvironmentRow { .name = "CL", .flavors = MsvcDrivers, .effect = EnvironmentEffect::ChangesTheCompile },
        CompilerEnvironmentRow { .name = "_CL_", .flavors = MsvcDrivers, .effect = EnvironmentEffect::ChangesTheCompile },
        // The clang driver's argument-editing hook (`+arg`, `s/x/y/`, `x-arg`, ...).
        CompilerEnvironmentRow {
            .name = "CCC_OVERRIDE_OPTIONS", .flavors = ClangDrivers, .effect = EnvironmentEffect::ChangesTheCompile },
        // Where a GNU driver looks for the programs it runs (cc1, as, collect2).
        CompilerEnvironmentRow {
            .name = "COMPILER_PATH", .flavors = GnuDrivers, .effect = EnvironmentEffect::ChangesTheCompile },
        CompilerEnvironmentRow {
            .name = "GCC_EXEC_PREFIX", .flavors = FlavorBit(Flavor::Gcc), .effect = EnvironmentEffect::ChangesTheCompile },
        // The MSVC include search path, and the external-header one beside it.
        CompilerEnvironmentRow { .name = "INCLUDE", .flavors = MsvcDrivers, .effect = EnvironmentEffect::SearchesIncludes },
        CompilerEnvironmentRow {
            .name = "EXTERNAL_INCLUDE", .flavors = MsvcDrivers, .effect = EnvironmentEffect::SearchesIncludes },
        // The GNU include search path variables, read by gcc and by clang in every mode.
        CompilerEnvironmentRow { .name = "CPATH", .flavors = EveryDriver, .effect = EnvironmentEffect::SearchesIncludes },
        CompilerEnvironmentRow {
            .name = "C_INCLUDE_PATH", .flavors = EveryDriver, .effect = EnvironmentEffect::SearchesIncludes },
        CompilerEnvironmentRow {
            .name = "CPLUS_INCLUDE_PATH", .flavors = EveryDriver, .effect = EnvironmentEffect::SearchesIncludes },
        CompilerEnvironmentRow {
            .name = "OBJC_INCLUDE_PATH", .flavors = EveryDriver, .effect = EnvironmentEffect::SearchesIncludes },
        // The SDK a clang driver on macOS takes its system headers from.
        CompilerEnvironmentRow { .name = "SDKROOT", .flavors = ClangDrivers, .effect = EnvironmentEffect::SearchesIncludes },
    };
} // namespace

std::span<CompilerEnvironmentRow const> CompilerEnvironmentTable() noexcept
{
    return Rows;
}

CompilerEnvironment ReadCompilerEnvironment(Flavor flavor, EnvironmentReader const& read)
{
    CompilerEnvironment result;
    auto const bit = FlavorBit(flavor);
    for (auto const& row: Rows)
    {
        if ((row.flavors & bit) == 0)
            continue;
        auto value = read(row.name);
        // Unset and empty are one answer: an empty CL adds nothing to the command line.
        if (!value.has_value() || value->empty())
            continue;
        switch (row.effect)
        {
            case EnvironmentEffect::ChangesTheCompile:
                if (!result.changesTheCompile.has_value())
                    result.changesTheCompile = row.name;
                break;
            case EnvironmentEffect::SearchesIncludes:
                result.includeSearch.push_back(std::format("{}={}", row.name, *value));
                break;
        }
    }
    return result;
}

} // namespace Pace::Cc
