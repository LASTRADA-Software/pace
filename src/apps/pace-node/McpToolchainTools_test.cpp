// SPDX-License-Identifier: Apache-2.0
#include "FingerprintInputsResponder.hpp"
#include "McpFleetTools.hpp"
#include "McpToolchainTools.hpp"
#include "McpUpstream.hpp"
#include "McpUpstreamFakes.hpp"

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/FleetView.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/FingerprintInputsWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
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

/// The id this node answers to in every case.
constexpr std::string_view SelfId = "node-self";

/// A cluster that has applied no state: only this node's own id names anybody.
McpFakes::FixedCluster const NoState { std::nullopt };

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
/// @param family The family the node read off the banner; empty for none.
/// @param label What the node called the toolchain; empty for none.
/// @return One registry entry.
[[nodiscard]] Dist::WorkerReport Entry(std::string fingerprint, std::string endpoint, std::string family, std::string label)
{
    auto report = Dist::WorkerReport {};
    report.info.id = fingerprint + "@" + endpoint;
    report.info.fingerprint = std::move(fingerprint);
    report.info.endpoint = std::move(endpoint);
    report.info.toolchainLabel = std::move(label);
    report.info.toolchainFamily = std::move(family);
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
    auto const tools = ToolchainTools(upstream, NoState, std::string { SelfId });
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

/// @param value An object.
/// @param key A member.
/// @return The member's string, or nothing when it is `null` or not a string -- so an absent value FAILS a
///         comparison rather than being dereferenced; the case fails when the member is missing.
[[nodiscard]] std::optional<std::string> TextAt(Json::Value const& value, std::string_view key)
{
    auto const* const text = At(value, key).AsString();
    return text != nullptr ? std::optional<std::string> { *text } : std::nullopt;
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
        Entry("fp-12", "a:1", "gcc", "gcc 12.3.0"),
        Entry("fp-13", "b:1", "gcc", "gcc 13.2.0"),
        // One machine with two toolchains: two registry entries, ONE machine.
        Entry("fp-13", "c:1", "gcc", "gcc 13.2.0"),
        Entry("fp-clang", "c:1", "clang", "clang 18.1.8"),
        // A node that stated neither a label nor a family: its toolchain joins no family.
        Entry("fp-pinned", "d:1", "", ""),
        // Apple clang installed as `gcc`: labelled by the name, placed by the banner -- never in GNU gcc's family.
        Entry("fp-apple", "e:1", "apple-clang", "gcc 17.0.0"),
    }));

    auto const result = CallToolchains(upstream);
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    // One FLEET-TEXT, in the Json format, to the leader: the one document the fleet tools read.
    REQUIRE(upstream.calls.size() == 1);
    CHECK(upstream.calls.front().target == UpstreamTarget::Leader);
    CHECK(FleetTextAsked(upstream).format == Wire::FleetTextFormat::Json);
    auto const& out = result.structured;

    CHECK(TextAt(out, "answered_by") == std::optional<std::string> { LeaderAt });
    CHECK(At(out, "term").AsInt() == std::optional<std::int64_t> { TermHere });
    CHECK(TextAt(out, "term_scope") == std::optional<std::string> { "this-node" });
    CHECK(At(out, "machine_count").AsInt() == std::optional<std::int64_t> { 5 });
    CHECK(At(out, "toolchain_count").AsInt() == std::optional<std::int64_t> { 5 });

    auto const& toolchains = At(out, "toolchains");
    CHECK(Elements(toolchains).size() == 5);
    auto const& gcc13 = RowWhere(toolchains, "fingerprint", "fp-13");
    CHECK(TextAt(gcc13, "label") == std::optional<std::string> { "gcc 13.2.0" });
    CHECK(TextAt(gcc13, "family") == std::optional<std::string> { "gcc" });
    CHECK(TextAt(gcc13, "version") == std::optional<std::string> { "13.2.0" });
    CHECK(StringsOf(At(gcc13, "nodes")) == std::vector<std::string> { "b:1", "c:1" });

    auto const& pinned = RowWhere(toolchains, "fingerprint", "fp-pinned");
    CHECK(At(pinned, "label").IsNull());
    CHECK(At(pinned, "family").IsNull());
    CHECK(TextAt(pinned, "version") == std::optional<std::string> { "unknown" });

    auto const& apple = RowWhere(toolchains, "fingerprint", "fp-apple");
    CHECK(TextAt(apple, "family") == std::optional<std::string> { "apple-clang" });
    CHECK(TextAt(apple, "version") == std::optional<std::string> { "17.0.0" });

    // Unserved is the fleet's toolchains in the machine's own families that it does not serve: c:1 serves gcc 13
    // and clang, and the fleet's other gcc is 12; a:1 lacks the 13; the Apple clang machine is alone in its family.
    auto const& machine = RowWhere(At(out, "nodes"), "endpoint", "c:1");
    CHECK(StringsOf(At(machine, "served")) == std::vector<std::string> { "fp-13", "fp-clang" });
    CHECK(StringsOf(At(machine, "unserved")) == std::vector<std::string> { "fp-12" });
    CHECK(StringsOf(At(RowWhere(At(out, "nodes"), "endpoint", "a:1"), "unserved")) == std::vector<std::string> { "fp-13" });
    CHECK(Elements(At(RowWhere(At(out, "nodes"), "endpoint", "e:1"), "unserved")).empty());
    // A machine serving no toolchain of a known family has no peers to lack: absent, not empty.
    CHECK(At(RowWhere(At(out, "nodes"), "endpoint", "d:1"), "unserved").IsNull());
    CHECK(At(out, "unserved_note").AsString() != nullptr);
    // No demand is recorded yet: absent, not zero.
    CHECK(At(out, "demand").IsNull());

    auto const& report = At(out, "upgrade_report");
    auto const& gcc = RowWhere(report, "family", "gcc");
    CHECK(TextAt(gcc, "recommended_version") == std::optional<std::string> { "13.2.0" });
    CHECK(TextAt(At(gcc, "recommended_fingerprints"), "gcc") == std::optional<std::string> { "fp-13" });
    CHECK(TextAt(RowWhere(At(out, "toolchains"), "fingerprint", "fp-13"), "driver") == std::optional<std::string> { "gcc" });
    CHECK(StringsOf(At(gcc, "behind")) == std::vector<std::string> { "a:1" });
    CHECK(Elements(At(gcc, "ahead")).empty());
    CHECK(Elements(At(gcc, "same_version_other_fingerprint")).empty());
    CHECK(At(gcc, "ordering_unknown").AsBool() == std::optional<bool> { false });
    CHECK(TextAt(gcc, "basis").value_or(std::string {}).contains("no demand data; highest labelled version served"));
    // gcc, clang and apple-clang; the family-less toolchain advises nobody.
    CHECK(Elements(report).size() == 3);
    CHECK(Elements(At(RowWhere(report, "family", "apple-clang"), "behind")).empty());
}

