// SPDX-License-Identifier: Apache-2.0
#include "McpFleetTools.hpp"
#include "McpToolSupport.hpp"
#include "McpToolchainTools.hpp"

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/CompileCache/VersionName.hpp>
#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/FingerprintDiff.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>
#include <Pace/Protocol/FingerprintInputsWire.hpp>
#include <Pace/Protocol/NodeStatusView.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Pace::Node
{

namespace
{
    using namespace McpSupport;

    /// What `unserved_note` says: what a machine's `unserved` lists, and what its `null` means.
    constexpr std::string_view UnservedNote =
        "a machine's `unserved` lists the fleet's toolchains in the families that machine serves which it does not "
        "serve itself, derived from the leader's workers; it is null (absent, not empty) for a machine serving no "
        "toolchain whose family is known, since a family is what decides which toolchains are its peers";

    /// What `demand_note` says: why `demand` is `null`.
    constexpr std::string_view DemandNote =
        "no demand per toolchain is recorded yet, so `demand` is null (absent) and every recommendation is made on "
        "versions alone";

    /// The `workers` rows, as the upgrade report reads them.
    using WorkerRows = std::vector<Distributed::WorkerReport>;

    /// @param row A `workers` row.
    /// @param key A member that must be a string or `null`.
    /// @return The member's string, empty for `null`, or nothing when it is absent or neither.
    [[nodiscard]] std::optional<std::string> StringOrNullAt(Json::Value const& row, std::string_view key)
    {
        auto const* const member = row.Find(key);
        if (member == nullptr || (!member->IsNull() && member->AsString() == nullptr))
            return std::nullopt;
        return member->IsNull() ? std::string {} : *member->AsString();
    }

    /// Read the fleet document's `workers` rows: each one's fingerprint, endpoint, label and family.
    ///
    /// A row this build cannot read refuses the whole answer rather than being skipped: a machine dropped from the
    /// count would be a node silently absent from the upgrade report.
    /// @param root The fleet document.
    /// @return The rows, or why they could not be read.
    [[nodiscard]] std::expected<WorkerRows, std::string> WorkersOf(Json::Value const& root)
    {
        auto const* const member = root.Find("workers");
        auto const* const rows = member != nullptr ? member->AsArray() : nullptr;
        if (rows == nullptr)
            return std::unexpected { std::string { "a \"workers\" that is not a list" } };

        auto workers = WorkerRows {};
        workers.reserve(rows->size());
        for (auto const& row: *rows)
        {
            auto fingerprint = StringAt(row, "toolchain");
            auto endpoint = StringAt(row, "endpoint");
            auto label = StringOrNullAt(row, "compiler");
            // A row with no `family` key at all is a leader that renders no such column -- read as `null`, so the row
            // joins no family rather than refusing every other row's answer with it.
            auto family = row.Find("family") == nullptr ? std::optional<std::string> { std::string {} }
                                                        : StringOrNullAt(row, "family");
            if (!fingerprint.has_value() || !endpoint.has_value() || !label.has_value() || !family.has_value())
                return std::unexpected { std::string { "a \"workers\" row without a string toolchain and endpoint "
                                                       "and a string-or-null compiler and family" } };
            auto worker = Distributed::WorkerReport {};
            worker.info.fingerprint = std::move(*fingerprint);
            worker.info.endpoint = std::move(*endpoint);
            worker.info.toolchainLabel = std::move(*label);
            worker.info.toolchainFamily = std::move(*family);
            workers.push_back(std::move(worker));
        }
        return workers;
    }

    /// @param strings Some strings.
    /// @return Them as a JSON array, in their order.
    template <typename Range>
    [[nodiscard]] Json::Value Strings(Range const& strings)
    {
        auto array = Json::Array {};
        for (auto const& text: strings)
            array.push_back(Str(text));
        return Arr(std::move(array));
    }

    /// @param items Some texts.
    /// @param separator What goes between two of them.
    /// @return Them joined, in their order.
    template <typename Range>
    [[nodiscard]] std::string Joined(Range const& items, std::string_view separator)
    {
        auto text = std::string {};
        for (auto const& item: items)
            text += std::format("{}{}", text.empty() ? std::string_view {} : separator, item);
        return text;
    }

    /// @param values A set.
    /// @return Its one value, or nothing when it holds none or several.
    [[nodiscard]] std::optional<std::string> OnlyOf(std::set<std::string> const& values)
    {
        return values.size() == 1 ? std::optional<std::string> { *values.begin() } : std::nullopt;
    }

    /// One fingerprint, gathered across its registry entries.
    struct ToolchainRow
    {
        std::set<std::string> labels;    ///< Every distinct label its entries carry.
        std::set<std::string> families;  ///< Every distinct family its entries registered.
        std::set<std::string> endpoints; ///< The machines serving it.
    };

    /// @param fingerprint The toolchain.
    /// @param row What its entries say.
    /// @return Its `toolchains` row: the label and version only where the entries agree on ONE label, the family
    ///         only where they agree on one family.
    [[nodiscard]] Json::Value ToolchainJson(std::string const& fingerprint, ToolchainRow const& row)
    {
        auto const label = OnlyOf(row.labels);
        auto const parsed = label.has_value() ? Distributed::ParseToolchainLabel(*label) : std::nullopt;
        return Obj(Json::Object {
            { "fingerprint", Str(fingerprint) },
            { "label", StrOrNull(label) },
            { "labels", Strings(row.labels) },
            { "family", StrOrNull(OnlyOf(row.families)) },
            { "driver",
              parsed.has_value() ? Str(std::string { CompileCache::DriverWithoutVersion(parsed->tool) }) : Null() },
            { "version", Str(parsed.has_value() ? Distributed::VersionText(parsed->parts) : std::string { "unknown" }) },
            { "nodes", Strings(row.endpoints) },
        });
    }

    /// @param advice One family's advice.
    /// @return `recommended_fingerprints`: driver to fingerprint, or `null` when the ordering is unknown.
    [[nodiscard]] Json::Value FingerprintsJson(Distributed::UpgradeAdvice const& advice)
    {
        if (advice.orderingUnknown)
            return Null();
        auto chosen = Json::Object {};
        for (auto const& [driver, fingerprint]: advice.recommendedFingerprints)
            chosen.emplace_back(driver, Str(fingerprint));
        return Obj(std::move(chosen));
    }

    /// @param advice One family's advice.
    /// @return Its `upgrade_report` row.
    [[nodiscard]] Json::Value AdviceJson(Distributed::UpgradeAdvice const& advice)
    {
        return Obj(Json::Object {
            { "family", Str(advice.family) },
            { "recommended_version", StrOrNull(advice.recommendedVersion) },
            { "recommended_fingerprints", FingerprintsJson(advice) },
            { "basis", Str(advice.basis) },
            { "behind", Strings(advice.nodesBehind) },
            { "ahead", Strings(advice.nodesAhead) },
            { "same_version_other_fingerprint", Strings(advice.nodesSameVersionOtherFingerprint) },
            { "ordering_unknown", Bool(advice.orderingUnknown) },
        });
    }

    /// @param endpoints Some machines.
    /// @return How the text lists them.
    [[nodiscard]] std::string MachinesWords(std::vector<std::string> const& endpoints)
    {
        return endpoints.empty() ? std::string { "nobody" } : Joined(endpoints, ", ");
    }

    /// @param advice One family's advice.
    /// @return How the text names the fingerprint chosen per driver.
    [[nodiscard]] std::string ChoicesWords(Distributed::UpgradeAdvice const& advice)
    {
        auto words = std::vector<std::string> {};
        for (auto const& [driver, fingerprint]: advice.recommendedFingerprints)
            words.push_back(std::format("{}: {}", driver, fingerprint));
        return words.empty() ? std::string { "no single fingerprint for any driver" } : Joined(words, ", ");
    }

    /// @param advice One family's advice.
    /// @return How the text says it.
    [[nodiscard]] std::string AdviceWords(Distributed::UpgradeAdvice const& advice)
    {
        if (!advice.recommendedVersion.has_value())
            return std::format("{}: no recommendation ({})", advice.family, advice.basis);
        return std::format("{}: converge on {} ({}) -- behind: {}; ahead: {}; same version, other fingerprint (ask "
                           "toolchain_diff): {} ({})",
                           advice.family,
                           *advice.recommendedVersion,
                           ChoicesWords(advice),
                           MachinesWords(advice.nodesBehind),
                           MachinesWords(advice.nodesAhead),
                           MachinesWords(advice.nodesSameVersionOtherFingerprint),
                           advice.basis);
    }

    /// The fleet's toolchains in @p endpoint's families that it does not serve.
    /// @param served The fingerprints the machine serves.
    /// @param toolchains Every fingerprint, with its families.
    /// @return The list, or nothing when the machine serves no toolchain whose family is known.
    [[nodiscard]] std::optional<std::vector<std::string>> UnservedBy(std::set<std::string> const& served,
                                                                     std::map<std::string, ToolchainRow> const& toolchains)
    {
        auto families = std::set<std::string> {};
        for (auto const& fingerprint: served)
            families.insert_range(toolchains.at(fingerprint).families);
        if (families.empty())
            return std::nullopt;
        auto unserved = std::vector<std::string> {};
        for (auto const& [fingerprint, row]: toolchains)
            if (!served.contains(fingerprint)
                && std::ranges::any_of(row.families, [&](std::string const& family) { return families.contains(family); }))
                unserved.push_back(fingerprint);
        return unserved;
    }

    [[nodiscard]] Mcp::ToolResult Toolchains(IMcpUpstream& upstream, Mcp::ToolCall const& /*call*/)
    {
        auto document = McpFleet::AskFleetDocument(upstream);
        if (!document.has_value())
            return std::move(document.error());
        auto workers = WorkersOf(document->root);
        if (!workers.has_value())
            return Error(std::format("the fleet document {} answered carries {}; nothing it said is reported",
                                     document->reply.answeredBy,
                                     workers.error()));

        auto toolchains = std::map<std::string, ToolchainRow> {};
        auto machines = std::map<std::string, std::set<std::string>> {};
        for (auto const& worker: *workers)
        {
            auto& row = toolchains[worker.info.fingerprint];
            row.endpoints.insert(worker.info.endpoint);
            if (!worker.info.toolchainLabel.empty())
                row.labels.insert(worker.info.toolchainLabel);
            if (!worker.info.toolchainFamily.empty())
                row.families.insert(worker.info.toolchainFamily);
            machines[worker.info.endpoint].insert(worker.info.fingerprint);
        }
        auto familyless = std::set<std::string> {};
        for (auto const& [fingerprint, row]: toolchains)
            if (row.families.empty())
                familyless.insert(fingerprint);

        // No demand is recorded yet: the report recommends on versions alone, and says so in every basis.
        auto const advice = Distributed::AdviseUpgrades(*workers, {});

        auto toolchainRows = Json::Array {};
        for (auto const& [fingerprint, row]: toolchains)
            toolchainRows.push_back(ToolchainJson(fingerprint, row));
        auto machineRows = Json::Array {};
        auto unservedWords = std::vector<std::string> {};
        for (auto const& [endpoint, served]: machines)
        {
            auto const unserved = UnservedBy(served, toolchains);
            if (unserved.has_value() && !unserved->empty())
                unservedWords.push_back(std::format("{} lacks {}", endpoint, Joined(*unserved, ", ")));
            machineRows.push_back(Obj(Json::Object {
                { "endpoint", Str(endpoint) },
                { "served", Strings(served) },
                { "unserved", unserved.has_value() ? Strings(*unserved) : Null() },
            }));
        }
        auto adviceRows = Json::Array {};
        auto adviceWords = std::vector<std::string> {};
        for (auto const& family: advice)
        {
            adviceRows.push_back(AdviceJson(family));
            adviceWords.push_back(AdviceWords(family));
        }

        auto text = std::format(
            "{} toolchain(s) on {} machine(s); upgrade report (conservative -- check each basis): {}{}; unserved in "
            "their families: {}; demand: not recorded; {}",
            toolchains.size(),
            machines.size(),
            adviceWords.empty() ? std::string { "no toolchain of a known family" } : Joined(adviceWords, " | "),
            familyless.empty() ? std::string {}
                               : std::format("; {} registered no family and join none", Joined(familyless, ", ")),
            unservedWords.empty() ? std::string { "none" } : Joined(unservedWords, "; "),
            Provenance(document->reply));

        auto result = Envelope(document->reply);
        result.emplace_back("toolchain_count", Count(toolchains.size()));
        result.emplace_back("machine_count", Count(machines.size()));
        result.emplace_back("toolchains", Arr(std::move(toolchainRows)));
        result.emplace_back("nodes", Arr(std::move(machineRows)));
        result.emplace_back("unserved_note", Str(std::string { UnservedNote }));
        result.emplace_back("demand", Null());
        result.emplace_back("demand_note", Str(std::string { DemandNote }));
        result.emplace_back("upgrade_report", Arr(std::move(adviceRows)));
        return Answer(std::move(result), std::move(text));
    }

    // ------------------------------------------------------------ toolchain_diff

    namespace Wire = CompileCacheWire;
    using McpToolchainDiff::MaxDiffBytesPerSide;
    using McpToolchainDiff::MaxDiffFiles;
    using McpToolchainDiff::MaxDiffPagesPerSide;
    using McpToolchainDiff::MaxListedDiffPaths;
    using McpToolchainDiff::MaxListedPathBytes;

    /// One side of a `toolchain_diff` call, resolved: whom to ask and which fingerprint.
    struct DiffSide
    {
        std::string_view key;                 ///< `a` or `b`, as the arguments name it.
        NodeTarget target;                    ///< The node, resolved exactly.
        std::string fingerprint;              ///< The fingerprint, given or resolved from `toolchain`.
        std::optional<std::string> toolchain; ///< The label or compiler path it was resolved from, when it was.
    };

    /// One side's whole record, as its node answered it.
    struct SideRecord
    {
        Cc::FingerprintInputs inputs;      ///< Every file when `complete`, none otherwise.
        std::string version;               ///< The node's `pace-node` version.
        std::string answeredBy;            ///< Who answered.
        std::optional<std::uint64_t> term; ///< This node's term when the answer arrived.
        std::uint64_t totalFiles { 0 };    ///< How many files the record holds.
        bool complete { false };           ///< Whether every file was fetched within every per-side bound.
        std::string incomplete;            ///< Why not, in words naming the bound; empty when `complete`.
    };

    /// @param side A side.
    /// @return How a sentence names its node: the name asked for, or `this node`.
    [[nodiscard]] std::string NodeWords(DiffSide const& side)
    {
        return side.target.asked.has_value() ? NodeStatusView::ReadableText(*side.target.asked)
                                             : std::string { "this node" };
    }

    /// @param text Text a node sent.
    /// @return It as a reader prints it: unchanged when UTF-8, escaped and marked otherwise.
    [[nodiscard]] std::string Readable(std::string_view text)
    {
        return NodeStatusView::ReadableText(text);
    }

    /// Ask @p side's node one FINGERPRINT-INPUTS question.
    /// @param upstream How a node is asked.
    /// @param side Whom.
    /// @param request What -- a frame this tool builds, never caller bytes.
    /// @return The reply, or the tool error naming the node and why.
    [[nodiscard]] std::expected<UpstreamReply, Mcp::ToolResult> AskInputs(IMcpUpstream& upstream,
                                                                          DiffSide const& side,
                                                                          Wire::FingerprintInputsRequest const& request)
    {
        auto answer =
            upstream.Ask(UpstreamTarget::Node, side.target.nodeId, FrameText(Wire::EncodeFingerprintInputsRequest(request)));
        if (!answer.has_value())
            return std::unexpected { Error(std::format("{} (side {}): {}: {}",
                                                       NodeWords(side),
                                                       side.key,
                                                       UpstreamFailureRowOf(answer.error().first).name,
                                                       answer.error().second)) };
        if (answer->status != Wire::Status::Ok)
            return std::unexpected { Error(std::format(
                "{} answered fingerprint-inputs with a miss, which that verb never answers; nothing it said is reported",
                answer->answeredBy)) };
        return std::move(*answer);
    }

    /// @param entry An index entry.
    /// @return How a candidate list names it.
    [[nodiscard]] std::string CandidateWords(Wire::FingerprintIndexEntry const& entry)
    {
        return std::format("{} ({}, {})",
                           entry.fingerprint,
                           entry.label.empty() ? std::string { "no label" } : Readable(entry.label),
                           Readable(entry.compiler));
    }

    /// @param entries Some index entries.
    /// @return Them named, the first `MaxListedCandidates`, and how many more.
    [[nodiscard]] std::string CandidatesWords(std::vector<Wire::FingerprintIndexEntry> const& entries)
    {
        auto words = std::vector<std::string> {};
        for (auto const& entry: entries | std::views::take(MaxListedCandidates))
            words.push_back(CandidateWords(entry));
        auto text = Joined(words, "; ");
        if (entries.size() > MaxListedCandidates)
            text += std::format("; and {} more", entries.size() - MaxListedCandidates);
        return text.empty() ? std::string { "none" } : text;
    }

    /// Resolve a toolchain named by label or compiler path to the one fingerprint it names on @p side's node.
    /// @param upstream How a node is asked.
    /// @param side Whom.
    /// @param name The label or compiler path, matched exactly.
    /// @return The fingerprint, or the tool error saying it names none or several -- listing the candidates.
    [[nodiscard]] std::expected<std::string, Mcp::ToolResult> ResolveToolchain(IMcpUpstream& upstream,
                                                                               DiffSide const& side,
                                                                               std::string const& name)
    {
        auto reply = AskInputs(upstream, side, Wire::FingerprintInputsRequest { .fingerprint = {}, .cursor = 0 });
        if (!reply.has_value())
            return std::unexpected { std::move(reply.error()) };
        auto const index = Wire::DecodeFingerprintIndex(PayloadBytes(reply->payload));
        if (!index.has_value())
            return std::unexpected { Error(std::format(
                "{} answered with a toolchain index this node cannot read; nothing it said is reported -- run the same "
                "pace-node build on both",
                reply->answeredBy)) };

        auto matches = std::vector<Wire::FingerprintIndexEntry> {};
        std::ranges::copy_if(index->entries, std::back_inserter(matches), [&name](Wire::FingerprintIndexEntry const& entry) {
            return entry.label == name || entry.compiler == name;
        });
        auto const cut = index->entries.size() < index->total
                             ? std::format(" (its list was cut to {} of {} toolchains by the reply ceiling)",
                                           index->entries.size(),
                                           index->total)
                             : std::string {};
        if (matches.empty())
            return std::unexpected { Error(std::format("`{}.toolchain` \"{}\" names no toolchain {} serves{}; it serves: {}",
                                                       side.key,
                                                       Readable(name),
                                                       NodeWords(side),
                                                       cut,
                                                       CandidatesWords(index->entries))) };
        if (matches.size() > 1)
            return std::unexpected { Error(std::format(
                "`{}.toolchain` \"{}\" names {} toolchains {} serves, so it is ambiguous; name one by `fingerprint`: {}",
                side.key,
                Readable(name),
                matches.size(),
                NodeWords(side),
                CandidatesWords(matches))) };
        return matches.front().fingerprint;
    }

    /// Read one side of the call's arguments.
    /// @param call The call.
    /// @param key `a` or `b`.
    /// @param upstream How a node is asked, to resolve a `toolchain`.
    /// @param cluster Where node names resolve.
    /// @param selfId This node's id.
    /// @return The side, or the tool error naming what is wrong with it.
    [[nodiscard]] std::expected<DiffSide, Mcp::ToolResult> SideOf(Mcp::ToolCall const& call,
                                                                  std::string_view key,
                                                                  IMcpUpstream& upstream,
                                                                  IAppliedClusterView const& cluster,
                                                                  std::string const& selfId)
    {
        auto const* const member = call.arguments.Find(key);
        if (member == nullptr || member->AsObject() == nullptr)
            return std::unexpected { Error(
                std::format("`{}` must be an object: {{\"node\": <member id, omitted for this node>, and exactly one of "
                            "\"fingerprint\" or \"toolchain\" (a served toolchain's label or compiler path)}}",
                            key)) };
        auto target = NodeTargetFrom(member->Find("node"), cluster, selfId);
        if (!target.has_value())
            return std::unexpected { std::move(target.error()) };

        auto const fingerprint = StringAt(*member, "fingerprint");
        auto const toolchain = StringAt(*member, "toolchain");
        if (fingerprint.has_value() == toolchain.has_value())
            return std::unexpected { Error(std::format(
                "`{}` names exactly one of `fingerprint` or `toolchain` (a string each), never both or neither", key)) };

        auto side = DiffSide { .key = key, .target = std::move(*target), .fingerprint = {}, .toolchain = toolchain };
        if (fingerprint.has_value())
        {
            side.fingerprint = *fingerprint;
            return side;
        }
        auto resolved = ResolveToolchain(upstream, side, *toolchain);
        if (!resolved.has_value())
            return std::unexpected { std::move(resolved.error()) };
        side.fingerprint = std::move(*resolved);
        return side;
    }

    /// Whether two pages of one record describe the same record: everything but the files must hold still.
    /// @param first The first page.
    /// @param later A later one.
    /// @return True when they agree.
    [[nodiscard]] bool SameRecord(Wire::FingerprintInputsPage const& first, Wire::FingerprintInputsPage const& later)
    {
        return first.version == later.version && first.totalFiles == later.totalFiles
               && first.inputs.schema == later.inputs.schema && first.inputs.banner == later.inputs.banner
               && first.inputs.driverGrammar == later.inputs.driverGrammar;
    }

    /// Fetch @p side's whole record, page by page, within the per-side bounds.
    ///
    /// **Two surveys are never merged.** A node's survey can re-run while it is being paged, so every page must
    /// repeat the first one's schema, banner, grammar, version and total, and start where the previous one said the
    /// next would; and the files fetched must add up to the total. Any break is refused, never stitched.
    ///
    /// **A side is bounded however its node answers**: past `MaxDiffFiles` claimed, `MaxDiffBytesPerSide` received or
    /// `MaxDiffPagesPerSide` asked, the record is INCOMPLETE -- its files are dropped there and then, so memory stays
    /// bounded, and the answer says which bound was reached rather than comparing part of a tree.
    /// @param upstream How a node is asked.
    /// @param side Whom, and which fingerprint.
    /// @return The record, or the tool error naming why there is none.
    [[nodiscard]] std::expected<SideRecord, Mcp::ToolResult> FetchRecord(IMcpUpstream& upstream, DiffSide const& side)
    {
        // Both causes, because this end cannot tell them apart: a survey that re-ran mid-read and a node whose pages do
        // not add up look identical from here, and naming one would send the reader after the wrong fault.
        auto const brokenStream = [&side] {
            return Error(std::format("{}'s pages did not describe one record: either its toolchain survey ran again "
                                     "while they were being read -- ask again -- or it answered pages that do not add "
                                     "up, which a repeat of this answer would point to; compare both nodes' pace-node "
                                     "builds",
                                     NodeWords(side)));
        };
        auto const stopAt = [](SideRecord record, std::string why) {
            record.complete = false;
            record.incomplete = std::move(why);
            record.inputs.files.clear();
            record.inputs.files.shrink_to_fit();
            return record;
        };
        auto record = SideRecord {};
        auto first = std::optional<Wire::FingerprintInputsPage> {};
        auto cursor = std::uint64_t { 0 };
        auto received = std::size_t { 0 };
        for (auto const asked: std::views::iota(std::size_t { 1 }, MaxDiffPagesPerSide + 1))
        {
            auto reply = AskInputs(
                upstream, side, Wire::FingerprintInputsRequest { .fingerprint = side.fingerprint, .cursor = cursor });
            if (!reply.has_value())
                return std::unexpected { std::move(reply.error()) };
            received += reply->payload.size();
            auto page = Wire::DecodeFingerprintInputsPage(PayloadBytes(reply->payload));
            if (!page.has_value() || page->fingerprint != side.fingerprint || page->cursor != cursor)
                return std::unexpected { Error(std::format(
                    "{} answered fingerprint-inputs with a page this node cannot read, or about another fingerprint or "
                    "position than it asked; nothing it said is reported -- run the same pace-node build on both",
                    reply->answeredBy)) };
            // A next page must start exactly where this one's files end, or a reader would skip or repeat files.
            if (page->nextCursor.has_value() && *page->nextCursor != cursor + page->inputs.files.size())
                return std::unexpected { Error(std::format(
                    "{} answered a page whose next cursor does not follow its files; nothing it said is reported",
                    reply->answeredBy)) };
            if (!first.has_value())
            {
                record.version = page->version;
                record.answeredBy = reply->answeredBy;
                record.term = reply->term;
                record.totalFiles = page->totalFiles;
                record.inputs.schema = page->inputs.schema;
                record.inputs.banner = page->inputs.banner;
                record.inputs.driverGrammar = page->inputs.driverGrammar;
                record.complete = true;
                if (page->totalFiles > MaxDiffFiles)
                    return stopAt(std::move(record),
                                  std::format("{} holds {} files, more than the {} this tool compares per side",
                                              NodeWords(side),
                                              page->totalFiles,
                                              MaxDiffFiles));
            }
            else if (!SameRecord(*first, *page))
                return std::unexpected { brokenStream() };
            std::ranges::move(page->inputs.files, std::back_inserter(record.inputs.files));
            auto const next = page->nextCursor;
            if (!first.has_value())
                first = std::move(*page);
            if (!next.has_value())
                break;
            if (received > MaxDiffBytesPerSide)
                return stopAt(std::move(record),
                              std::format("{}'s record passed {} bytes over {} pages before it ended, more than the {} "
                                          "bytes this tool reads per side",
                                          NodeWords(side),
                                          received,
                                          asked,
                                          MaxDiffBytesPerSide));
            if (asked >= MaxDiffPagesPerSide)
                return stopAt(std::move(record),
                              std::format("{}'s record had not ended after {} pages, the most this tool reads per side",
                                          NodeWords(side),
                                          MaxDiffPagesPerSide));
            cursor = *next;
        }
        if (record.inputs.files.size() != record.totalFiles)
            return std::unexpected { brokenStream() };
        return record;
    }

    /// @param path A path to list.
    /// @return It as a reader prints it, cut to `MaxListedPathBytes` with the cut said.
    [[nodiscard]] std::string ListedPath(std::string_view path)
    {
        if (path.size() <= MaxListedPathBytes)
            return Readable(path);
        // Cut back to a character boundary, so a cut through a multi-byte character does not read as bytes that are
        // not UTF-8.
        auto cut = MaxListedPathBytes;
        while (cut > 0 && (static_cast<unsigned char>(path[cut]) & 0xC0U) == 0x80U)
            --cut;
        return std::format("{}... ({} bytes in all)", Readable(path.substr(0, cut)), path.size());
    }

    /// @param paths A group of differing paths.
    /// @return `{count, paths, listed}`: the first `MaxListedDiffPaths`, and how many there are.
    [[nodiscard]] Json::Value PathGroup(std::vector<std::string> const& paths)
    {
        auto listed = Json::Array {};
        for (auto const& path: paths | std::views::take(MaxListedDiffPaths))
            listed.push_back(Str(ListedPath(path)));
        return Obj(Json::Object {
            { "count", Count(paths.size()) },
            { "listed", Count(listed.size()) },
            { "paths", Arr(std::move(listed)) },
        });
    }

    /// @param side A side.
    /// @param record Its record.
    /// @return Its result row.
    [[nodiscard]] Json::Value SideJson(DiffSide const& side, SideRecord const& record)
    {
        return Obj(Json::Object {
            { "node", side.target.asked.has_value() ? Str(Readable(*side.target.asked)) : Null() },
            { "fingerprint", Str(side.fingerprint) },
            { "toolchain", side.toolchain.has_value() ? Str(Readable(*side.toolchain)) : Null() },
            { "answered_by", Str(record.answeredBy) },
            { "version", Str(Readable(record.version)) },
            { "schema", Str(Readable(record.inputs.schema)) },
            { "grammar", Str(Readable(record.inputs.driverGrammar)) },
            { "total_files", Json::Unsigned(record.totalFiles) },
        });
    }

    /// @param pair Both texts, or nothing.
    /// @return `{a, b}`, or `null` when they agree.
    [[nodiscard]] Json::Value PairOrNull(std::optional<std::pair<std::string, std::string>> const& pair)
    {
        if (!pair.has_value())
            return Null();
        return Obj(Json::Object { { "a", Str(Readable(pair->first)) }, { "b", Str(Readable(pair->second)) } });
    }

    [[nodiscard]] Mcp::ToolResult ToolchainDiff(IMcpUpstream& upstream,
                                                IAppliedClusterView const& cluster,
                                                std::string const& selfId,
                                                std::string const& thisVersion,
                                                Mcp::ToolCall const& call)
    {
        auto a = SideOf(call, "a", upstream, cluster, selfId);
        if (!a.has_value())
            return std::move(a.error());
        auto b = SideOf(call, "b", upstream, cluster, selfId);
        if (!b.has_value())
            return std::move(b.error());
        auto left = FetchRecord(upstream, *a);
        if (!left.has_value())
            return std::move(left.error());
        auto right = FetchRecord(upstream, *b);
        if (!right.has_value())
            return std::move(right.error());

        auto const complete = left->complete && right->complete;
        auto const verdictFacts = [&] {
            // An incomplete side has no file list, so neither side's files are compared: the identity alone, and a
            // verdict that says the files were not compared rather than one that reads two empty lists as agreeing.
            return complete ? Distributed::DiffFingerprintInputs(left->inputs, right->inputs)
                            : Distributed::DiffFingerprintIdentity(left->inputs, right->inputs);
        };
        auto const versions = std::format("{} runs pace-node {} under fingerprint schema {}, {} runs pace-node {} under "
                                          "{}",
                                          NodeWords(*a),
                                          Readable(left->version),
                                          Readable(left->inputs.schema),
                                          NodeWords(*b),
                                          Readable(right->version),
                                          Readable(right->inputs.schema));

        auto skew = std::optional<std::string> {};
        auto recompute = std::string_view { "not-run" };
        if (complete)
        {
            // The recompute runs for every side digested under THIS build's schema, whatever its version string says:
            // a version is a label, and the digest is the fact. A side of another schema cannot be recomputed here,
            // and it does not stop the OTHER side from being checked: a stale side is reported whatever its partner is.
            auto const checked = [](DiffSide const& side, SideRecord const& record) -> std::optional<bool> {
                if (record.inputs.schema != Cc::FingerprintSchema)
                    return std::nullopt;
                return Cc::ComputeToolchainFingerprint(record.inputs) == side.fingerprint;
            };
            auto const leftOk = checked(*a, *left);
            auto const rightOk = checked(*b, *right);
            auto const failed = [](std::optional<bool> const& ok) {
                return ok.has_value() && !*ok;
            };
            auto const sameInputs =
                left->inputs.banner == right->inputs.banner && left->inputs.driverGrammar == right->inputs.driverGrammar
                && std::ranges::equal(left->inputs.files, right->inputs.files, [](auto const& x, auto const& y) {
                       return x.relativePath == y.relativePath && x.contentHash == y.contentHash;
                   });
            if (failed(leftOk) && failed(rightOk) && sameInputs)
            {
                // Both fail on ONE set of inputs: this node digests them to neither fingerprint, so it is this build
                // that differs, not either survey.
                skew = std::format("identical inputs, and this node (pace-node {}) reproduces neither fingerprint from "
                                   "them: a version skew between this node's digest and the two nodes' -- {}",
                                   Readable(thisVersion),
                                   versions);
                recompute = "failed";
            }
            else if (failed(leftOk) || failed(rightOk))
            {
                auto stale = std::vector<std::string> {};
                auto otherBuild = std::vector<std::string> {};
                for (auto const& [side, record, ok]:
                     { std::tuple { &*a, &*left, leftOk }, std::tuple { &*b, &*right, rightOk } })
                {
                    if (!failed(ok))
                        continue;
                    stale.push_back(NodeWords(*side));
                    // The recompute ran HERE, so a failing side on another build than this one may be this build's
                    // digest moving without a schema bump -- the cause the comparison cannot rule out.
                    if (record->version != thisVersion)
                        otherBuild.push_back(
                            std::format("{} runs pace-node {}", NodeWords(*side), Readable(record->version)));
                }
                auto refusal = stale.size() == 2
                                   ? std::format("{}'s and {}'s inputs no longer reproduce their fingerprints; their "
                                                 "toolchain surveys changed",
                                                 stale[0],
                                                 stale[1])
                                   : std::format("{}'s inputs no longer reproduce its fingerprint; its toolchain survey "
                                                 "changed",
                                                 stale[0]);
                if (!otherBuild.empty())
                    refusal += std::format(" -- inferred: or this node's pace-node {} digests differently from {} without "
                                           "a schema bump ({})",
                                           Readable(thisVersion),
                                           Joined(otherBuild, " and "),
                                           versions);
                for (auto const& [side, ok]: { std::pair { &*a, leftOk }, std::pair { &*b, rightOk } })
                    if (!ok.has_value())
                        refusal += std::format("; {} digests under another fingerprint schema, which this node cannot "
                                               "check ({})",
                                               NodeWords(*side),
                                               versions);
                return Error(std::move(refusal));
            }
            else if (!leftOk.has_value() || !rightOk.has_value())
                // Every side this node could check reproduced; a side it could not is a skew, named with both builds
                // rather than diffing inputs nobody checked.
                return Error(std::format("this node digests under fingerprint schema {} and cannot recompute a side "
                                         "digested under another, so it compares neither: a schema skew -- {}; run one "
                                         "pace-node build on both",
                                         Cc::FingerprintSchema,
                                         versions));
            else
                recompute = "passed";
        }

        auto const diff = verdictFacts();
        // Every incomplete side's reason, in order; both when both are.
        auto reasons = std::vector<std::string> {};
        for (auto const* const record: { &*left, &*right })
            if (!record->complete)
                reasons.push_back(record->incomplete);
        auto const incomplete =
            complete ? std::optional<std::string> {}
                     : std::optional<std::string> { std::format("{}; only the schema, banner and grammar were compared "
                                                                "and neither fingerprint was recomputed",
                                                                Joined(reasons, "; and ")) };
        auto const verdict = skew.value_or(diff.verdict);

        auto result = Json::Object {
            { "term", left->term.has_value() ? Int(static_cast<std::int64_t>(*left->term)) : Null() },
            { "term_scope", Str(std::string { TermScope }) },
            { "a", SideJson(*a, *left) },
            { "b", SideJson(*b, *right) },
            { "identical", Bool(!skew.has_value() && diff.Identical()) },
            { "same_fingerprint", Bool(a->fingerprint == b->fingerprint) },
            { "recompute", Str(std::string { recompute }) },
            { "complete", Bool(complete) },
            { "incomplete", StrOrNull(incomplete) },
            { "skew", StrOrNull(skew) },
            { "schema_differs", Bool(diff.schemaDiffers) },
            { "banner", PairOrNull(diff.banner) },
            { "grammar", PairOrNull(diff.grammar) },
            { "only_a", complete ? PathGroup(diff.onlyA) : Null() },
            { "only_b", complete ? PathGroup(diff.onlyB) : Null() },
            { "content_differs", complete ? PathGroup(diff.contentDiffers) : Null() },
            { "verdict", Str(verdict) },
            { "inferred", Bool(!skew.has_value() && diff.inferred) },
        };
        auto text = std::format("{} ({}) vs {} ({}): {}; recompute {}{}",
                                NodeWords(*a),
                                a->fingerprint,
                                NodeWords(*b),
                                b->fingerprint,
                                Readable(verdict),
                                recompute,
                                incomplete.has_value() ? std::format("; incomplete: {}", *incomplete) : std::string {});
        return Answer(std::move(result), std::move(text));
    }

    constexpr std::string_view ToolchainDiffInput =
        R"({"type":"object","required":["a","b"],"properties":{)"
        R"("a":{"type":"object","description":"One side: node (a member's id, matched exactly; omit for this node) and exactly one of fingerprint or toolchain (a served toolchain's label, e.g. \"gcc 13.2.0\", or its compiler path, matched exactly).","properties":{"node":{"type":"string"},"fingerprint":{"type":"string"},"toolchain":{"type":"string"}},"additionalProperties":false},)"
        R"("b":{"type":"object","description":"The other side, in the same shape as a.","properties":{"node":{"type":"string"},"fingerprint":{"type":"string"},"toolchain":{"type":"string"}},"additionalProperties":false}},"additionalProperties":false})";

    constexpr std::string_view ToolchainDiffOutput =
        R"({"type":"object","required":["term","term_scope","a","b","identical","same_fingerprint","recompute","complete","incomplete","skew","schema_differs","banner","grammar","only_a","only_b","content_differs","verdict","inferred"],"properties":{)"
        R"("term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("a":{"type":"object","description":"node (null: this node), fingerprint, toolchain (what it was resolved from, or null), answered_by, version (its pace-node), schema, grammar, total_files."},"b":{"type":"object"},)"
        R"("identical":{"type":"boolean"},"same_fingerprint":{"type":"boolean"},)"
        R"("recompute":{"enum":["passed","not-run","failed"],"description":"passed: both records recomputed to their fingerprints on this node before they were compared. failed: neither reproduced from identical inputs (see skew). not-run: a side was incomplete, so nothing was recomputed. A side that alone fails is refused as stale."},)"
        R"("complete":{"type":"boolean"},"incomplete":{"type":["string","null"],"description":"Why the files were not compared: a side passed the files, bytes or pages this tool reads per side. Only schema, banner and grammar are then compared, and inferred is false."},)"
        R"("skew":{"type":["string","null"],"description":"Identical inputs that this node reproduces to neither fingerprint: a version skew, naming both versions and schemas."},)"
        R"("schema_differs":{"type":"boolean"},"banner":{"type":["object","null"],"description":"Both banners verbatim when they differ."},"grammar":{"type":["object","null"]},)"
        R"("only_a":{"type":["object","null"],"description":"{count, listed, paths}: relative paths only side a holds; at most 500 listed."},"only_b":{"type":["object","null"]},"content_differs":{"type":["object","null"]},)"
        R"("verdict":{"type":"string"},"inferred":{"type":"boolean","description":"true when the verdict carries an inference (marked 'inferred:'), beyond what the comparison shows."}}})";

    constexpr std::string_view NoArguments = R"({"type":"object","properties":{},"additionalProperties":false})";

    constexpr std::string_view ToolchainsOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","toolchain_count","machine_count","toolchains","nodes","unserved_note","demand","demand_note","upgrade_report"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("toolchain_count":{"type":"integer","description":"Distinct fingerprints."},)"
        R"("machine_count":{"type":"integer","description":"Distinct worker endpoints: a machine serving two toolchains counts once."},)"
        R"("toolchains":{"type":"array","description":"Per fingerprint: label and version (\"unknown\" when the label does not parse) where its entries agree on one label, every label in labels, family as the node read it off the banner (null when none or disagreeing), driver (the label's compiler name without a version suffix, null when the label does not parse), and the machines serving it."},)"
        R"("nodes":{"type":"array","description":"Per machine: the fingerprints it serves, and unserved -- the fleet's fingerprints in the same families it does not serve; null when it serves no toolchain of a known family (see unserved_note)."},)"
        R"("unserved_note":{"type":"string"},"demand":{"type":"null","description":"Absent: no demand per toolchain is recorded yet."},"demand_note":{"type":"string"},)"
        R"("upgrade_report":{"type":"array","description":"Per banner-derived compiler family: recommended_version, and recommended_fingerprints -- an object from DRIVER (gcc, cc, g++: one install answers to several) to fingerprint, a driver absent when its builds of the version cannot be told apart, null when the ordering is unknown; basis (the versions compared and any demand), and machines placed by their highest version of the family: behind (lower: should upgrade), ahead (higher), same_version_other_fingerprint (the recommended version under another fingerprint for the SAME driver: ask toolchain_diff); ordering_unknown."}}})";

} // namespace

