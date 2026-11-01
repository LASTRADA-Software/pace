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

    // ------------------------------------------------------------ the fleet document

    /// The leader's fleet document, and who answered it.
    struct FleetDocument
    {
        UpstreamReply reply; ///< Who answered, and under which term as this node sees it.
        Json::Value root;    ///< The parsed document.
    };

    /// Read the leader's fleet document: FLEET-TEXT's `Json` format, which is `RenderFleetJson`'s bytes.
    ///
    /// **A document of another schema is refused**, not read: the projection below knows this build's keys and
    /// units, and a generation it does not know could carry a key of the same name meaning something else.
    /// @param upstream How the leader is reached.
    /// @return The document, or the tool error to answer.
    [[nodiscard]] std::expected<FleetDocument, Mcp::ToolResult> AskFleetDocument(IMcpUpstream& upstream)
    {
        // No section, which a Json read refuses; no range, which is the day `/fleet.json` defaults to and only
        // the `kpi` figures read.
        auto reply =
            AskLeader(upstream,
                      Wire::EncodeFleetTextRequest(Wire::FleetTextRequest { .format = Wire::FleetTextFormat::Json }),
                      "fleet-text");
        if (!reply.has_value())
            return std::unexpected { std::move(reply.error()) };

        auto parsed = Json::Parse(reply->payload, McpFleet::FleetDocumentLimits);
        if (!parsed.has_value())
            return std::unexpected { Error(std::format(
                "the fleet document {} answered did not parse (error {} at byte {}); nothing it said is reported",
                reply->answeredBy,
                static_cast<unsigned>(parsed.error().code),
                parsed.error().offset)) };

        auto const* const schema = parsed->Find("schema");
        auto const generation = schema != nullptr ? schema->AsInt() : std::nullopt;
        if (generation != std::optional<std::int64_t> { Distributed::FleetJsonSchema })
            return std::unexpected { Error(std::format(
                "{} serves fleet document schema {}, and this node reads schema {}; nothing it said is reported -- "
                "run the same pace-node build on both",
                reply->answeredBy,
                generation.has_value() ? std::format("{}", *generation) : std::string { "(none)" },
                Distributed::FleetJsonSchema)) };

        return FleetDocument { .reply = std::move(*reply), .root = std::move(*parsed) };
    }

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

    [[nodiscard]] Mcp::ToolResult FleetOverview(IMcpUpstream& upstream, Mcp::ToolCall const& /*call*/)
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

        auto const skew = VersionsOf(ElementsOf(*nodes));
        auto const flag = SkewFlag(skew);
        auto const leader = StringAt(document->root, "leader");
        auto const flagText = SkewWord(flag.AsBool());
        auto text =
            std::format("fleet led by {}: {} member(s), {} machine(s); versions {} ({}){}; {}",
                        LeaderWords(leader, StringAt(document->root, "role"), document->reply),
                        members->IsNull() ? std::string { "no cluster" } : std::format("{}", ElementsOf(*members).size()),
                        ElementsOf(*nodes).size(),
                        skew.known.empty() ? std::string { "none reported" } : std::format("{}", skew.known),
                        flagText,
                        skew.unknown.empty() ? std::string {} : std::format("; no version from {}", skew.unknown),
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
    [[nodiscard]] CompileCounts CountCompiles(Json::Array const& rows)
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
            counts.perRequester.push_back(Obj(
                { { "requester", Str(who.first) }, { "requester_key", StrOrNull(who.second) }, { "count", Count(count) } }));
        for (auto const& [worker, count]: workers)
            counts.perWorker.push_back(Obj({ { "worker", Str(worker) }, { "count", Count(count) } }));
        return counts;
    }

    [[nodiscard]] Mcp::ToolResult RunningCompiles(IMcpUpstream& upstream, Mcp::ToolCall const& call)
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

        auto const& rows = ElementsOf(*compiles);
        auto counts = CountCompiles(rows);
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
                                "shown{}{}; {}",
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
                                Provenance(document->reply));

        result.emplace_back("compiles", Arr(std::move(listed)));
        result.emplace_back("compiles_truncated", Int(*truncated));
        result.emplace_back("totals", Obj(std::move(totalsOut)));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ conditions

    [[nodiscard]] Mcp::ToolResult Conditions(IMcpUpstream& upstream, Mcp::ToolCall const& /*call*/)
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
        for (auto const& node: ElementsOf(*nodes))
            std::ignore = visit(StringAt(node, "endpoint").value_or(std::string {}));
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
            machines.push_back(
                Obj({ { "endpoint", Str(endpoint) }, { "conditions", Arr(std::move(perMachine[endpoint])) } }));
        }

        auto text = std::format("{} condition(s) raised across {} machine(s) that reported{}; {}{}",
                                raised,
                                machines.size(),
                                notReported.empty() ? std::string {}
                                                    : std::format("; {} reported no conditions at all", notReported),
                                Provenance(document->reply),
                                raisedLines);
        auto result = Envelope(document->reply);
        result.emplace_back("raised_count", Count(raised));
        result.emplace_back("nodes", Arr(std::move(machines)));
        result.emplace_back("not_reported", Strings(notReported));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ history

    /// What one series kind's values are measured in.
    struct SeriesUnitRow
    {
        Distributed::FleetSeriesKind kind; ///< The kind.
        std::string_view unit;             ///< Its unit, as the tool names it.
    };

    /// One row per `FleetSeriesKind`; what `ValuesFor` derives for each.
    constexpr std::array SeriesUnits {
        SeriesUnitRow { .kind = Distributed::FleetSeriesKind::Rate, .unit = "per-minute" },
        SeriesUnitRow { .kind = Distributed::FleetSeriesKind::Level, .unit = "count" },
        SeriesUnitRow { .kind = Distributed::FleetSeriesKind::Share, .unit = "percent" },
    };

    static_assert(std::ranges::all_of(Distributed::FleetSeriesTable,
                                      [](Distributed::FleetSeriesRow const& series) {
                                          return std::ranges::count(SeriesUnits, series.kind, &SeriesUnitRow::kind) == 1;
                                      }),
                  "every series kind a series uses names its unit exactly once");

    /// @param kind A series kind.
    /// @return Its unit.
    [[nodiscard]] std::string_view UnitOf(Distributed::FleetSeriesKind kind) noexcept
    {
        for (auto const& row: SeriesUnits)
            if (row.kind == kind)
                return row.unit;
        return {};
    }

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

    /// @param millis An instant, in milliseconds since the epoch.
    /// @return It as RFC 3339 UTC, to the second.
    [[nodiscard]] std::string Rfc3339(std::int64_t millis)
    {
        auto const instant = std::chrono::floor<std::chrono::seconds>(
            std::chrono::sys_time<std::chrono::milliseconds> { std::chrono::milliseconds { millis } });
        return std::format("{:%FT%TZ}", instant);
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

    [[nodiscard]] Mcp::ToolResult History(IMcpUpstream& upstream, Mcp::ToolCall const& call)
    {
        auto const seriesKey = StringAt(call.arguments, "series").value_or(std::string {});
        auto const rangeKey = StringAt(call.arguments, "range").value_or(std::string {});

        // Judged HERE, against this build's tables, before anything is asked: the refusal lists what this node
        // would accept, read off the tables the renderers walk, never restated.
        auto const* const series = Distributed::FleetSeriesFromKey(seriesKey);
        if (series == nullptr)
            return Error(std::format("unknown series \"{}\"; the series are: {}",
                                     seriesKey,
                                     KeysOf(Distributed::FleetSeriesTable, &Distributed::FleetSeriesRow::key)));
        auto const* const range =
            core::findIfOrNull(Distributed::FleetRangeTable,
                               [&rangeKey](Distributed::FleetRangeRow const& row) { return row.key == rangeKey; });
        if (range == nullptr)
            return Error(std::format("unknown range \"{}\"; the ranges are: {}",
                                     rangeKey,
                                     KeysOf(Distributed::FleetRangeTable, &Distributed::FleetRangeRow::key)));

        auto const section = Distributed::FleetSectionTable[static_cast<std::size_t>(Distributed::FleetSection::Series)].key;
        auto reply =
            AskLeader(upstream,
                      Wire::EncodeFleetTextRequest(Wire::FleetTextRequest {
                          .section = std::string { section }, .range = rangeKey, .format = Wire::FleetTextFormat::Text }),
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
        auto const unit = UnitOf(series->kind);
        auto const bucketSeconds = std::chrono::duration_cast<std::chrono::seconds>(range->bucket).count();
        auto text = std::format("{} over {}: {} bucket(s) of {} s, {} with a value ({}); {}",
                                series->key,
                                range->key,
                                buckets->size(),
                                bucketSeconds,
                                known,
                                unit,
                                Provenance(*reply));

        auto result = Envelope(*reply);
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
        R"("nodes":{"type":"array","description":"/fleet.json nodes, snake_case; heartbeat_age_s in seconds; null cells are absent, never 0."},)"
        R"("versions":{"type":"array","items":{"type":"string"}},"versions_unreported":{"type":"array","items":{"type":"string"}},)"
        R"("version_skew":{"type":["boolean","null"],"description":"true: two or more builds; false: one build everywhere; null: cannot be told."}}})";

    constexpr std::string_view RunningCompilesInput =
        R"({"type":"object","properties":{)"
        R"("requester":{"type":"string","description":"Only compiles requested by this machine: its display name or identity key, matched exactly."},)"
        R"("worker":{"type":"string","description":"Only compiles running on this worker id (or `local`), matched exactly."}},"additionalProperties":false})";

    constexpr std::string_view RunningCompilesOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","compiles","compiles_truncated","totals"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("compiles":{"type":["array","null"],"description":"/fleet.json compiles rows, oldest first, after the filter; age_s and lease_expires_in_s in seconds; null when the answering node leads no queue."},)"
        R"("compiles_truncated":{"type":["integer","null"]},)"
        R"("totals":{"type":["object","null"],"description":"running/queued/waiting count EVERY ticket; oldest_wait_s, per_requester and per_worker are over the listed rows, before the filter, complete when per_counts_cover_every_ticket."}}})";

    constexpr std::string_view ConditionsOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","raised_count","nodes","not_reported"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("raised_count":{"type":"integer"},)"
        R"("nodes":{"type":"array","description":"Every machine that reported a list, with each condition: condition, state, persistence, severity, detail, remedy."},)"
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
                R"("series":{{"type":"string","enum":[{}],"description":"A fleet series, as /fleet/series.json keys it."}},)"
                R"("range":{{"type":"string","enum":[{}],"description":"How far back, as the dashboard's range buttons name it."}}}},"additionalProperties":false}})",
                quoted(Distributed::FleetSeriesTable, &Distributed::FleetSeriesRow::key),
                quoted(Distributed::FleetRangeTable, &Distributed::FleetRangeRow::key));
        }();
        return schema;
    }

    constexpr std::string_view HistoryOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","series","range","unit","bucket_seconds","full_coverage","buckets"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("series":{"type":"string"},"range":{"type":"string"},"unit":{"type":"string"},)"
        R"("bucket_seconds":{"type":"integer"},"full_coverage":{"type":"integer"},)"
        R"("buckets":{"type":"array","description":"Oldest first: start (RFC 3339 UTC), value (null for a gap), coverage (samples, null when unsampled), backfilled."}}})";

    constexpr std::string_view EnrollmentStatusOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","enrollment","auto_approve_left_s","pending"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("enrollment":{"type":"string","description":"manual, or auto-approve while a deadline is armed."},)"
        R"("auto_approve_left_s":{"type":["integer","null"],"description":"Seconds left on the armed deadline; null when manual."},)"
        R"("pending":{"type":"array","description":"Every row the window holds, oldest first; durations in seconds (_s)."}}})";

} // namespace

std::vector<Mcp::ToolRow> FleetReadTools(IMcpUpstream& upstream)
{
    auto* const to = &upstream;
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
            .handler = [to](Mcp::ToolCall const& call) { return FleetOverview(*to, call); },
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
            .handler = [to](Mcp::ToolCall const& call) { return RunningCompiles(*to, call); },
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
            .handler = [to](Mcp::ToolCall const& call) { return Conditions(*to, call); },
        },
        Mcp::ToolRow {
            .name = "history",
            .title = "Fleet history",
            .description = "One fleet series over a range, as bucket values with explicit gaps (null), from the same "
                           "history /fleet/series.json serves. Rates are per minute, shares percent, levels counts; "
                           "bucket_seconds is in SECONDS and each start is RFC 3339 UTC. An unknown series or range is "
                           "refused with the valid ones listed. `term` is the term THIS node holds.",
            .inputSchema = HistoryInput(),
            .outputSchema = HistoryOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to](Mcp::ToolCall const& call) { return History(*to, call); },
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
