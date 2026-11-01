// SPDX-License-Identifier: Apache-2.0
#include "NodeNameResolver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

using namespace Pace;
using namespace Pace::Node;

namespace
{

/// @param ids Member ids.
/// @return A state recording exactly those members.
[[nodiscard]] Cluster::ClusterState StateOf(std::initializer_list<std::string_view> ids)
{
    auto state = Cluster::ClusterState {};
    for (auto const id: ids)
    {
        auto member = Cluster::ClusterMember {};
        member.id = std::string { id };
        state.members.push_back(std::move(member));
    }
    return state;
}

} // namespace

TEST_CASE("A name resolves to the member whose id it is, exactly", "[node-name]")
{
    auto const state = StateOf({ "alpha-1", "alpha-12", "beta" });
    CHECK(ResolveNodeName(state, "alpha-1") == std::expected<Consensus::NodeId, std::string> { "alpha-1" });
    CHECK(ResolveNodeName(state, "alpha-12") == std::expected<Consensus::NodeId, std::string> { "alpha-12" });
    CHECK(ResolveNodeName(state, "beta") == std::expected<Consensus::NodeId, std::string> { "beta" });
}

TEST_CASE("A prefix, a suffix or another case of an id names nobody, and the error lists the members", "[node-name]")
{
    auto const state = StateOf({ "alpha-1", "beta" });
    for (auto const name: { std::string_view { "alpha" }, std::string_view { "lpha-1" }, std::string_view { "ALPHA-1" } })
    {
        INFO(name);
        auto const resolved = ResolveNodeName(state, name);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().find("alpha-1") != std::string::npos);
        CHECK(resolved.error().find("beta") != std::string::npos);
        CHECK(resolved.error().find("never by prefix") != std::string::npos);
    }
}

TEST_CASE("An empty name, an empty fleet and a long list each say what they are", "[node-name]")
{
    CHECK_FALSE(ResolveNodeName(StateOf({ "alpha" }), "").has_value());

    auto const nobody = ResolveNodeName(Cluster::ClusterState {}, "alpha");
    REQUIRE_FALSE(nobody.has_value());
    CHECK(nobody.error().find("records no members") != std::string::npos);

    // A fleet past the listing ceiling: the first ones by id, the rest counted, never silently dropped.
    auto many = Cluster::ClusterState {};
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxListedCandidates + 3))
    {
        auto member = Cluster::ClusterMember {};
        member.id = std::format("node-{:03}", index);
        many.members.push_back(std::move(member));
    }
    auto const unknown = ResolveNodeName(many, "elsewhere");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().find("and 3 more") != std::string::npos);

    // A name too long to quote is described by its length rather than echoed whole.
    auto const longName = std::string(MaxQuotedNameBytes + 1, 'x');
    auto const tooLong = ResolveNodeName(StateOf({ "alpha" }), longName);
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().find(longName) == std::string::npos);
    CHECK(tooLong.error().find(std::format("a name of {} bytes", longName.size())) != std::string::npos);
}