std::vector<Mcp::ToolRow> ToolchainTools(IMcpUpstream& upstream,
                                         IAppliedClusterView const& cluster,
                                         std::string selfId,
                                         std::string thisVersion)
{
    auto* const to = &upstream;
    auto const* const in = &cluster;
    return {
        Mcp::ToolRow {
            .name = "toolchains",
            .title = "Toolchains",
            .description =
                "Which compiler toolchains the fleet serves, from the leader's /fleet.json: per fingerprint its "
                "label, compiler family (as the node read it off the compiler's banner, so Apple clang installed as "
                "`gcc` is apple-clang, never gcc), version as parsed or `unknown`, and the machines serving it; per "
                "machine the fingerprints it serves and the fleet's toolchains in its families it does not serve. "
                "Machines are counted once however many toolchains they serve. Includes a conservative upgrade "
                "report per family: it names a version to converge on only when every version in the family parses "
                "unambiguously, and places each machine by its highest version -- behind (lower), ahead (higher), or "
                "same_version_other_fingerprint (the same version built differently for the same driver: ask "
                "toolchain_diff why). It names a fingerprint per DRIVER (recommended_fingerprints: gcc and cc are one "
                "install under two names), and none for a driver whose builds of one version demand cannot tell "
                "apart. Every row carries its "
                "basis -- the versions compared and the demand behind them -- so it can be checked. `demand` is "
                "null: absent, not empty. `term` is the term THIS node holds.",
            .inputSchema = NoArguments,
            .outputSchema = ToolchainsOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to](Mcp::ToolCall const& call) { return Toolchains(*to, call); },
        },
        Mcp::ToolRow {
            .name = "toolchain_diff",
            .title = "Toolchain diff",
            .description =
                "Why two toolchain fingerprints differ, even when the name and version are the same. Each side is a "
                "node (a member's id, matched exactly; this node when omitted) and a fingerprint, or a toolchain label "
                "or compiler path that names exactly one toolchain that node serves -- an ambiguous one is an error "
                "listing the candidates. Each node sends the inputs its survey digested -- the schema, the compiler's "
                "banner, the driver grammar and every include-tree file with its content digest -- and THIS node "
                "recomputes each fingerprint from them before comparing (recompute: passed), refusing a record that "
                "no longer reproduces. The result names what differs: schema, both banners, both grammars, files only "
                "on a, only on b, and on both with different content, each with a count. The verdict states only what "
                "the comparison shows; anything more is marked 'inferred:' and inferred is true. A side whose "
                "record does not reproduce its fingerprint is refused as stale; identical inputs reproducing neither "
                "fingerprint, or a side digested under another schema, are reported as a skew naming both versions and "
                "schemas. A side past 200000 files, its byte bound or its page bound is compared on schema, banner and "
                "grammar only (complete: false), and the verdict says the files were not compared. `term` is the term "
                "THIS node holds.",
            .inputSchema = ToolchainDiffInput,
            .outputSchema = ToolchainDiffOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to, in, self = std::move(selfId), version = std::move(thisVersion)](
                           Mcp::ToolCall const& call) { return ToolchainDiff(*to, *in, self, version, call); },
        },
    };
}

} // namespace Pace::Node