TEST_CASE("toolchains reports an unparseable family as ordering unknown, recommending nothing",
          "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(DocumentWith({
        Entry("fp-17", "a:1", "clang", "clang 17.0.6"),
        Entry("fp-x", "b:1", "clang", "clang"),
    }));
    auto const result = CallToolchains(upstream);
    REQUIRE_FALSE(result.isError);
    auto const& clang = RowWhere(At(result.structured, "upgrade_report"), "family", "clang");
    CHECK(At(clang, "ordering_unknown").AsBool() == std::optional<bool> { true });
    CHECK(At(clang, "recommended_fingerprints").IsNull());
    CHECK(At(clang, "recommended_version").IsNull());
    for (auto const key: { std::string_view { "behind" },
                           std::string_view { "ahead" },
                           std::string_view { "same_version_other_fingerprint" } })
        CHECK(Elements(At(clang, key)).empty());
    CHECK(TextAt(RowWhere(At(result.structured, "toolchains"), "fingerprint", "fp-x"), "version")
          == std::optional<std::string> { "unknown" });
    CHECK(result.text.contains("ordering unknown"));
}

TEST_CASE("toolchains places a machine on a newer version ahead, and another build of the target apart",
          "[pace-node][mcp][toolchains]")
{
    // Without demand the target is the highest labelled version, so nothing can be ahead of it; a same-version
    // second build with no demand to separate them names no fingerprint. What the split must never do is call the
    // newer machine "behind": here 14 is the target and the 13.2.0 builds are both behind it.
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(DocumentWith({
        Entry("fp-13-x", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-13-y", "b:1", "gcc", "gcc 13.2.0"),
        Entry("fp-14", "c:1", "gcc", "gcc 14.1.0"),
    }));
    auto const result = CallToolchains(upstream);
    REQUIRE_FALSE(result.isError);
    auto const& gcc = RowWhere(At(result.structured, "upgrade_report"), "family", "gcc");
    CHECK(TextAt(At(gcc, "recommended_fingerprints"), "gcc") == std::optional<std::string> { "fp-14" });
    CHECK(StringsOf(At(gcc, "behind")) == std::vector<std::string> { "a:1", "b:1" });
    CHECK(Elements(At(gcc, "ahead")).empty());
    CHECK(result.text.contains("behind: a:1, b:1"));
}

TEST_CASE("toolchains names a fingerprint per driver, so gcc and cc of one install are both recommended",
          "[pace-node][mcp][toolchains]")
{
    auto upstream = LeaderUpstream();
    upstream.AnswerOk(DocumentWith({
        Entry("fp-gcc", "a:1", "gcc", "gcc 13.2.0"),
        Entry("fp-cc", "a:1", "gcc", "cc 13.2.0"),
        Entry("fp-gcc", "b:1", "gcc", "gcc 13.2.0"),
        Entry("fp-cc", "b:1", "gcc", "cc 13.2.0"),
    }));
    auto const result = CallToolchains(upstream);
    REQUIRE_FALSE(result.isError);
    auto const& gcc = RowWhere(At(result.structured, "upgrade_report"), "family", "gcc");
    auto const& chosen = At(gcc, "recommended_fingerprints");
    REQUIRE(chosen.AsObject() != nullptr);
    CHECK(chosen.AsObject()->size() == 2);
    CHECK(TextAt(chosen, "gcc") == std::optional<std::string> { "fp-gcc" });
    CHECK(TextAt(chosen, "cc") == std::optional<std::string> { "fp-cc" });
    CHECK(Elements(At(gcc, "same_version_other_fingerprint")).empty());
    CHECK(result.text.contains("cc: fp-cc, gcc: fp-gcc"));
}

TEST_CASE("toolchains reads a worker row with no family key as no family, never refusing the answer",
          "[pace-node][mcp][toolchains]")
{
    // A leader that renders no `family` column at all: each row joins no family, and the rest of the answer stands.
    auto document =
        Unwrap(Json::Parse(DocumentWith({ Entry("fp", "a:1", "gcc", "gcc 13.2.0") }), McpFleet::FleetDocumentLimits));
    auto* const members = std::get_if<Json::Object>(&document.v);
    REQUIRE(members != nullptr);
    auto const workers = std::ranges::find(*members, std::string_view { "workers" }, &Json::Member::first);
    REQUIRE(workers != members->end());
    auto* const rows = std::get_if<Json::Array>(&workers->second.v);
    REQUIRE(rows != nullptr);
    REQUIRE(rows->size() == 1);
    auto* const row = std::get_if<Json::Object>(&rows->front().v);
    REQUIRE(row != nullptr);
    auto const before = row->size();
    std::erase_if(*row, [](Json::Member const& member) { return member.first == "family"; });
    REQUIRE(row->size() + 1 == before);

    auto upstream = LeaderUpstream();
    upstream.AnswerOk(Json::Serialize(document));
    auto const result = CallToolchains(upstream);
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    CHECK(At(RowWhere(At(result.structured, "toolchains"), "fingerprint", "fp"), "family").IsNull());
    CHECK(Elements(At(result.structured, "upgrade_report")).empty());
    CHECK(At(RowWhere(At(result.structured, "nodes"), "endpoint", "a:1"), "unserved").IsNull());
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
    auto document =
        Unwrap(Json::Parse(DocumentWith({ Entry("fp", "a:1", "gcc", "gcc 13.2.0") }), McpFleet::FleetDocumentLimits));
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
    auto const tools = ToolchainTools(upstream, NoState, std::string { SelfId });
    REQUIRE(tools.size() == 2);
    CHECK(tools.front().name == "toolchains");
    CHECK(tools.front().effect == Mcp::ToolEffect::ReadOnly);
    CHECK(tools.front().description.contains("conservative"));
    CHECK(tools.front().description.contains("basis"));
    CHECK(tools.back().name == "toolchain_diff");
    CHECK(tools.back().effect == Mcp::ToolEffect::ReadOnly);
}

// ------------------------------------------------------------ toolchain_diff

namespace
{

/// A fleet of in-process nodes, each answering FINGERPRINT-INPUTS through the production responder.
///
/// **The real responder and the real codecs**, so a case exercises the pages a node would send -- cut where the
/// production packer cuts them -- rather than pages a fake decided on.
struct DiffFleet
{
    /// One node: its published survey and the responder over it.
    struct Member
    {
        std::shared_ptr<ServedInputs const> served; ///< What its survey published; swapped by a case mid-run.
        std::string version;                        ///< Its `pace-node` version.
    };

    /// A source reading a member's CURRENT publication, so a case can re-run a survey between two pages.
    class MemberSource final: public IServedInputsSource
    {
      public:
        /// @param member The member; must outlive this.
        explicit MemberSource(Member const& member):
            _member { member }
        {
        }

        /// @copydoc IServedInputsSource::ServedInputsNow
        [[nodiscard]] std::shared_ptr<ServedInputs const> ServedInputsNow() const override
        {
            return _member.served;
        }

      private:
        Member const& _member;
    };

    std::map<std::string, Member> members;       ///< By node id; the empty id is this node.
    std::size_t pageEntries { 3 };               ///< Small, so every record crosses page boundaries.
    std::function<void(std::size_t)> afterAsked; ///< Called after every call with the number asked so far.
    AtomicMetricsSink metrics;                   ///< Where a responder counts.

    /// @param call One call.
    /// @return What that member's responder answered, as the upstream reports it.
    [[nodiscard]] UpstreamResult Answer(McpFakes::AskedCall const& call)
    {
        auto const found = members.find(call.nodeIdOrEmpty);
        if (found == members.end())
            return std::unexpected { std::pair { UpstreamFailure::UnknownNode, call.nodeIdOrEmpty } };
        auto const source = MemberSource { found->second };
        auto const responder = FingerprintInputsResponder { &source, metrics, found->second.version, pageEntries };
        auto const frame = std::as_bytes(std::span { call.frame.data(), call.frame.size() });
        auto const header = Wire::DecodeRequestHeader(frame);
        if (!header.has_value() || header->opRaw != static_cast<std::uint8_t>(Wire::Op::FingerprintInputs))
            return std::unexpected { std::pair { UpstreamFailure::Refused, std::string { "not fingerprint-inputs" } } };
        auto const reply = responder.Answer(frame.subspan(Wire::RequestHeaderSize));
        auto const replyHeader = Wire::DecodeReplyHeader(reply);
        if (!replyHeader.has_value())
            return std::unexpected { std::pair { UpstreamFailure::Unreachable, std::string { "no reply" } } };
        auto const payload = std::span { reply }.subspan(Wire::ReplyHeaderSize);
        if (replyHeader->status != Wire::Status::Ok)
        {
            auto const refusal = Wire::DecodeErrorPayload(payload);
            return std::unexpected { std::pair { UpstreamFailure::Refused,
                                                 refusal.has_value() ? std::string { refusal->second } : std::string {} } };
        }
        return UpstreamReply { .status = Wire::Status::Ok,
                               .payload = McpFakes::AsUpstreamText(payload),
                               .answeredBy =
                                   call.nodeIdOrEmpty.empty() ? std::string { "self:6674" } : call.nodeIdOrEmpty + ":6674",
                               .term = TermHere };
    }
};

/// @param count How many files.
/// @param salt What makes this tree's digests its own; files whose index is in @p changed take it.
/// @param changed Indices whose content differs from the base.
/// @return A record captured as a survey captures one.
[[nodiscard]] Cc::FingerprintInputs DiffRecord(std::size_t count,
                                               std::string_view banner = "gcc (GCC) 13.2.0",
                                               std::vector<std::size_t> const& changed = {})
{
    auto files = std::vector<Cc::ToolchainFile> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        files.push_back(Cc::ToolchainFile {
            .relativePath = std::format("c++/13/bits/h{:03}.h", index),
            .contentHash = std::format("{}{:030x}", std::ranges::contains(changed, index) ? "ff" : "00", index) });
    return Cc::CaptureFingerprintInputs(banner, "gnu", std::move(files));
}

/// @param record A record.
/// @param fingerprint What it is filed under; its own digest when empty.
/// @param label Its label.
/// @param compiler Its compiler path.
/// @return One served toolchain.
[[nodiscard]] ServedToolchainInputs Served(Cc::FingerprintInputs record,
                                           std::string fingerprint = {},
                                           std::string label = "gcc 13.2.0",
                                           std::string compiler = "/usr/bin/gcc")
{
    if (fingerprint.empty())
        fingerprint = Cc::ComputeToolchainFingerprint(record);
    return ServedToolchainInputs { .fingerprint = std::move(fingerprint),
                                   .label = std::move(label),
                                   .compiler = std::move(compiler),
                                   .inputs = std::make_shared<Cc::FingerprintInputs const>(std::move(record)) };
}

/// @param toolchains What one node serves.
/// @return Its publication.
[[nodiscard]] std::shared_ptr<ServedInputs const> Survey(std::vector<ServedToolchainInputs> toolchains)
{
    return std::make_shared<ServedInputs const>(std::move(toolchains));
}

/// Run `toolchain_diff` with @p arguments against @p fleet.
/// @param fleet The nodes.
/// @param upstream The upstream answering through @p fleet; its calls are what a case reads back.
/// @param arguments The arguments, as JSON text.
/// @return What it answered.
[[nodiscard]] Mcp::ToolResult CallDiff(McpFakes::RespondingUpstream& upstream, std::string_view arguments)
{
    static McpFakes::FixedCluster const cluster { McpFakes::StateOf({ SelfId, "node-b" }) };
    auto const tools = ToolchainTools(upstream, cluster, std::string { SelfId });
    auto const found = std::ranges::find(tools, std::string_view { "toolchain_diff" }, &Mcp::ToolRow::name);
    REQUIRE(found != tools.end());
    auto const args = Json::Parse(arguments);
    REQUIRE(args.has_value());
    return found->handler(Mcp::ToolCall { .arguments = Unwrap(args), .clientName = "test" });
}

/// @param fleet The nodes.
/// @return An upstream answering through them.
[[nodiscard]] McpFakes::RespondingUpstream UpstreamOf(DiffFleet& fleet)
{
    return McpFakes::RespondingUpstream { [&fleet](McpFakes::AskedCall const& call) {
        auto result = fleet.Answer(call);
        if (fleet.afterAsked)
            fleet.afterAsked(0);
        return result;
    } };
}

/// @param value A group: `{count, listed, paths}`.
/// @return Its count.
[[nodiscard]] std::int64_t CountOf(Json::Value const& value)
{
    auto const count = At(value, "count").AsInt();
    REQUIRE(count.has_value());
    return Unwrap(count);
}

} // namespace

TEST_CASE("toolchain_diff of one node's fingerprint against itself is identical, and the recompute passed",
          "[pace-node][mcp][toolchain-diff]")
{
    DiffFleet fleet;
    auto const record = DiffRecord(10);
    auto const fingerprint = Cc::ComputeToolchainFingerprint(record);
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(record) }), .version = "1.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result =
        CallDiff(upstream, std::format(R"({{"a":{{"fingerprint":"{0}"}},"b":{{"fingerprint":"{0}"}}}})", fingerprint));
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    auto const& out = result.structured;
    CHECK(At(out, "identical").AsBool() == std::optional { true });
    CHECK(At(out, "same_fingerprint").AsBool() == std::optional { true });
    CHECK(TextAt(out, "recompute") == "passed");
    CHECK(At(out, "complete").AsBool() == std::optional { true });
    CHECK(At(out, "skew").IsNull());
    CHECK(At(out, "banner").IsNull());
    CHECK(CountOf(At(out, "only_a")) == 0);
    CHECK(At(out, "inferred").AsBool() == std::optional { false });
    CHECK(TextAt(At(out, "a"), "version") == "1.0.0");
    CHECK(At(At(out, "a"), "total_files").AsInt() == std::optional<std::int64_t> { 10 });
    CHECK(TextAt(out, "verdict").value_or("").starts_with("the inputs are identical"));

    // Every frame is a FINGERPRINT-INPUTS this tool built, put to this node: ten files at three a page is four pages a
    // side.
    CHECK(upstream.Calls().size() == 8);
    for (auto const& call: upstream.Calls())
    {
        CHECK(call.target == UpstreamTarget::Node);
        CHECK(call.nodeIdOrEmpty.empty());
    }
}

