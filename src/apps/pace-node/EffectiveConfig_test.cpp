// SPDX-License-Identifier: Apache-2.0
#include "EffectiveConfig.hpp"
#include "NodeConfig.hpp"

#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Pace;
using namespace Pace::Node;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// @param rows The wire rows.
/// @param flag A setting.
/// @return Every row of that setting.
[[nodiscard]] std::vector<Wire::NodeSettingFields> RowsNamed(std::vector<Wire::NodeSettingFields> const& rows,
                                                             std::string_view flag)
{
    auto out = std::vector<Wire::NodeSettingFields> {};
    std::ranges::copy_if(rows, std::back_inserter(out), [flag](auto const& row) { return row.name == flag; });
    return out;
}

} // namespace

TEST_CASE("Every setting a configuration file can carry is shown or named as unshown, exactly once", "[effective-config]")
{
    // Classification is MANDATORY: a row a file can carry that is in neither list is a setting `node_status`
    // silently leaves out, and that reads exactly like complete coverage.
    auto shown = std::multiset<std::string_view> {};
    for (auto const& row: NodeSettingTable())
        shown.insert(row.flag);
    auto unshown = std::multiset<std::string_view> {};
    for (auto const& row: NodeUnshownSettings())
        unshown.insert(row.flag);

    auto keyed = std::size_t { 0 };
    for (auto const& option: NodeOptions())
    {
        if (option.yamlKey.empty())
            continue;
        ++keyed;
        INFO(option.primary);
        CHECK(shown.count(option.primary) + unshown.count(option.primary) == 1);
    }
    // A scan that matched nothing would pass every row above: say it saw the table.
    CHECK(keyed > 40);

    // And nothing here names a flag the option table does not have.
    auto const known = [](std::string_view flag) {
        return std::ranges::any_of(NodeOptions(), [flag](auto const& option) { return option.primary == flag; });
    };
    for (auto const flag: shown)
    {
        INFO(flag);
        CHECK(known(flag));
    }
    for (auto const flag: unshown)
    {
        INFO(flag);
        CHECK(known(flag));
    }
}

TEST_CASE("The rows whose value is a path to a secret file are exactly the secret-file table's", "[effective-config]")
{
    auto secretFiles = std::set<std::string_view> {};
    for (auto const& row: NodeSecretFileTable())
        secretFiles.insert(row.flag);
    auto marked = std::set<std::string_view> {};
    for (auto const& row: NodeSettingTable())
        if (row.disclosure == SettingDisclosure::PathToSecretFile)
            marked.insert(row.flag);
    CHECK(marked == secretFiles);
    CHECK_FALSE(secretFiles.empty());
}

TEST_CASE("The redacted rows are exactly the option rows whose operand marks a credential", "[effective-config]")
{
    // The marker is the option table's: a row spelling `SecretOperand` IS a credential, and only such a row may be
    // -- and must be -- `Redacted`. So a `--foo-token=<secret>` added later and shown in clear fails here.
    auto secrets = std::set<std::string_view> {};
    for (auto const& option: NodeOptions())
        if (option.operand == SecretOperand && !option.yamlKey.empty())
            secrets.insert(option.primary);
    auto redacted = std::set<std::string_view> {};
    for (auto const& row: NodeSettingTable())
        if (row.disclosure == SettingDisclosure::Redacted)
            redacted.insert(row.flag);
    CHECK(redacted == secrets);
    // Absence of the negative is not the positive: the marker matched a row at all.
    CHECK(secrets.contains("--requirepass"));
}

TEST_CASE("A credential travels as set or unset, never as its value", "[effective-config]")
{
    constexpr std::string_view Secret = "correct-horse-battery-staple";
    auto cfg = NodeConfig {};
    cfg.requirePass = Secret;

    auto const rows = EffectiveConfigOf(cfg);
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(row.text.find(Secret) == std::string::npos);
    }
    CHECK(RowsNamed(rows, "--requirepass")
          == std::vector {
              Wire::NodeSettingFields { .name = "--requirepass", .kind = Wire::WireSettingValue::Redacted, .text = {} } });

    cfg.requirePass = std::string_view {};
    CHECK(RowsNamed(EffectiveConfigOf(cfg), "--requirepass")
          == std::vector {
              Wire::NodeSettingFields { .name = "--requirepass", .kind = Wire::WireSettingValue::Unset, .text = {} } });
}

