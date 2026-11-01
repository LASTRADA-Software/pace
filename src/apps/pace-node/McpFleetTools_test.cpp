// SPDX-License-Identifier: Apache-2.0
#include "McpFleetTools.hpp"
#include "McpUpstream.hpp"
#include "McpUpstreamFakes.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Distributed/FleetChart.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/FleetView.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/NodeConditionWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::Unwrap;
using Pace::Testing::UnwrapMutable;

namespace Wire = Pace::CompileCacheWire;
namespace Dist = Pace::Distributed;

namespace
{

/// Where the leader answers, as the scripted upstream reports it.
constexpr std::string_view LeaderAt = "10.0.0.1:6674";

/// The term this node holds, as the scripted upstream reports it.
constexpr std::uint64_t TermHere = 7;

/// @return An upstream answering as the leader under `TermHere`, refusing `NoLeaderKnown` until scripted.
[[nodiscard]] McpFakes::ScriptedUpstream LeaderUpstream()
{
    return McpFakes::ScriptedUpstream { McpFakes::UpstreamParticipant { .answeredBy = std::string { LeaderAt },
                                                                        .term = TermHere },
                                        UpstreamFailure::NoLeaderKnown };
}

/// @param tools The table.
/// @param name A tool's name.
/// @return Its row.
[[nodiscard]] Mcp::ToolRow const& RowNamed(std::vector<Mcp::ToolRow> const& tools, std::string_view name)
{
    auto const found = std::ranges::find(tools, name, &Mcp::ToolRow::name);
    REQUIRE(found != tools.end());
    return *found;
}

/// Run one tool with @p arguments.
/// @param upstream What it asks.
/// @param name The tool.
/// @param arguments Its arguments, as JSON text.
/// @return What it answered.
[[nodiscard]] Mcp::ToolResult Call(IMcpUpstream& upstream,
                                   std::string_view name,
                                   std::string_view arguments = "{}",
                                   std::optional<Cluster::ClusterState> state = std::nullopt)
{
    McpFakes::FixedCluster const cluster { std::move(state) };
    auto const tools = FleetReadTools(upstream, cluster);
    auto const args = Json::Parse(arguments);
    REQUIRE(args.has_value());
    return RowNamed(tools, name).handler(Mcp::ToolCall { .arguments = Unwrap(args), .clientName = "test" });
}

/// @param value A JSON value.
/// @return Its text, for comparing two values by what they say.
[[nodiscard]] std::string Text(Json::Value const& value)
{
    return Json::Serialize(value);
}

/// @param value An object.
/// @param key A member.
/// @return The member; the case fails when it is missing.
[[nodiscard]] Json::Value const& At(Json::Value const& value, std::string_view key)
{
    auto const* const member = value.Find(key);
    INFO("key " << key << " in " << Text(value));
    REQUIRE(member != nullptr);
    return *member;
}

/// @param value An array.
/// @return Its elements; the case fails when it is not one.
[[nodiscard]] Json::Array const& Elements(Json::Value const& value)
{
    INFO(Text(value));
    REQUIRE(value.AsArray() != nullptr);
    return *value.AsArray();
}

/// The fleet document's milliseconds columns, which every tool reports in seconds as `<snake>_s`.
constexpr std::array MillisColumns { std::string_view { "age" },
                                     std::string_view { "lease-expires-in" },
                                     std::string_view { "heartbeat-age" } };

/// Check that @p projected says what @p document's row says, key for key: snake_case, and every milliseconds
/// column in seconds -- a `null` staying `null`, never `0`.
/// @param document A row of the fleet document.
/// @param projected The same row, as a tool reported it.
void CheckSameRow(Json::Value const& document, Json::Value const& projected)
{
    REQUIRE(document.AsObject() != nullptr);
    for (auto const& [key, value]: *document.AsObject())
    {
        auto snake = key;
        std::ranges::replace(snake, '-', '_');
        INFO("column " << key);
        if (std::ranges::contains(MillisColumns, key))
        {
            auto const& seconds = At(projected, snake + "_s");
            if (value.IsNull())
                CHECK(seconds.IsNull());
            else
            {
                auto const millis = value.AsInt();
                REQUIRE(millis.has_value());
                CHECK(Text(seconds) == Text(Json::Value { .v = static_cast<double>(Unwrap(millis)) / 1000.0 }));
            }
            continue;
        }
        CHECK(Text(At(projected, snake)) == Text(value));
    }
}

/// @param id A condition id.
/// @param state Its state word.
/// @return The condition, as a machine reports it.
[[nodiscard]] Wire::NodeConditionFields Condition(std::string id, std::string state)
{
    auto const raised = state == "raised";
    return Wire::NodeConditionFields { .id = std::move(id),
                                       .persistence = "live",
                                       .severity = "warning",
                                       .state = std::move(state),
                                       .detail = raised ? "scratch is 97% full" : "",
                                       .remedy = "free scratch space" };
}

/// @param endpoint Where it answers.
/// @param version Its build, or empty for one that did not say.
/// @param conditions What it reported, or nothing for a report that carried no list.
/// @return The machine.
[[nodiscard]] Dist::NodeReport Machine(std::string endpoint,
                                       std::string version,
                                       std::optional<std::vector<Wire::NodeConditionFields>> conditions)
{
    auto node = Dist::NodeReport {};
    node.endpoint = std::move(endpoint);
    node.version = std::move(version);
    node.heartbeatAge = std::chrono::milliseconds { 1250 };
    node.capacity.logicalCores = 16;
    node.conditions = std::move(conditions);
    return node;
}

/// @param ticket Its ticket.
/// @param requester Who asked.
/// @param state Where it stands.
/// @param age Since it was queued.
/// @param worker Who runs it, or nothing.
/// @param leaseExpiresIn What is left of its lease, or nothing.
/// @return One compile row.
[[nodiscard]] Dist::FleetCompile Compile(Dist::Ticket ticket,
                                         std::string requester,
                                         Dist::TicketView::State state,
                                         std::chrono::milliseconds age,
                                         std::optional<std::string> worker,
                                         std::optional<std::chrono::milliseconds> leaseExpiresIn)
{
    return Dist::FleetCompile {
        .compile = Dist::CompileRow { .ticket = Dist::TicketView { .ticket = ticket,
                                                                   .caller = "m",
                                                                   .sourcePath = std::format("src/tu-{}.cpp", ticket),
                                                                   .fingerprint = "gcc-14-abc",
                                                                   .treeLabel = "wt",
                                                                   .state = state,
                                                                   .age = age,
                                                                   .placement = std::nullopt },
                                      .toolchainLabel = "gcc 14.2.0",
                                      .worker = std::move(worker),
                                      .leaseExpiresIn = leaseExpiresIn },
        .requester = std::move(requester),
    };
}

/// @param id The member's id.
/// @param raft Where its consensus port answers.
/// @param scheduler Where its 0xFC port answers, or empty for one that announced none.
/// @return The member record.
[[nodiscard]] Cluster::ClusterMember Member(std::string id, std::string raft, std::string scheduler)
{
    auto member = Cluster::ClusterMember {};
    member.id = std::move(id);
    member.raftEndpoint = std::move(raft);
    member.schedulerEndpoint = std::move(scheduler);
    return member;
}

/// The scripted fleet every equivalence case reads: two running compiles and one queued, one raised condition,
/// one machine whose report carried no conditions, one that reported an empty list, and one that reported no
/// version.
[[nodiscard]] Dist::FleetSnapshot ScriptedFleet()
{
    auto snapshot = Dist::FleetSnapshot {};
    snapshot.role = Dist::SchedulerRole::Leader;
    snapshot.leaderEndpoint = std::string { LeaderAt };
    snapshot.cluster = Cluster::ClusterState {};
    snapshot.cluster->members = { Member("n1", "10.0.0.1:6680", "10.0.0.1:6674"), Member("n2", "10.0.0.2:6680", "") };
    snapshot.nodes = {
        Machine(
            "10.0.0.1:6674", "0.9.1", std::vector { Condition("scratch-low", "raised"), Condition("clock-skew", "clear") }),
        Machine("10.0.0.2:6674", "0.9.1", std::nullopt),
        Machine("10.0.0.3:6674", "", std::vector<Wire::NodeConditionFields> {}),
    };
    snapshot.leases = std::vector<std::uint64_t>(Dist::LeaseOutcomeTable.size(), 0);

    auto compiles = Dist::CompileSection {};
    compiles.rows.push_back(Compile(1,
                                    "ws-1",
                                    Dist::TicketView::State::Running,
                                    std::chrono::milliseconds { 9000 },
                                    "w-7",
                                    std::chrono::milliseconds { 1500 }));
    compiles.rows.push_back(Compile(2,
                                    "ws-2",
                                    Dist::TicketView::State::Running,
                                    std::chrono::milliseconds { 6000 },
                                    std::string { Dist::LocalCompileWorker },
                                    std::nullopt));
    compiles.rows.push_back(
        Compile(3, "ws-1", Dist::TicketView::State::Queued, std::chrono::milliseconds { 4200 }, std::nullopt, std::nullopt));
    compiles.totals[static_cast<std::size_t>(Dist::TicketView::State::Running)] = 2;
    compiles.totals[static_cast<std::size_t>(Dist::TicketView::State::Queued)] = 1;
    snapshot.compiles = std::move(compiles);
    return snapshot;
}

/// @return The scripted fleet's document, as `/fleet.json` serves it, and parsed.
[[nodiscard]] std::pair<std::string, Json::Value> ScriptedDocument()
{
    auto body = Dist::RenderFleetJson(ScriptedFleet(), Dist::FleetHistoryView {});
    auto parsed = Json::Parse(body, McpFleet::FleetDocumentLimits);
    REQUIRE(parsed.has_value());
    return { std::move(body), std::move(*parsed) };
}

/// @param upstream What was asked.
/// @return The FLEET-TEXT request the one call carried.
[[nodiscard]] Wire::FleetTextRequest FleetTextAsked(McpFakes::ScriptedUpstream const& upstream)
{
    REQUIRE(upstream.calls.size() == 1);
    auto const& frame = upstream.calls.front().frame;
    auto const bytes = std::as_bytes(std::span { frame.data(), frame.size() });
    auto const header = Wire::DecodeRequestHeader(bytes);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).opRaw == static_cast<std::uint8_t>(Wire::Op::FleetText));
    auto request = Wire::DecodeFleetTextRequest(bytes.subspan(Wire::RequestHeaderSize));
    REQUIRE(request.has_value());
    return Unwrap(request);
}