TEST_CASE("toolchain_diff pages two nodes across cursor boundaries and loses nothing", "[pace-node][mcp][toolchain-diff]")
{
    // Seventeen files at three a page, so every boundary is crossed; the differences sit on both sides of several.
    DiffFleet fleet;
    auto const mine = DiffRecord(17);
    auto theirs = DiffRecord(17, "gcc (GCC) 13.2.0", { 0, 2, 3, 8, 16 });
    theirs.files.push_back(Cc::ToolchainFile { .relativePath = "c++/13/bits/zz_extra.h", .contentHash = "e1" });
    theirs = Cc::CaptureFingerprintInputs(theirs.banner, theirs.driverGrammar, std::move(theirs.files));
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(mine) }), .version = "1.0.0" };
    fleet.members["node-b"] = DiffFleet::Member { .served = Survey({ Served(theirs) }), .version = "1.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result = CallDiff(upstream,
                                 std::format(R"({{"a":{{"fingerprint":"{}"}},"b":{{"node":"node-b","fingerprint":"{}"}}}})",
                                             Cc::ComputeToolchainFingerprint(mine),
                                             Cc::ComputeToolchainFingerprint(theirs)));
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    auto const& out = result.structured;
    CHECK(TextAt(out, "recompute") == "passed");
    CHECK(At(out, "identical").AsBool() == std::optional { false });
    CHECK(CountOf(At(out, "content_differs")) == 5);
    CHECK(CountOf(At(out, "only_b")) == 1);
    CHECK(CountOf(At(out, "only_a")) == 0);
    auto const& paths = Elements(At(At(out, "content_differs"), "paths"));
    REQUIRE(paths.size() == 5);
    CHECK(*paths.front().AsString() == "c++/13/bits/h000.h");
    CHECK(*paths.back().AsString() == "c++/13/bits/h016.h");
    CHECK(At(out, "inferred").AsBool() == std::optional { true });
    CHECK(TextAt(out, "verdict").value_or("").contains("inferred: likely a different libstdc++ package build"));
    CHECK(TextAt(At(out, "b"), "node") == "node-b");
}

