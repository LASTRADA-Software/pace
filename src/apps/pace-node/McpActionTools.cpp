// SPDX-License-Identifier: Apache-2.0
#include "EnrollAutoApprove.hpp"
#include "EnrollClient.hpp"
#include "McpActionTools.hpp"
#include "McpToolSupport.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/PeerText.hpp>
#include <Pace/Distributed/MembershipWire.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/NodeStatusView.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <core/Ranges.hpp>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;
    using namespace McpSupport;

    /// What every action handler is built over.
    struct ActionContext
    {
        IMcpUpstream* upstream;             ///< How the leader and this node are asked.
        IAppliedClusterView const* cluster; ///< Where a `node` argument resolves.
        std::string selfId;                 ///< This node's id.
        ILogger* logger;                    ///< Where a successful action is recorded.
    };

    // ------------------------------------------------------------ arguments

    /// The first argument @p call carries that @p allowed does not name.
    ///
    /// **Refused, never ignored**: `additionalProperties: false` is a promise the envelope does not keep (it checks
    /// required keys and primitive types only), and an argument a tool skipped is the model's word silently dropped
    /// -- a `node` handed to `explain_admission`, which answers about the CALLER, would answer about a machine
    /// nobody named.
    /// @param call The call.
    /// @param allowed The keys the tool's input schema names.
    /// @return The tool error naming the first unknown key, or nothing.
    [[nodiscard]] std::optional<Mcp::ToolResult> UnknownArgument(Mcp::ToolCall const& call,
                                                                 std::span<std::string_view const> allowed)
    {
        auto const* const members = call.arguments.AsObject();
        if (members == nullptr)
            return std::nullopt;
        for (auto const& [key, value]: *members)
            if (!std::ranges::contains(allowed, std::string_view { key }))
                return Error(std::format("this tool takes no argument `{}`", BoundedPeerText(key, Wire::MaxIdBytes)));
        return std::nullopt;
    }

    /// A machine id an enrollment verb names: present, a string, not empty, and within the one id bound every reader
    /// of an id holds it to (`MaxIdBytes`). Matched EXACTLY by the leader -- the id of a machine still waiting, which
    /// no member list holds, so it is not resolved against the cluster state.
    /// @param call The call.
    /// @return The id, or the tool error naming why it is not one.
    [[nodiscard]] std::expected<std::string, Mcp::ToolResult> PendingIdOf(Mcp::ToolCall const& call)
    {
        auto const id = StringAt(call.arguments, "node_id");
        if (!id.has_value() || id->empty())
            return std::unexpected { Error("`node_id` must name the machine: its id as `enrollment_status` lists it") };
        if (id->size() > Wire::MaxIdBytes)
            return std::unexpected { Error(
                std::format("`node_id` is {} bytes; a machine id is at most {}, so no machine is waiting under it",
                            id->size(),
                            Wire::MaxIdBytes)) };
        return *id;
    }

    // ------------------------------------------------------------ asking and recording

    /// Ask @p target one tool-built frame, and refuse what no action verb answers.
    /// @param context The tools' collaborators.
    /// @param target Whom.
    /// @param frame The request, built by an encoder from validated arguments.
    /// @param verb What the request is called, for the refusal.
    /// @return The reply, or the tool error to answer: a refusal of this node's standing by name, any other failure
    ///         as the upstream says it.
    [[nodiscard]] std::expected<UpstreamReply, Mcp::ToolResult> Ask(ActionContext const& context,
                                                                    UpstreamTarget target,
                                                                    std::vector<std::byte> const& frame,
                                                                    std::string_view verb)
    {
        // Empty for a node target: this node, answered in-process.
        auto answer = context.upstream->Ask(target, {}, FrameText(frame));
        if (!answer.has_value())
        {
            if (auto sentence = CallerRefusalSentence(answer.error().first, context.selfId); sentence.has_value())
                return std::unexpected { Error(std::move(*sentence)) };
            return std::unexpected { UpstreamError(answer.error()) };
        }
        if (answer->status != Wire::Status::Ok)
            return std::unexpected { Error(
                std::format("{} answered {} with a miss, which that verb never answers; nothing it said is reported",
                            answer->answeredBy,
                            verb)) };
        return std::move(*answer);
    }

    /// The client's self-reported name, as a log line may carry it: escaped, bounded, and with no control character
    /// left to start a forged line.
    /// @param clientName What the client said it is called; unverified.
    /// @return The text.
    [[nodiscard]] std::string LoggableClientName(std::string_view clientName)
    {
        if (clientName.empty())
            return "an unnamed client";
        auto escaped = std::string {};
        for (auto const ch: EscapeNonUtf8(clientName))
        {
            auto const byte = static_cast<unsigned char>(ch);
            if (byte < 0x20U || byte == 0x7FU)
                escaped += std::format("\\x{:02X}", byte);
            else
                escaped += ch;
        }
        return ClampUtf8(std::move(escaped), McpAction::MaxAuditClientNameBytes);
    }

    /// Record one action that was answered: `mcp action <tool> <args> by <client> answered by <node>`.
    /// @param context The tools' collaborators.
    /// @param call The call, for the client's name.
    /// @param tool The tool.
    /// @param arguments The arguments as VALIDATED, never as sent.
    /// @param reply Who answered.
    void Audit(ActionContext const& context,
               Mcp::ToolCall const& call,
               std::string_view tool,
               Json::Object arguments,
               UpstreamReply const& reply)
    {
        context.logger->Logf(LogLevel::Info,
                             "mcp action {} {} by {} answered by {}",
                             tool,
                             Json::Serialize(Obj(std::move(arguments))),
                             LoggableClientName(call.clientName),
                             reply.answeredBy);
    }

    /// @param reply What the leader answered an enroll-control verb with.
    /// @return The report it carries, or the tool error saying it could not be read.
    [[nodiscard]] std::expected<Wire::EnrollmentReport, Mcp::ToolResult> ReportOf(UpstreamReply const& reply)
    {
        auto report = Wire::DecodeEnrollmentReport(PayloadBytes(reply.payload));
        if (!report.has_value())
            return std::unexpected { Error(std::format(
                "{} carried out the request, but answered with an enrollment report this node cannot read; run the "
                "same pace-node build on both, and read `enrollment_status` to see where it stands",
                reply.answeredBy)) };
        return std::move(*report);
    }

    /// @param report The leader's report.
    /// @return `enrollment` and `auto_approve_left_s`, as `enrollment_status` spells them.
    [[nodiscard]] Json::Object WindowMembers(Wire::EnrollmentReport const& report)
    {
        auto const armed = report.state == Wire::WireEnrollmentState::AutoApprove;
        return Json::Object {
            { "enrollment", Str(std::string { EnrollmentStateName(report.state) }) },
            { "auto_approve_left_s", armed ? Json::Unsigned(report.autoApproveSecondsLeft) : Null() },
        };
    }

    /// @param report The leader's report.
    /// @return The window in words: `manual`, or `auto-approve (N s left)`.
    [[nodiscard]] std::string WindowWords(Wire::EnrollmentReport const& report)
    {
        auto const armed = report.state == Wire::WireEnrollmentState::AutoApprove;
        return std::format("{}{}",
                           EnrollmentStateName(report.state),
                           armed ? std::format(" ({} s left)", report.autoApproveSecondsLeft) : std::string {});
    }

    // ------------------------------------------------------------ enrollment_approve / enrollment_reject

    /// One decision a person can make about a waiting machine.
    struct DecisionRow
    {
        std::string_view tool; ///< The tool that makes it.
        std::string_view verb; ///< What it is called in a sentence.
    };

    constexpr DecisionRow ApproveRow { .tool = "enrollment_approve", .verb = "approve" };
    constexpr DecisionRow RejectRow { .tool = "enrollment_reject", .verb = "reject" };

    /// Answer a decision the leader carried out: the row it now holds for the machine, and the window.
    /// @param context The tools' collaborators.
    /// @param call The call.
    /// @param row Which decision.
    /// @param nodeId The machine.
    /// @param audited The validated arguments, for the audit line.
    /// @param reply What the leader answered.
    /// @return The result.
    [[nodiscard]] Mcp::ToolResult Decided(ActionContext const& context,
                                          Mcp::ToolCall const& call,
                                          DecisionRow const& row,
                                          std::string const& nodeId,
                                          Json::Object audited,
                                          UpstreamReply const& reply)
    {
        // The leader carried it out once it answered Ok, whatever this node can read of the report: recorded first.
        Audit(context, call, row.tool, std::move(audited), reply);
        auto report = ReportOf(reply);
        if (!report.has_value())
            return std::move(report.error());

        auto const* const entry = core::findOrNull(report->pending, nodeId, &Wire::EnrollmentPendingEntry::nodeId);
        // Null, not a guess, when the report no longer lists the row: the leader's list moved after the decision.
        auto const decision =
            entry != nullptr ? std::optional { std::string { DescribeEnrollmentDecision(entry->decision) } } : std::nullopt;
        auto result = Envelope(reply);
        result.emplace_back("node_id", Str(nodeId));
        result.emplace_back("decision", StrOrNull(decision));
        std::ranges::move(WindowMembers(*report), std::back_inserter(result));
        return Answer(std::move(result),
                      std::format("{} {}: the leader's row now reads {}; enrollment: {}; {}",
                                  row.verb,
                                  nodeId,
                                  decision.value_or("absent (the list moved after the decision)"),
                                  WindowWords(*report),
                                  Provenance(reply)));
    }

    constexpr std::array ApproveArguments { std::string_view { "node_id" }, std::string_view { "public_key" } };

    [[nodiscard]] Mcp::ToolResult EnrollmentApprove(ActionContext const& context, Mcp::ToolCall const& call)
    {
        if (auto unknown = UnknownArgument(call, ApproveArguments); unknown.has_value())
            return std::move(*unknown);
        auto nodeId = PendingIdOf(call);
        if (!nodeId.has_value())
            return std::move(nodeId.error());
        auto const keyText = StringAt(call.arguments, "public_key");
        if (!keyText.has_value())
            return Error("`public_key` must be the key `enrollment_status` shows for that row, as text");
        // The one parser, which refuses a small-order or non-canonical key by name: the door a key enters by.
        auto const key = ParseEd25519PublicKey(*keyText);
        if (!key.has_value())
            return Error(
                std::format("`public_key` is not a key this fleet accepts: {}", DescribePublicKeyFault(key.error())));

        auto reply = Ask(context, UpstreamTarget::Leader, Wire::EncodeEnrollApprove(*nodeId, *key), "enroll-control");
        if (!reply.has_value())
            return std::move(reply.error());
        return Decided(context,
                       call,
                       ApproveRow,
                       *nodeId,
                       { { "node_id", Str(*nodeId) }, { "public_key", Str(FormatEd25519PublicKey(*key)) } },
                       *reply);
    }

    constexpr std::array RejectArguments { std::string_view { "node_id" } };

    [[nodiscard]] Mcp::ToolResult EnrollmentReject(ActionContext const& context, Mcp::ToolCall const& call)
    {
        if (auto unknown = UnknownArgument(call, RejectArguments); unknown.has_value())
            return std::move(*unknown);
        auto nodeId = PendingIdOf(call);
        if (!nodeId.has_value())
            return std::move(nodeId.error());
        auto reply = Ask(context,
                         UpstreamTarget::Leader,
                         Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Reject, *nodeId),
                         "enroll-control");
        if (!reply.has_value())
            return std::move(reply.error());
        return Decided(context, call, RejectRow, *nodeId, { { "node_id", Str(*nodeId) } }, *reply);
    }

    // ------------------------------------------------------------ enrollment_auto_approve

    /// One refusal of a window's length, as this tool words it: the RULE is `JudgeAutoApprove`'s, the remedy is
    /// spelled in this tool's arguments rather than the CLI's flag.
    struct AutoApproveWordingRow
    {
        AutoApproveRefusal refusal; ///< Which rule.
        std::string_view sentence;  ///< What the model reads; a `{}` is `McpAction::MaxAutoApproveMinutes`.
    };

    constexpr EnumTable<AutoApproveRefusal, AutoApproveWordingRow> AutoApproveWording { {
        { .refusal = AutoApproveRefusal::Zero,
          .sentence = "a window of no length admits nobody: `minutes` is a whole number from 1 to {}; `off: true` "
                      "ends a window" },
        { .refusal = AutoApproveRefusal::OverCeiling,
          .sentence = "an auto-approve window is at most {} minutes; re-arm it before it ends to extend it" },
    } };

    static_assert(RowsInEnumeratorOrder(AutoApproveWording, &AutoApproveWordingRow::refusal),
                  "AutoApproveWording must hold one row per AutoApproveRefusal, in enumerator order");

    /// @param refusal Which rule refused.
    /// @return The tool error saying so.
    [[nodiscard]] Mcp::ToolResult AutoApproveRefused(AutoApproveRefusal refusal)
    {
        auto const ceiling = McpAction::MaxAutoApproveMinutes;
        return Error(
            std::vformat(AutoApproveWording[static_cast<std::size_t>(refusal)].sentence, std::make_format_args(ceiling)));
    }

    /// A whole count of minutes, in any of the forms the envelope admits as `integer` (`5`, `5.0`), CLAMPED to the
    /// range the rule distinguishes before anything converts it: below one reads as zero, past the ceiling as one
    /// past it -- so a count too large to be seconds is refused as over the ceiling rather than wrapped into one
    /// that passes.
    /// @param value The argument.
    /// @return The clamped count, or nothing when it is not a whole number.
    [[nodiscard]] std::optional<std::int64_t> WholeMinutes(Json::Value const& value)
    {
        constexpr auto PastCeiling = McpAction::MaxAutoApproveMinutes + 1;
        if (auto const whole = value.AsInt(); whole.has_value())
            return std::clamp<std::int64_t>(*whole, 0, PastCeiling);
        if (value.AsUint().has_value())
            return PastCeiling;
        auto const* const real = std::get_if<double>(&value.v);
        if (real == nullptr || !std::isfinite(*real) || *real != std::trunc(*real))
            return std::nullopt;
        return static_cast<std::int64_t>(std::clamp(*real, 0.0, static_cast<double>(PastCeiling)));
    }

    /// The window `minutes` asks for, judged by `JudgeAutoApprove` -- the rule the CLI and the leader both apply.
    /// @param value The argument.
    /// @return The window, or the tool error naming why it is not one.
    [[nodiscard]] std::expected<std::chrono::seconds, Mcp::ToolResult> WindowOf(Json::Value const& value)
    {
        auto const minutes = WholeMinutes(value);
        if (!minutes.has_value())
            return std::unexpected { Error("`minutes` must be a whole number") };
        if (*minutes < 1)
            return std::unexpected { AutoApproveRefused(AutoApproveRefusal::Zero) };
        if (*minutes > McpAction::MaxAutoApproveMinutes)
            return std::unexpected { AutoApproveRefused(AutoApproveRefusal::OverCeiling) };
        auto const window = std::chrono::seconds { std::chrono::minutes { *minutes } };
        if (auto const refusal = JudgeAutoApprove(window); refusal.has_value())
            return std::unexpected { AutoApproveRefused(*refusal) };
        return window;
    }

    constexpr std::array AutoApproveArguments { std::string_view { "minutes" }, std::string_view { "off" } };

    [[nodiscard]] Mcp::ToolResult EnrollmentAutoApprove(ActionContext const& context, Mcp::ToolCall const& call)
    {
        if (auto unknown = UnknownArgument(call, AutoApproveArguments); unknown.has_value())
            return std::move(*unknown);
        auto const* const minutes = call.arguments.Find("minutes");
        auto const* const off = call.arguments.Find("off");
        if ((minutes == nullptr) == (off == nullptr))
            return Error("give `minutes` to arm the leader's auto-approve window, or `off: true` to end it -- exactly "
                         "one of the two");

        auto frame = std::vector<std::byte> {};
        auto audited = Json::Object {};
        if (off != nullptr)
        {
            // `off: false` is not "arm it": arming names how long, and a window nobody sized is not one to open.
            if (off->AsBool() != std::optional { true })
                return Error("`off` ends the window and takes only `true`; to arm it, give `minutes`");
            frame = Wire::EncodeEnrollControl(Wire::EnrollControlVerb::AutoApproveOff);
            audited.emplace_back("off", Bool(true));
        }
        else
        {
            auto const window = WindowOf(*minutes);
            if (!window.has_value())
                return window.error();
            frame = Wire::EncodeEnrollAutoApprove(*window);
            audited.emplace_back("minutes", Int(std::chrono::duration_cast<std::chrono::minutes>(*window).count()));
        }

        auto reply = Ask(context, UpstreamTarget::Leader, frame, "enroll-control");
        if (!reply.has_value())
            return std::move(reply.error());
        Audit(context, call, "enrollment_auto_approve", std::move(audited), *reply);
        auto report = ReportOf(*reply);
        if (!report.has_value())
            return std::move(report.error());

        auto result = Envelope(*reply);
        std::ranges::move(WindowMembers(*report), std::back_inserter(result));
        return Answer(std::move(result),
                      std::format("enrollment: {}; held in the leader's memory alone, so a leader restart or a change "
                                  "of leader ends it; {}",
                                  WindowWords(*report),
                                  Provenance(*reply)));
    }

    // ------------------------------------------------------------ cordon / uncordon

    /// One direction of the cordon.
    struct CordonRow
    {
        std::string_view tool;     ///< The tool.
        Wire::CordonAction action; ///< What it asks the worker.
    };

    constexpr CordonRow CordonOn { .tool = "cordon", .action = Wire::CordonAction::Cordon };
    constexpr CordonRow CordonOff { .tool = "uncordon", .action = Wire::CordonAction::Lift };

    constexpr std::array CordonArguments { std::string_view { "node" } };

    [[nodiscard]] Mcp::ToolResult Cordon(ActionContext const& context, Mcp::ToolCall const& call, CordonRow const& row)
    {
        if (auto unknown = UnknownArgument(call, CordonArguments); unknown.has_value())
            return std::move(*unknown);
        auto target = NodeTargetOf(call, *context.cluster, context.selfId);
        if (!target.has_value())
            return std::move(target.error());
        // **This node, or nothing**: a worker answers a cordon only from its own machine (`CordonNotLocal`), the gate
        // `pacectl cordon` meets too. Forwarding would be refused there and counted as a refusal on that machine;
        // refused here instead, by the same rule, naming where it can be done.
        if (!target->nodeId.empty() && target->nodeId != context.selfId)
            return Error(std::format(
                "{} is another machine: a worker is cordoned only from its own machine, so this node can {} itself alone; "
                "run `{}` from {}'s own MCP surface, or `pacectl {}` there",
                target->nodeId,
                row.tool,
                row.tool,
                target->nodeId,
                row.tool));

        auto reply = Ask(context, UpstreamTarget::Node, Wire::EncodeCordonRequest(row.action), row.tool);
        if (!reply.has_value())
            return std::move(reply.error());
        Audit(context, call, row.tool, { { "node", Str(context.selfId) } }, *reply);
        auto const fields = Wire::DecodeCordonFields(PayloadBytes(reply->payload));
        if (!fields.has_value())
            return Error(std::format("{} carried out the {}, but answered with a body this node cannot read; read "
                                     "`node_status` to see where it stands",
                                     reply->answeredBy,
                                     row.tool));

        auto const state = std::string { NodeStatusView::CordonStateName(fields->state) };
        auto result = Envelope(*reply);
        result.emplace_back("node", Str(context.selfId));
        result.emplace_back("cordon", Str(state));
        result.emplace_back("compiles_in_flight", Json::Unsigned(fields->inFlight));
        return Answer(std::move(result),
                      std::format("node {} cordon {}, {} compile(s) in flight; not persisted: `uncordon` or a restart "
                                  "of this node lifts it; {}",
                                  context.selfId,
                                  state,
                                  fields->inFlight,
                                  Provenance(*reply)));
    }

    // ------------------------------------------------------------ set_display_name

    constexpr std::array DisplayNameArguments { std::string_view { "node" }, std::string_view { "name" } };

    [[nodiscard]] Mcp::ToolResult SetDisplayName(ActionContext const& context, Mcp::ToolCall const& call)
    {
        if (auto unknown = UnknownArgument(call, DisplayNameArguments); unknown.has_value())
            return std::move(*unknown);
        // REQUIRED, unlike a read's `node`: renaming is a decision about one machine, and a rename that fell back to
        // this node because the argument was forgotten would rename a machine nobody named.
        if (call.arguments.Find("node") == nullptr)
            return Error("`node` must name the member to rename: its id or its current display name, matched exactly");
        auto target = NodeTargetOf(call, *context.cluster, context.selfId);
        if (!target.has_value())
            return std::move(target.error());
        auto const nodeId = target->nodeId.empty() ? context.selfId : target->nodeId;
        if (nodeId.empty())
            return Error("this node has no id, so it cannot name itself; give `node` as a member's id");

        auto const* const nameValue = call.arguments.Find("name");
        auto const* const name = nameValue != nullptr ? nameValue->AsString() : nullptr;
        if (name == nullptr)
            return Error(R"(`name` must be a string: the display name, or "" to clear it)");
        // The leader's own rule, asked here so a name no member may carry is refused before anything is sent -- and
        // never a second copy of it that could refuse what the leader accepts.
        if (auto const refused = Cluster::RefuseDisplayName(*name); refused.has_value())
            return Error(std::format("`name` cannot be a display name: {}", *refused));

        auto reply = Ask(context,
                         UpstreamTarget::Leader,
                         Wire::EncodeMemberEdit(Wire::MemberEditRequest {
                             .verb = Wire::MemberEditVerb::SetDisplayName, .memberId = nodeId, .value = *name }),
                         "member-edit");
        if (!reply.has_value())
            return std::move(reply.error());
        Audit(context, call, "set_display_name", { { "node", Str(nodeId) }, { "name", Str(*name) } }, *reply);

        auto result = Envelope(*reply);
        result.emplace_back("node_id", Str(nodeId));
        result.emplace_back("display_name", name->empty() ? Null() : Str(*name));
        result.emplace_back("state", Str("replicating"));
        // Appended, never committed: the leader cannot know the difference until a majority answers.
        return Answer(std::move(result),
                      std::format("{}: the leader appended the change, which is replicating (not yet committed); read "
                                  "`fleet_overview` to see it in force; {}",
                                  name->empty() ? std::format("{}'s display name cleared", nodeId)
                                                : std::format("{} named \"{}\"", nodeId, *name),
                                  Provenance(*reply)));
    }

    // ------------------------------------------------------------ explain_admission

    constexpr std::array ExplainArguments { std::string_view { "machine" } };

    [[nodiscard]] Mcp::ToolResult ExplainAdmission(ActionContext const& context, Mcp::ToolCall const& call)
    {
        if (auto unknown = UnknownArgument(call, ExplainArguments); unknown.has_value())
            return std::move(*unknown);
        // Handed to the node verbatim, NOT resolved against the member list: the subject may be a key's text, a
        // machine still waiting, or one whose key was revoked -- none of which a member list names, and each of
        // which the node answers by its standing (`pending`, `revoked`, `unknown`). The node matches it exactly.
        auto const machine = StringAt(call.arguments, "machine").value_or(std::string {});
        auto const asked = call.arguments.Find("machine") != nullptr;
        if (asked && StringAt(call.arguments, "machine") == std::nullopt)
            return Error("`machine` must be a string: a machine's id or its key's text");
        // An EMPTY subject is the wire's spelling of "this caller's own connection", so `"machine": ""` sent on
        // would answer about the caller while the model believes it asked about a machine. Refused, naming both.
        if (asked && machine.empty())
            return Error("`machine` is empty, which names no machine: give a machine's id or its key's text, or omit "
                         "`machine` to ask about this caller's own connection");
        if (machine.size() > McpAction::MaxExplainSubjectBytes)
            return Error(std::format("`machine` is {} bytes; neither an id nor a key's text is longer than {}",
                                     machine.size(),
                                     McpAction::MaxExplainSubjectBytes));

        auto reply = Ask(context, UpstreamTarget::Node, Wire::EncodeExplainAdmissionRequest(machine), "explain-admission");
        if (!reply.has_value())
            return std::move(reply.error());
        auto const fields = Wire::DecodeAdmissionExplanation(PayloadBytes(reply->payload));
        if (!fields.has_value())
            return Error(std::format("{} answered explain-admission with a body this node cannot read; nothing it said "
                                     "is reported -- run the same pace-node build on both",
                                     reply->answeredBy));

        // The standing is ABSENT for a question about the caller, which has no place in the roster -- not `unknown`,
        // which is an answer about a machine.
        auto const standing = fields->standing.has_value() ? Distributed::StandingOnTheWire(*fields->standing)
                                                           : std::optional<Distributed::MachineStanding> {};
        auto const routes = Distributed::AdmissionRouteNames(fields->decidedBy);
        auto decidedBy = Json::Array {};
        auto routeWords = std::string {};
        for (auto const& route: routes)
        {
            decidedBy.push_back(Str(route));
            routeWords += (routeWords.empty() ? "" : ", ") + route;
        }
        auto const verdict = std::string { Distributed::VerdictName(fields->verdict) };
        auto const subject = NodeStatusView::ReadableText(fields->subject);

        auto result = Envelope(*reply);
        result.emplace_back("machine", machine.empty() ? Null() : Str(machine));
        result.emplace_back("subject", Str(subject));
        result.emplace_back("standing",
                            standing.has_value() ? Str(std::string { Distributed::StandingName(*standing) }) : Null());
        result.emplace_back("verdict", Str(verdict));
        result.emplace_back("decided_by", Arr(std::move(decidedBy)));
        auto text = std::format("{} is {}{}, decided by {}",
                                subject,
                                verdict,
                                standing.has_value() ? std::format(" (standing: {})", Distributed::StandingName(*standing))
                                                     : std::string {},
                                routeWords.empty() ? std::string { "no route" } : routeWords);
        if (fields->verdict == Wire::WireMembership::Forgotten)
            text += "; a forgotten machine's revoked key outranks every admission route, so opening the node with "
                    "--fleet-open will not bring it back; only --cluster-admit under a NEW key does";
        return Answer(std::move(result), std::format("{}; {}", text, Provenance(*reply)));
    }

    // ------------------------------------------------------------ the schemas

    constexpr std::string_view EnrollmentDecisionOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","node_id","decision","enrollment","auto_approve_left_s"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("node_id":{"type":"string"},)"
        R"("decision":{"type":["string","null"],"description":"pending, approved or rejected, as the leader's row now reads; null when its list no longer holds the row."},)"
        R"("enrollment":{"type":"string","description":"manual, or auto-approve while a deadline is armed."},)"
        R"("auto_approve_left_s":{"type":["integer","null"],"description":"Seconds left on the armed deadline; null when none is armed."}}})";

    constexpr std::string_view ApproveInput =
        R"({"type":"object","properties":{)"
        R"("node_id":{"type":"string","description":"The waiting machine's id, exactly as enrollment_status lists it."},)"
        R"("public_key":{"type":"string","description":"The key enrollment_status shows for that row (public_key). Compare it with the key the machine itself printed before approving."}},)"
        R"("required":["node_id","public_key"],"additionalProperties":false})";

    constexpr std::string_view RejectInput =
        R"({"type":"object","properties":{)"
        R"("node_id":{"type":"string","description":"The waiting machine's id, exactly as enrollment_status lists it."}},)"
        R"("required":["node_id"],"additionalProperties":false})";

    constexpr std::string_view AutoApproveOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","enrollment","auto_approve_left_s"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("enrollment":{"type":"string","description":"manual, or auto-approve while a deadline is armed."},)"
        R"("auto_approve_left_s":{"type":["integer","null"],"description":"Seconds left on the armed deadline; null when none is armed."}}})";

    /// @return `enrollment_auto_approve`'s input schema, its `maximum` taken from `McpAction::MaxAutoApproveMinutes`
    ///         rather than restated. Built once; the view a row holds stays valid for the process.
    [[nodiscard]] std::string_view AutoApproveInput()
    {
        static std::string const schema =
            R"({"type":"object","properties":{)"
            R"("minutes":{"type":"integer","minimum":1,"maximum":)"
            + std::to_string(McpAction::MaxAutoApproveMinutes)
            + R"(,"description":"Arm the window for this many minutes from now; arming again restarts it from now."},)"
              R"("off":{"type":"boolean","const":true,"description":"End the armed window."}},)"
              R"("oneOf":[{"required":["minutes"]},{"required":["off"]}],"additionalProperties":false})";
        return schema;
    }

    /// @return `set_display_name`'s input schema, its byte bound taken from `Cluster::MaxDisplayNameBytes`.
    [[nodiscard]] std::string_view DisplayNameInput()
    {
        static std::string const schema =
            R"({"type":"object","properties":{)"
            R"("node":{"type":"string","description":"The member to rename: its id or its current display name, matched exactly -- never by prefix."},)"
            R"("name":{"type":"string","description":"The new display name, at most )"
            + std::to_string(Cluster::MaxDisplayNameBytes)
            + R"( bytes of text with no control character; \"\" clears it. Unique across the fleet, with ASCII letters compared in one case."}},)"
              R"("required":["node","name"],"additionalProperties":false})";
        return schema;
    }

    constexpr std::string_view DisplayNameOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","node_id","display_name","state"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("node_id":{"type":"string","description":"The member renamed, by its id."},)"
        R"("display_name":{"type":["string","null"],"description":"The name asked for; null when it was cleared."},)"
        R"("state":{"const":"replicating","description":"Appended at the leader, not yet committed."}}})";

    constexpr std::string_view CordonInput =
        R"({"type":"object","properties":{)"
        R"("node":{"type":"string","description":"This node's own id, matched exactly; omit it. A worker is cordoned only from its own machine, so naming another member is refused."}},"additionalProperties":false})";

    constexpr std::string_view CordonOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","node","cordon","compiles_in_flight"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("node":{"type":"string"},)"
        R"("cordon":{"type":"string","description":"serving, draining (cordoned, compiles still running) or drained (cordoned, nothing running)."},)"
        R"("compiles_in_flight":{"type":"integer"}}})";

    /// @return `explain_admission`'s input schema, its `maxLength` taken from `McpAction::MaxExplainSubjectBytes`.
    [[nodiscard]] std::string_view ExplainInput()
    {
        static std::string const schema =
            R"({"type":"object","properties":{)"
            R"("machine":{"type":"string","maxLength":)"
            + std::to_string(McpAction::MaxExplainSubjectBytes)
            + R"(,"description":"A machine's id or its identity key's text, matched exactly by this node. Omit it to ask about this MCP caller's own connection to this node."}},"additionalProperties":false})";
        return schema;
    }

    constexpr std::string_view ExplainOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","machine","subject","standing","verdict","decided_by"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("machine":{"type":["string","null"],"description":"The machine asked about; null when the caller's own connection was."},)"
        R"("subject":{"type":"string","description":"Who the node answered about: the machine, or the caller's id or host."},)"
        R"("standing":{"type":["string","null"],"description":"voter, learner, pending, revoked or unknown; null for a question about the caller, which has no place in the roster."},)"
        R"("verdict":{"type":"string","description":"admitted, refused or forgotten."},)"
        R"("decided_by":{"type":"array","items":{"type":"string"},"description":"Every route that decided: loopback, fleet-open, proven-key, ticket, key-revoked; a route this build cannot name as its bit, 0x...; empty when no route had an opinion."}}})";

} // namespace

