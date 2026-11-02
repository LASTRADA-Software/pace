// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "EnrollmentWindow.hpp"
#include "McpFleetTools.hpp"
#include "McpToolSupport.hpp"

#include <Pace/Cluster/Roster.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/NumericText.hpp>
#include <Pace/Distributed/FleetChart.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;
    using Distributed::CellFormat;

    using namespace McpSupport;

    /// Ask the leader one leader-gated frame, and refuse a `Miss`, which neither verb asked here answers.
    /// @param upstream How the leader is reached.
    /// @param frame The request.
    /// @param verb What the request is called, for the refusal.
    /// @return The reply, or the tool error to answer.
    [[nodiscard]] std::expected<UpstreamReply, Mcp::ToolResult> AskLeader(IMcpUpstream& upstream,
                                                                          std::vector<std::byte> const& frame,
                                                                          std::string_view verb)
    {
        auto answer = upstream.Ask(UpstreamTarget::Leader, {}, FrameText(frame));
        if (!answer.has_value())
            return std::unexpected { UpstreamError(answer.error()) };
        if (answer->status != Wire::Status::Ok)
            return std::unexpected { Error(
                std::format("{} answered {} with a miss, which that verb never answers; nothing it said is reported",
                            answer->answeredBy,
                            verb)) };
        return std::move(*answer);
    }

} // namespace

// ------------------------------------------------------------ the fleet document

std::expected<McpFleet::FleetDocument, Mcp::ToolResult> McpFleet::AskFleetDocument(IMcpUpstream& upstream,
                                                                                   std::string_view range)
{
    // No section, which a Json read refuses. The range is the day `/fleet.json` defaults to unless one is named: only
    // the `kpi` figures and the keyed histories read it.
    auto reply = AskLeader(upstream,
                           Wire::EncodeFleetTextRequest(Wire::FleetTextRequest { .range = std::string { range },
                                                                                 .format = Wire::FleetTextFormat::Json }),
                           "fleet-text");
    if (!reply.has_value())
        return std::unexpected { std::move(reply.error()) };

    auto parsed = Json::Parse(reply->payload, McpFleet::FleetDocumentLimits);
    if (!parsed.has_value())
        return std::unexpected { Error(
            std::format("the fleet document {} answered did not parse (error {} at byte {}); nothing it said is reported",
                        reply->answeredBy,
                        static_cast<unsigned>(parsed.error().code),
                        parsed.error().offset)) };

    auto const* const schema = parsed->Find("schema");
    auto const generation = schema != nullptr ? schema->AsInt() : std::nullopt;
    if (generation != std::optional<std::int64_t> { Distributed::FleetJsonSchema })
        return std::unexpected { Error(
            std::format("{} serves fleet document schema {}, and this node reads schema {}; nothing it said is reported -- "
                        "run the same pace-node build on both",
                        reply->answeredBy,
                        generation.has_value() ? std::format("{}", *generation) : std::string { "(none)" },
                        Distributed::FleetJsonSchema)) };

    return FleetDocument { .reply = std::move(*reply), .root = std::move(*parsed) };
}

namespace
{
    using McpFleet::AskFleetDocument;
    using McpFleet::FleetDocument;

    /// How one fleet-document array's columns read: the fleet tables' answer for its rows.
    using ColumnFormatOf = std::function<std::optional<CellFormat>(std::string_view)>;

    /// @param section The fleet section whose table holds the array's columns.
    /// @return The lookup for that section.
    [[nodiscard]] ColumnFormatOf SectionFormats(Distributed::FleetSection section)
    {
        return [section](std::string_view name) {
            return Distributed::FleetColumnFormat(section, name);
        };
    }

    /// @param kebab A fleet document key.
    /// @return The same key in snake_case.
    [[nodiscard]] std::string SnakeCase(std::string_view kebab)
    {
        auto snake = std::string { kebab };
        std::ranges::replace(snake, '-', '_');
        return snake;
    }

    /// Project one fleet-document row: keys in snake_case, every milliseconds column in seconds as `<key>_s`.
    ///
    /// @param row A row object.
    /// @param formatOf How its columns read.
    /// @return The projected row, or why a cell could not be read.
    [[nodiscard]] std::expected<Json::Value, std::string> ProjectRow(Json::Value const& row, ColumnFormatOf const& formatOf)
    {
        auto const* const members = row.AsObject();
        if (members == nullptr)
            return std::unexpected { std::string { "a row that is not an object" } };
        auto projected = Json::Object {};
        projected.reserve(members->size());
        for (auto const& [key, value]: *members)
        {
            if (formatOf(key) != CellFormat::Millis)
            {
                projected.emplace_back(SnakeCase(key), value);
                continue;
            }
            auto const millis = value.AsInt();
            if (!value.IsNull() && !millis.has_value())
                return std::unexpected { std::format("a duration \"{}\" that is not whole milliseconds", key) };
            projected.emplace_back(SnakeCase(key) + "_s",
                                   millis.has_value() ? Real(static_cast<double>(*millis) / 1000.0) : Null());
        }
        return Obj(std::move(projected));
    }

    /// Project a fleet-document array, or keep its `null`.
    /// @param root The document.
    /// @param key The array's key.
    /// @param formatOf How its rows' columns read.
    /// @return The projected array, `null` where the document has `null`, or why it could not be read.
    [[nodiscard]] std::expected<Json::Value, std::string> ProjectRows(Json::Value const& root,
                                                                      std::string_view key,
                                                                      ColumnFormatOf const& formatOf)
    {
        auto const* const member = root.Find(key);
        if (member == nullptr)
            return std::unexpected { std::format("no \"{}\"", key) };
        if (member->IsNull())
            return Null();
        auto const* const rows = member->AsArray();
        if (rows == nullptr)
            return std::unexpected { std::format("a \"{}\" that is neither a list nor null", key) };
        auto projected = Json::Array {};
        projected.reserve(rows->size());
        for (auto const& row: *rows)
        {
            auto one = ProjectRow(row, formatOf);
            if (!one.has_value())
                return std::unexpected { std::format("in \"{}\", {}", key, one.error()) };
            projected.push_back(std::move(*one));
        }
        return Arr(std::move(projected));
    }

    /// @param document The document a projection failed on.
    /// @param why What could not be read.
    /// @return The tool error saying so.
    [[nodiscard]] Mcp::ToolResult Unreadable(FleetDocument const& document, std::string const& why)
    {
        return Error(std::format(
            "the fleet document {} answered carries {}; nothing it said is reported", document.reply.answeredBy, why));
    }

    /// @param value A projected array, or `null`.
    /// @return Its elements; empty for `null`.
    [[nodiscard]] Json::Array const& ElementsOf(Json::Value const& value)
    {
        static Json::Array const none;
        auto const* const elements = value.AsArray();
        return elements != nullptr ? *elements : none;
    }