/// @param value A JSON number, or anything else.
/// @return The number, whichever way it was written, or nothing for a value that is not one.
[[nodiscard]] std::optional<double> NumberOf(Json::Value const& value)
{
    if (auto const integer = value.AsInt(); integer.has_value())
        return static_cast<double>(*integer);
    if (auto const* const real = std::get_if<double>(&value.v); real != nullptr)
        return *real;
    return std::nullopt;
}

/// @param bytes A payload.
/// @return The same bytes, as the upstream carries a payload.
[[nodiscard]] std::string AsText(std::vector<std::byte> const& bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(
        bytes, text.begin(), [](std::byte const byte) { return static_cast<char>(std::to_integer<unsigned char>(byte)); });
    return text;
}

/// Check the envelope every fleet result opens with.
void CheckEnvelope(Json::Value const& structured)
{
    CHECK(Text(At(structured, "answered_by")) == Text(Json::Value { .v = std::string { LeaderAt } }));
    CHECK(At(structured, "term").AsInt() == std::optional<std::int64_t> { TermHere });
    CHECK(Text(At(structured, "term_scope")) == R"("this-node")");
}

} // namespace

TEST_CASE("Every fleet tool reads the leader's fleet document and reports what it says, absent kept absent",
          "[node][mcp][fleet-tools]")
{
    // The equivalence the design demands: each tool's structured values are the values `RenderFleetJson` wrote
    // for the same snapshot -- the bytes `/fleet.json` serves -- re-keyed in snake_case with every millisecond
    // column in seconds, and a `null` never turned into a zero.
    auto const [body, document] = ScriptedDocument();
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(body);

    SECTION("fleet_overview")
    {
        auto const result = Call(upstream, "fleet_overview");
        REQUIRE_FALSE(result.isError);
        auto const& structured = result.structured;
        CheckEnvelope(structured);

        // Asked of the LEADER, for the json document with no section.
        REQUIRE(upstream.calls.size() == 1);
        CHECK(upstream.calls.front().target == UpstreamTarget::Leader);
        auto const asked = FleetTextAsked(upstream);
        CHECK(asked.format == Wire::FleetTextFormat::Json);
        CHECK(asked.section.empty());

        CHECK(Text(At(structured, "leader")) == Text(At(document, "leader")));
        CHECK(Text(At(structured, "role")) == Text(At(document, "role")));
        auto const& nodes = Elements(At(structured, "nodes"));
        auto const& documentNodes = Elements(At(document, "nodes"));
        REQUIRE(nodes.size() == documentNodes.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, nodes.size()))
            CheckSameRow(documentNodes[index], nodes[index]);
        auto const& members = Elements(At(structured, "members"));
        auto const& documentMembers = Elements(At(document, "members"));
        REQUIRE(members.size() == documentMembers.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, members.size()))
            CheckSameRow(documentMembers[index], members[index]);

        // The machine that reported no version is absent there, and makes the skew undecidable rather than false.
        CHECK(At(nodes[2], "version").IsNull());
        CHECK(Text(At(structured, "versions")) == R"(["0.9.1"])");
        CHECK(Text(At(structured, "versions_unreported")) == R"(["10.0.0.3:6674"])");
        CHECK(At(structured, "version_skew").IsNull());
        CHECK(result.text.contains("term 7 as this node sees it"));
    }

    SECTION("running_compiles")
    {
        auto const result = Call(upstream, "running_compiles");
        REQUIRE_FALSE(result.isError);
        auto const& structured = result.structured;
        CheckEnvelope(structured);
        CHECK(upstream.calls.front().target == UpstreamTarget::Leader);

        auto const& compiles = Elements(At(structured, "compiles"));
        auto const& documentCompiles = Elements(At(document, "compiles"));
        REQUIRE(compiles.size() == 3);
        REQUIRE(documentCompiles.size() == 3);
        for (auto const index: std::views::iota(std::size_t { 0 }, compiles.size()))
            CheckSameRow(documentCompiles[index], compiles[index]);
        // The queued row has no worker and no lease: absent, never `""` or `0`.
        CHECK(At(compiles[2], "worker").IsNull());
        CHECK(At(compiles[2], "lease_expires_in_s").IsNull());

        auto const& totals = At(structured, "totals");
        auto const& documentTotals = At(document, "compile-totals");
        for (auto const* const state: { "running", "queued", "waiting" })
            CHECK(Text(At(totals, state)) == Text(At(documentTotals, state)));
        CHECK(Text(At(totals, "oldest_wait_s")) == "4.2");
        CHECK(Text(At(totals, "per_worker")) == R"([{"worker":"local","count":1},{"worker":"w-7","count":1}])");
        CHECK(Text(At(totals, "per_requester"))
              == R"([{"requester":"ws-1","requester_key":null,"node_id":null,"display_name":null,"count":2},)"
                 R"({"requester":"ws-2","requester_key":null,"node_id":null,"display_name":null,"count":1}])");
        CHECK(At(totals, "per_counts_cover_every_ticket").AsBool() == std::optional { true });
        CHECK(Text(At(structured, "compiles_truncated")) == Text(At(document, "compiles-truncated")));
    }

    SECTION("running_compiles narrowed to one worker keeps the totals over every ticket")
    {
        auto const result = Call(upstream, "running_compiles", R"({"worker":"w-7"})");
        REQUIRE_FALSE(result.isError);
        auto const& compiles = Elements(At(result.structured, "compiles"));
        REQUIRE(compiles.size() == 1);
        CHECK(At(compiles[0], "ticket").AsInt() == std::optional<std::int64_t> { 1 });
        CHECK(At(At(result.structured, "totals"), "running").AsInt() == std::optional<std::int64_t> { 2 });
    }

    SECTION("conditions")
    {
        auto const result = Call(upstream, "conditions");
        REQUIRE_FALSE(result.isError);
        auto const& structured = result.structured;
        CheckEnvelope(structured);

        // The machine whose report carried no list is NOT REPORTED -- absent -- and not a machine with nothing raised.
        CHECK(Text(At(structured, "not_reported")) == R"(["10.0.0.2:6674"])");
        CHECK(At(structured, "raised_count").AsInt() == std::optional<std::int64_t> { 1 });

        auto const& machines = Elements(At(structured, "nodes"));
        REQUIRE(machines.size() == 2);
        CHECK(Text(At(machines[0], "endpoint")) == R"("10.0.0.1:6674")");
        // The machine that reported an empty list is a machine with none, not an absent one.
        CHECK(Text(At(machines[1], "endpoint")) == R"("10.0.0.3:6674")");
        CHECK(Elements(At(machines[1], "conditions")).empty());

        // Each condition is the document's row, less the endpoint it is filed under.
        auto const& conditions = Elements(At(machines[0], "conditions"));
        auto const& documentRows = Elements(At(document, "conditions"));
        REQUIRE(conditions.size() == 2);
        for (auto const index: std::views::iota(std::size_t { 0 }, conditions.size()))
            for (auto const& [key, value]: *documentRows[index].AsObject())
                if (key != "endpoint")
                    CHECK(Text(At(conditions[index], key)) == Text(value));
    }
}

