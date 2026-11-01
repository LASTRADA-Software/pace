// SPDX-License-Identifier: Apache-2.0
#include "McpFleetTools.hpp"
#include "McpToolchainTools.hpp"
#include "McpUpstream.hpp"
#include "McpUpstreamFakes.hpp"

#include <Pace/Core/Json.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/FleetView.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;
namespace Dist = Pace::Distributed;

namespace
{

/// Where the leader answers, as the scripted upstream reports it.
constexpr std::string_view LeaderAt = "10.0.0.1:6674";

/// The term this node holds, as the scripted upstream reports it.
constexpr std::uint64_t TermHere = 4;

/// @return An upstream answering as the leader under `TermHere`, refusing `NoLeaderKnown` until scripted.
[[nodiscard]] McpFakes::ScriptedUpstream LeaderUpstream()
{
    return McpFakes::ScriptedUpstream { McpFakes::UpstreamParticipant { .answeredBy = std::string { LeaderAt },
                                                                        .term = TermHere },
                                        UpstreamFailure::NoLeaderKnown };
}


/// @param fingerprint The toolchain.
/// @param endpoint The machine.
/// @param label What the node called the toolchain; empty for none.
/// @return One registry entry.
[[nodiscard]] Dist::WorkerReport Entry(std::string fingerprint, std::string endpoint, std::string label)
{
    auto report = Dist::WorkerReport {};
    report.info.id = fingerprint + "@" + endpoint;
    report.info.fingerprint = std::move(fingerprint);
    report.info.endpoint = std::move(endpoint);
    report.info.toolchainLabel = std::move(label);
    report.info.slots = 2;
    return report;
}

/// @param upstream What was asked.
/// @return The FLEET-TEXT request the first call carried; the case fails when it carried another verb.
[[nodiscard]] Wire::FleetTextRequest FleetTextAsked(McpFakes::ScriptedUpstream const& upstream)
{
    REQUIRE_FALSE(upstream.calls.empty());
    auto const& frame = upstream.calls.front().frame;
    auto const bytes = std::as_bytes(std::span { frame.data(), frame.size() });
    auto const header = Wire::DecodeRequestHeader(bytes);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).opRaw == static_cast<std::uint8_t>(Wire::Op::FleetText));
    auto request = Wire::DecodeFleetTextRequest(bytes.subspan(Wire::RequestHeaderSize));
    REQUIRE(request.has_value());
    return Unwrap(request);
}

/// @param workers The registry entries.
/// @return The leader's fleet document carrying them, as `/fleet.json` serves it.
[[nodiscard]] std::string DocumentWith(std::vector<Dist::WorkerReport> workers)
{
    auto snapshot = Dist::FleetSnapshot {};
    snapshot.role = Dist::SchedulerRole::Leader;
    snapshot.leases = std::vector<std::uint64_t>(Dist::LeaseOutcomeTable.size(), 0);
    snapshot.workers = std::move(workers);
    return Dist::RenderFleetJson(snapshot, Dist::FleetHistoryView {});
}

/// Run `toolchains` against @p upstream.
/// @param upstream What it asks.
/// @return What it answered.
[[nodiscard]] Mcp::ToolResult CallToolchains(IMcpUpstream& upstream)
{
    auto const tools = ToolchainTools(upstream);
    auto const found = std::ranges::find(tools, std::string_view { "toolchains" }, &Mcp::ToolRow::name);
    REQUIRE(found != tools.end());
    auto const args = Json::Parse("{}");
    REQUIRE(args.has_value());
    return found->handler(Mcp::ToolCall { .arguments = Unwrap(args), .clientName = "test" });
}

/// @param value An object.
/// @param key A member.
/// @return The member; the case fails when it is missing.
[[nodiscard]] Json::Value const& At(Json::Value const& value, std::string_view key)
{
    auto const* const member = value.Find(key);
    INFO("key " << key << " in " << Json::Serialize(value));
    REQUIRE(member != nullptr);
    return *member;
}

/// @param value An array.
/// @return Its elements; the case fails when it is not one.
[[nodiscard]] Json::Array const& Elements(Json::Value const& value)
{
    INFO(Json::Serialize(value));
    REQUIRE(value.AsArray() != nullptr);
    return *value.AsArray();
}

/// @param rows An array of objects.
/// @param key The member that identifies a row.
/// @param value What it must say.
/// @return The row; the case fails when none says it.
[[nodiscard]] Json::Value const& RowWhere(Json::Value const& rows, std::string_view key, std::string_view value)
{
    auto const& elements = Elements(rows);
    auto const found = std::ranges::find_if(elements, [&](Json::Value const& row) {
        auto const* const member = row.Find(key);
        return member != nullptr && member->AsString() != nullptr && *member->AsString() == value;
    });
    INFO("no row with " << key << " == " << value << " in " << Json::Serialize(rows));
    REQUIRE(found != elements.end());
    return *found;
}

/// @param value A JSON array of strings.
/// @return The strings.
[[nodiscard]] std::vector<std::string> StringsOf(Json::Value const& value)
{
    auto strings = std::vector<std::string> {};
    for (auto const& element: Elements(value))
    {
        REQUIRE(element.AsString() != nullptr);
        strings.push_back(*element.AsString());
    }
    return strings;
}

} // namespace