    /// Name every machine row of a projected array, in place, as every tool names a machine (`AppendNaming`).
    /// @param rows A projected array, or `null`.
    /// @param state The applied state the machines are placed in.
    /// @param namingOf How one row's machine is named.
    /// @param prefix Prepended to the naming keys; empty when the row is about the machine.
    /// @return How many rows the state could not place.
    template <typename NamingOf>
    std::size_t NameRows(Json::Value& rows,
                         std::optional<Cluster::ClusterState> const& state,
                         NamingOf namingOf,
                         std::string_view prefix = {})
    {
        auto* const elements = std::get_if<Json::Array>(&rows.v);
        if (elements == nullptr)
            return 0;
        auto unplaced = std::size_t { 0 };
        auto const idKey = std::format("{}node_id", prefix);
        auto const nameKey = std::format("{}display_name", prefix);
        for (auto& row: *elements)
            if (auto* const members = std::get_if<Json::Object>(&row.v); members != nullptr)
            {
                auto const naming = namingOf(state, row);
                unplaced += Unplaced(naming) ? 1U : 0U;
                // A row may already carry the leader's own `node_id` cell (`node-id`, projected): the naming READ it,
                // and replaces it rather than standing beside it as a second key of the same name.
                std::erase_if(*members, [&idKey, &nameKey](auto const& member) {
                    return member.first == idKey || member.first == nameKey;
                });
                AppendNaming(*members, naming, prefix);
            }
        return unplaced;
    }

    /// A machine row named from ITSELF: the leader's `node_id` cell (the member holding the key the machine PROVED) and,
    /// as that member's display name, the row's own `name` when its `name_source` says `replicated` -- one document,
    /// one state, so a lagging node never prints the leader's name beside an older or absent one of its own (the rule
    /// `node_status` follows for its two fields). Never by the endpoint the row also carries, which is a claim.
    /// @param row A projected machine row.
    /// @return The machine's naming.
    [[nodiscard]] MachineNaming NamingOfFleetRow(Json::Value const& row)
    {
        auto id = StringAt(row, "node_id");
        if (!id.has_value() || id->empty())
            return MachineNaming {};
        auto const replicated = StringAt(row, "name_source") == std::optional<std::string> { "replicated" };
        return MachineNaming { .nodeId = std::move(id), .displayName = replicated ? StringAt(row, "name") : std::nullopt };
    }

    /// @param row A machine row of the fleet document.
    /// @return Its naming, from the row alone (`NamingOfFleetRow`); the applied state is not consulted.
    [[nodiscard]] MachineNaming ByNodeId(std::optional<Cluster::ClusterState> const& /*state*/, Json::Value const& row)
    {
        return NamingOfFleetRow(row);
    }

    /// @param state The applied state.
    /// @param row A row carrying the requester's `requester_key`.
    /// @return The requesting machine's naming, by that key.
    [[nodiscard]] MachineNaming ByRequesterKey(std::optional<Cluster::ClusterState> const& state, Json::Value const& row)
    {
        return NamingOfKeyText(state, StringAt(row, "requester_key").value_or(std::string {}));
    }

    // ------------------------------------------------------------ fleet_overview

    /// Whether the fleet's machines run more than one build.
    struct VersionSkew
    {
        std::vector<std::string> known;   ///< Every version a machine reported, once each, sorted.
        std::vector<std::string> unknown; ///< The machines that reported none.
    };

    /// @param nodes The projected `nodes` rows.
    /// @return The versions, and the machines that said none.
    [[nodiscard]] VersionSkew VersionsOf(Json::Array const& nodes)
    {
        auto known = std::set<std::string> {};
        auto skew = VersionSkew {};
        for (auto const& node: nodes)
        {
            if (auto version = StringAt(node, "version"); version.has_value())
                known.insert(std::move(*version));
            else
                skew.unknown.push_back(StringAt(node, "endpoint").value_or(std::string {}));
        }
        skew.known.assign(known.begin(), known.end());
        return skew;
    }

    /// The mixed-version flag: true once two builds are seen, false only when every machine said the same one,
    /// and `null` when that cannot be told -- a machine that reported no version, or no machine at all.
    /// @param skew The versions.
    /// @return The flag.
    [[nodiscard]] Json::Value SkewFlag(VersionSkew const& skew)
    {
        if (skew.known.size() > 1)
            return Bool(true);
        if (skew.known.size() == 1 && skew.unknown.empty())
            return Bool(false);
        return Null();
    }

    /// @param skew The mixed-version flag, or nothing when it cannot be told.
    /// @return How the text says it.
    [[nodiscard]] std::string_view SkewWord(std::optional<bool> skew) noexcept
    {
        if (!skew.has_value())
            return "cannot be told";
        return *skew ? "MIXED" : "uniform";
    }

    /// Who leads, in words: the document names the leader only from a node that follows it, so a document
    /// whose `role` is `leader` and whose `leader` is `null` was rendered BY the leader -- which is who answered.
    /// @param leader The document's `leader`.
    /// @param role The document's `role`.
    /// @param reply Who answered.
    /// @return The words.
    [[nodiscard]] std::string LeaderWords(std::optional<std::string> const& leader,
                                          std::optional<std::string> const& role,
                                          UpstreamReply const& reply)
    {
        if (leader.has_value())
            return *leader;
        if (role == std::optional<std::string> { "leader" })
            return std::format("{}, which answered", reply.answeredBy);
        return "nobody known";
    }

    /// @param strings Some strings.
    /// @return Them as a JSON array.
    [[nodiscard]] Json::Value Strings(std::vector<std::string> const& strings)
    {
        auto array = Json::Array {};
        array.reserve(strings.size());
        for (auto const& text: strings)
            array.push_back(Str(text));
        return Arr(std::move(array));
    }

