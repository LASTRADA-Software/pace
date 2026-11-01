// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Distributed/WorkerRegistry.hpp>

#include <cstdint>
#include <map>
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
/// **The family is NOT read from the label.** The label's first token is the name the compiler was invoked by, and
/// Apple clang installed as `gcc` is labelled `gcc 17.0.0`. The family is what the node read off the BANNER when it
/// registered (`Cc::ToolchainFamily`, carried as `WorkerInfo::toolchainFamily`): `gcc`, `g++`, `clang`,
/// `apple-clang`, `cl`. An entry that registered no family joins none and is advised on by nobody.

namespace Pace::Distributed
{

/// A compiler version as a toolchain label states it.
struct CompilerVersion
{
    /// The label's first token: the name the compiler was invoked by (`gcc`, `g++-13`, `cl`). Display only -- the
    /// upgrade report's family is the registration's banner-derived `toolchainFamily`, never this.
    std::string tool;
    std::vector<std::uint32_t> parts; ///< The version, one number per dot-separated part, in order.
};

/// How many compiles a fleet's clients asked of one toolchain. Produced by the demand history (Part 3).
struct DemandRow
{
    std::string fingerprint;   ///< The toolchain, as a registry entry names it.
    std::uint64_t compiles {}; ///< Compiles asked for it over the window the producer states.
};

/// What the upgrade report says about one compiler family.
///
/// Every machine list is of worker endpoints, sorted, and relative to `recommendedVersion`: a machine is placed by
/// the HIGHEST version of the family it serves. All three are empty whenever the ordering is unknown.
struct UpgradeAdvice
{
    std::string family; ///< The banner-derived family key, as the registration carried it.
    /// The version the family should converge on, or nothing when the ordering is unknown.
    std::optional<std::string> recommendedVersion;
    /// The toolchain that version should be, per DRIVER (`gcc`, `cc`, `g++` -- a label's tool with its version suffix
    /// dropped, `CompileCache::DriverWithoutVersion`): `gcc` and `cc` are one install under two names, each with its
    /// own fingerprint, so neither is the other's "other build". A driver is absent when the ordering is unknown or
    /// when it serves the version under several fingerprints that demand does not tell apart.
    std::map<std::string, std::string> recommendedFingerprints;
    /// Why, in words a reader can check: the versions compared and, where there is any, the demand behind each.
    std::string basis;
    /// Machines whose highest version of the family is LOWER than the recommended one: these should upgrade.
    std::vector<std::string> nodesBehind;
    /// Machines whose highest version of the family is HIGHER than the recommended one: ahead, never "behind".
    std::vector<std::string> nodesAhead;
    /// Machines serving the recommended version under another fingerprint for the SAME driver, never the recommended
    /// one: the same version built differently, which `toolchain_diff` explains. Empty for a driver with no
    /// recommended fingerprint.
    std::vector<std::string> nodesSameVersionOtherFingerprint;
    /// True when the family's versions cannot be ordered unambiguously, so nothing is recommended.
    bool orderingUnknown {};
};

/// Parse a toolchain label of exactly the shape `Cc::ToolchainLabel` writes when the banner states a version:
/// `<tool> <digits>(.<digits>)*`, one space, nothing before or after, and no part of more than one digit that
/// starts with `0` (`13.02` could be read two ways, so it is read neither).
/// @param label A label, as a registry entry carries it.
/// @return The tool and the version's parts, or nothing for any other text -- including the tool alone, which is
///         what `ToolchainLabel` writes when the banner states no version.
[[nodiscard]] std::optional<CompilerVersion> ParseToolchainLabel(std::string_view label);

/// @param parts A version's parts.
/// @return The version as dotted text, e.g. `13.2.0`.
[[nodiscard]] std::string VersionText(std::span<std::uint32_t const> parts);

/// The conservative upgrade report: one row per registered family, sorted by family.
///
/// Within a family, versions are ordered only when every toolchain carries a label that parses, no toolchain is
/// labelled with two versions, and every version has the same number of parts; otherwise `orderingUnknown`. The
/// target version is the highest one the most demand uses (ties going to the higher version); with no demand for
/// the family it is the highest labelled version served, and the basis says "no demand data". The FAMILY orders
/// versions and places machines, so `gcc-13` and `gcc-14` are compared and a `cc`-only machine is behind `gcc 14`;
/// only the fingerprint choice at the target is per DRIVER. A driver serving the target under more than one
/// fingerprint names one only when demand clearly favours it; otherwise the basis names the competitors and points
/// at `toolchain_diff`.
///
/// **A node is a machine**, counted by its worker endpoint: the registry keys on (fingerprint, endpoint), so a
/// machine serving two toolchains is two entries and appears at most once per family. An entry with no family
/// belongs to none.
/// @param workers Every live registry entry.
/// @param demand Compiles per fingerprint; empty when no demand is recorded.
/// @return The advice per family.
[[nodiscard]] std::vector<UpgradeAdvice> AdviseUpgrades(std::span<WorkerReport const> workers,
                                                        std::span<DemandRow const> demand);

} // namespace Pace::Distributed