TEST_CASE("toolchain_diff refuses stale inputs", "[pace-node][mcp][toolchain-diff]")
{
    // A record filed under a fingerprint it does not reproduce: the survey moved after the digest was registered.
    DiffFleet fleet;
    auto const record = DiffRecord(5);
    auto const stale = std::string { "00000000000000000000000000000abc" };
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(record, stale) }), .version = "1.0.0" };
    fleet.members["node-b"] = DiffFleet::Member { .served = Survey({ Served(record) }), .version = "1.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result = CallDiff(upstream,
                                 std::format(R"({{"a":{{"fingerprint":"{}"}},"b":{{"node":"node-b","fingerprint":"{}"}}}})",
                                             stale,
                                             Cc::ComputeToolchainFingerprint(record)));
    CHECK(result.isError);
    CHECK(result.text == "this node's inputs no longer reproduce its fingerprint; its toolchain survey changed");
}

TEST_CASE("toolchain_diff refuses to stitch two surveys together", "[pace-node][mcp][toolchain-diff]")
{
    // The survey re-runs between two pages: the next page carries a different banner over the SAME number of files,
    // so the pages would add up and the stitched record would even recompute -- only holding every page to the first
    // one's identity refuses it rather than merging two surveys.
    DiffFleet fleet;
    auto const before = DiffRecord(10);
    auto const after = DiffRecord(10, "gcc (GCC) 13.3.0");
    auto const fingerprint = Cc::ComputeToolchainFingerprint(before);
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(before) }), .version = "1.0.0" };
    auto asked = std::size_t { 0 };
    fleet.afterAsked = [&](std::size_t /*unused*/) {
        if (++asked == 1)
            fleet.members[""].served = Survey({ Served(after, fingerprint) });
    };
    auto upstream = UpstreamOf(fleet);

    auto const result =
        CallDiff(upstream, std::format(R"({{"a":{{"fingerprint":"{0}"}},"b":{{"fingerprint":"{0}"}}}})", fingerprint));
    CHECK(result.isError);
    // Both causes, since this end cannot tell them apart: a survey that ran again, or a node whose pages do not add up.
    CHECK(result.text.contains("its toolchain survey ran again"));
    CHECK(result.text.contains("pages that do not add up"));
}

