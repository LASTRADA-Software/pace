// SPDX-License-Identifier: Apache-2.0
#include "McpActionTools.hpp"
#include "McpUpstream.hpp"
#include "McpUpstreamFakes.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/Mcp/McpServer.hpp>
#include <Pace/Server/AdminHttpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <ranges>
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

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// The id this node answers to in every case.
constexpr std::string_view SelfId = "node-self";

/// Where the answering node is, as the scripted upstream reports it.
constexpr std::string_view AnsweredAt = "10.0.0.2:6674";

/// The client name every call states.
constexpr std::string_view ClientName = "claude-code";

/// @param bytes A frame or payload.
/// @return It, as the upstream carries it.
[[nodiscard]] std::string Text(std::vector<std::byte> const& bytes)
{
    return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

/// A cluster view holding one state.
class FixedCluster final: public IAppliedClusterView
{
  public:
    /// @param ids The members.
    explicit FixedCluster(std::initializer_list<std::string_view> ids)
    {
        auto state = Cluster::ClusterState {};
        for (auto const id: ids)
        {
            auto member = Cluster::ClusterMember {};
            member.id = std::string { id };
            state.members.push_back(std::move(member));
        }
        _state = std::move(state);
    }

    /// @copydoc IAppliedClusterView::AppliedState
    [[nodiscard]] std::optional<Cluster::ClusterState> AppliedState() const override
    {
        return _state;
    }

  private:
    std::optional<Cluster::ClusterState> _state;
};

/// Everything a case calls the tools through.
struct Rig
{
    McpFakes::ScriptedUpstream upstream { McpFakes::UpstreamParticipant { .answeredBy = std::string { AnsweredAt },
                                                                          .term = std::uint64_t { 4 } },
                                          UpstreamFailure::Unreachable };
    FixedCluster cluster { std::string_view { SelfId }, std::string_view { "node-b" } };
    CapturingLogger logger;
    std::vector<Mcp::ToolRow> tools = ActionTools(upstream, cluster, std::string { SelfId }, logger);

    /// Run @p tool with @p arguments.
    /// @param tool The tool's name.
    /// @param arguments Its arguments, as JSON text.
    /// @return What it answered.
    [[nodiscard]] Mcp::ToolResult Call(std::string_view tool, std::string_view arguments = "{}") const
    {
        auto const* const row = core::findOrNull(tools, tool, &Mcp::ToolRow::name);
        REQUIRE(row != nullptr);
        auto const parsed = Unwrap(Json::Parse(arguments));
        return row->handler(Mcp::ToolCall { .arguments = parsed, .clientName = ClientName });
    }

    /// @return The Info lines recorded, in order.
    [[nodiscard]] std::vector<std::string> InfoLines() const
    {
        auto lines = std::vector<std::string> {};
        for (auto const& record: logger.Snapshot())
            if (record.level == LogLevel::Info)
                lines.push_back(record.message);
        return lines;
    }
};

/// @param result A tool result.
/// @param key A member of its structured content.
/// @return That member.
[[nodiscard]] Json::Value const& At(Mcp::ToolResult const& result, std::string_view key)
{
    auto const* const member = result.structured.Find(key);
    REQUIRE(member != nullptr);
    return *member;
}

/// A key a waiting machine asks under.
[[nodiscard]] Ed25519PublicKey JoinerKey()
{
    return Testing::TestKeyPair("joiner-n4").PublicKey();
}

/// @param decision What the leader's row for `n4` now reads.
/// @param state The window's state.
/// @param secondsLeft Seconds left on an armed deadline.
/// @return The report the leader answers an enroll-control verb with.
[[nodiscard]] std::vector<std::byte> ReportWith(Wire::EnrollmentDecision decision,
                                                Wire::WireEnrollmentState state = Wire::WireEnrollmentState::Manual,
                                                std::uint64_t secondsLeft = 0)
{
    auto entry = Wire::EnrollmentPendingEntry {};
    entry.nodeId = "n4";
    entry.peerId = "10.0.0.9";
    entry.decision = decision;
    entry.publicKey = JoinerKey();
    return Wire::EncodeEnrollmentReport(
        Wire::EnrollmentReport { .state = state, .autoApproveSecondsLeft = secondsLeft, .pending = { entry } });
}

} // namespace

