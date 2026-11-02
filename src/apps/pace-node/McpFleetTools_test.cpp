// SPDX-License-Identifier: Apache-2.0
#include "McpFleetTools.hpp"
#include "McpUpstream.hpp"
#include "McpUpstreamFakes.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Distributed/FleetChart.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/FleetKeyedHistory.hpp>
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
#include <tests/FleetHistoryFakes.hpp>
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

/// This node's id, as the tools are built with it.
constexpr std::string_view SelfId = "self-node";

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
    auto const tools = FleetReadTools(upstream, cluster, std::string { SelfId });
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
    // One helper names a machine for every tool (`AppendNaming`): a machine row by the `node-id` the LEADER joined it
    // to by the key it PROVED (`CollectFleet`), a requester by the member holding its key -- and null, said in the
    // text, where nothing places it. NEVER by an endpoint: a registration's is the machine's own claim.
    auto snapshot = ScriptedFleet();
    // 10.0.0.1 proved n1's key (as `CollectFleet` records it); n1's recorded 0xFC endpoint is elsewhere, as a one-node
    // fleet's always is -- a member records no loopback endpoint. 10.0.0.3 proved no member's key and CLAIMS the
    // endpoint n2 records: an endpoint join would name it n2.
    snapshot.nodes[0].memberId = "n1";
    // And the name the LEADER's state records for n1, which the row renders as `name` / `replicated`.
    snapshot.nodes[0].replicatedName = "office-pc";
    auto key = Ed25519PublicKey {};
    key.fill(std::byte { 0x42 });
    auto const rawKey = std::string(reinterpret_cast<char const*>(key.data()), key.size());
    UnwrapMutable(snapshot.compiles).rows[0].compile.ticket.caller = rawKey;
    auto const body = Dist::RenderFleetJson(snapshot, Dist::FleetHistoryView {});
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(body);

    auto state = Cluster::ClusterState {};
    auto office = Member("n1", "10.0.0.1:6680", "10.0.0.9:6674");
    office.displayName = "office-pc";
    office.publicKey = key;
    auto claimed = Member("n2", "10.0.0.2:6680", "10.0.0.3:6674");
    claimed.displayName = "build-box";
    state.members = { office, claimed };

    SECTION("fleet_overview: each machine row")
    {
        auto const result = Call(upstream, "fleet_overview", "{}", state);
        REQUIRE_FALSE(result.isError);
        auto const& nodes = Elements(At(result.structured, "nodes"));
        REQUIRE(nodes.size() == 3);
        CHECK(Text(At(nodes[0], "node_id")) == R"("n1")");
        CHECK(Text(At(nodes[0], "display_name")) == R"("office-pc")");
        // One `node_id` per row: the naming REPLACES the leader's projected cell rather than standing beside it.
        auto const* const firstRow = nodes[0].AsObject();
        REQUIRE(firstRow != nullptr);
        CHECK(std::ranges::count_if(*firstRow, [](auto const& member) { return member.first == "node_id"; }) == 1);
        // 10.0.0.2 proved no member's key: null, never guessed from the host.
        CHECK(At(nodes[1], "node_id").IsNull());
        // 10.0.0.3 claims n2's recorded endpoint and proved no key: NOT n2.
        CHECK(At(nodes[2], "node_id").IsNull());
        CHECK(At(nodes[2], "display_name").IsNull());
        CHECK(result.text.contains("2 machine(s) proved no identity key a member holds"));
    }
    SECTION("conditions: each machine")
    {
        auto const result = Call(upstream, "conditions", "{}", state);
        REQUIRE_FALSE(result.isError);
        auto const& machines = Elements(At(result.structured, "nodes"));
        REQUIRE(machines.size() == 2);
        CHECK(Text(At(machines[0], "node_id")) == R"("n1")");
        CHECK(Text(At(machines[0], "display_name")) == R"("office-pc")");
        // 10.0.0.3 again: the endpoint n2 records names nobody.
        CHECK(Text(At(machines[1], "endpoint")) == R"("10.0.0.3:6674")");
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
    SECTION("no applied state here: the row names itself, id and name from the leader's one document")
    {
        // The id is the LEADER's join, by the key the machine proved, and the display name is the row's own
        // replicated name: neither needs this node's state.
        auto const result = Call(upstream, "fleet_overview");
        REQUIRE_FALSE(result.isError);
        auto const& nodes = Elements(At(result.structured, "nodes"));
        REQUIRE(nodes.size() == 3);
        CHECK(Text(At(nodes[0], "node_id")) == R"("n1")");
        CHECK(Text(At(nodes[0], "display_name")) == R"("office-pc")");
        CHECK(At(nodes[1], "node_id").IsNull());
        CHECK(At(nodes[1], "display_name").IsNull());
        CHECK(At(nodes[2], "node_id").IsNull());
    }
    SECTION("this node LAGS the leader: a row's display_name is the row's own name, never this node's older one")
    {
        // This node's state still records n1 under an older name (or none); the leader's row says office-pc,
        // replicated. One row, one state: display_name agrees with name, in fleet_overview and in conditions.
        for (auto const& older: { std::string { "old-name" }, std::string {} })
        {
            INFO("this node's name for n1: '" << older << "'");
            auto lagging = state;
            lagging.members[0].displayName = older;
            auto const overview = Call(upstream, "fleet_overview", "{}", lagging);
            REQUIRE_FALSE(overview.isError);
            auto const& row = Elements(At(overview.structured, "nodes"))[0];
            CHECK(Text(At(row, "name")) == R"("office-pc")");
            CHECK(Text(At(row, "name_source")) == R"("replicated")");
            CHECK(Text(At(row, "display_name")) == R"("office-pc")");
            auto const conditions = Call(upstream, "conditions", "{}", lagging);
            REQUIRE_FALSE(conditions.isError);
            CHECK(Text(At(Elements(At(conditions.structured, "nodes"))[0], "display_name")) == R"("office-pc")");
        }
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
    for (auto const& row: FleetReadTools(upstream, noState, std::string { SelfId }))
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

namespace
{

/// Five one-minute buckets on the hour range for one machine: a gap, then four sampled, the third of which could not
/// read the CPU -- rendered exactly as the leader renders a machine's `series` table.
[[nodiscard]] std::string MachineSeriesText()
{
    auto const range = Dist::FleetRange::OneHour;
    auto buckets = std::vector<Dist::FleetBucket>(5);
    auto const cpu = std::array<std::uint64_t, 5> { 0, 300, 450, 0, 800 };
    for (auto const index: std::views::iota(std::size_t { 0 }, buckets.size()))
    {
        auto& bucket = buckets[index];
        bucket.startMillis = 1'790'000'000'000 + (static_cast<std::int64_t>(index) * 60'000);
        bucket.sampleMillis = bucket.startMillis;
        bucket.present = index != 0;
        bucket.coverage = index != 0 ? 1 : 0;
        bucket.values[static_cast<std::size_t>(Dist::FleetMetric::CpuBusyPermille)] = cpu[index];
        bucket.absent = Dist::FleetScopedSlots() | (index == 3 ? Dist::SlotBit(Dist::FleetMetric::CpuBusyPermille) : 0);
    }
    auto text = std::string {};
    Dist::AppendSeriesText(text, buckets, range);
    return text;
}

/// A state naming two members, `n1` called `office-pc` and `n2`.
[[nodiscard]] Cluster::ClusterState TwoMembers()
{
    auto state = Cluster::ClusterState {};
    auto office = Member("n1", "10.0.0.1:6680", "10.0.0.1:6674");
    office.displayName = "office-pc";
    state.members = { office, Member("n2", "10.0.0.2:6680", "10.0.0.2:6674") };
    return state;
}

} // namespace

TEST_CASE("history asks for one machine's series by its member id, named by id and display name, gaps kept",
          "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(MachineSeriesText());
    // Named by its DISPLAY name; the leader is asked by the member id it resolves to.
    auto const result = Call(upstream, "history", R"({"series":"cpu-busy","range":"1h","node":"office-pc"})", TwoMembers());
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    CheckEnvelope(result.structured);

    auto const asked = FleetTextAsked(upstream);
    CHECK(asked.section == "series");
    CHECK(asked.range == "1h");
    CHECK(asked.node == "n1");
    CHECK(asked.format == Wire::FleetTextFormat::Text);

    CHECK(Text(At(result.structured, "scope")) == R"("node")");
    CHECK(Text(At(result.structured, "node_id")) == R"("n1")");
    CHECK(Text(At(result.structured, "display_name")) == R"("office-pc")");
    CHECK(Text(At(result.structured, "unit")) == R"("permille")");
    CHECK(result.text.contains("n1"));
    CHECK(result.text.contains("office-pc"));

    auto const& buckets = Elements(At(result.structured, "buckets"));
    REQUIRE(buckets.size() == 5);
    // A gauge, NOT rate-converted: the bucket's own reading.
    CHECK(NumberOf(At(buckets[1], "value")) == 300.0);
    CHECK(NumberOf(At(buckets[2], "value")) == 450.0);
    CHECK(NumberOf(At(buckets[4], "value")) == 800.0);
    // The unsampled minute and the minute whose CPU could not be read are both gaps, never 0.
    CHECK(At(buckets[0], "value").IsNull());
    CHECK(At(buckets[3], "value").IsNull());
    // And the unreadable minute WAS sampled: a gap in this slot, not an unwatched window.
    CHECK(Text(At(buckets[3], "coverage")) == "1");
}

TEST_CASE("history's fleet answer names no machine", "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(MachineSeriesText());
    auto const result = Call(upstream, "history", R"({"series":"in-flight","range":"1h"})", TwoMembers());
    REQUIRE_FALSE(result.isError);
    CHECK(FleetTextAsked(upstream).node.empty());
    CHECK(Text(At(result.structured, "scope")) == R"("fleet")");
    CHECK(At(result.structured, "node_id").IsNull());
    CHECK(At(result.structured, "display_name").IsNull());
}

TEST_CASE("history refuses a machine-only series without node and a scheduler series with one, asking nothing",
          "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();

    // cpu-busy has no fleet figure: refused naming the argument, never answered with a fleet aggregate.
    auto const cpu = Call(upstream, "history", R"({"series":"cpu-busy","range":"1h"})", TwoMembers());
    CHECK(cpu.isError);
    CHECK(cpu.text.contains("`node`"));
    CHECK(cpu.text.contains("cpu-busy"));

    // A dispatch outcome is the scheduler's; no machine answers for it.
    auto const dispatched = Call(upstream, "history", R"({"series":"dispatched","range":"1h","node":"n1"})", TwoMembers());
    CHECK(dispatched.isError);
    CHECK(dispatched.text.contains("omit `node`"));

    CHECK(upstream.calls.empty());

    // A node-scoped series with a fleet total answers both ways: per machine here.
    upstream.AnswerOk(MachineSeriesText());
    auto const hitRate = Call(upstream, "history", R"({"series":"hit-rate","range":"1h","node":"n2"})", TwoMembers());
    CHECK_FALSE(hitRate.isError);
    CHECK(FleetTextAsked(upstream).node == "n2");
}

TEST_CASE("history refuses a node it cannot name, listing who it could", "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();
    auto const result = Call(upstream, "history", R"({"series":"cpu-busy","range":"1h","node":"nobody"})", TwoMembers());
    CHECK(result.isError);
    CHECK(result.text.contains("nobody"));
    CHECK(result.text.contains("n1"));
    CHECK(result.text.contains("n2"));
    CHECK(upstream.calls.empty());

    // And on a node with no applied state, only its own id names anything -- asked for as that id.
    auto own = LeaderUpstream();
    own.AnswerOk(MachineSeriesText());
    auto const self = Call(own, "history", std::format(R"({{"series":"cpu-busy","range":"1h","node":"{}"}})", SelfId));
    CHECK_FALSE(self.isError);
    CHECK(FleetTextAsked(own).node == SelfId);
}

TEST_CASE("history passes the leader's refusal of a node selector through as a tool error", "[node][mcp][fleet-tools]")
{
    auto upstream = LeaderUpstream();
    upstream.answer = std::unexpected { std::pair {
        UpstreamFailure::Refused, std::string { "this leader holds no history for n2: none was handed over" } } };
    auto const result = Call(upstream, "history", R"({"series":"cpu-busy","range":"1h","node":"n2"})", TwoMembers());
    CHECK(result.isError);
    CHECK(result.text.contains("no history for n2"));
}

namespace
{
/// @param key An identity key.
/// @return It as the queue spells a machine: the raw bytes.
[[nodiscard]] std::string RawMachine(Ed25519PublicKey const& key)
{
    return std::string { reinterpret_cast<char const*>(key.data()), key.size() };
}

/// The scripted fleet, with member n1 holding a real key and a display name, and its leader's keyed histories:
/// n1 asked three compiles of one toolchain, another machine one of another, and a third machine one more -- so with
/// a ceiling of two, the least recently active is folded into `other`.
struct KeyedFleet
{
    KeyedFleet()
    {
        auto& member = UnwrapMutable(snapshot.cluster).members.at(0);
        member.publicKey = memberKey;
        member.displayName = "office-pc";
        demand.CompileGranted(RawMachine(Testing::TestKeyPair("early").PublicKey()), "clang-19-def");
        wall.Advance(std::chrono::seconds { 1 });
        for ([[maybe_unused]] auto const index: std::views::iota(0, 3))
            demand.CompileGranted(RawMachine(memberKey), "gcc-14-abc");
        wall.Advance(std::chrono::seconds { 1 });
        demand.CompileGranted(RawMachine(Testing::TestKeyPair("late").PublicKey()), "gcc-14-abc");
        auto view = Dist::FleetHistoryView { .range = Dist::FleetRange::Day, .buckets = {}, .durable = false, .keyed = {} };
        for (auto const& row: Dist::KeyedDimensionTable)
            view.keyed[static_cast<std::size_t>(row.dimension)] = demand.Of(row.dimension).View(Dist::FleetRange::Day);
        body = Dist::RenderFleetJson(snapshot, view);
        document = Unwrap(Json::Parse(body, McpFleet::FleetDocumentLimits));
    }

    Ed25519PublicKey const memberKey = Testing::TestKeyPair("n1").PublicKey();
    Dist::FleetSnapshot snapshot = ScriptedFleet();
    Testing::PlacedWallClock wall;
    Dist::CompileDemandHistory demand { 2, wall };
    std::string body;
    Json::Value document;
};
} // namespace

TEST_CASE("requesters tool output equals fleet.json requesters section", "[node][mcp][fleet-tools][fleet-keyed-history]")
{
    KeyedFleet const fleet;
    for (auto const& row: Dist::KeyedDimensionTable)
    {
        INFO("series " << row.series);
        auto upstream = LeaderUpstream();
        upstream.AnswerOk(fleet.body);
        auto const result = Call(upstream, "history", std::format(R"({{"series":"{}","range":"24h"}})", row.series));
        REQUIRE_FALSE(result.isError);
        CheckEnvelope(result.structured);
        // Asked of the leader for the ONE document /fleet.json serves, over the range named.
        auto const asked = FleetTextAsked(upstream);
        CHECK(asked.format == Wire::FleetTextFormat::Json);
        CHECK(asked.range == "24h");
        // The section, exactly as the document writes it: one derivation, two readers.
        CHECK(Text(At(result.structured, row.section)) == Text(At(fleet.document, row.section)));
        CHECK(At(result.structured, "key").IsNull());
    }

    // And what that section says: n1 named by member id and display name, a machine no member holds named by
    // nobody, the folded one counted in `other`, and the total exact.
    auto const& requesters = At(fleet.document, "requesters");
    CHECK(At(requesters, "total").AsInt() == std::optional<std::int64_t> { 5 });
    auto const& keys = Elements(At(requesters, "keys"));
    REQUIRE(keys.size() == 2);
    CHECK(Text(At(keys[0], "key")) == Text(Json::Value { .v = FormatEd25519PublicKey(fleet.memberKey) }));
    CHECK(Text(At(keys[0], "node_id")) == R"("n1")");
    CHECK(Text(At(keys[0], "display_name")) == R"("office-pc")");
    CHECK(At(keys[0], "total").AsInt() == std::optional<std::int64_t> { 3 });
    CHECK(Text(At(keys[0], "earlier_counts")) == R"("none")");
    CHECK(At(keys[1], "node_id").IsNull());
    CHECK(At(keys[1], "display_name").IsNull());
    auto const& other = At(requesters, "other");
    CHECK(At(other, "keys_folded").AsInt() == std::optional<std::int64_t> { 1 });
    CHECK(At(other, "total").AsInt() == std::optional<std::int64_t> { 1 });
    CHECK(Text(At(other, "noun")) == R"("machines")");
    CHECK_FALSE(At(requesters, "coverage_since_ms").IsNull());

    auto const& demand = At(fleet.document, "toolchain_demand");
    auto const& toolchains = Elements(At(demand, "keys"));
    REQUIRE_FALSE(toolchains.empty());
    CHECK(Text(At(toolchains[0], "fingerprint")) == R"("gcc-14-abc")");
    CHECK(At(toolchains[0], "total").AsInt() == std::optional<std::int64_t> { 4 });
    CHECK(toolchains[0].Find("node_id") == nullptr);
}

TEST_CASE("history's key selects one row of a keyed series, by key, node id or display name",
          "[node][mcp][fleet-tools][fleet-keyed-history]")
{
    KeyedFleet const fleet;
    auto const keyText = FormatEd25519PublicKey(fleet.memberKey);
    for (auto const& selector: { keyText, std::string { "n1" }, std::string { "office-pc" } })
    {
        INFO("key " << selector);
        auto upstream = LeaderUpstream();
        upstream.AnswerOk(fleet.body);
        auto const result =
            Call(upstream, "history", std::format(R"({{"series":"requesters","range":"24h","key":"{}"}})", selector));
        REQUIRE_FALSE(result.isError);
        auto const& keys = Elements(At(At(result.structured, "requesters"), "keys"));
        REQUIRE(keys.size() == 1);
        CHECK(Text(At(keys[0], "node_id")) == R"("n1")");
        // Everything but the rows is the document's: the total and `other` still say the whole.
        CHECK(Text(At(At(result.structured, "requesters"), "other")) == Text(At(At(fleet.document, "requesters"), "other")));
        CHECK(Text(At(result.structured, "key")) == Text(Json::Value { .v = selector }));
    }

    auto upstream = LeaderUpstream();
    upstream.AnswerOk(fleet.body);
    auto const none = Call(upstream, "history", R"({"series":"requesters","range":"24h","key":"nobody"})");
    REQUIRE_FALSE(none.isError);
    CHECK(Elements(At(At(none.structured, "requesters"), "keys")).empty());
    CHECK(none.text.contains("in other"));
}

TEST_CASE("history refuses node on a keyed series and key on a slot series, asking nothing",
          "[node][mcp][fleet-tools][fleet-keyed-history]")
{
    auto upstream = LeaderUpstream();
    auto const node = Call(upstream, "history", R"({"series":"requesters","range":"24h","node":"n1"})");
    CHECK(node.isError);
    CHECK(node.text.contains("`key`"));
    auto const key = Call(upstream, "history", R"({"series":"dispatched","range":"24h","key":"x"})");
    CHECK(key.isError);
    CHECK(key.text.contains("toolchain-demand"));
    auto const unknown = Call(upstream, "history", R"({"series":"made-up","range":"24h"})");
    CHECK(unknown.isError);
    for (auto const& row: Dist::KeyedDimensionTable)
        CHECK(unknown.text.contains(row.series));
    CHECK(upstream.calls.empty());
}

TEST_CASE("a leader that keeps no keyed history is answered null, never an empty history",
          "[node][mcp][fleet-tools][fleet-keyed-history]")
{
    auto const [body, document] = ScriptedDocument();
    CHECK(At(document, "requesters").IsNull());
    CHECK(At(document, "toolchain_demand").IsNull());
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(body);
    auto const result = Call(upstream, "history", R"({"series":"toolchain-demand","range":"7d"})");
    REQUIRE_FALSE(result.isError);
    CHECK(At(result.structured, "toolchain_demand").IsNull());
    CHECK(result.text.contains("keeps no such history"));
}