TEST_CASE("toolchains projects the leader's workers by fingerprint and counts machines, not registry rows",
          "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(DocumentWith({
        Entry("fp-12", "a:1", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc 13.2.0"),
        // One machine with two toolchains: two registry entries, ONE machine.
        Entry("fp-13", "c:1", "gcc 13.2.0"),
        Entry("fp-clang", "c:1", "clang 18.1.8"),
        // A node that stated no label: its toolchain joins no family.
        Entry("fp-pinned", "d:1", ""),
    }));

    auto const result = CallToolchains(upstream);
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    // One FLEET-TEXT, in the Json format, to the leader: the one document the fleet tools read.
    REQUIRE(upstream.calls.size() == 1);
    CHECK(upstream.calls.front().target == UpstreamTarget::Leader);
    CHECK(FleetTextAsked(upstream).format == Wire::FleetTextFormat::Json);
    auto const& out = result.structured;

    CHECK(*At(out, "answered_by").AsString() == LeaderAt);
    CHECK(At(out, "term").AsInt() == std::optional<std::int64_t> { TermHere });
    CHECK(*At(out, "term_scope").AsString() == "this-node");
    CHECK(At(out, "machine_count").AsInt() == std::optional<std::int64_t> { 4 });
    CHECK(At(out, "toolchain_count").AsInt() == std::optional<std::int64_t> { 4 });

    auto const& toolchains = At(out, "toolchains");
    CHECK(Elements(toolchains).size() == 4);
    auto const& gcc13 = RowWhere(toolchains, "fingerprint", "fp-13");
    CHECK(*At(gcc13, "label").AsString() == "gcc 13.2.0");
    CHECK(*At(gcc13, "family").AsString() == "gcc");
    CHECK(*At(gcc13, "version").AsString() == "13.2.0");
    CHECK(StringsOf(At(gcc13, "nodes")) == std::vector<std::string> { "b:1", "c:1" });

    auto const& pinned = RowWhere(toolchains, "fingerprint", "fp-pinned");
    CHECK(At(pinned, "label").IsNull());
    CHECK(At(pinned, "family").IsNull());
    CHECK(*At(pinned, "version").AsString() == "unknown");

    auto const& machine = RowWhere(At(out, "nodes"), "endpoint", "c:1");
    CHECK(StringsOf(At(machine, "served")) == std::vector<std::string> { "fp-13", "fp-clang" });
    // The fleet document carries no list of toolchains a machine identified but does not serve: absent, not empty.
    CHECK(At(machine, "unserved").IsNull());
    CHECK(At(out, "unserved_note").AsString() != nullptr);
    // No demand is recorded yet: absent, not zero.
    CHECK(At(out, "demand").IsNull());

    auto const& report = At(out, "upgrade_report");
    auto const& gcc = RowWhere(report, "family", "gcc");
    CHECK(*At(gcc, "recommended_fingerprint").AsString() == "fp-13");
    CHECK(StringsOf(At(gcc, "nodes_behind")) == std::vector<std::string> { "a:1" });
    CHECK(At(gcc, "ordering_unknown").AsBool() == std::optional<bool> { false });
    CHECK(At(gcc, "basis").AsString()->contains("no demand data; highest version served"));
    CHECK(Elements(report).size() == 2);
}

TEST_CASE("toolchains reports an unparseable family as ordering unknown, recommending nothing",
          "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(DocumentWith({
        Entry("fp-17", "a:1", "clang 17.0.6"),
        Entry("fp-x", "b:1", "clang"),
    }));
    auto const result = CallToolchains(upstream);
    REQUIRE_FALSE(result.isError);
    auto const& clang = RowWhere(At(result.structured, "upgrade_report"), "family", "clang");
    CHECK(At(clang, "ordering_unknown").AsBool() == std::optional<bool> { true });
    CHECK(At(clang, "recommended_fingerprint").IsNull());
    CHECK(Elements(At(clang, "nodes_behind")).empty());
    CHECK(*At(RowWhere(At(result.structured, "toolchains"), "fingerprint", "fp-x"), "version").AsString() == "unknown");
    CHECK(result.text.contains("ordering unknown"));
}

TEST_CASE("toolchains on an empty fleet says so rather than failing", "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(DocumentWith({}));
    auto const result = CallToolchains(upstream);
    REQUIRE_FALSE(result.isError);
    CHECK(Elements(At(result.structured, "toolchains")).empty());
    CHECK(Elements(At(result.structured, "upgrade_report")).empty());
    CHECK(At(result.structured, "machine_count").AsInt() == std::optional<std::int64_t> { 0 });
}

TEST_CASE("toolchains carries the upstream's failure as a tool error", "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    auto const result = CallToolchains(upstream);
    CHECK(result.isError);
    CHECK(result.structured.IsNull());
    CHECK(result.text.contains("no-leader-known"));
}

TEST_CASE("toolchains refuses a worker row it cannot read rather than skipping it", "[pace-node][mcp][toolchains]")
{
    auto document = Unwrap(Json::Parse(DocumentWith({ Entry("fp", "a:1", "gcc 13.2.0") }), McpFleet::FleetDocumentLimits));
    auto* const members = std::get_if<Json::Object>(&document.v);
    REQUIRE(members != nullptr);
    auto const workers = std::ranges::find(*members, std::string_view { "workers" }, &Json::Member::first);
    REQUIRE(workers != members->end());
    workers->second = Json::Value { .v = Json::Array { Json::Value {
                                        .v = Json::Object { { "toolchain", Json::Value { .v = std::int64_t { 7 } } } } } } };

    auto upstream = LeaderUpstream();
    upstream.AnswerOk(Json::Serialize(document));
    auto const result = CallToolchains(upstream);
    CHECK(result.isError);
    CHECK(result.text.contains("workers"));
}

TEST_CASE("toolchains is read-only and says its report is conservative and checkable", "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    auto const tools = ToolchainTools(upstream);
    REQUIRE(tools.size() == 1);
    CHECK(tools.front().effect == Mcp::ToolEffect::ReadOnly);
    CHECK(tools.front().description.contains("conservative"));
    CHECK(tools.front().description.contains("basis"));
}