TEST_CASE("toolchain_diff recomputes whatever the version strings say, and refuses the one side that fails as stale",
          "[pace-node][mcp][toolchain-diff]")
{
    // Identical inputs, one side reproducing and one not, on two builds: the recompute runs on BOTH (the schema is
    // this build's), the failing side is refused as stale, and the other cause the comparison cannot rule out -- a
    // digest change without a schema bump -- is named, labelled as an inference, with both versions.
    DiffFleet fleet;
    auto const record = DiffRecord(4);
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(record) }), .version = "1.0.0" };
    fleet.members["node-b"] =
        DiffFleet::Member { .served = Survey({ Served(record, "ffffffffffffffffffffffffffffffff") }), .version = "2.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result = CallDiff(upstream,
                                 std::format(R"({{"a":{{"fingerprint":"{}"}},"b":{{"node":"node-b","fingerprint":"{}"}}}})",
                                             Cc::ComputeToolchainFingerprint(record),
                                             "ffffffffffffffffffffffffffffffff"));
    INFO(result.text);
    CHECK(result.isError);
    CHECK(result.text.starts_with("node-b's inputs no longer reproduce its fingerprint; its toolchain survey changed"));
    CHECK(result.text.contains("inferred: or the two pace-node builds digest differently without a schema bump"));
    CHECK(result.text.contains("pace-node 1.0.0"));
    CHECK(result.text.contains("pace-node 2.0.0"));
}