TEST_CASE("Every fleet tool names each machine by member id and display name, and never guesses one",
          "[node][mcp][fleet-tools][display-name]")
{
    // One helper names a machine for every tool (`AppendNaming`): placed through the applied state -- a registration's
    // endpoint by the member recording it as its 0xFC endpoint, a requester by the member holding its key -- and null,
    // said in the text, where nothing places it.
    auto snapshot = ScriptedFleet();
    auto key = Ed25519PublicKey {};
    key.fill(std::byte { 0x42 });
    auto const rawKey = std::string(reinterpret_cast<char const*>(key.data()), key.size());
    UnwrapMutable(snapshot.compiles).rows[0].compile.ticket.caller = rawKey;
    auto const body = Dist::RenderFleetJson(snapshot, Dist::FleetHistoryView {});
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(body);

    auto state = Cluster::ClusterState {};
    auto office = Member("n1", "10.0.0.1:6680", "10.0.0.1:6674");
    office.displayName = "office-pc";
    office.publicKey = key;
    state.members = { office, Member("n2", "10.0.0.2:6680", "") };

    SECTION("fleet_overview: each machine row")
    {
        auto const result = Call(upstream, "fleet_overview", "{}", state);
        REQUIRE_FALSE(result.isError);
        auto const& nodes = Elements(At(result.structured, "nodes"));
        REQUIRE(nodes.size() == 3);
        CHECK(Text(At(nodes[0], "node_id")) == R"("n1")");
        CHECK(Text(At(nodes[0], "display_name")) == R"("office-pc")");
        // n2 records no 0xFC endpoint, and nothing records 10.0.0.3's: null, never guessed from the host.
        CHECK(At(nodes[1], "node_id").IsNull());
        CHECK(At(nodes[2], "display_name").IsNull());
        CHECK(result.text.contains("2 machine(s) match no member"));
    }
    SECTION("conditions: each machine")
    {
        auto const result = Call(upstream, "conditions", "{}", state);
        REQUIRE_FALSE(result.isError);
        auto const& machines = Elements(At(result.structured, "nodes"));
        REQUIRE(machines.size() == 2);
        CHECK(Text(At(machines[0], "node_id")) == R"("n1")");
        CHECK(Text(At(machines[0], "display_name")) == R"("office-pc")");
        CHECK(At(machines[1], "node_id").IsNull());
    }
    SECTION("running_compiles: each requester, by its key")
    {
        auto const result = Call(upstream, "running_compiles", "{}", state);
        REQUIRE_FALSE(result.isError);
        auto const& compiles = Elements(At(result.structured, "compiles"));
        REQUIRE(compiles.size() == 3);
        CHECK(Text(At(compiles[0], "requester_node_id")) == R"("n1")");
        CHECK(Text(At(compiles[0], "requester_display_name")) == R"("office-pc")");
        CHECK(At(compiles[1], "requester_node_id").IsNull());
        auto const& perRequester = Elements(At(At(result.structured, "totals"), "per_requester"));
        auto const* const named = core::findIfOrNull(
            perRequester, [](Json::Value const& row) { return row.Find("node_id")->AsString() != nullptr; });
        REQUIRE(named != nullptr);
        CHECK(Text(At(*named, "display_name")) == R"("office-pc")");
    }
    SECTION("no applied state names nobody")
    {
        auto const result = Call(upstream, "fleet_overview");
        REQUIRE_FALSE(result.isError);
        for (auto const& node: Elements(At(result.structured, "nodes")))
            CHECK(At(node, "node_id").IsNull());
    }
}

