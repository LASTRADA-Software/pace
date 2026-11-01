// SPDX-License-Identifier: Apache-2.0
#include "McpFleetTools.hpp"
#include "McpToolSupport.hpp"
#include "McpToolchainTools.hpp"

#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>

#include <expected>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
            auto family = StringOrNullAt(row, "family");
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
            { "version", Str(parsed.has_value() ? Distributed::VersionText(parsed->parts) : std::string { "unknown" }) },
            { "nodes", Strings(row.endpoints) },
        });
    }

    /// @param advice One family's advice.
    /// @return Its `upgrade_report` row.
    [[nodiscard]] Json::Value AdviceJson(Distributed::UpgradeAdvice const& advice)
    {
        return Obj(Json::Object {
            { "family", Str(advice.family) },
            { "recommended_version", StrOrNull(advice.recommendedVersion) },
            { "recommended_fingerprint", StrOrNull(advice.recommendedFingerprint) },
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
    /// @return How the text says it.
    [[nodiscard]] std::string AdviceWords(Distributed::UpgradeAdvice const& advice)
    {
        if (!advice.recommendedVersion.has_value())
            return std::format("{}: no recommendation ({})", advice.family, advice.basis);
        return std::format("{}: converge on {} ({}) -- behind: {}; ahead: {}; same version, other fingerprint (ask "
                           "toolchain_diff): {} ({})",
                           advice.family,
                           *advice.recommendedVersion,
                           advice.recommendedFingerprint.value_or("no single fingerprint"),
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

    constexpr std::string_view NoArguments = R"({"type":"object","properties":{},"additionalProperties":false})";

    constexpr std::string_view ToolchainsOutput =
        R"({"type":"object","required":["answered_by","term","term_scope","toolchain_count","machine_count","toolchains","nodes","unserved_note","demand","demand_note","upgrade_report"],"properties":{)"
        R"("answered_by":{"type":"string"},"term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("toolchain_count":{"type":"integer","description":"Distinct fingerprints."},)"
        R"("machine_count":{"type":"integer","description":"Distinct worker endpoints: a machine serving two toolchains counts once."},)"
        R"("toolchains":{"type":"array","description":"Per fingerprint: label and version (\"unknown\" when the label does not parse) where its entries agree on one label, every label in labels, family as the node read it off the banner (null when none or disagreeing), and the machines serving it."},)"
        R"("nodes":{"type":"array","description":"Per machine: the fingerprints it serves, and unserved -- the fleet's fingerprints in the same families it does not serve; null when it serves no toolchain of a known family (see unserved_note)."},)"
        R"("unserved_note":{"type":"string"},"demand":{"type":"null","description":"Absent: no demand per toolchain is recorded yet."},"demand_note":{"type":"string"},)"
        R"("upgrade_report":{"type":"array","description":"Per banner-derived compiler family: recommended_version and recommended_fingerprint (null when the report cannot say), basis (the versions compared and any demand), and machines placed by their highest version of the family: behind (lower: should upgrade), ahead (higher), same_version_other_fingerprint (the recommended version built differently: ask toolchain_diff); ordering_unknown."}}})";

} // namespace

std::vector<Mcp::ToolRow> ToolchainTools(IMcpUpstream& upstream)
{
    auto* const to = &upstream;
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
                "same_version_other_fingerprint (the same version built differently: ask toolchain_diff why); it "
                "names no fingerprint between two of one version unless demand settles it. Every row carries its "
                "basis -- the versions compared and the demand behind them -- so it can be checked. `demand` is "
                "null: absent, not empty. `term` is the term THIS node holds.",
            .inputSchema = NoArguments,
            .outputSchema = ToolchainsOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to](Mcp::ToolCall const& call) { return Toolchains(*to, call); },
        },
    };
}

} // namespace Pace::Node