TEST_CASE("toolchain_diff reports a skew where both sides fail on identical inputs", "[pace-node][mcp][toolchain-diff]")
{
    // This node reproduces neither fingerprint from ONE set of inputs: it is this build's digest that differs, so it is
    // a skew -- reported naming both versions and both schemas -- and neither survey is blamed.
    DiffFleet fleet;
    auto const record = DiffRecord(4);
    fleet.members[""] =
        DiffFleet::Member { .served = Survey({ Served(record, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee") }), .version = "1.0.0" };
    fleet.members["node-b"] =
        DiffFleet::Member { .served = Survey({ Served(record, "ffffffffffffffffffffffffffffffff") }), .version = "2.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result = CallDiff(upstream,
                                 R"({"a":{"fingerprint":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"},)"
                                 R"("b":{"node":"node-b","fingerprint":"ffffffffffffffffffffffffffffffff"}})");
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    auto const skew = TextAt(result.structured, "skew");
    REQUIRE(skew.has_value());
    CHECK(Unwrap(skew).contains("version skew"));
    CHECK(Unwrap(skew).contains("pace-node 1.0.0"));
    CHECK(Unwrap(skew).contains("pace-node 2.0.0"));
    CHECK(Unwrap(skew).contains(std::string { Cc::FingerprintSchema }));
    CHECK(TextAt(result.structured, "recompute") == "failed");
    CHECK(At(result.structured, "identical").AsBool() == std::optional { false });
    CHECK(At(result.structured, "inferred").AsBool() == std::optional { false });
}

TEST_CASE("toolchain_diff refuses a side digested under another schema, naming both builds",
          "[pace-node][mcp][toolchain-diff]")
{
    DiffFleet fleet;
    auto const record = DiffRecord(4);
    auto foreign = record;
    foreign.schema = "toolchain-v2";
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(record) }), .version = "1.0.0" };
    fleet.members["node-b"] = DiffFleet::Member { .served = Survey({ Served(foreign) }), .version = "0.9.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result = CallDiff(upstream,
                                 std::format(R"({{"a":{{"fingerprint":"{}"}},"b":{{"node":"node-b","fingerprint":"{}"}}}})",
                                             Cc::ComputeToolchainFingerprint(record),
                                             Cc::ComputeToolchainFingerprint(foreign)));
    CHECK(result.isError);
    CHECK(result.text.contains("schema skew"));
    CHECK(result.text.contains("pace-node 1.0.0 under fingerprint schema toolchain-v3"));
    CHECK(result.text.contains("pace-node 0.9.0 under toolchain-v2"));
}

TEST_CASE("toolchain_diff resolves a toolchain by label or compiler path, exactly, and lists the candidates otherwise",
          "[pace-node][mcp][toolchain-diff]")
{
    DiffFleet fleet;
    auto const gcc = DiffRecord(3);
    auto const gxx = DiffRecord(3, "g++ (GCC) 13.2.0");
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(gcc, {}, "gcc 13.2.0", "/usr/bin/gcc"),
                                                               Served(gxx, {}, "gcc 13.2.0", "/usr/bin/g++") }),
                                            .version = "1.0.0" };
    auto upstream = UpstreamOf(fleet);

    SECTION("a compiler path names one")
    {
        auto const result = CallDiff(upstream, R"({"a":{"toolchain":"/usr/bin/g++"},"b":{"toolchain":"/usr/bin/gcc"}})");
        INFO(result.text);
        REQUIRE_FALSE(result.isError);
        CHECK(TextAt(At(result.structured, "a"), "fingerprint") == Cc::ComputeToolchainFingerprint(gxx));
        CHECK(TextAt(At(result.structured, "a"), "toolchain") == "/usr/bin/g++");
        CHECK(At(result.structured, "banner").AsObject() != nullptr);
    }
    SECTION("a label two toolchains carry is ambiguous, and both are listed")
    {
        auto const result = CallDiff(upstream, R"({"a":{"toolchain":"gcc 13.2.0"},"b":{"toolchain":"/usr/bin/gcc"}})");
        CHECK(result.isError);
        CHECK(result.text.contains("ambiguous"));
        CHECK(result.text.contains("/usr/bin/gcc"));
        CHECK(result.text.contains("/usr/bin/g++"));
    }
    SECTION("a name nothing carries lists what is served")
    {
        auto const result = CallDiff(upstream, R"({"a":{"toolchain":"gcc"},"b":{"toolchain":"/usr/bin/gcc"}})");
        CHECK(result.isError);
        CHECK(result.text.contains("names no toolchain"));
        CHECK(result.text.contains(Cc::ComputeToolchainFingerprint(gcc)));
    }
}

TEST_CASE("toolchain_diff refuses a fingerprint the node does not serve, and malformed sides",
          "[pace-node][mcp][toolchain-diff]")
{
    DiffFleet fleet;
    auto const record = DiffRecord(3);
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(record) }), .version = "1.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const missing = CallDiff(upstream,
                                  std::format(R"({{"a":{{"fingerprint":"{}"}},"b":{{"fingerprint":"nope"}}}})",
                                              Cc::ComputeToolchainFingerprint(record)));
    CHECK(missing.isError);
    CHECK(missing.text.contains("refused"));
    CHECK(missing.text.contains("serves no toolchain with fingerprint nope"));

    for (auto const* arguments: { R"({"a":{},"b":{"fingerprint":"x"}})",
                                  R"({"a":{"fingerprint":"x","toolchain":"y"},"b":{"fingerprint":"x"}})",
                                  R"({"a":"x","b":{"fingerprint":"x"}})",
                                  R"({"a":{"node":"nobody","fingerprint":"x"},"b":{"fingerprint":"x"}})" })
    {
        INFO(arguments);
        CHECK(CallDiff(upstream, arguments).isError);
    }
}