TEST_CASE("Absent is unset, an emptied listen spec is a value, and a list setting travels a row per value",
          "[effective-config]")
{
    auto cfg = NodeConfig {};
    cfg.slots = std::nullopt;
    cfg.nodeListen = {};
    cfg.toolchains = { "gcc", "clang" };
    auto const rows = EffectiveConfigOf(cfg);

    CHECK(
        RowsNamed(rows, "--slots")
        == std::vector { Wire::NodeSettingFields { .name = "--slots", .kind = Wire::WireSettingValue::Unset, .text = {} } });
    CHECK(RowsNamed(rows, "--listen-node")
          == std::vector {
              Wire::NodeSettingFields { .name = "--listen-node", .kind = Wire::WireSettingValue::Value, .text = {} } });
    CHECK(RowsNamed(rows, "--toolchain")
          == std::vector {
              Wire::NodeSettingFields { .name = "--toolchain", .kind = Wire::WireSettingValue::Value, .text = "gcc" },
              Wire::NodeSettingFields { .name = "--toolchain", .kind = Wire::WireSettingValue::Value, .text = "clang" } });

    cfg.slots = 0;
    CHECK(RowsNamed(EffectiveConfigOf(cfg), "--slots")
          == std::vector {
              Wire::NodeSettingFields { .name = "--slots", .kind = Wire::WireSettingValue::Value, .text = "0" } });
}

TEST_CASE("A path that is not UTF-8 travels as its bytes, for the reader to mark", "[effective-config]")
{
    // The wire carries the bytes the node holds; `NodeStatusView::ReadableText` is where they are marked, never
    // replaced -- and replacing them here would name a different file.
    //
    // Built from the platform's own form: on Windows `std::filesystem::path`'s narrow constructor THROWS for bytes
    // that are not UTF-8 (`PathFromNarrowText` is the tree's one catch), so a path that is not text is one holding a
    // lone surrogate, and it travels as the WTF-8 bytes of that surrogate -- never a throw out of `path.string()`.
    auto cfg = NodeConfig {};
#if defined(_WIN32)
    cfg.cacheDir = std::filesystem::path { std::wstring { L"C:\\cache\\pace-\xD800" } };
    auto const expected = std::string { "C:\\cache\\pace-\xED\xA0\x80" };
#else
    cfg.cacheDir = std::filesystem::path { std::string { "/var/cache/pace-\xff" } };
    auto const expected = std::string { "/var/cache/pace-\xff" };
#endif
    auto const rows = RowsNamed(EffectiveConfigOf(cfg), "--cache-dir");
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().kind == Wire::WireSettingValue::Value);
    CHECK(rows.front().text == expected);
}

TEST_CASE("A value too long to carry, and values past the ceiling, are omitted by name and never cut", "[effective-config]")
{
    auto cfg = NodeConfig {};
    cfg.toolchains = { std::string(Wire::MaxNodeSettingTextBytes + 1, 'p') };
    auto const longRows = RowsNamed(EffectiveConfigOf(cfg), "--toolchain");
    REQUIRE(longRows.size() == 1);
    CHECK(longRows.front().kind == Wire::WireSettingValue::Omitted);
    CHECK(longRows.front().text.find("longer than") != std::string::npos);

    cfg.toolchains.assign(Wire::MaxNodeSettings, "gcc");
    auto const many = EffectiveConfigOf(cfg);
    CHECK(many.size() == Wire::MaxNodeSettings);
    CHECK(many.back().kind == Wire::WireSettingValue::Omitted);
    CHECK(many.back().text.find("more setting value(s)") != std::string::npos);
}