namespace
{
    /// One refusal of this node as the caller, and what a tool says for it.
    struct CallerRefusalRow
    {
        UpstreamFailure failure;   ///< The failure.
        std::string_view sentence; ///< What the model reads; the `{}` is this node's id.
    };

    /// The failures that refuse THIS node as the caller, each with its own remedy.
    constexpr std::array CallerRefusals {
        CallerRefusalRow {
            .failure = UpstreamFailure::StandingRequired,
            .sentence = "{} lacks operator standing (it is not a voter); run it from a voter's machine, or promote this "
                        "node" },
        CallerRefusalRow {
            .failure = UpstreamFailure::NotIdentified,
            .sentence = "{} could not be identified as the caller of this control verb by the node that answers it: no "
                        "key it proved is one that node's roster records; run it on that node's own machine, or from a "
                        "member whose key the fleet has recorded" },
    };
} // namespace

std::optional<std::string> CallerRefusalSentence(UpstreamFailure failure, std::string const& selfId)
{
    auto const* const row = core::findOrNull(CallerRefusals, failure, &CallerRefusalRow::failure);
    if (row == nullptr)
        return std::nullopt;
    auto const node = selfId.empty() ? std::string { "this node" } : selfId;
    return std::vformat(row->sentence, std::make_format_args(node));
}