    [[nodiscard]] Mcp::ToolResult FleetOverview(IMcpUpstream& upstream,
                                                IAppliedClusterView const& /*cluster*/,
                                                Mcp::ToolCall const& /*call*/)
    {
        auto document = AskFleetDocument(upstream);
        if (!document.has_value())
            return std::move(document.error());

        auto members = ProjectRows(document->root, "members", SectionFormats(Distributed::FleetSection::Members));
        auto nodes = ProjectRows(document->root, "nodes", SectionFormats(Distributed::FleetSection::Machines));
        if (!members.has_value())
            return Unreadable(*document, members.error());
        if (!nodes.has_value())
            return Unreadable(*document, nodes.error());

        // Every machine row named by its member id and display name, both from the row itself (`NamingOfFleetRow`):
        // the id the leader joined by the PROVEN key and the name its own applied state records for that member.
        auto const unplaced = NameRows(*nodes, std::nullopt, ByNodeId);
        auto const skew = VersionsOf(ElementsOf(*nodes));
        auto const flag = SkewFlag(skew);
        auto const leader = StringAt(document->root, "leader");
        auto const flagText = SkewWord(flag.AsBool());
        auto text =
            std::format("fleet led by {}: {} member(s), {} machine(s); versions {} ({}){}{}; {}",
                        LeaderWords(leader, StringAt(document->root, "role"), document->reply),
                        members->IsNull() ? std::string { "no cluster" } : std::format("{}", ElementsOf(*members).size()),
                        ElementsOf(*nodes).size(),
                        skew.known.empty() ? std::string { "none reported" } : std::format("{}", skew.known),
                        flagText,
                        skew.unknown.empty() ? std::string {} : std::format("; no version from {}", skew.unknown),
                        UnplacedNote(unplaced),
                        Provenance(document->reply));

        auto result = Envelope(document->reply);
        result.emplace_back("role", StrOrNull(StringAt(document->root, "role")));
        result.emplace_back("leader", StrOrNull(leader));
        result.emplace_back("members", std::move(*members));
        result.emplace_back("nodes", std::move(*nodes));
        result.emplace_back("versions", Strings(skew.known));
        result.emplace_back("versions_unreported", Strings(skew.unknown));
        result.emplace_back("version_skew", flag);
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ running_compiles

    /// What the compile list may be narrowed to.
    struct CompileFilter
    {
        std::optional<std::string> requester; ///< A requester's display name or identity key, exactly.
        std::optional<std::string> worker;    ///< A worker's id, or `local`, exactly.

        /// @param row A projected compile row.
        /// @return Whether it passes.
        [[nodiscard]] bool Admits(Json::Value const& row) const
        {
            if (requester.has_value() && StringAt(row, "requester") != requester
                && StringAt(row, "requester_key") != requester)
                return false;
            return !worker.has_value() || StringAt(row, "worker") == worker;
        }
    };

    /// @param arguments The call's arguments.
    /// @param key An optional string argument.
    /// @return Its value, or nothing when it was not given.
    [[nodiscard]] std::optional<std::string> OptionalArgument(Json::Value const& arguments, std::string_view key)
    {
        return StringAt(arguments, key);
    }

    /// The per-requester and per-worker counts over the LISTED rows.
    struct CompileCounts
    {
        Json::Array perRequester; ///< `{requester, requester_key, count}`, one per requesting machine.
        Json::Array perWorker;    ///< `{worker, count}`, one per worker running something.
        Json::Value oldestWait;   ///< The oldest queued row's age in seconds, or `null`.
    };

    /// @param rows The projected compile rows, oldest first.
    /// @return The counts.
    [[nodiscard]] CompileCounts CountCompiles(Json::Array const& rows, std::optional<Cluster::ClusterState> const& state)
    {
        // Keyed by display name AND key: two machines may share a display name, and merging them would be a
        // wrong count presented as a right one.
        auto requesters = std::map<std::pair<std::string, std::optional<std::string>>, std::size_t> {};
        auto workers = std::map<std::string, std::size_t> {};
        auto oldest = std::optional<double> {};
        for (auto const& row: rows)
        {
            ++requesters[{ StringAt(row, "requester").value_or(std::string {}), StringAt(row, "requester_key") }];
            if (auto worker = StringAt(row, "worker"); worker.has_value())
                ++workers[std::move(*worker)];
            auto const* const age = row.Find("age_s");
            if (StringAt(row, "state") == "queued" && age != nullptr)
                if (auto const* const seconds = std::get_if<double>(&age->v); seconds != nullptr)
                    oldest = std::max(oldest.value_or(*seconds), *seconds);
        }
        auto counts =
            CompileCounts { .perRequester = {}, .perWorker = {}, .oldestWait = oldest.has_value() ? Real(*oldest) : Null() };
        for (auto const& [who, count]: requesters)
        {
            auto row = Json::Object { { "requester", Str(who.first) }, { "requester_key", StrOrNull(who.second) } };
            AppendNaming(row, NamingOfKeyText(state, who.second.value_or(std::string {})));
            row.emplace_back("count", Count(count));
            counts.perRequester.push_back(Obj(std::move(row)));
        }
        for (auto const& [worker, count]: workers)
            counts.perWorker.push_back(Obj({ { "worker", Str(worker) }, { "count", Count(count) } }));
        return counts;
    }

    [[nodiscard]] Mcp::ToolResult RunningCompiles(IMcpUpstream& upstream,
                                                  IAppliedClusterView const& cluster,
                                                  Mcp::ToolCall const& call)
    {
        auto document = AskFleetDocument(upstream);
        if (!document.has_value())
            return std::move(document.error());

        auto compiles = ProjectRows(document->root, "compiles", Distributed::FleetCompileColumnFormat);
        if (!compiles.has_value())
            return Unreadable(*document, compiles.error());

        auto const filter = CompileFilter { .requester = OptionalArgument(call.arguments, "requester"),
                                            .worker = OptionalArgument(call.arguments, "worker") };
        auto result = Envelope(document->reply);

        // `null`: this node leads no queue -- a different claim from a queue with nothing in it.
        if (compiles->IsNull())
        {
            result.emplace_back("compiles", Null());
            result.emplace_back("compiles_truncated", Null());
            result.emplace_back("totals", Null());
            return Answer(std::move(result),
                          std::format("the node that answered leads no compile queue, so there is no list of "
                                      "compiles to report; {}",
                                      Provenance(document->reply)));
        }

        auto const* const totals = document->root.Find("compile-totals");
        auto const* const truncatedMember = document->root.Find("compiles-truncated");
        auto const truncated = truncatedMember != nullptr ? truncatedMember->AsInt() : std::nullopt;
        if (totals == nullptr || totals->AsObject() == nullptr || !truncated.has_value())
            return Unreadable(*document, "a compile list without its totals");

        // Each compile's requesting machine named by its member id and display name, placed by its identity key.
        auto const state = cluster.AppliedState();
        auto const unplaced = NameRows(*compiles, state, ByRequesterKey, "requester_");
        auto const& rows = ElementsOf(*compiles);
        auto counts = CountCompiles(rows, state);
        auto listed = Json::Array {};
        for (auto const& row: rows)
            if (filter.Admits(row))
                listed.push_back(row);

        // The totals count EVERY ticket, the unlisted ones included, keyed as the document keys them.
        auto totalsOut = Json::Object {};
        for (auto const& [state, count]: *totals->AsObject())
            totalsOut.emplace_back(SnakeCase(state), count);
        totalsOut.emplace_back("oldest_wait_s", counts.oldestWait);
        totalsOut.emplace_back("per_requester", Arr(std::move(counts.perRequester)));
        totalsOut.emplace_back("per_worker", Arr(std::move(counts.perWorker)));
        totalsOut.emplace_back("per_counts_cover_every_ticket", Bool(*truncated == 0));

        // `-` for a state the leader's totals do not carry, or carry as null: absent, never a zero.
        auto const countOf = [totals](std::string_view state) {
            auto const* const member = totals->Find(state);
            auto const count = member != nullptr ? member->AsInt() : std::nullopt;
            return count.has_value() ? std::format("{}", *count) : std::string { "-" };
        };
        auto text = std::format("{} running, {} queued, {} waiting; oldest queued wait {}; {} of {} listed row(s) "
                                "shown{}{}{}; {}",
                                countOf("running"),
                                countOf("queued"),
                                countOf("waiting"),
                                counts.oldestWait.IsNull() ? std::string { "none" }
                                                           : std::format("{:.1f} s", std::get<double>(counts.oldestWait.v)),
                                listed.size(),
                                rows.size(),
                                filter.requester.has_value() || filter.worker.has_value() ? " after the filter" : "",
                                *truncated == 0 ? std::string {}
                                                : std::format("; {} more ticket(s) past the list's ceiling", *truncated),
                                UnplacedNote(unplaced),
                                Provenance(document->reply));

        result.emplace_back("compiles", Arr(std::move(listed)));
        result.emplace_back("compiles_truncated", Int(*truncated));
        result.emplace_back("totals", Obj(std::move(totalsOut)));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ conditions

    [[nodiscard]] Mcp::ToolResult Conditions(IMcpUpstream& upstream,
                                             IAppliedClusterView const& /*cluster*/,
                                             Mcp::ToolCall const& /*call*/)
    {
        auto document = AskFleetDocument(upstream);
        if (!document.has_value())
            return std::move(document.error());

        auto rows = ProjectRows(document->root, "conditions", SectionFormats(Distributed::FleetSection::Conditions));
        auto nodes = ProjectRows(document->root, "nodes", SectionFormats(Distributed::FleetSection::Machines));
        if (!rows.has_value())
            return Unreadable(*document, rows.error());
        if (!nodes.has_value())
            return Unreadable(*document, nodes.error());

        // Every machine, in the document's order, with the rows it reported. A machine whose report carried no
        // list is ONE row with no condition (`RenderFleetJson`), which is `not_reported` -- absent, never a
        // machine with nothing raised. One whose list is empty has no row at all, and is a machine with none.
        auto order = std::vector<std::string> {};
        auto perMachine = std::map<std::string, Json::Array, std::less<>> {};
        auto unreported = std::set<std::string, std::less<>> {};
        auto const visit = [&order, &perMachine](std::string const& endpoint) -> Json::Array& {
            auto [entry, inserted] = perMachine.try_emplace(endpoint);
            if (inserted)
                order.push_back(endpoint);
            return entry->second;
        };
        // Each machine's member id as the LEADER joined its row, by the key the machine proved (`node-id`). The
        // endpoint is only this document's row key, which ties a condition row to its machine row; it never names a
        // member.
        auto namings = std::map<std::string, MachineNaming, std::less<>> {};
        for (auto const& node: ElementsOf(*nodes))
        {
            auto const endpoint = StringAt(node, "endpoint").value_or(std::string {});
            std::ignore = visit(endpoint);
            namings.try_emplace(endpoint, NamingOfFleetRow(node));
        }
        auto raised = std::size_t { 0 };
        for (auto const& row: ElementsOf(*rows))
        {
            auto const endpoint = StringAt(row, "endpoint").value_or(std::string {});
            auto& list = visit(endpoint);
            auto const* const condition = row.Find("condition");
            if (condition == nullptr || condition->IsNull())
            {
                unreported.insert(endpoint);
                continue;
            }
            if (StringAt(row, "state") == "raised")
                ++raised;
            auto fields = Json::Object {};
            for (auto const& [key, value]: *row.AsObject())
                if (key != "endpoint")
                    fields.emplace_back(key, value);
            list.push_back(Obj(std::move(fields)));
        }

        auto machines = Json::Array {};
        auto notReported = std::vector<std::string> {};
        auto raisedLines = std::string {};
        auto unplaced = std::size_t { 0 };
        for (auto const& endpoint: order)
        {
            if (unreported.contains(endpoint))
            {
                notReported.push_back(endpoint);
                continue;
            }
            for (auto const& condition: perMachine[endpoint])
                if (StringAt(condition, "state") == "raised")
                    raisedLines += std::format("\n  {} {} ({}, {}): {} -- {}",
                                               endpoint,
                                               StringAt(condition, "condition").value_or(std::string {}),
                                               StringAt(condition, "severity").value_or(std::string { "-" }),
                                               StringAt(condition, "persistence").value_or(std::string { "-" }),
                                               StringAt(condition, "detail").value_or(std::string {}),
                                               StringAt(condition, "remedy").value_or(std::string {}));
            // Named by its member id and display name, the id its machine row carries.
            auto const naming = namings.contains(endpoint) ? namings.at(endpoint) : MachineNaming {};
            unplaced += Unplaced(naming) ? 1U : 0U;
            auto row = Json::Object { { "endpoint", Str(endpoint) } };
            AppendNaming(row, naming);
            row.emplace_back("conditions", Arr(std::move(perMachine[endpoint])));
            machines.push_back(Obj(std::move(row)));
        }

        auto text = std::format("{} condition(s) raised across {} machine(s) that reported{}{}; {}{}",
                                raised,
                                machines.size(),
                                notReported.empty() ? std::string {}
                                                    : std::format("; {} reported no conditions at all", notReported),
                                UnplacedNote(unplaced),
                                Provenance(document->reply),
                                raisedLines);
        auto result = Envelope(document->reply);
        result.emplace_back("raised_count", Count(raised));
        result.emplace_back("nodes", Arr(std::move(machines)));
        result.emplace_back("not_reported", Strings(notReported));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ history

    /// @param keys Some keys.
    /// @return Them, comma-separated, for a refusal that lists what it would accept.
    template <typename Rows, typename Key>
    [[nodiscard]] std::string KeysOf(Rows const& rows, Key key)
    {
        auto out = std::string {};
        for (auto const& row: rows)
            out += std::format("{}{}", out.empty() ? "" : ", ", std::invoke(key, row));
        return out;
    }

    /// The `series` section's per-bucket columns this tool reads, by the names the section writes them under.
    constexpr std::string_view StartColumn = "start";
    constexpr std::string_view CoverageColumn = "coverage";
    constexpr std::string_view BackfilledColumn = "backfilled";

    /// What the `series` section writes for a value that cannot be known.
    constexpr std::string_view AbsentCell = "-";

    /// @param line A tab-separated line.
    /// @return Its fields.
    [[nodiscard]] std::vector<std::string_view> FieldsOf(std::string_view line)
    {
        auto fields = std::vector<std::string_view> {};
        for (auto const field: line | std::views::split('\t'))
            fields.emplace_back(field.begin(), field.end());
        return fields;
    }

    /// @param text A cell.
    /// @return Its integer, or nothing when it is not one.
    [[nodiscard]] std::optional<std::int64_t> IntegerCell(std::string_view text)
    {
        auto number = 0.0;
        if (!ParseFiniteDouble(text, number) || number != static_cast<double>(static_cast<std::int64_t>(number)))
            return std::nullopt;
        return static_cast<std::int64_t>(number);
    }

    /// One bucket of the series, read off its line.
    /// @param fields The line's fields.
    /// @param at Where each column sits.
    /// @return The bucket, or why it could not be read.
    [[nodiscard]] std::expected<Json::Value, std::string> BucketOf(std::vector<std::string_view> const& fields,
                                                                   std::array<std::size_t, 4> const& at)
    {
        auto const [startAt, coverageAt, backfilledAt, valueAt] = at;
        auto const start = IntegerCell(fields[startAt]);
        if (!start.has_value())
            return std::unexpected { std::string { "a bucket with no start" } };

        auto coverage = Null();
        if (fields[coverageAt] != AbsentCell)
        {
            auto const counted = IntegerCell(fields[coverageAt]);
            if (!counted.has_value())
                return std::unexpected { std::string { "a coverage that is not a count" } };
            coverage = Int(*counted);
        }

        if (fields[backfilledAt] != "true" && fields[backfilledAt] != "false")
            return std::unexpected { std::string { "a backfilled flag that is neither true nor false" } };

        auto value = Null();
        if (fields[valueAt] != AbsentCell)
        {
            auto number = 0.0;
            if (!ParseFiniteDouble(fields[valueAt], number))
                return std::unexpected { std::string { "a value that is not a number" } };
            value = Real(number);
        }

        return Obj({ { "start", Str(Rfc3339(*start)) },
                     { "value", std::move(value) },
                     { "coverage", std::move(coverage) },
                     { "backfilled", Bool(fields[backfilledAt] == "true") } });
    }

    /// Read the `series` section's table for one series.
    /// @param table The section, as the leader wrote it.
    /// @param series The series' key.
    /// @return One bucket per line, oldest first, or why the table could not be read.
    [[nodiscard]] std::expected<Json::Array, std::string> SeriesBuckets(std::string_view table, std::string_view series)
    {
        auto lines = std::vector<std::string_view> {};
        for (auto const line: table | std::views::split('\n'))
            if (!std::ranges::empty(line))
                lines.emplace_back(line.begin(), line.end());
        if (lines.empty())
            return std::unexpected { std::string { "a series table with no header" } };

        auto const header = FieldsOf(lines.front());
        auto const column = [&header](std::string_view name) -> std::optional<std::size_t> {
            auto const found = std::ranges::find(header, name);
            if (found == header.end())
                return std::nullopt;
            return static_cast<std::size_t>(std::ranges::distance(header.begin(), found));
        };
        auto const start = column(StartColumn);
        auto const coverage = column(CoverageColumn);
        auto const backfilled = column(BackfilledColumn);
        auto const value = column(series);
        if (!start || !coverage || !backfilled)
            return std::unexpected { std::string { "a series table without its per-bucket columns" } };
        if (!value)
            return std::unexpected { std::format("no series named \"{}\"", series) };

        auto const at = std::array { *start, *coverage, *backfilled, *value };
        auto buckets = Json::Array {};
        buckets.reserve(lines.size() - 1);
        for (auto const line: lines | std::views::drop(1))
        {
            auto const fields = FieldsOf(line);
            if (fields.size() != header.size())
                return std::unexpected { std::string { "a bucket line whose fields do not match the header" } };
            auto bucket = BucketOf(fields, at);
            if (!bucket.has_value())
                return std::unexpected { std::move(bucket.error()) };
            buckets.push_back(std::move(*bucket));
        }
        return buckets;
    }

    /// @param row A row of a keyed section's `keys`.
    /// @param field A member of it.
    /// @param key What the caller named.
    /// @return Whether that member is @p key, exactly.
    [[nodiscard]] bool RowFieldIs(Json::Value const& row, std::string_view field, std::string_view key)
    {
        auto const* const value = row.Find(field);
        return value != nullptr && value->AsString() != nullptr && *value->AsString() == key;
    }

    /// Which member `key` is matched against: ONE rule, the first that matches any row -- the row's key, then (for a
    /// row naming a machine) its node id, then its display name -- so a display name equal to another member's id
    /// never selects two rows, as `node` resolution never does.
    /// @param rows The section's `keys`.
    /// @param dimension Which keyed history.
    /// @param key What the caller named.
    /// @return The member to match by, or nothing when no row matches by any.
    [[nodiscard]] std::optional<std::string_view> KeySelectorField(Json::Array const& rows,
                                                                   Distributed::KeyedDimensionRow const& dimension,
                                                                   std::string_view key)
    {
        auto const fields =
            std::array { dimension.keyField, std::string_view { "node_id" }, std::string_view { "display_name" } };
        auto const consulted = std::span { fields }.first(dimension.namesMachines ? fields.size() : 1);
        for (auto const field: consulted)
            if (std::ranges::any_of(rows, [&](Json::Value const& row) { return RowFieldIs(row, field, key); }))
                return field;
        return std::nullopt;
    }

    /// The `history` tool for a keyed series: the leader's `/fleet.json` section for it, returned exactly as the
    /// document writes it -- one derivation, two readers -- with only `key` narrowing its rows.
    /// @param upstream How the leader is reached.
    /// @param dimension Which keyed history.
    /// @param range Which window.
    /// @param call The call, for `node` and `key`.
    /// @return The answer.
    [[nodiscard]] Mcp::ToolResult KeyedHistory(IMcpUpstream& upstream,
                                               Distributed::KeyedDimensionRow const& dimension,
                                               Distributed::FleetRangeRow const& range,
                                               Mcp::ToolCall const& call)
    {
        if (auto const* const node = call.arguments.Find("node"); node != nullptr && !node->IsNull())
            return Error(std::format("series \"{}\" is the leader's own count of what its queue granted, which no "
                                     "single machine's history answers for; omit `node`, and select one row with `key`",
                                     dimension.series));
        auto const* const keyArgument = call.arguments.Find("key");
        auto const key = keyArgument != nullptr && !keyArgument->IsNull() && keyArgument->AsString() != nullptr
                             ? std::optional<std::string> { *keyArgument->AsString() }
                             : std::nullopt;
        if (keyArgument != nullptr && !keyArgument->IsNull() && !key.has_value())
            return Error("`key` must be a string, matched exactly");

        auto document = AskFleetDocument(upstream, range.key);
        if (!document.has_value())
            return std::move(document.error());
        auto const* const found = document->root.Find(dimension.section);
        if (found == nullptr)
            return Error(std::format("the fleet document {} answered carries no \"{}\" section; nothing it said is "
                                     "reported",
                                     document->reply.answeredBy,
                                     dimension.section));
        auto section = *found;

        auto result = Envelope(document->reply);
        result.emplace_back("series", Str(std::string { dimension.series }));
        result.emplace_back("range", Str(std::string { range.key }));
        result.emplace_back("key", StrOrNull(key));
        if (section.IsNull())
        {
            result.emplace_back(std::string { dimension.section }, Null());
            return Answer(std::move(result),
                          std::format("{} over {}: {} keeps no such history -- it counts only while it leads; {}",
                                      dimension.series,
                                      range.key,
                                      document->reply.answeredBy,
                                      Provenance(document->reply)));
        }

        auto const* const listed = section.Find("keys");
        if (listed == nullptr || listed->AsArray() == nullptr)
            return Error(std::format("the \"{}\" section {} answered has no list of keys; nothing it said is reported",
                                     dimension.section,
                                     document->reply.answeredBy));
        // Narrowed in place, so every other member of the section stays exactly what the document wrote.
        auto const selector =
            key.has_value() ? KeySelectorField(*listed->AsArray(), dimension, *key) : std::optional<std::string_view> {};
        if (key.has_value())
            if (auto* const members = std::get_if<Json::Object>(&section.v); members != nullptr)
                for (auto& [name, value]: *members)
                    if (auto* const rows = std::get_if<Json::Array>(&value.v); name == "keys" && rows != nullptr)
                        std::erase_if(*rows, [&](Json::Value const& row) {
                            return !selector.has_value() || !RowFieldIs(row, *selector, *key);
                        });
        auto const named = section.Find("keys")->AsArray()->size();

        auto const numberAt = [&section](std::string_view field) {
            auto const* const value = section.Find(field);
            return value != nullptr ? value->AsUint().value_or(0) : std::uint64_t { 0 };
        };
        auto const* const other = section.Find("other");
        auto const otherText =
            other != nullptr && !other->IsNull()
                ? std::format("; other ({} {} folded): {}",
                              other->Find("keys_folded") != nullptr ? other->Find("keys_folded")->AsUint().value_or(0)
                                                                    : std::uint64_t { 0 },
                              dimension.noun,
                              other->Find("total") != nullptr ? other->Find("total")->AsUint().value_or(0)
                                                              : std::uint64_t { 0 })
                : std::string {};
        auto const* const since = section.Find("coverage_since_ms");
        auto const sinceText =
            since != nullptr && since->AsInt().has_value() ? Rfc3339(*since->AsInt()) : std::string { "never" };
        auto selected = std::string {};
        if (key.has_value() && named == 0)
            selected = std::format("; no row is \"{}\" in this window -- if it was folded, its counts are in other", *key);
        else if (key.has_value())
            selected = std::format("; the row for \"{}\"", *key);
        auto const* const unattributed = section.Find("unattributed");
        auto const unattributedText =
            unattributed != nullptr && !unattributed->IsNull()
                ? std::format("; unattributed: {}",
                              unattributed->Find("total") != nullptr ? unattributed->Find("total")->AsUint().value_or(0)
                                                                     : std::uint64_t { 0 })
                : std::string {};
        // Over the minutes the leader COUNTED, never "since" an instant alone: a demotion's gap, or a bucket watched
        // for one minute of sixty, is a part of the window nobody counted, and the total is that part's.
        // The headline is the SELECTED rows' own totals when `key` narrowed the list -- "N compile(s) granted" beside
        // "the row for X" must be X's N, with the whole window's total still in the section for a reader who wants it.
        auto grantedWords = std::format("{} compile(s) granted", numberAt("total"));
        if (key.has_value())
        {
            auto granted = std::uint64_t { 0 };
            auto floor = false;
            for (auto const& row: *section.Find("keys")->AsArray())
            {
                auto const* const rowTotal = row.Find("total");
                granted += rowTotal != nullptr ? rowTotal->AsUint().value_or(0) : std::uint64_t { 0 };
                // A key folded away and seen again shows only what it did since: its count is a floor.
                auto const* const earlier = row.Find("earlier_counts");
                floor = floor || earlier == nullptr || earlier->AsString() == nullptr || *earlier->AsString() != "none";
            }
            // No row means UNKNOWN -- folded into other, or never seen -- never zero.
            grantedWords = named == 0 ? std::format("the count for \"{}\" is unknown (no row: folded into other, or never "
                                                    "counted)",
                                                    *key)
                                      : std::format("{}{} compile(s) granted", floor ? "at least " : "", granted);
            selected += std::format("; the window's whole total is {}", numberAt("total"));
        }
        auto text = std::format("{} over {}: {}, {} {} named{}{}{}; counted for {} of the window's {} "
                                "minute(s), first at {}; {}",
                                dimension.series,
                                range.key,
                                grantedWords,
                                named,
                                dimension.noun,
                                otherText,
                                unattributedText,
                                selected,
                                numberAt("observed_minutes"),
                                numberAt("window_minutes"),
                                sinceText,
                                Provenance(document->reply));
        result.emplace_back(std::string { dimension.section }, std::move(section));
        return Answer(std::move(result), std::move(text));
    }

    [[nodiscard]] Mcp::ToolResult History(IMcpUpstream& upstream,
                                          IAppliedClusterView const& cluster,
                                          std::string const& selfId,
                                          Mcp::ToolCall const& call)
    {
        auto const seriesKey = StringAt(call.arguments, "series").value_or(std::string {});
        auto const rangeKey = StringAt(call.arguments, "range").value_or(std::string {});

        // Judged HERE, against this build's tables, before anything is asked: the refusal lists what this node
        // would accept, read off the tables the renderers walk, never restated.
        auto const* const keyed =
            core::findIfOrNull(Distributed::KeyedDimensionTable,
                               [&seriesKey](Distributed::KeyedDimensionRow const& row) { return row.series == seriesKey; });
        auto const* const series = keyed == nullptr ? Distributed::FleetSeriesFromKey(seriesKey) : nullptr;
        if (series == nullptr && keyed == nullptr)
            return Error(std::format("unknown series \"{}\"; the series are: {}, {}",
                                     seriesKey,
                                     KeysOf(Distributed::FleetSeriesTable, &Distributed::FleetSeriesRow::key),
                                     KeysOf(Distributed::KeyedDimensionTable, &Distributed::KeyedDimensionRow::series)));
        auto const* const range =
            core::findIfOrNull(Distributed::FleetRangeTable,
                               [&rangeKey](Distributed::FleetRangeRow const& row) { return row.key == rangeKey; });
        if (range == nullptr)
            return Error(std::format("unknown range \"{}\"; the ranges are: {}",
                                     rangeKey,
                                     KeysOf(Distributed::FleetRangeTable, &Distributed::FleetRangeRow::key)));
        if (keyed != nullptr)
            return KeyedHistory(upstream, *keyed, *range, call);
        if (auto const* const keyArgument = call.arguments.Find("key"); keyArgument != nullptr && !keyArgument->IsNull())
            return Error(std::format("`key` selects one row of a keyed series ({}), and series \"{}\" has none",
                                     KeysOf(Distributed::KeyedDimensionTable, &Distributed::KeyedDimensionRow::series),
                                     series->key));

        // Whose series: the fleet's, or one machine's. Which a series HAS is read off the slot table's columns,
        // never restated: a load ratio has no fleet figure (`FleetTotal::None`), a dispatch outcome no machine
        // figure (`FleetMetricScope::Fleet`). Asked the wrong way, each is refused naming the argument -- never a
        // fleet aggregate silently standing in for a machine, nor a machine's absences standing in for a count.
        auto const* const nodeArgument = call.arguments.Find("node");
        auto const forMachine = nodeArgument != nullptr && !nodeArgument->IsNull();
        if (!forMachine && !Distributed::HasFleetValue(*series))
            return Error(std::format("series \"{}\" is one machine's own reading and has no fleet-wide value; name the "
                                     "machine with `node` (its member id or display name)",
                                     series->key));
        if (forMachine && !Distributed::HasMachineValue(*series))
            return Error(std::format("series \"{}\" is the fleet scheduler's own count, which no single machine answers "
                                     "for; omit `node` to read the fleet's",
                                     series->key));

        auto naming = MachineNaming {};
        auto memberId = std::string {};
        if (forMachine)
        {
            auto target = NodeTargetFrom(nodeArgument, cluster, selfId);
            if (!target.has_value())
                return std::move(target.error());
            // Empty means this node, named by the id it runs under.
            memberId = target->nodeId.empty() ? selfId : target->nodeId;
            if (memberId.empty())
                return Error("this node runs with no id, so `node` can name no machine here");
            naming = std::move(target->naming);
        }

        auto const section = Distributed::FleetSectionTable[static_cast<std::size_t>(Distributed::FleetSection::Series)].key;
        auto reply = AskLeader(upstream,
                               Wire::EncodeFleetTextRequest(Wire::FleetTextRequest { .section = std::string { section },
                                                                                     .range = rangeKey,
                                                                                     .format = Wire::FleetTextFormat::Text,
                                                                                     .node = memberId }),
                               "fleet-text");
        if (!reply.has_value())
            return std::move(reply.error());

        auto buckets = SeriesBuckets(reply->payload, series->key);
        if (!buckets.has_value())
            return Error(std::format(
                "the history {} answered carries {}; nothing it said is reported", reply->answeredBy, buckets.error()));

        auto const known = std::ranges::count_if(*buckets, [](Json::Value const& bucket) {
            auto const* const value = bucket.Find("value");
            return value != nullptr && !value->IsNull();
        });
        auto const unit = series->unit;
        auto const bucketSeconds = std::chrono::duration_cast<std::chrono::seconds>(range->bucket).count();
        auto const whose = forMachine ? std::format(" for node {} ({})",
                                                    naming.nodeId.value_or(memberId),
                                                    naming.displayName.value_or(std::string { "no display name" }))
                                      : std::string { " for the fleet" };
        auto text = std::format("{} over {}{}: {} bucket(s) of {} s, {} with a value ({}); {}",
                                series->key,
                                range->key,
                                whose,
                                buckets->size(),
                                bucketSeconds,
                                known,
                                unit,
                                Provenance(*reply));

        auto result = Envelope(*reply);
        result.emplace_back("scope", Str(std::string { forMachine ? "node" : "fleet" }));
        // Named as every answer names a machine: id AND display name; both null for the fleet's series.
        AppendNaming(result, naming);
        result.emplace_back("series", Str(std::string { series->key }));
        result.emplace_back("range", Str(std::string { range->key }));
        result.emplace_back("unit", Str(std::string { unit }));
        result.emplace_back("bucket_seconds", Int(bucketSeconds));
        result.emplace_back("full_coverage", Int(static_cast<std::int64_t>(Distributed::FullCoverageOf(range->range))));
        result.emplace_back("buckets", Arr(std::move(*buckets)));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ enrollment_status

    /// @param entry One waiting machine.
    /// @return It, projected.
    [[nodiscard]] Json::Value PendingEntry(Wire::EnrollmentPendingEntry const& entry)
    {
        return Obj({
            { "node_id", Str(entry.nodeId) },
            { "role", Str(std::string { EnrollRoleRowFor(entry.role).name }) },
            { "decision", Str(std::string { DescribeEnrollmentDecision(entry.decision) }) },
            { "claimed_endpoint", entry.nodeEndpoint.empty() ? Null() : Str(entry.nodeEndpoint) },
            { "peer", Str(entry.peerId) },
            { "first_peer", entry.firstPeerId.empty() ? Null() : Str(entry.firstPeerId) },
            { "first_seen_age_s", Int(static_cast<std::int64_t>(entry.firstSeenSecondsAgo)) },
            { "attempts", Int(entry.attempts) },
            { "claims_changed", Int(entry.claimsChanged) },
            { "public_key", Str(FormatEd25519PublicKey(entry.publicKey)) },
            { "roster_fingerprint",
              entry.rosterFingerprint.has_value() ? Str(Cluster::RenderRosterFingerprint(*entry.rosterFingerprint))
                                                  : Null() },
            { "auto_approve_window_armed_age_s",
              entry.autoApprovedArmedSecondsAgo.has_value()
                  ? Int(static_cast<std::int64_t>(*entry.autoApprovedArmedSecondsAgo))
                  : Null() },
        });
    }

    [[nodiscard]] Mcp::ToolResult EnrollmentStatus(IMcpUpstream& upstream, Mcp::ToolCall const& /*call*/)
    {
        auto reply = AskLeader(upstream, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::List), "enroll-control");
        if (!reply.has_value())
            return std::move(reply.error());
        auto const report = Wire::DecodeEnrollmentReport(PayloadBytes(reply->payload));
        if (!report.has_value())
            return Error(std::format("the enrollment report {} answered did not decode; nothing it said is reported",
                                     reply->answeredBy));

        auto const armed = report->state == Wire::WireEnrollmentState::AutoApprove;
        auto pending = Json::Array {};
        auto waiting = std::size_t { 0 };
        for (auto const& entry: report->pending)
        {
            waiting += entry.decision == Wire::EnrollmentDecision::Pending ? 1U : 0U;
            pending.push_back(PendingEntry(entry));
        }

        auto text = std::format("enrollment: {}{}; {} row(s), {} waiting for a decision; {}",
                                EnrollmentStateName(report->state),
                                armed ? std::format(" ({} s left)", report->autoApproveSecondsLeft) : std::string {},
                                report->pending.size(),
                                waiting,
                                Provenance(*reply));
        auto result = Envelope(*reply);
        result.emplace_back("enrollment", Str(std::string { EnrollmentStateName(report->state) }));
        result.emplace_back("auto_approve_left_s",
                            armed ? Int(static_cast<std::int64_t>(report->autoApproveSecondsLeft)) : Null());
        result.emplace_back("pending", Arr(std::move(pending)));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ the schemas

    constexpr std::string_view NoArguments = R"({"type":"object","properties":{},"additionalProperties":false})";

    constexpr std::string_view FleetOverviewOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","role","leader","members","nodes","versions","versions_unreported","version_skew"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("role":{"type":["string","null"]},"leader":{"type":["string","null"],"description":"The leader's 0xFC endpoint as /fleet.json names it: null while the answering node itself leads (role: leader) -- then answered_by is the leader -- or while none is known."},)"
        R"("members":{"type":["array","null"],"description":"/fleet.json members, snake_case; null when the answering node runs no cluster."},)"
        R"("nodes":{"type":"array","description":"/fleet.json nodes, snake_case; heartbeat_age_s in seconds; null cells are absent, never 0. Each also carries node_id -- the member holding the identity key the machine PROVED (the leader's node-id column), never matched by the row's endpoint, which the machine only claims -- and display_name, that member's name as the same document records it (the row's name when name_source is replicated); both null when no member holds the key, never guessed."},)"
        R"("versions":{"type":"array","items":{"type":"string"}},"versions_unreported":{"type":"array","items":{"type":"string"}},)"
        R"("version_skew":{"type":["boolean","null"],"description":"true: two or more builds; false: one build everywhere; null: cannot be told."}}})";

    constexpr std::string_view RunningCompilesInput =
        R"({"type":"object","properties":{)"
        R"("requester":{"type":"string","description":"Only compiles requested by this machine: its display name or identity key, matched exactly."},)"
        R"("worker":{"type":"string","description":"Only compiles running on this worker id (or `local`), matched exactly."}},"additionalProperties":false})";

    constexpr std::string_view RunningCompilesOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","compiles","compiles_truncated","totals"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("compiles":{"type":["array","null"],"description":"/fleet.json compiles rows, oldest first, after the filter; age_s and lease_expires_in_s in seconds; each carries requester_node_id and requester_display_name, of the member holding requester_key (both null when no member holds it); null when the answering node leads no queue."},)"
        R"("compiles_truncated":{"type":["integer","null"]},)"
        R"("totals":{"type":["object","null"],"description":"running/queued/waiting count EVERY ticket; oldest_wait_s, per_requester (each with node_id and display_name, by its key) and per_worker are over the listed rows, before the filter, complete when per_counts_cover_every_ticket."}}})";

    constexpr std::string_view ConditionsOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","raised_count","nodes","not_reported"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("raised_count":{"type":"integer"},)"
        R"("nodes":{"type":"array","description":"Every machine that reported a list: endpoint; node_id and display_name of the member holding the key that machine proved, as its fleet machine row says (both null when none does); a machine's conditions are shown only when the announcement carrying them proved that same key; and each condition: condition, state, persistence, severity, detail, remedy."},)"
        R"("not_reported":{"type":"array","items":{"type":"string"},"description":"Machines whose report carried no conditions at all: absent, not clear."}}})";

    /// The `history` tool's input schema: its `enum`s are the series and range tables' keys, composed once from
    /// the tables the renderers walk rather than restated, so a series or range added there is offered here.
    /// @return The schema text; static, so the tool row's view of it never dangles.
    [[nodiscard]] std::string_view HistoryInput()
    {
        static std::string const schema = [] {
            auto const quoted = [](auto const& rows, auto key) {
                auto out = std::string {};
                for (auto const& row: rows)
                    out += std::format("{}\"{}\"", out.empty() ? "" : ",", std::invoke(key, row));
                return out;
            };
            return std::format(
                R"({{"type":"object","required":["series","range"],"properties":{{)"
                R"("series":{{"type":"string","enum":[{},{}],"description":"A series, as /fleet/series.json keys it, or a keyed one: requesters (compiles granted per requesting machine) and toolchain-demand (per toolchain fingerprint), as /fleet.json's requesters and toolchain_demand sections. cpu-busy (permille) is one machine's own reading and needs `node`; the dispatch series and the keyed series are the scheduler's and refuse it."}},)"
                R"("range":{{"type":"string","enum":[{}],"description":"How far back, as the dashboard's range buttons name it."}},)"
                R"("node":{{"type":"string","description":"One machine's own series instead of the fleet's: a member id or display name, matched exactly."}},)"
                R"("key":{{"type":"string","description":"A keyed series only: keep the one row whose key is this, exactly -- a requester's identity key, node id or display name; a toolchain's fingerprint."}}}},"additionalProperties":false}})",
                quoted(Distributed::FleetSeriesTable, &Distributed::FleetSeriesRow::key),
                quoted(Distributed::KeyedDimensionTable, &Distributed::KeyedDimensionRow::series),
                quoted(Distributed::FleetRangeTable, &Distributed::FleetRangeRow::key));
        }();
        return schema;
    }

    constexpr std::string_view HistoryOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","series","range"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("scope":{"enum":["fleet","node"]},"node_id":{"type":["string","null"],"description":"The machine whose series this is; null for the fleet's."},"display_name":{"type":["string","null"]},)"
        R"("series":{"type":"string"},"range":{"type":"string"},"unit":{"type":"string"},)"
        R"("bucket_seconds":{"type":"integer"},"full_coverage":{"type":"integer"},)"
        R"("buckets":{"type":"array","description":"Oldest first: start (RFC 3339 UTC), value (null for a gap), coverage (samples, null when unsampled), backfilled. A slot series carries scope, node_id, display_name, unit, bucket_seconds, full_coverage and buckets."},)"
        R"("key":{"type":["string","null"],"description":"A keyed series: the row selected, or null for every row."},)"
        R"("requesters":{"type":["object","null"],"description":"The requesters series: /fleet.json's requesters section exactly (only `key` narrows its keys). counts says what total counts; bucket_starts_ms and coverage_since_ms in milliseconds since the epoch; coverage per bucket in minutes counted of full_coverage, observed_minutes of window_minutes for the window; keys_kept_max is the ceiling; each key row has key (identity key), node_id, display_name (null when no member holds the key), total, earlier_counts (none, in-other: its earlier counts were folded into other, unknown) and buckets (null where the leader was not counting); other (null until a key was folded) has keys_folded, total and buckets; unattributed (null until a grant named no machine) has total and buckets. null when the answering leader keeps none."},)"
        R"("toolchain_demand":{"type":["object","null"],"description":"The toolchain-demand series: /fleet.json's toolchain_demand section exactly, rows keyed by fingerprint, otherwise as requesters."}}})";

    constexpr std::string_view EnrollmentStatusOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","enrollment","auto_approve_left_s","pending"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("enrollment":{"type":"string","description":"manual, or auto-approve while a deadline is armed."},)"
        R"("auto_approve_left_s":{"type":["integer","null"],"description":"Seconds left on the armed deadline; null when manual."},)"
        R"("pending":{"type":"array","description":"Every row the window holds, oldest first; durations in seconds (_s)."}}})";

} // namespace

std::vector<Mcp::ToolRow> FleetReadTools(IMcpUpstream& upstream, IAppliedClusterView const& cluster, std::string selfId)
{
    auto* const to = &upstream;
    auto const* const in = &cluster;
    return {
        Mcp::ToolRow {
            .name = "fleet_overview",
            .title = "Fleet overview",
            .description = "The leader's view of the fleet: who leads, the cluster members, every machine with its "
                           "version, load and heartbeat age, and whether the machines run mixed builds. Read from the "
                           "same document /fleet.json serves. Durations are in SECONDS (fields ending _s); a null field "
                           "is absent, never zero. `term` is the term THIS node holds, not the leader's report.",
            .inputSchema = NoArguments,
            .outputSchema = FleetOverviewOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to, in](Mcp::ToolCall const& call) { return FleetOverview(*to, *in, call); },
        },
        Mcp::ToolRow {
            .name = "running_compiles",
            .title = "Running compiles",
            .description = "Every compile in progress on the fleet's queue: running, queued, and waiting on an "
                           "identical compile -- requester, source, toolchain, worker, age and lease expiry -- with "
                           "totals over every ticket. Optional exact filters: requester, worker. Durations are in "
                           "SECONDS (fields ending _s); a null field is absent, never zero. `compiles: null` means the "
                           "answering node leads no queue. `term` is the term THIS node holds.",
            .inputSchema = RunningCompilesInput,
            .outputSchema = RunningCompilesOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to, in](Mcp::ToolCall const& call) { return RunningCompiles(*to, *in, call); },
        },
        Mcp::ToolRow {
            .name = "conditions",
            .title = "Node conditions",
            .description = "Every condition each machine reports to the leader (raised, clear, not-evaluated, "
                           "undecided; latched or live; with detail and remedy in the machine's own words). Machines "
                           "whose report carried no conditions are listed under not_reported: absent, not clear. "
                           "`term` is the term THIS node holds.",
            .inputSchema = NoArguments,
            .outputSchema = ConditionsOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to, in](Mcp::ToolCall const& call) { return Conditions(*to, *in, call); },
        },
        Mcp::ToolRow {
            .name = "history",
            .title = "Fleet history",
            .description = "One series over a range, as bucket values with explicit gaps (null), from the same "
                           "history /fleet/series.json serves: the fleet's, or with `node` one machine's own, kept by "
                           "the leader from what that machine handed over. `unit` names each series' unit: rates per "
                           "minute, shares percent, slot levels counts, cpu-busy permille. bucket_seconds is in SECONDS "
                           "and each start is RFC 3339 UTC. cpu-busy needs `node`; the dispatch series refuse it. "
                           "requesters and toolchain-demand are compiles the leader's queue GRANTED (on a worker or on "
                           "the requesting machine, or by a fail-open lease; one per compile -- never a refusal, an "
                           "abandoned wait, a duplicate answered by another compile's object, nor a retry of a compile "
                           "already granted), per requesting machine and per toolchain, returned exactly as "
                           "/fleet.json's requesters and toolchain_demand sections, with `key` selecting one row. The "
                           "keys_kept_max most recently active (--history-requesters, default 256) are kept by name, "
                           "the rest summed into `other`, and what names no machine or toolchain into `unattributed`, "
                           "exactly. coverage gives the minutes counted per bucket of full_coverage, and "
                           "observed_minutes of window_minutes the window's: a total is over the minutes counted. "
                           "These are counted by the LEADER and are not handed over at an election: a new leader starts "
                           "empty, and a demotion is a gap. An "
                           "unknown series, range or node is refused with the valid ones listed. `term` is the term "
                           "THIS node holds.",
            .inputSchema = HistoryInput(),
            .outputSchema = HistoryOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler =
                [to, in, self = std::move(selfId)](Mcp::ToolCall const& call) { return History(*to, *in, self, call); },
        },
        Mcp::ToolRow {
            .name = "enrollment_status",
            .title = "Enrollment status",
            .description = "The leader's enrollment window: manual, or auto-approve with the seconds left on its "
                           "deadline, and every machine that asked to join -- id, role, claimed endpoint, the host it "
                           "came from, its key, first-seen age and claims_changed. The same report pace-node "
                           "--enroll-list prints. Durations are in SECONDS (fields ending _s). Needs a node with "
                           "operator standing (a voter). `term` is the term THIS node holds.",
            .inputSchema = NoArguments,
            .outputSchema = EnrollmentStatusOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to](Mcp::ToolCall const& call) { return EnrollmentStatus(*to, call); },
        },
    };
}

} // namespace Pace::Node