namespace
{

/// @param call A FINGERPRINT-INPUTS call.
/// @return The request it carried; the case fails when it carried anything else.
[[nodiscard]] Wire::FingerprintInputsRequest InputsAsked(McpFakes::AskedCall const& call)
{
    auto const frame = std::as_bytes(std::span { call.frame.data(), call.frame.size() });
    auto request = Wire::DecodeFingerprintInputsRequest(frame.subspan(Wire::RequestHeaderSize));
    REQUIRE(request.has_value());
    return Unwrap(request);
}

/// @param page A page.
/// @return It, answered `Ok` as the upstream reports it.
[[nodiscard]] UpstreamResult Answered(Wire::FingerprintInputsPage const& page)
{
    return UpstreamReply { .status = Wire::Status::Ok,
                           .payload = McpFakes::AsUpstreamText(Wire::EncodeFingerprintInputsPage(page)),
                           .answeredBy = "self:6674",
                           .term = TermHere };
}

/// A node claiming far more files than it ever finishes, answering a page per file: one path of @p pathBytes at the
/// cursor asked, and the next cursor one further on -- every page well-formed, the record never ending.
/// @param pathBytes How long each page's one path is.
/// @return What answers every call.
[[nodiscard]] McpFakes::RespondingUpstream::Responder EndlessPages(std::size_t pathBytes)
{
    return [pathBytes](McpFakes::AskedCall const& call) {
        auto const cursor = InputsAsked(call).cursor;
        return Answered(Wire::FingerprintInputsPage {
            .fingerprint = "0123456789abcdef0123456789abcdef",
            .version = "1.0.0",
            .cursor = cursor,
            .totalFiles = 1000,
            .nextCursor = cursor + 1,
            .inputs = Cc::FingerprintInputs {
                .schema = std::string { Cc::FingerprintSchema },
                .banner = "gcc 13",
                .driverGrammar = "gnu",
                .files = { Cc::ToolchainFile { .relativePath = std::string(pathBytes, 'p'), .contentHash = "00" } } } });
    };
}

/// The arguments asking the endless node about one fingerprint on both sides.
constexpr std::string_view EndlessArguments = R"({"a":{"fingerprint":"0123456789abcdef0123456789abcdef"},)"
                                              R"("b":{"fingerprint":"0123456789abcdef0123456789abcdef"}})";

} // namespace

TEST_CASE("toolchain_diff stops a node that never ends its record at the page bound, and says so",
          "[pace-node][mcp][toolchain-diff]")
{
    // Small paths, so the PAGE bound is what stops it: 32 asks a side, every file dropped, reported incomplete.
    auto upstream = McpFakes::RespondingUpstream { EndlessPages(64U * 1024U) };
    auto const result = CallDiff(upstream, EndlessArguments);
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    CHECK(upstream.Calls().size() == 2 * McpToolchainDiff::MaxDiffPagesPerSide);
    CHECK(At(result.structured, "complete").AsBool() == std::optional { false });
    CHECK(TextAt(result.structured, "incomplete").value_or("").contains("had not ended after 32 pages"));
    CHECK(At(result.structured, "only_a").IsNull());
    CHECK(At(result.structured, "inferred").AsBool() == std::optional { false });
}

TEST_CASE("toolchain_diff stops a node at the byte bound before the page bound when its pages are huge",
          "[pace-node][mcp][toolchain-diff]")
{
    // 2.5 MiB a page: the 64 MiB a side is passed within 26 pages, well before the 32-page bound.
    static_assert((McpToolchainDiff::MaxDiffBytesPerSide / (std::size_t { 5 } * 512U * 1024U))
                  < McpToolchainDiff::MaxDiffPagesPerSide);
    auto upstream = McpFakes::RespondingUpstream { EndlessPages(std::size_t { 5 } * 512U * 1024U) };
    auto const result = CallDiff(upstream, EndlessArguments);
    INFO(result.text.substr(0, 400));
    REQUIRE_FALSE(result.isError);
    CHECK(upstream.Calls().size() < 2 * McpToolchainDiff::MaxDiffPagesPerSide);
    CHECK(TextAt(result.structured, "incomplete").value_or("").contains("bytes this tool reads per side"));
}