TEST_CASE("A leader that leads no compile queue is reported as none, never as an empty one", "[node][mcp][fleet-tools]")
{
    auto snapshot = ScriptedFleet();
    snapshot.compiles.reset();
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(Dist::RenderFleetJson(snapshot, Dist::FleetHistoryView {}));

    auto const result = Call(upstream, "running_compiles");
    REQUIRE_FALSE(result.isError);
    CHECK(At(result.structured, "compiles").IsNull());
    CHECK(At(result.structured, "totals").IsNull());
    CHECK(result.text.contains("leads no compile queue"));
}

TEST_CASE("A compile total the leader does not carry reads as absent in the text, never as zero", "[node][mcp][fleet-tools]")
{
    // A leader whose totals name no `waiting` (another build), and one whose `queued` is null: the text says `-`
    // for both, never `0 waiting`, and the structured totals carry exactly what the document carried.
    auto body = Dist::RenderFleetJson(ScriptedFleet(), Dist::FleetHistoryView {});
    auto const totals = std::string { R"("compile-totals":{"queued":1,"waiting":0,"running":2})" };
    auto const at = body.find(totals);
    REQUIRE(at != std::string::npos);
    body.replace(at, totals.size(), R"("compile-totals":{"queued":null,"running":2})");
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(body);

    auto const result = Call(upstream, "running_compiles");
    REQUIRE_FALSE(result.isError);
    CHECK(result.text.starts_with("2 running, - queued, - waiting;"));
    CHECK(At(At(result.structured, "totals"), "queued").IsNull());
    CHECK(At(result.structured, "totals").Find("waiting") == nullptr);
}

