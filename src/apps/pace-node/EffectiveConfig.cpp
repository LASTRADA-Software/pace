// SPDX-License-Identifier: Apache-2.0
#include "EffectiveConfig.hpp"

#include <Pace/Cli/Duration.hpp>
#include <Pace/Cluster/FleetPin.hpp>
#include <Pace/Core/Compression.hpp>
#include <Pace/Distributed/NodePolicy.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A text setting whose EMPTY value is a value: a listen spec emptied is a surface not served.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> Text(NodeConfig const& cfg)
    {
        return { std::string { cfg.*Member } };
    }

    /// A text setting whose empty value means not set: the node derives it, or does without.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> TextOrUnset(NodeConfig const& cfg)
    {
        auto const& text = cfg.*Member;
        if (text.empty())
            return {};
        return { std::string { text } };
    }

    /// The bytes a path holds, as text the wire carries -- never throwing, and never substituting.
    ///
    /// `path.string()` is the obvious spelling and the wrong one on Windows: it converts the UTF-16 the path holds
    /// to the narrow encoding and THROWS for a sequence that does not convert (a lone surrogate, which a name read
    /// through a wide API can carry), turning a status request into a 500. On POSIX the path IS its bytes. On
    /// Windows each code unit is written as WTF-8 -- UTF-8 for every valid sequence, and a lone surrogate as the
    /// three bytes UTF-8 would give it -- so a name that is not text travels as bytes that are not UTF-8, which
    /// is what the reader marks (`NodeStatusView::ReadableText`), exactly as a POSIX path's stray bytes do.
    /// @param path A path.
    /// @return Its bytes.
    [[nodiscard]] std::string PathBytes(std::filesystem::path const& path)
    {
#if defined(_WIN32)
        constexpr auto HighFirst = std::uint32_t { 0xD800 };
        constexpr auto HighLast = std::uint32_t { 0xDBFF };
        constexpr auto LowFirst = std::uint32_t { 0xDC00 };
        constexpr auto LowLast = std::uint32_t { 0xDFFF };
        constexpr auto Supplementary = std::uint32_t { 0x10000 };
        auto const& units = path.native();
        auto text = std::string {};
        auto const append = [&text](std::uint32_t codePoint) {
            auto const byte = [](std::uint32_t value) {
                return static_cast<char>(static_cast<unsigned char>(value));
            };
            if (codePoint < 0x80U)
                text += byte(codePoint);
            else if (codePoint < 0x800U)
            {
                text += byte(0xC0U | (codePoint >> 6U));
                text += byte(0x80U | (codePoint & 0x3FU));
            }
            else if (codePoint < Supplementary)
            {
                text += byte(0xE0U | (codePoint >> 12U));
                text += byte(0x80U | ((codePoint >> 6U) & 0x3FU));
                text += byte(0x80U | (codePoint & 0x3FU));
            }
            else
            {
                text += byte(0xF0U | (codePoint >> 18U));
                text += byte(0x80U | ((codePoint >> 12U) & 0x3FU));
                text += byte(0x80U | ((codePoint >> 6U) & 0x3FU));
                text += byte(0x80U | (codePoint & 0x3FU));
            }
        };
        // A `while`: a surrogate pair consumes two units and anything else one, which no `for` head states.
        auto index = std::size_t { 0 };
        while (index < units.size())
        {
            auto codePoint = static_cast<std::uint32_t>(units[index]);
            ++index;
            if (codePoint >= HighFirst && codePoint <= HighLast && index < units.size())
            {
                auto const low = static_cast<std::uint32_t>(units[index]);
                if (low >= LowFirst && low <= LowLast)
                {
                    codePoint = Supplementary + ((codePoint - HighFirst) << 10U) + (low - LowFirst);
                    ++index;
                }
            }
            append(codePoint);
        }
        return text;
#else
        return path.native();
#endif
    }

    /// A path setting; an empty path names no file.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> Path(NodeConfig const& cfg)
    {
        auto const& path = cfg.*Member;
        if (path.empty())
            return {};
        return { PathBytes(path) };
    }

    /// A repeatable setting: one value per entry, none for an empty list.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> List(NodeConfig const& cfg)
    {
        return cfg.*Member;
    }

    /// A flag whose meaning is its presence, spelled as the configuration file spells it.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> Flag(NodeConfig const& cfg)
    {
        return { cfg.*Member ? "true" : "false" };
    }

    /// A number that always has a value.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> Number(NodeConfig const& cfg)
    {
        return { std::format("{}", cfg.*Member) };
    }

    /// A number whose absence is an instruction -- usually *derive it* -- so absent is not set, never zero.
    template <auto Member>
    [[nodiscard]] std::vector<std::string> OptionalNumber(NodeConfig const& cfg)
    {
        auto const& value = cfg.*Member;
        if (!value.has_value())
            return {};
        return { std::format("{}", *value) };
    }

    /// @param cfg The configuration in force.
    /// @return `--node-class` as the flag spells it.
    [[nodiscard]] std::vector<std::string> NodeClassValue(NodeConfig const& cfg)
    {
        return { std::string { Distributed::TraitsFor(cfg.nodeClass).name } };
    }

    /// @param cfg The configuration in force.
    /// @return `--drain-timeout` as the flag spells it.
    [[nodiscard]] std::vector<std::string> DrainTimeoutValue(NodeConfig const& cfg)
    {
        return { FormatDuration(cfg.drainTimeout) };
    }

    /// @param cfg The configuration in force.
    /// @return `--no-toolchain-discovery`: whether discovery is off.
    [[nodiscard]] std::vector<std::string> NoDiscoveryValue(NodeConfig const& cfg)
    {
        return { cfg.toolchainDiscovery ? "false" : "true" };
    }

    /// @param cfg The configuration in force.
    /// @return `--fleet-id` as the flag spells it, or nothing on a node that trusts on first use.
    [[nodiscard]] std::vector<std::string> FleetPinValue(NodeConfig const& cfg)
    {
        if (!cfg.fleetPin.has_value())
            return {};
        return { Cluster::FormatPinnedFleet(*cfg.fleetPin) };
    }

    /// @param cfg The configuration in force.
    /// @return `--compression` by name: a codec is a byte-wide enum the parser would refuse as a number.
    [[nodiscard]] std::vector<std::string> CompressionValue(NodeConfig const& cfg)
    {
        return { std::string { Compression::NameOf(cfg.compression) } };
    }

    /// @param cfg The configuration in force.
    /// @return `--memory-compression` by name.
    [[nodiscard]] std::vector<std::string> MemoryCompressionValue(NodeConfig const& cfg)
    {
        return { std::string { Compression::NameOf(cfg.memoryCompression) } };
    }

    /// @param cfg The configuration in force.
    /// @return `--log-level` as the flag spells it.
    [[nodiscard]] std::vector<std::string> LogLevelValue(NodeConfig const& cfg)
    {
        return { std::string { NodeLogLevelName(cfg.logLevel) } };
    }

    /// @param cfg The configuration in force.
    /// @return Whether a `--requirepass` credential is set -- the only thing asked of it.
    [[nodiscard]] bool RequirePassIsSet(NodeConfig const& cfg)
    {
        return !cfg.requirePass.empty();
    }

    /// @param flag A shown row's flag.
    /// @param render How its value reads.
    /// @return The row.
    [[nodiscard]] constexpr SettingRow Shown(std::string_view flag, RenderSetting render) noexcept
    {
        return SettingRow { .flag = flag, .disclosure = SettingDisclosure::Shown, .render = render, .present = nullptr };
    }

    /// @param flag A row whose value is a path to a secret file.
    /// @param render How its path reads.
    /// @return The row.
    [[nodiscard]] constexpr SettingRow SecretFilePath(std::string_view flag, RenderSetting render) noexcept
    {
        return SettingRow {
            .flag = flag, .disclosure = SettingDisclosure::PathToSecretFile, .render = render, .present = nullptr
        };
    }

    /// @param flag A credential's row.
    /// @param present Whether it is set.
    /// @return The row: it has no renderer, so nothing here can turn its value into text.
    [[nodiscard]] constexpr SettingRow Credential(std::string_view flag, SettingIsSet present) noexcept
    {
        return SettingRow { .flag = flag, .disclosure = SettingDisclosure::Redacted, .render = nullptr, .present = present };
    }

    constexpr auto Settings = std::to_array<SettingRow>({
        Shown("--scheduler", &List<&NodeConfig::schedulers>),
        Shown("--advertise", &TextOrUnset<&NodeConfig::advertise>),
        Shown("--toolchain", &List<&NodeConfig::toolchains>),
        Shown("--allow-compile-arg", &List<&NodeConfig::extraAllowedArgs>),
        Shown("--no-toolchain-discovery", &NoDiscoveryValue),
        Shown("--slots", &OptionalNumber<&NodeConfig::slots>),
        Shown("--node-class", &NodeClassValue),
        Shown("--drain-timeout", &DrainTimeoutValue),
        Shown("--compile-request-max", &OptionalNumber<&NodeConfig::compileRequestMaxBytes>),
        Shown("--compile-inflight-max", &OptionalNumber<&NodeConfig::compileInflightMaxBytes>),
        Shown("--machine-slots", &OptionalNumber<&NodeConfig::machineSlots>),
        Shown("--machine-memory-budget", &OptionalNumber<&NodeConfig::machineMemoryBudgetBytes>),
        Shown("--compile-estimate-default", &OptionalNumber<&NodeConfig::compileEstimateDefaultBytes>),
        Shown("--reserve-cores", &OptionalNumber<&NodeConfig::reservedCores>),
        Shown("--node-id", &TextOrUnset<&NodeConfig::nodeId>),
        Shown("--listen-raft", &Text<&NodeConfig::raftListen>),
        Shown("--raft-self", &TextOrUnset<&NodeConfig::raftSelf>),
        // The directory the operator TYPED; the one resolved travels as `stateDirectory` beside its reason.
        SecretFilePath("--cluster-dir", &Path<&NodeConfig::clusterDir>),
        Shown("--discovery", &Text<&NodeConfig::discoveryAddress>),
        Shown("--discovery-reply-port", &Number<&NodeConfig::discoveryReplyPort>),
        Shown("--fleet-seed", &List<&NodeConfig::fleetSeeds>),
        Shown("--fleet-id", &FleetPinValue),
        Shown("--admin-listen", &Text<&NodeConfig::adminListen>),
        Shown("--mcp-listen", &Text<&NodeConfig::mcpListen>),
        Shown("--dashboard", &Flag<&NodeConfig::dashboard>),
        Shown("--history-requesters", &OptionalNumber<&NodeConfig::historyRequesters>),
        SecretFilePath("--dashboard-token-file", &Path<&NodeConfig::dashboardTokenFile>),
        Shown("--tls-self-signed", &Flag<&NodeConfig::tlsSelfSigned>),
        Shown("--tls-cert", &Path<&NodeConfig::tlsCertFile>),
        SecretFilePath("--tls-key", &Path<&NodeConfig::tlsKeyFile>),
        Shown("--fleet-open", &Flag<&NodeConfig::fleetOpen>),
        Shown("--cache-memory", &Number<&NodeConfig::cacheMemoryBytes>),
        Shown("--cache-disk", &Number<&NodeConfig::cacheDiskBytes>),
        Shown("--shared-cache-disk", &Number<&NodeConfig::sharedCacheDiskBytes>),
        Shown("--cache-dir", &Path<&NodeConfig::cacheDir>),
        Shown("--compression", &CompressionValue),
        Shown("--compression-level", &Number<&NodeConfig::compressionLevel>),
        Shown("--compression-min-bytes", &Number<&NodeConfig::compressionMinBytes>),
        Shown("--memory-compression", &MemoryCompressionValue),
        Shown("--memory-compression-level", &Number<&NodeConfig::memoryCompressionLevel>),
        Shown("--memory-compression-min-bytes", &Number<&NodeConfig::memoryCompressionMinBytes>),
        Shown("--listen-node", &Text<&NodeConfig::nodeListen>),
        Shown("--upstream", &TextOrUnset<&NodeConfig::upstream>),
        Credential("--requirepass", &RequirePassIsSet),
        Shown("--log-level", &LogLevelValue),
        Shown("--log-timestamps", &Flag<&NodeConfig::logTimestamps>),
        Shown("--pidfile", &TextOrUnset<&NodeConfig::pidfile>),
    });

    /// Whether a row's two columns agree with its disclosure: a credential has no renderer and asks only
    /// whether it is set, every other row renders and asks nothing else. Checked at compile time, so a row
    /// that could turn a credential into text is a build failure rather than a review comment.
    /// @param row A row.
    /// @return Whether it is consistent.
    [[nodiscard]] constexpr bool ConsistentRow(SettingRow const& row) noexcept
    {
        if (row.disclosure == SettingDisclosure::Redacted)
            return row.render == nullptr && row.present != nullptr;
        return row.render != nullptr && row.present == nullptr;
    }

    static_assert(std::ranges::all_of(Settings, ConsistentRow),
                  "a Redacted setting row renders nothing and only asks whether it is set; every other row renders");

    constexpr auto Unshown = std::to_array<UnshownSetting>({
        { .flag = "--no-log-timestamps",
          .why = "the negative spelling of --log-timestamps, whose row already shows the one field both set" },
    });

    /// @param flag The setting.
    /// @param text Its value.
    /// @return The row carrying it, or one saying why it cannot be carried whole.
    [[nodiscard]] Wire::NodeSettingFields ValueRow(std::string_view flag, std::string text)
    {
        if (text.size() > Wire::MaxNodeSettingTextBytes)
            return Wire::NodeSettingFields {
                .name = std::string { flag },
                .kind = Wire::WireSettingValue::Omitted,
                .text = std::format("a value of {} bytes, longer than the {} a status reply carries",
                                    text.size(),
                                    Wire::MaxNodeSettingTextBytes),
            };
        return Wire::NodeSettingFields { .name = std::string { flag },
                                         .kind = Wire::WireSettingValue::Value,
                                         .text = std::move(text) };
    }

    /// @param row A row.
    /// @param cfg The configuration in force.
    /// @return Its rows on the wire: one per value, or one saying it is not set or redacted.
    [[nodiscard]] std::vector<Wire::NodeSettingFields> RowsOf(SettingRow const& row, NodeConfig const& cfg)
    {
        auto const unset =
            Wire::NodeSettingFields { .name = std::string { row.flag }, .kind = Wire::WireSettingValue::Unset, .text = {} };
        if (row.disclosure == SettingDisclosure::Redacted)
        {
            if (!row.present(cfg))
                return { unset };
            return { Wire::NodeSettingFields {
                .name = std::string { row.flag }, .kind = Wire::WireSettingValue::Redacted, .text = {} } };
        }
        auto values = row.render(cfg);
        if (values.empty())
            return { unset };
        auto rows = std::vector<Wire::NodeSettingFields> {};
        rows.reserve(values.size());
        for (auto& value: values)
            rows.push_back(ValueRow(row.flag, std::move(value)));
        return rows;
    }
} // namespace

std::span<SettingRow const> NodeSettingTable() noexcept
{
    return Settings;
}

std::span<UnshownSetting const> NodeUnshownSettings() noexcept
{
    return Unshown;
}

std::vector<CompileCacheWire::NodeSettingFields> EffectiveConfigOf(NodeConfig const& cfg)
{
    auto rows = std::vector<Wire::NodeSettingFields> {};
    for (auto const& row: Settings)
        std::ranges::move(RowsOf(row, cfg), std::back_inserter(rows));

    // Folded, never dropped: one row naming where the cut fell and how much went past it.
    if (rows.size() > Wire::MaxNodeSettings)
    {
        auto const kept = Wire::MaxNodeSettings - 1;
        auto folded = Wire::NodeSettingFields {
            .name = rows[kept].name,
            .kind = Wire::WireSettingValue::Omitted,
            .text = std::format("{} more setting value(s), from this setting on, past the {} a status reply carries",
                                rows.size() - kept,
                                Wire::MaxNodeSettings),
        };
        rows.resize(kept);
        rows.push_back(std::move(folded));
    }
    return rows;
}

} // namespace Pace::Node