TEST_CASE("toolchain_diff compares only the identity when ONE side is incomplete, and says the files were not compared",
          "[pace-node][mcp][toolchain-diff]")
{
    // Side a claims too many files; side b is a real, complete record with the same banner. Neither side's files are
    // compared -- b's list must not be set against an empty a and read as "only on b" -- and nothing is inferred.
    DiffFleet fleet;
    auto const record = DiffRecord(5, "gcc 13");
    fleet.members["node-b"] = DiffFleet::Member { .served = Survey({ Served(record) }), .version = "1.0.0" };
    auto const tooMany = Wire::FingerprintInputsPage {
        .fingerprint = "0123456789abcdef0123456789abcdef",
        .version = "1.0.0",
        .cursor = 0,
        .totalFiles = McpToolchainDiff::MaxDiffFiles + 1,
        .nextCursor = 1,
        .inputs = Cc::FingerprintInputs { .schema = std::string { Cc::FingerprintSchema },
                                          .banner = "gcc 13",
                                          .driverGrammar = "gnu",
                                          .files = { Cc::ToolchainFile { .relativePath = "a.h", .contentHash = "00" } } },
    };
    auto upstream = McpFakes::RespondingUpstream { [&](McpFakes::AskedCall const& call) {
        return call.nodeIdOrEmpty.empty() ? Answered(tooMany) : fleet.Answer(call);
    } };
    auto const result = CallDiff(upstream,
                                 std::format(R"({{"a":{{"fingerprint":"0123456789abcdef0123456789abcdef"}},)"
                                             R"("b":{{"node":"node-b","fingerprint":"{}"}}}})",
                                             Cc::ComputeToolchainFingerprint(record)));
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    CHECK(At(result.structured, "complete").AsBool() == std::optional { false });
    CHECK(TextAt(result.structured, "recompute") == "not-run");
    CHECK(At(result.structured, "inferred").AsBool() == std::optional { false });
    CHECK(At(result.structured, "identical").AsBool() == std::optional { false });
    CHECK(At(result.structured, "only_b").IsNull());
    CHECK(TextAt(result.structured, "verdict")
          == "same schema, banner and grammar; the include-tree files were not compared, so whether they agree is not "
             "known");
    CHECK(TextAt(result.structured, "incomplete").value_or("").contains("200000"));
}

TEST_CASE("toolchain_diff compares only the identity when BOTH sides are too large to fetch, and says so",
          "[pace-node][mcp][toolchain-diff]")
{
    // A page claiming more files than the tool fetches: one page is read, the files are not compared, and nothing is
    // recomputed -- said, never presented as a complete comparison.
    auto const page = Wire::FingerprintInputsPage {
        .fingerprint = "0123456789abcdef0123456789abcdef",
        .version = "1.0.0",
        .cursor = 0,
        .totalFiles = McpToolchainDiff::MaxDiffFiles + 1,
        .nextCursor = 1,
        .inputs = Cc::FingerprintInputs { .schema = std::string { Cc::FingerprintSchema },
                                          .banner = "gcc 13",
                                          .driverGrammar = "gnu",
                                          .files = { Cc::ToolchainFile { .relativePath = "a.h", .contentHash = "00" } } },
    };
    auto upstream = McpFakes::RespondingUpstream { [&page](McpFakes::AskedCall const& /*call*/) -> UpstreamResult {
        return UpstreamReply { .status = Wire::Status::Ok,
                               .payload = McpFakes::AsUpstreamText(Wire::EncodeFingerprintInputsPage(page)),
                               .answeredBy = "self:6674",
                               .term = TermHere };
    } };
    auto const result = CallDiff(upstream,
                                 R"({"a":{"fingerprint":"0123456789abcdef0123456789abcdef"},)"
                                 R"("b":{"fingerprint":"0123456789abcdef0123456789abcdef"}})");
    INFO(result.text);
    REQUIRE_FALSE(result.isError);
    CHECK(At(result.structured, "complete").AsBool() == std::optional { false });
    CHECK(TextAt(result.structured, "recompute") == "not-run");
    CHECK(TextAt(result.structured, "incomplete").value_or("").contains("200000"));
    CHECK(At(result.structured, "only_a").IsNull());
    CHECK(At(result.structured, "identical").AsBool() == std::optional { false });
    CHECK(At(result.structured, "inferred").AsBool() == std::optional { false });
    CHECK(TextAt(result.structured, "verdict").value_or("").contains("the include-tree files were not compared"));
    // Both sides' reasons, not only the first's.
    CHECK(TextAt(result.structured, "incomplete").value_or("").contains("; and "));
    // One page a side, and no more.
    CHECK(upstream.Calls().size() == 2);
}

TEST_CASE("toolchain_diff cuts a long listed path and says how long it was", "[pace-node][mcp][toolchain-diff]")
{
    DiffFleet fleet;
    auto const mine = DiffRecord(2);
    auto longer = DiffRecord(2);
    longer.files.push_back(Cc::ToolchainFile { .relativePath = "c++/13/" + std::string(2000, 'x'), .contentHash = "e1" });
    longer = Cc::CaptureFingerprintInputs(longer.banner, longer.driverGrammar, std::move(longer.files));
    fleet.members[""] = DiffFleet::Member { .served = Survey({ Served(mine) }), .version = "1.0.0" };
    fleet.members["node-b"] = DiffFleet::Member { .served = Survey({ Served(longer) }), .version = "1.0.0" };
    auto upstream = UpstreamOf(fleet);

    auto const result = CallDiff(upstream,
                                 std::format(R"({{"a":{{"fingerprint":"{}"}},"b":{{"node":"node-b","fingerprint":"{}"}}}})",
                                             Cc::ComputeToolchainFingerprint(mine),
                                             Cc::ComputeToolchainFingerprint(longer)));
    INFO(result.text.substr(0, 300));
    REQUIRE_FALSE(result.isError);
    auto const& paths = Elements(At(At(result.structured, "only_b"), "paths"));
    REQUIRE(paths.size() == 1);
    auto const listed = *paths.front().AsString();
    CHECK(listed.size() < McpToolchainDiff::MaxListedPathBytes + 64);
    CHECK(listed.ends_with("... (2007 bytes in all)"));
}