TEST_CASE("An unreachable leader is a tool error naming the endpoint, and nothing structured", "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();
    upstream.answer =
        std::unexpected { std::pair { UpstreamFailure::Unreachable,
                                      std::format("{} at 10.0.0.9:6674: connection refused",
                                                  UpstreamFailureRowOf(UpstreamFailure::Unreachable).sentence) } };

    McpFakes::FixedCluster const noState { std::nullopt };
    for (auto const& row: FleetReadTools(upstream, noState))
    {
        INFO(row.name);
        auto const arguments =
            row.name == "history" ? Json::Parse(R"({"series":"dispatched","range":"24h"})") : Json::Parse("{}");
        auto const result = row.handler(Mcp::ToolCall { .arguments = Unwrap(arguments), .clientName = "test" });
        CHECK(result.isError);
        CHECK(result.structured.IsNull());
        CHECK(result.text.contains("unreachable"));
        CHECK(result.text.contains("10.0.0.9:6674"));
    }
}

TEST_CASE("A fleet document of another schema is refused, not read", "[node][mcp][fleet-tools]")
{
    auto body = Dist::RenderFleetJson(ScriptedFleet(), Dist::FleetHistoryView {});
    auto const current = std::format(R"("schema":{},)", Dist::FleetJsonSchema);
    REQUIRE(body.starts_with("{" + current));
    body.replace(1, current.size(), std::format(R"("schema":{},)", Dist::FleetJsonSchema + 1));
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(body);

    auto const result = Call(upstream, "fleet_overview");
    CHECK(result.isError);
    CHECK(result.text.contains(std::format("schema {}", Dist::FleetJsonSchema + 1)));
}

