// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Distributed/WorkerRegistry.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file CompilerVersion.hpp
/// What a toolchain label says about the compiler's version, and the conservative upgrade report built on it.
///
/// **The label is display text, never an identity** (`CompileCacheWire::MaxToolchainLabelBytes`): the
/// fingerprint decides every match. This file reads the label for one question only -- which of a family's
/// toolchains is newer -- and refuses to answer it wherever the label cannot settle it. A wrong "upgrade to X"
/// sends an operator to install a compiler, so **an unknown ordering is reported as unknown**, never guessed.
///
/// **The family is the label's first token**: the compiler's normalized file name, as `Cc::ToolchainLabel`
/// writes it (`gcc`, `cl`, `g++-13`, `aarch64-none-elf-gcc`). The fleet document carries nothing else that
/// names a family -- no banner and no driver row travel -- so this is the field the family is read from, and
/// two spellings of one product (`g++-13` and `g++-14`, `gcc` and `g++`) are two families, never compared.
/// That errs toward saying less, which is the direction this report may err in.

namespace Pace::Distributed
{

/// A compiler version as a toolchain label states it.
struct CompilerVersion
{
    std::string family;               ///< The label's first token: the compiler's normalized file name.
    std::vector<std::uint32_t> parts; ///< The version, one number per dot-separated part, in order.
};

/// How many compiles a fleet's clients asked of one toolchain. Produced by the demand history (Part 3).
struct DemandRow
{
    std::string fingerprint;   ///< The toolchain, as a registry entry names it.
    std::uint64_t compiles {}; ///< Compiles asked for it over the window the producer states.
};

/// What the upgrade report says about one compiler family.
struct UpgradeAdvice
{
    std::string family; ///< The family, as `ToolchainFamilyOf` names it.
    /// The toolchain the family should converge on, or nothing when the report cannot say which: an unknown
    /// ordering, or one version served under several fingerprints that demand does not tell apart.
    std::optional<std::string> recommendedFingerprint;
    /// Why, in words a reader can check: the versions compared and, where there is any, the demand behind each.
    std::string basis;
    /// The machines (worker endpoints) serving the family that lack the recommended toolchain, sorted. Empty
    /// whenever nothing is recommended: nobody is behind a target the report could not name.
    std::vector<std::string> nodesBehind;
    /// True when the family's versions cannot be ordered unambiguously, so nothing is recommended.
    bool orderingUnknown {};
};

/// The driver names that name a POLICY rather than a product.
///
/// `cc` and `c++` are whichever compiler the host installed under that name -- Apple clang on macOS, GNU on most
/// Linux -- and `pace-cc` corrects the flavour from the banner for exactly these two
/// (`ClassifyCompilerFromBanner`). The label keeps the name as invoked and the fleet document carries no banner,
/// so a family of this name may hold two products whose version numbers mean nothing against each other.
inline constexpr std::array<std::string_view, 2> PolicyDriverNames { "cc", "c++" };

/// Parse a toolchain label of exactly the shape `Cc::ToolchainLabel` writes when the banner states a version:
/// `<family> <digits>(.<digits>)*`, one space, nothing before or after.
/// @param label A label, as a registry entry carries it.
/// @return The family and the version's parts, or nothing for any other text -- including the tool alone, which
///         is what `ToolchainLabel` writes when the banner states no version.
[[nodiscard]] std::optional<CompilerVersion> ParseToolchainLabel(std::string_view label);

/// The family a label belongs to, whether or not its version parses.
/// @param label A label.
/// @return Its first space-separated token, or nothing when the label is empty or starts with a space.
[[nodiscard]] std::optional<std::string> ToolchainFamilyOf(std::string_view label);

/// @param parts A version's parts.
/// @return The version as dotted text, e.g. `13.2.0`.
[[nodiscard]] std::string VersionText(std::span<std::uint32_t const> parts);

/// The conservative upgrade report: one row per labelled family, sorted by family.
///
/// Within a family, versions are ordered only when every label parses and every version has the same number of
/// parts, and the family's name does not name a policy (`PolicyDriverNames`); otherwise `orderingUnknown`. The
/// target version is the highest one the most demand uses (ties going to the higher version); with no demand
/// for the family it is the highest version served, and the basis says "no demand data". A target version
/// served under more than one fingerprint is recommended only when demand clearly favours one of them; otherwise
/// the basis names the competitors and points at `toolchain_diff`.
///
/// **A node is a machine**, counted by its worker endpoint: the registry keys on (fingerprint, endpoint), so a
/// machine serving two toolchains is two entries and is listed behind at most once per family. An entry with no
/// label belongs to no family and is behind in none.
/// @param workers Every live registry entry.
/// @param demand Compiles per fingerprint; empty when no demand is recorded.
/// @return The advice per family.
[[nodiscard]] std::vector<UpgradeAdvice> AdviseUpgrades(std::span<WorkerReport const> workers,
                                                        std::span<DemandRow const> demand);

} // namespace Pace::Distributed