TEST_CASE("mcp-action: enrollment_approve sends the approval naming the id and the key, to the leader",
          "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Approved));
    auto const key = FormatEd25519PublicKey(JoinerKey());

    auto const result = rig.Call("enrollment_approve", std::format(R"({{"node_id":"n4","public_key":"{}"}})", key));

    REQUIRE_FALSE(result.isError);
    REQUIRE(rig.upstream.calls.size() == 1);
    CHECK(rig.upstream.calls[0].target == UpstreamTarget::Leader);
    CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeEnrollApprove("n4", JoinerKey())));
    CHECK(*At(result, "node_id").AsString() == "n4");
    CHECK(*At(result, "decision").AsString() == "approved");
    CHECK(*At(result, "enrollment").AsString() == "manual");
    CHECK(At(result, "auto_approve_left_s").IsNull());
    CHECK(*At(result, "answered_by").AsString() == AnsweredAt);
}

TEST_CASE("mcp-action: enrollment_approve refuses a key that is not one, and asks nobody", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Approved));
    // The all-zero key: well-formed text, and a small-order key the one parser refuses by name.
    auto const zero = FormatEd25519PublicKey(Ed25519PublicKey {});

    auto const result = rig.Call("enrollment_approve", std::format(R"({{"node_id":"n4","public_key":"{}"}})", zero));

    CHECK(result.isError);
    CHECK(result.text.contains("`public_key` is not a key this fleet accepts"));
    CHECK(rig.upstream.calls.empty());
    CHECK(rig.InfoLines().empty());
}

TEST_CASE("mcp-action: enrollment_reject sends the rejection naming the id, to the leader", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Rejected));

    auto const result = rig.Call("enrollment_reject", R"({"node_id":"n4"})");

    REQUIRE_FALSE(result.isError);
    REQUIRE(rig.upstream.calls.size() == 1);
    CHECK(rig.upstream.calls[0].target == UpstreamTarget::Leader);
    CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Reject, "n4")));
    CHECK(*At(result, "decision").AsString() == "rejected");
}

TEST_CASE("mcp-action: enrollment_auto_approve arms for minutes, and ends with off", "[node][mcp][mcp-action]")
{
    Rig rig;

    SECTION("minutes arms the deadline, in seconds on the wire")
    {
        rig.upstream.AnswerOk(
            ReportWith(Wire::EnrollmentDecision::Pending, Wire::WireEnrollmentState::AutoApprove, std::uint64_t { 60 }));
        auto const result = rig.Call("enrollment_auto_approve", R"({"minutes":1})");
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].target == UpstreamTarget::Leader);
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeEnrollAutoApprove(std::chrono::seconds { 60 })));
        CHECK(*At(result, "enrollment").AsString() == "auto-approve");
        CHECK(At(result, "auto_approve_left_s").AsInt() == 60);
        CHECK(result.text.contains("a leader restart or a change of leader ends it"));
    }

    SECTION("off ends it")
    {
        rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Pending));
        auto const result = rig.Call("enrollment_auto_approve", R"({"off":true})");
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeEnrollControl(Wire::EnrollControlVerb::AutoApproveOff)));
        CHECK(*At(result, "enrollment").AsString() == "manual");
        CHECK(At(result, "auto_approve_left_s").IsNull());
    }

    SECTION("the ceiling itself is accepted")
    {
        rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Pending));
        auto const result =
            rig.Call("enrollment_auto_approve", std::format(R"({{"minutes":{}}})", McpAction::MaxAutoApproveMinutes));
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeEnrollAutoApprove(AutoApproveCeiling)));
    }
}