TEST_CASE("An unknown history range or series is refused before anything is asked, listing the valid ones",
          "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();

    auto const range = Call(upstream, "history", R"({"series":"dispatched","range":"3h"})");
    CHECK(range.isError);
    for (auto const& row: Dist::FleetRangeTable)
        CHECK(range.text.contains(row.key));

    auto const series = Call(upstream, "history", R"({"series":"made-up","range":"24h"})");
    CHECK(series.isError);
    for (auto const& row: Dist::FleetSeriesTable)
        CHECK(series.text.contains(row.key));

    CHECK(upstream.calls.empty());
}

TEST_CASE("history reports the values /fleet/series.json carries for one series, gaps kept", "[node][mcp][fleet-tools]")
{
    // Five hourly buckets on the day range: a gap, three sampled, and one where the counter went backwards
    // (a restart) so its rate is unknowable. The tool reads FLEET-TEXT's `series` section; its values must
    // be the ones `RenderSeriesJson` -- `/fleet/series.json` -- carries for the same buckets.
    auto const range = Dist::FleetRange::Day;
    auto buckets = std::vector<Dist::FleetBucket>(5);
    auto const width =
        std::chrono::duration_cast<std::chrono::milliseconds>(Dist::FleetRangeTable[static_cast<std::size_t>(range)].bucket)
            .count();
    auto const granted = std::array<std::uint64_t, 5> { 0, 100, 160, 40, 70 };
    for (auto const index: std::views::iota(std::size_t { 0 }, buckets.size()))
    {
        auto& bucket = buckets[index];
        bucket.startMillis = 1'790'000'000'000 + (static_cast<std::int64_t>(index) * width);
        bucket.sampleMillis = bucket.startMillis + width;
        bucket.present = index != 0;
        bucket.coverage = index != 0 ? 3 : 0;
        bucket.values[static_cast<std::size_t>(Dist::FleetMetric::DispatchGranted)] = granted[index];
    }
    auto text = std::string {};
    Dist::AppendSeriesText(text, buckets, range);
    auto const seriesJson = Json::Parse(Dist::RenderSeriesJson(buckets, range));
    REQUIRE(seriesJson.has_value());
    auto const& expected = Elements(At(At(Unwrap(seriesJson), "series"), "dispatched"));

    auto upstream = LeaderUpstream();
    upstream.AnswerOk(text);
    auto const result = Call(upstream, "history", R"({"series":"dispatched","range":"24h"})");
    REQUIRE_FALSE(result.isError);
    CheckEnvelope(result.structured);

    // Asked of the leader, for the `series` section over the range named.
    CHECK(upstream.calls.front().target == UpstreamTarget::Leader);
    auto const asked = FleetTextAsked(upstream);
    CHECK(asked.format == Wire::FleetTextFormat::Text);
    CHECK(asked.section == "series");
    CHECK(asked.range == "24h");

    auto const& reported = Elements(At(result.structured, "buckets"));
    REQUIRE(reported.size() == expected.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, reported.size()))
    {
        INFO("bucket " << index);
        // Compared as NUMBERS: the document writes `12` where the tool writes `12.0`, one value.
        CHECK(NumberOf(At(reported[index], "value")) == NumberOf(expected[index]));
        CHECK(At(reported[index], "value").IsNull() == expected[index].IsNull());
        CHECK(Text(At(reported[index], "coverage")) == Text(At(Unwrap(seriesJson), "coverage").AsArray()->at(index)));
    }
    // The gap stays a gap, and a counter that went backwards is unknowable rather than negative.
    CHECK(At(reported[0], "value").IsNull());
    CHECK(At(reported[0], "coverage").IsNull());
    CHECK(At(reported[3], "value").IsNull());
    CHECK(Text(At(reported[0], "start")) == R"("2026-09-21T14:13:20Z")");
    CHECK(Text(At(result.structured, "unit")) == R"("per-minute")");
    CHECK(At(result.structured, "bucket_seconds").AsInt()
          == std::optional<std::int64_t> {
              std::chrono::duration_cast<std::chrono::seconds>(Dist::FleetRangeTable[static_cast<std::size_t>(range)].bucket)
                  .count() });
}

