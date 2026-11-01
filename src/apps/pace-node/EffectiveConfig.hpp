// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <Pace/Protocol/CompileCacheWire.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pace::Node
{

/// @file EffectiveConfig.hpp
/// The configuration a node runs by, as `node_status` and `pacectl node` show it: one row per setting a
/// configuration file can carry, rendered from the configuration IN FORCE (`INodeConfigSource`), with every
/// credential redacted before it leaves this file.
///
/// **Classification is mandatory with a named opt-out, never an opt-in list** -- the rule the per-binary
/// secret-file table follows (`platform-service-and-config.md`). Every `NodeOptions()` row with a `yamlKey`
/// is a row here or on `NodeUnshownSettings()` with its reason, and every row says how it is disclosed;
/// `EffectiveConfig_test` refuses one that is neither, and a `PathToSecretFile` row that is not the secret-file
/// table's.

/// How a setting's value may be shown to whoever asks this node for its status.
///
/// **PRIVATE: persisted and transmitted nowhere.** What travels is the result, a `WireSettingValue`.
enum class SettingDisclosure : std::uint8_t
{
    /// The value is shown as it is in force.
    Shown,

    /// The value is a PATH to a file holding a secret (`NodeSecretFileTable()`), and the path is shown: the path
    /// is not the secret and the file is -- the same rule that reports such a file's exposure whoever typed its
    /// path. The file is never opened here.
    PathToSecretFile,

    /// The value IS a credential: whether one is set is shown, its value never leaves this process. Rendered
    /// through `SettingRow::present`, so no code path here ever holds the value as text.
    Redacted,
};

/// One setting's rendering: each value as a flag would take it, one entry per value of a list setting, and
/// none for a setting that is not set.
using RenderSetting = std::vector<std::string> (*)(NodeConfig const& cfg);

/// Whether a credential is set, for a `Redacted` row: the only question asked of its value.
using SettingIsSet = bool (*)(NodeConfig const& cfg);

/// One setting a configuration file can carry, and how it is shown.
struct SettingRow
{
    std::string_view flag;        ///< The row's `NodeOptions()` spelling, e.g. `--slots`.
    SettingDisclosure disclosure; ///< How it is shown.
    RenderSetting render;         ///< Its value; null exactly for a `Redacted` row.
    SettingIsSet present;         ///< Whether it is set; set exactly for a `Redacted` row.
};

/// Every shown setting, in `NodeOptions()` order.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<SettingRow const> NodeSettingTable() noexcept;

/// A `NodeOptions()` row with a `yamlKey` that `NodeSettingTable()` deliberately does not show, and why.
struct UnshownSetting
{
    std::string_view flag; ///< The row's spelling.
    std::string_view why;  ///< Why it has no row of its own.
};

/// Every setting row deliberately not shown.
/// @return The list; stable for the life of the process.
[[nodiscard]] std::span<UnshownSetting const> NodeUnshownSettings() noexcept;

/// The configuration in force, as `NodeStatus` carries it.
///
/// A `Redacted` row travels `Redacted` when set and `Unset` when not, with NO text either way. A value longer
/// than `MaxNodeSettingTextBytes` travels `Omitted` with the reason, never cut -- a cut path names another file
/// -- and values past `MaxNodeSettings` fold into one `Omitted` row saying how many were left out.
/// @param cfg The configuration in force.
/// @return One row per value, in `NodeSettingTable()` order; never empty.
[[nodiscard]] std::vector<CompileCacheWire::NodeSettingFields> EffectiveConfigOf(NodeConfig const& cfg);

} // namespace Pace::Node