TEST_CASE("mcp-action: auto_approve rejects 0 minutes", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Pending));

    // Zero, past the ceiling, a negative, a past-the-ceiling count too large to be seconds, both and neither: each is
    // refused by its own sentence, and nothing is sent for any of them.
    struct Case
    {
        std::string_view arguments;
        std::string_view says;
    };
    auto const overCeiling = std::format(R"({{"minutes":{}}})", McpAction::MaxAutoApproveMinutes + 1);
    auto const overCeilingSays =
        std::format("an auto-approve window is at most {} minutes", McpAction::MaxAutoApproveMinutes);
    for (auto const& [arguments, says]: {
             Case { .arguments = R"({"minutes":0})", .says = "a window of no length admits nobody" },
             Case { .arguments = R"({"minutes":-5})", .says = "a window of no length admits nobody" },
             Case { .arguments = overCeiling, .says = overCeilingSays },
             Case { .arguments = R"({"minutes":9223372036854775807})", .says = "is at most" },
             Case { .arguments = R"({"minutes":18446744073709551615})", .says = "is at most" },
             Case { .arguments = R"({"off":false})", .says = "takes only `true`" },
             Case { .arguments = R"({"minutes":5,"off":true})", .says = "exactly one of the two" },
             Case { .arguments = R"({})", .says = "exactly one of the two" },
         })
    {
        CAPTURE(arguments);
        auto const result = rig.Call("enrollment_auto_approve", arguments);
        CHECK(result.isError);
        CHECK(result.text.contains(says));
    }
    CHECK(rig.upstream.calls.empty());
}

TEST_CASE("mcp-action: minutes takes a whole number, and a whole real is that number", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(ReportWith(Wire::EnrollmentDecision::Pending));

    // A fraction, a string and null are not whole numbers, and nothing is sent for any of them.
    for (auto const arguments: { std::string_view { R"({"minutes":5.5})" },
                                 std::string_view { R"({"minutes":"5"})" },
                                 std::string_view { R"({"minutes":null})" } })
    {
        CAPTURE(arguments);
        auto const result = rig.Call("enrollment_auto_approve", arguments);
        CHECK(result.isError);
        CHECK(result.text == "`minutes` must be a whole number");
    }
    CHECK(rig.upstream.calls.empty());

    // 5.0 is the envelope's `integer` too, and it arms exactly what 5 arms: five minutes, three hundred seconds.
    auto const result = rig.Call("enrollment_auto_approve", R"({"minutes":5.0})");
    REQUIRE_FALSE(result.isError);
    REQUIRE(rig.upstream.calls.size() == 1);
    CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeEnrollAutoApprove(std::chrono::minutes { 5 })));
    CHECK(rig.InfoLines()
          == std::vector<std::string> { std::format(
              R"(mcp action enrollment_auto_approve {{"minutes":5}} by {} answered by {})", ClientName, AnsweredAt) });
}

TEST_CASE("mcp-action: cordon and uncordon ask this node, in-process", "[node][mcp][mcp-action]")
{
    Rig rig;

    SECTION("cordon, with no node named")
    {
        rig.upstream.AnswerOk(
            Wire::EncodeCordonFields(Wire::CordonFields { .state = Wire::WireCordonState::Draining, .inFlight = 2 }));
        auto const result = rig.Call("cordon");
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].target == UpstreamTarget::Node);
        CHECK(rig.upstream.calls[0].nodeIdOrEmpty.empty());
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeCordonRequest(Wire::CordonAction::Cordon)));
        CHECK(*At(result, "node").AsString() == SelfId);
        CHECK(*At(result, "cordon").AsString() == "draining");
        CHECK(At(result, "compiles_in_flight").AsInt() == 2);
        CHECK(result.text.contains("a restart of this node lifts it"));
    }

    SECTION("uncordon, naming this node by its id")
    {
        rig.upstream.AnswerOk(
            Wire::EncodeCordonFields(Wire::CordonFields { .state = Wire::WireCordonState::Serving, .inFlight = 0 }));
        auto const result = rig.Call("uncordon", std::format(R"({{"node":"{}"}})", SelfId));
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].target == UpstreamTarget::Node);
        CHECK(rig.upstream.calls[0].nodeIdOrEmpty.empty());
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeCordonRequest(Wire::CordonAction::Lift)));
        CHECK(*At(result, "cordon").AsString() == "serving");
    }
}

TEST_CASE("mcp-action: a cordon of another member is refused here, by the gate that member enforces",
          "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(
        Wire::EncodeCordonFields(Wire::CordonFields { .state = Wire::WireCordonState::Drained, .inFlight = 0 }));

    auto const other = rig.Call("cordon", R"({"node":"node-b"})");
    CHECK(other.isError);
    CHECK(other.text.contains("node-b is another machine"));
    // Exact, never a prefix: `node-` names nobody, and is refused as such rather than read as this node.
    auto const prefix = rig.Call("cordon", R"({"node":"node-"})");
    CHECK(prefix.isError);
    CHECK(rig.upstream.calls.empty());
    CHECK(rig.InfoLines().empty());
}