TEST_CASE("enrollment_status reports the leader's window as --enroll-list reads it", "[node][mcp][fleet-tools]")
{
    auto report = Wire::EnrollmentReport {
        .state = Wire::WireEnrollmentState::AutoApprove,
        .autoApproveSecondsLeft = 300,
        .pending = { Wire::EnrollmentPendingEntry {
            .nodeId = "lt-12",
            .nodeEndpoint = "10.0.0.12:6674",
            .peerId = "10.0.0.12",
            .firstSeenSecondsAgo = 90,
            .attempts = 4,
            .claimsChanged = 0,
            .decision = Wire::EnrollmentDecision::Pending,
            .role = Wire::EnrollRole::Learner,
            .publicKey = Testing::TestKeyPair("lt-12").PublicKey(),
        } },
    };
    auto const payload = Wire::EncodeEnrollmentReport(report);
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(AsText(payload));

    auto const result = Call(upstream, "enrollment_status");
    REQUIRE_FALSE(result.isError);
    CheckEnvelope(result.structured);
    CHECK(upstream.calls.front().target == UpstreamTarget::Leader);
    CHECK(Text(At(result.structured, "enrollment")) == R"("auto-approve")");
    CHECK(At(result.structured, "auto_approve_left_s").AsInt() == std::optional<std::int64_t> { 300 });
    auto const& pending = Elements(At(result.structured, "pending"));
    REQUIRE(pending.size() == 1);
    CHECK(Text(At(pending[0], "node_id")) == R"("lt-12")");
    CHECK(Text(At(pending[0], "decision")) == R"("pending")");
    CHECK(Text(At(pending[0], "role")) == R"("learner")");
    CHECK(At(pending[0], "roster_fingerprint").IsNull());
    CHECK(At(pending[0], "first_peer").IsNull());
    CHECK(At(pending[0], "first_seen_age_s").AsInt() == std::optional<std::int64_t> { 90 });

    SECTION("and a manual window has no deadline, absent rather than zero")
    {
        report.state = Wire::WireEnrollmentState::Manual;
        report.autoApproveSecondsLeft = 0;
        auto const manual = Wire::EncodeEnrollmentReport(report);
        upstream.AnswerOk(AsText(manual));
        auto const again = Call(upstream, "enrollment_status");
        REQUIRE_FALSE(again.isError);
        CHECK(Text(At(again.structured, "enrollment")) == R"("manual")");
        CHECK(At(again.structured, "auto_approve_left_s").IsNull());
    }
}
