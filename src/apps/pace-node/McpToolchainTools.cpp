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

    /// What `unserved_note` says: why every machine's `unserved` is `null`.
    constexpr std::string_view UnservedNote =
        "the fleet document lists the toolchains each machine serves and carries no list of toolchains a machine "
        "identified but does not serve, so every machine's `unserved` is null (absent), not an empty list";

    /// What `demand_note` says: why `demand` is `null`.
    constexpr std::string_view DemandNote =
        "no demand per toolchain is recorded yet, so `demand` is null (absent) and every recommendation is made on "
        "versions alone";

    /// The `workers` rows, as the upgrade report reads them.
    using WorkerRows = std::vector<Distributed::WorkerReport>;

    /// Read the fleet document's `workers` rows: each one's fingerprint, endpoint and label.
    ///
    /// A row this build cannot read refuses the whole answer rather than being skipped: a machine dropped from the
    /// count would be a node silently absent from "nodes behind".
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
            auto const* const compiler = row.Find("compiler");
            if (!fingerprint.has_value() || !endpoint.has_value() || compiler == nullptr
                || (!compiler->IsNull() && compiler->AsString() == nullptr))
                return std::unexpected { std::string {
                    "a \"workers\" row without a string toolchain and endpoint and a string-or-null compiler" } };
            auto worker = Distributed::WorkerReport {};
            worker.info.fingerprint = std::move(*fingerprint);
            worker.info.endpoint = std::move(*endpoint);
            if (auto const* const label = compiler->AsString(); label != nullptr)
                worker.info.toolchainLabel = *label;
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

    /// One fingerprint, gathered across its registry entries.
    struct ToolchainRow
    {
        std::set<std::string> labels;    ///< Every distinct label its entries carry.
        std::set<std::string> endpoints; ///< The machines serving it.
    };

    /// @param fingerprint The toolchain.
    /// @param row What its entries say.
    /// @return Its `toolchains` row: the label, family and version only where the entries agree on ONE label.
    [[nodiscard]] Json::Value ToolchainJson(std::string const& fingerprint, ToolchainRow const& row)
    {
        auto const single = row.labels.size() == 1 ? std::optional<std::string> { *row.labels.begin() } : std::nullopt;
        auto const family = single.has_value() ? Distributed::ToolchainFamilyOf(*single) : std::nullopt;
        auto const parsed = single.has_value() ? Distributed::ParseToolchainLabel(*single) : std::nullopt;
        return Obj(Json::Object {
            { "fingerprint", Str(fingerprint) },
            { "label", StrOrNull(single) },
            { "labels", Strings(row.labels) },
            { "family", StrOrNull(family) },
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
            { "recommended_fingerprint", StrOrNull(advice.recommendedFingerprint) },
            { "basis", Str(advice.basis) },
            { "nodes_behind", Strings(advice.nodesBehind) },
            { "ordering_unknown", Bool(advice.orderingUnknown) },
        });
    }

    /// @param advice One family's advice.
    /// @return How the text says it.
    [[nodiscard]] std::string AdviceWords(Distributed::UpgradeAdvice const& advice)
    {
        if (!advice.recommendedFingerprint.has_value())
            return std::format("{}: no recommendation ({})", advice.family, advice.basis);
        return std::format("{}: recommend {} ({}); behind: {}",
                           advice.family,
                           *advice.recommendedFingerprint,
                           advice.basis,
                           advice.nodesBehind.empty() ? std::string { "nobody" } : Joined(advice.nodesBehind, ", "));
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
        auto unlabelled = std::set<std::string> {};
        for (auto const& worker: *workers)
        {
            auto& row = toolchains[worker.info.fingerprint];
            row.endpoints.insert(worker.info.endpoint);
            if (!worker.info.toolchainLabel.empty())
                row.labels.insert(worker.info.toolchainLabel);
            machines[worker.info.endpoint].insert(worker.info.fingerprint);
        }
        for (auto const& [fingerprint, row]: toolchains)
            if (row.labels.empty())
                unlabelled.insert(fingerprint);

        // No demand is recorded yet: the report recommends on versions alone, and says so in every basis.
        auto const advice = Distributed::AdviseUpgrades(*workers, {});

        auto toolchainRows = Json::Array {};
        for (auto const& [fingerprint, row]: toolchains)
            toolchainRows.push_back(ToolchainJson(fingerprint, row));
        auto machineRows = Json::Array {};
        for (auto const& [endpoint, served]: machines)
            machineRows.push_back(Obj(Json::Object {
                { "endpoint", Str(endpoint) },
                { "served", Strings(served) },
                { "unserved", Null() },
            }));
        auto adviceRows = Json::Array {};
        auto adviceWords = std::vector<std::string> {};
        for (auto const& family: advice)
        {
            adviceRows.push_back(AdviceJson(family));
            adviceWords.push_back(AdviceWords(family));
        }

        auto text = std::format(
            "{} toolchain(s) on {} machine(s); upgrade report (conservative -- check each basis): {}{}; unserved "
            "toolchains per machine: not reported ({}); demand: not recorded; {}",
            toolchains.size(),
            machines.size(),
            adviceWords.empty() ? std::string { "no labelled toolchain" } : Joined(adviceWords, " | "),
            unlabelled.empty() ? std::string {}
                               : std::format("; {} carry no label and join no family", Joined(unlabelled, ", ")),
            UnservedNote,
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
        R"("toolchains":{"type":"array","description":"Per fingerprint: label, family and version (\"unknown\" when the label does not parse) where its entries agree on one label, every label in labels, and the machines serving it."},)"
        R"("nodes":{"type":"array","description":"Per machine: the fingerprints it serves; unserved is null (absent, see unserved_note)."},)"
        R"("unserved_note":{"type":"string"},"demand":{"type":"null","description":"Absent: no demand per toolchain is recorded yet."},"demand_note":{"type":"string"},)"
        R"("upgrade_report":{"type":"array","description":"Per compiler family: recommended_fingerprint (null when the report cannot say), basis (the versions compared and any demand), nodes_behind, ordering_unknown."}}})";

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
                "label, compiler family (the label's compiler name), version as parsed or `unknown`, and the "
                "machines serving it; per machine the fingerprints it serves. Machines are counted once however "
                "many toolchains they serve. Includes a conservative upgrade report per family: it names a "
                "recommended toolchain and the machines behind it only when every version in the family parses "
                "unambiguously, never for `cc`/`c++` (a policy, not a product), and never between two fingerprints "
                "of one version (ask toolchain_diff why they differ); every row carries its basis -- the versions "
                "compared and the demand behind them -- so it can be checked. `unserved` and `demand` are null: "
                "absent, not empty. `term` is the term THIS node holds.",
            .inputSchema = NoArguments,
            .outputSchema = ToolchainsOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to](Mcp::ToolCall const& call) { return Toolchains(*to, call); },
        },
    };
}

} // namespace Pace::Node