std::vector<Mcp::ToolRow> ActionTools(IMcpUpstream& upstream,
                                      IAppliedClusterView const& cluster,
                                      std::string selfId,
                                      ILogger& logger)
{
    auto const context =
        ActionContext { .upstream = &upstream, .cluster = &cluster, .selfId = std::move(selfId), .logger = &logger };
    return {
        Mcp::ToolRow {
            .name = "enrollment_approve",
            .title = "Approve enrollment",
            .description = "Admit one machine waiting to enrol, under the key named with it: the public_key "
                           "enrollment_status shows for that row, which a person must first compare with the key the "
                           "machine itself printed -- that comparison is the whole of what makes this safe. An "
                           "approval naming any other key is refused and admits nothing. The leader decides; this "
                           "node asks it as itself, and needs an operator's standing there (a voter's seat, or being "
                           "the leader).",
            .inputSchema = ApproveInput,
            .outputSchema = EnrollmentDecisionOutput,
            .effect = Mcp::ToolEffect::Mutating,
            .handler = [context](Mcp::ToolCall const& call) { return EnrollmentApprove(context, call); },
        },
        Mcp::ToolRow {
            .name = "enrollment_reject",
            .title = "Reject enrollment",
            .description = "Refuse one machine waiting to enrol, by its id. Final for that request: the machine is "
                           "told so and stops asking. A machine already APPROVED is not removed by this -- the "
                           "approval committed it to the cluster, so a reject only stops it being handed the roster; "
                           "`pacectl cluster-forget <id>` (or `pace-node --cluster-forget=<id>`) removes it and "
                           "revokes its key, and is deliberately not offered over MCP. Needs this node to hold an "
                           "operator's standing at the leader (a voter's seat, or being the leader).",
            .inputSchema = RejectInput,
            .outputSchema = EnrollmentDecisionOutput,
            .effect = Mcp::ToolEffect::Destructive,
            .handler = [context](Mcp::ToolCall const& call) { return EnrollmentReject(context, call); },
        },
        Mcp::ToolRow {
            .name = "enrollment_auto_approve",
            .title = "Arm or end auto-approve",
            .description = "Arm the leader's auto-approve window for `minutes` from now -- arming again restarts it "
                           "from now -- or end it with `off: true`. While it is armed, ANY machine that asks to enrol "
                           "is admitted under the key it asks with, with nobody comparing it, including rows already "
                           "waiting at their next poll: reject a row that must not be admitted first. The window is "
                           "held in the leader's memory alone, so it ends at a leader restart or a change of leader. "
                           "Needs this node to hold an operator's standing at the leader. Durations are in SECONDS "
                           "(fields ending _s).",
            .inputSchema = AutoApproveInput(),
            .outputSchema = AutoApproveOutput,
            .effect = Mcp::ToolEffect::Mutating,
            .handler = [context](Mcp::ToolCall const& call) { return EnrollmentAutoApprove(context, call); },
        },
        Mcp::ToolRow {
            .name = "set_display_name",
            .title = "Set display name",
            .description = "Give a member a display name every surface shows -- the fleet page, pacectl, every MCP "
                           "answer -- or clear it with an empty `name`. Replicated: the leader proposes it, and every "
                           "node agrees. A label that decides nothing: no admission, route, key or redirect reads it. "
                           "Unique across the fleet, compared with ASCII letters folded to one case (other bytes "
                           "exactly). Tools accept the name wherever they accept a node id. Needs this node to hold an "
                           "operator's standing at the leader (a voter's seat, or being the leader). The same change "
                           "as `pacectl cluster-name` and `pace-node --cluster-name`.",
            .inputSchema = DisplayNameInput(),
            .outputSchema = DisplayNameOutput,
            .effect = Mcp::ToolEffect::Mutating,
            .handler = [context](Mcp::ToolCall const& call) { return SetDisplayName(context, call); },
        },
        Mcp::ToolRow {
            .name = "cordon",
            .title = "Cordon this node",
            .description = "Take this node's worker out of the fleet: it refuses new compiles and lets the running "
                           "ones finish (draining, then drained). Not persisted: `uncordon`, or a restart of this "
                           "node, lifts it. A worker is cordoned only from its own machine -- the gate `pacectl "
                           "cordon` meets too -- so this cordons THIS node; another member is cordoned from its own "
                           "MCP surface. A node running no worker has nothing to cordon and says so.",
            .inputSchema = CordonInput,
            .outputSchema = CordonOutput,
            .effect = Mcp::ToolEffect::Mutating,
            .handler = [context](Mcp::ToolCall const& call) { return Cordon(context, call, CordonOn); },
        },
        Mcp::ToolRow {
            .name = "uncordon",
            .title = "Uncordon this node",
            .description = "Put this node's worker back into the fleet after `cordon`. A cordon is not persisted, so a "
                           "restart of this node lifts it too. Like `cordon`, it acts on THIS node alone.",
            .inputSchema = CordonInput,
            .outputSchema = CordonOutput,
            .effect = Mcp::ToolEffect::Mutating,
            .handler = [context](Mcp::ToolCall const& call) { return Cordon(context, call, CordonOff); },
        },
        Mcp::ToolRow {
            .name = "explain_admission",
            .title = "Explain admission",
            .description = "Why this node admits or refuses a machine -- or, with no `machine`, this MCP caller's own "
                           "connection: the verdict, the machine's standing in the roster, and EVERY route that "
                           "decided, not just the winner. The answer `pacectl explain-admission` prints, from the fold "
                           "the node's surfaces enforce. `machine` is an id or a key's text, matched exactly; a "
                           "machine still waiting or one whose key was revoked is answered by its standing.",
            .inputSchema = ExplainInput(),
            .outputSchema = ExplainOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [context](Mcp::ToolCall const& call) { return ExplainAdmission(context, call); },
        },
    };
}

} // namespace Pace::Node