TEST_CASE("mcp-action: explain_admission asks this node, with the machine as the subject", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(Wire::EncodeAdmissionExplanation(Wire::AdmissionExplanationFields {
        .verdict = Wire::WireMembership::Member,
        .decidedBy = Wire::WireMembershipRoute::ProvenIdentity | Wire::WireMembershipRoute::MachineTicket,
        .standing = Wire::WireMachineStanding::Learner,
        .subject = "node-b" }));

    SECTION("about a machine")
    {
        auto const result = rig.Call("explain_admission", R"({"machine":"node-b"})");
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].target == UpstreamTarget::Node);
        CHECK(rig.upstream.calls[0].nodeIdOrEmpty.empty());
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeExplainAdmissionRequest("node-b")));
        CHECK(*At(result, "machine").AsString() == "node-b");
        CHECK(*At(result, "verdict").AsString() == "admitted");
        CHECK(*At(result, "standing").AsString() == "learner");
        auto const& routes = *At(result, "decided_by").AsArray();
        REQUIRE(routes.size() == 2);
        CHECK(*routes[0].AsString() == "proven-key");
        CHECK(*routes[1].AsString() == "ticket");
    }

    SECTION("about the caller's own connection")
    {
        auto const result = rig.Call("explain_admission");
        REQUIRE_FALSE(result.isError);
        REQUIRE(rig.upstream.calls.size() == 1);
        CHECK(rig.upstream.calls[0].frame == Text(Wire::EncodeExplainAdmissionRequest("")));
        CHECK(At(result, "machine").IsNull());
    }

    // A read: it records no action.
    CHECK(rig.InfoLines().empty());
}

TEST_CASE("mcp-action: learner standing refusal message", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.answer = std::unexpected { std::pair {
        UpstreamFailure::StandingRequired,
        std::string { "10.0.0.1:6674 refused the request: rejected (operator-standing-required): a learner" } } };
    auto const key = FormatEd25519PublicKey(JoinerKey());

    for (auto const& [tool, arguments]: {
             std::pair { std::string_view { "enrollment_approve" },
                         std::format(R"({{"node_id":"n4","public_key":"{}"}})", key) },
             std::pair { std::string_view { "enrollment_reject" }, std::string { R"({"node_id":"n4"})" } },
             std::pair { std::string_view { "enrollment_auto_approve" }, std::string { R"({"minutes":5})" } },
         })
    {
        CAPTURE(tool);
        auto const result = rig.Call(tool, arguments);
        CHECK(result.isError);
        CHECK(result.text
              == "node-self lacks operator standing (it is not a voter); run it from a voter's machine, or promote "
                 "this node");
    }
    // Refused, so nothing happened, and nothing is recorded as having happened.
    CHECK(rig.InfoLines().empty());
}

TEST_CASE("mcp-action: a caller the authority could not identify is told to identify itself, never to find a voter",
          "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.answer = std::unexpected { std::pair {
        UpstreamFailure::NotIdentified,
        std::string { "10.0.0.1:6674 refused the request: rejected (identified-caller-required): say who you are" } } };

    for (auto const& [tool, arguments]: {
             std::pair { std::string_view { "enrollment_reject" }, std::string_view { R"({"node_id":"n4"})" } },
             std::pair { std::string_view { "enrollment_auto_approve" }, std::string_view { R"({"off":true})" } },
         })
    {
        CAPTURE(tool);
        auto const result = rig.Call(tool, arguments);
        CHECK(result.isError);
        CHECK(result.text
              == "node-self could not be identified as the caller of this control verb by the node that answers it: "
                 "no key it proved is one that node's roster records; run it on that node's own machine, or from a "
                 "member whose key the fleet has recorded");
        CHECK_FALSE(result.text.contains("voter"));
    }
    CHECK(rig.InfoLines().empty());
}

