// SPDX-License-Identifier: Apache-2.0
#include <Pace/Cluster/BudgetOverride.hpp>
#include <Pace/Cluster/ClusterState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Cluster;
using Pace::Testing::Unwrap;

namespace
{
constexpr std::uint64_t MiB = 1024ULL * 1024ULL;
constexpr std::uint64_t GiB = 1024ULL * MiB;
} // namespace

TEST_CASE("budget below the startup minimum refused with the flag's own message", "[cluster][budget]")
{
    // The flags take sizes through `ParseByteSize` and accept zero as "unbounded" or "no tier"; an override takes the
    // same sizes and refuses the one zero, naming the tier's own flag.
    CHECK(Unwrap(ParseOverrideBudget(StorageTier::Memory, "64m")) == 64 * MiB);
    CHECK(Unwrap(ParseOverrideBudget(StorageTier::Disk, "2g")) == 2 * GiB);
    CHECK(Unwrap(ParseOverrideBudget(StorageTier::Disk, "4096")) == 4096);

    auto const zero = ParseOverrideBudget(StorageTier::Memory, "0");
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error().context == TierOverrideCannotRemoveTier);
    CHECK(zero.error().field == "--cache-memory");

    auto const zeroDisk = ParseOverrideBudget(StorageTier::Disk, "0k");
    REQUIRE_FALSE(zeroDisk.has_value());
    CHECK(zeroDisk.error().context == TierOverrideCannotRemoveTier);
    CHECK(zeroDisk.error().field == "--cache-disk");

    // Not a size: the size parser's own words, under the flag's name.
    auto const garbage = ParseOverrideBudget(StorageTier::Disk, "lots");
    REQUIRE_FALSE(garbage.has_value());
    CHECK(garbage.error().field == "--cache-disk");
    CHECK(garbage.error().code == ConfigErrorCode::TypeMismatch);
}

TEST_CASE("A budget override is an absolute size: a share of RAM is refused for every tier", "[cluster][budget]")
{
    // `--cache-memory=25%` is a share of the machine that READS it; an override is read on the leader and applied on
    // its member, so a percentage is refused rather than resolved against the wrong machine's RAM.
    for (auto const tier: { StorageTier::Memory, StorageTier::Disk })
    {
        auto const share = ParseOverrideBudget(tier, "25%");
        REQUIRE_FALSE(share.has_value());
        CHECK(share.error().code == ConfigErrorCode::TypeMismatch);
        CHECK(share.error().context.find("absolute size") != std::string::npos);
        CHECK(share.error().field == BudgetTierTable[static_cast<std::size_t>(tier)].flag);
    }
}

TEST_CASE("A budget spec names a member and at least one tier, each once", "[cluster][budget]")
{
    auto const both = Unwrap(ParseBudgetSpec("a3f5:memory=64m,disk=2g"));
    CHECK(both.memberId == "a3f5");
    CHECK(both.bytes[static_cast<std::size_t>(StorageTier::Memory)] == std::optional { 64 * MiB });
    CHECK(both.bytes[static_cast<std::size_t>(StorageTier::Disk)] == std::optional { 2 * GiB });

    auto const one = Unwrap(ParseBudgetSpec("office:disk=1g"));
    CHECK_FALSE(one.bytes[static_cast<std::size_t>(StorageTier::Memory)].has_value());
    CHECK(one.bytes[static_cast<std::size_t>(StorageTier::Disk)] == std::optional { GiB });

    // Split at the LAST colon, so an id is opaque.
    CHECK(Unwrap(ParseBudgetSpec("host:a:memory=1m")).memberId == "host:a");

    CHECK_FALSE(ParseBudgetSpec("a3f5").has_value());
    CHECK_FALSE(ParseBudgetSpec(":memory=1m").has_value());
    CHECK_FALSE(ParseBudgetSpec("a3f5:").has_value());
    CHECK_FALSE(ParseBudgetSpec("a3f5:memory").has_value());
    CHECK(ParseBudgetSpec("a3f5:ram=1m").error().find("no cache tier") != std::string::npos);
    CHECK(ParseBudgetSpec("a3f5:memory=1m,memory=2m").error().find("named twice") != std::string::npos);
    CHECK(ParseBudgetSpec("a3f5:memory=0").error().find(std::string { TierOverrideCannotRemoveTier }) != std::string::npos);
    CHECK(ParseBudgetSpec("a3f5:memory=10%").error().find("absolute size") != std::string::npos);
    CHECK_FALSE(ParseBudgetSpec(std::string(65, 'x') + ":memory=1m").has_value());
}

TEST_CASE("A budget clear names a member and one tier", "[cluster][budget]")
{
    auto const clear = Unwrap(ParseBudgetClearSpec("a3f5:disk"));
    CHECK(clear.memberId == "a3f5");
    CHECK(clear.tier == StorageTier::Disk);
    CHECK(Unwrap(ParseBudgetClearSpec("a3f5:memory")).tier == StorageTier::Memory);
    CHECK_FALSE(ParseBudgetClearSpec("a3f5:memory=1m").has_value());
    CHECK_FALSE(ParseBudgetClearSpec("a3f5").has_value());
    CHECK_FALSE(ParseBudgetClearSpec("a3f5:both").has_value());
}

TEST_CASE("An override renders its figure, its baseline or that it has none, and the leader's time", "[cluster][budget]")
{
    // 2026-11-02T10:00:00Z.
    auto const unset = BudgetOverride { .bytes = 64 * MiB, .baseline = std::nullopt, .setAtUnixSeconds = 1793613600 };
    CHECK(FormatBudgetOverride(unset) == "64M baseline=unset set=2026-11-02T10:00:00Z");
    auto const measured = BudgetOverride { .bytes = 2 * GiB, .baseline = 4 * GiB, .setAtUnixSeconds = 1793613600 };
    CHECK(FormatBudgetOverride(measured) == "2G baseline=4G set=2026-11-02T10:00:00Z");
    // A present zero is a real configuration -- unbounded -- never rendered as "unset".
    auto const unbounded = BudgetOverride { .bytes = MiB, .baseline = 0, .setAtUnixSeconds = 1793613600 };
    CHECK(FormatBudgetOverride(unbounded) == "1M baseline=0B set=2026-11-02T10:00:00Z");
}

TEST_CASE("A budget override decides no credential destination: it is no replicated setting at all", "[cluster][budget]")
{
    // `RefusedSettingTable` refuses a replicated SETTING that would decide where a node presents a credential. A budget
    // is not a setting -- it is a member-record field, set by MEMBER-EDIT -- so no setting row may be spelled like one,
    // and the record carries nothing but byte counts and a time: no address, no peer, no secret.
    for (auto const& row: BudgetTierTable)
    {
        CHECK(FindSetting(row.flag.substr(2)) == nullptr);
        CHECK(FindRefusedSetting(row.flag.substr(2)) == nullptr);
    }
}