TEST_CASE("mcp-action: any other failure is reported as the upstream says it", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.answer =
        std::unexpected { std::pair { UpstreamFailure::Refused, std::string { "n4 is already approved" } } };

    auto const result = rig.Call("enrollment_reject", R"({"node_id":"n4"})");
    CHECK(result.isError);
    CHECK(result.text == "refused: n4 is already approved");
}

TEST_CASE("mcp-action: action logs the audit line", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(
        Wire::EncodeCordonFields(Wire::CordonFields { .state = Wire::WireCordonState::Drained, .inFlight = 0 }));

    auto const result = rig.Call("cordon");

    REQUIRE_FALSE(result.isError);
    CHECK(rig.InfoLines()
          == std::vector<std::string> {
              std::format(R"(mcp action cordon {{"node":"{}"}} by {} answered by {})", SelfId, ClientName, AnsweredAt) });
}

TEST_CASE("mcp-action: an empty machine is refused, never answered about the caller", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(Wire::EncodeAdmissionExplanation(Wire::AdmissionExplanationFields {}));

    // The wire spells "this caller's own connection" as an EMPTY subject, so "" sent on would answer a question the
    // model did not ask.
    auto const result = rig.Call("explain_admission", R"({"machine":""})");
    CHECK(result.isError);
    CHECK(result.text.contains("`machine` is empty, which names no machine"));
    CHECK(rig.upstream.calls.empty());
}

TEST_CASE("mcp-action: an argument the tool does not take is refused, never ignored", "[node][mcp][mcp-action]")
{
    Rig rig;
    rig.upstream.AnswerOk(Wire::EncodeAdmissionExplanation(Wire::AdmissionExplanationFields {}));

    // `node` handed to explain_admission would otherwise answer about the CALLER: a machine nobody named.
    auto const result = rig.Call("explain_admission", R"({"node":"node-b"})");
    CHECK(result.isError);
    CHECK(result.text.contains("takes no argument `node`"));
    CHECK(rig.upstream.calls.empty());
}

TEST_CASE("mcp-action: tools/list shows readOnlyHint false on every action", "[node][mcp][mcp-action]")
{
    Rig rig;
    auto const server =
        Mcp::McpServer { ActionTools(rig.upstream, rig.cluster, std::string { SelfId }, rig.logger), "0.0.0", rig.logger };
    auto request = AdminRequest {};
    request.method = AdminMethod::Post;
    request.path = "/mcp";
    request.peerHost = "127.0.0.1";
    request.body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
                   R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                   R"("io.modelcontextprotocol/clientCapabilities":{}}}})";
    request.headers[static_cast<std::size_t>(AdminHeader::McpProtocolVersion)] = "2026-07-28";
    request.headers[static_cast<std::size_t>(AdminHeader::McpMethod)] = "tools/list";

    auto const response = server.Handle(request);
    REQUIRE(response.status == "200 OK");
    auto const body = Unwrap(Json::Parse(response.body));
    auto const& tools = *body.Find("result")->Find("tools")->AsArray();

    struct Expected
    {
        std::string_view name;
        bool readOnly;
        bool destructive;
    };
    auto const expected = std::vector<Expected> {
        { .name = "enrollment_approve", .readOnly = false, .destructive = false },
        { .name = "enrollment_reject", .readOnly = false, .destructive = true },
        { .name = "enrollment_auto_approve", .readOnly = false, .destructive = false },
        { .name = "cordon", .readOnly = false, .destructive = false },
        { .name = "uncordon", .readOnly = false, .destructive = false },
        { .name = "explain_admission", .readOnly = true, .destructive = false },
    };
    REQUIRE(tools.size() == expected.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, tools.size()))
    {
        auto const& tool = tools[index];
        auto const& want = expected[index];
        CAPTURE(want.name);
        CHECK(*tool.Find("name")->AsString() == want.name);
        CHECK(tool.Find("annotations")->Find("readOnlyHint")->AsBool() == want.readOnly);
        CHECK(tool.Find("annotations")->Find("destructiveHint")->AsBool() == want.destructive);
    }
    // The auto-approve ceiling is the flag's, derived: the schema states it as the maximum.
    auto const& minutes = *tools[2].Find("inputSchema")->Find("properties")->Find("minutes");
    CHECK(minutes.Find("maximum")->AsInt() == McpAction::MaxAutoApproveMinutes);
}
