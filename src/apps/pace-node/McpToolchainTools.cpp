// SPDX-License-Identifier: Apache-2.0
#include "McpFleetTools.hpp"
#include "McpToolSupport.hpp"
#include "McpToolchainTools.hpp"

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/CompileCache/VersionName.hpp>
#include <Pace/Core/SaturatingAdd.hpp>
#include <Pace/Distributed/CompilerVersion.hpp>
#include <Pace/Distributed/FingerprintDiff.hpp>
#include <Pace/Distributed/FleetHistory.hpp>
#include <Pace/Distributed/WorkerRegistry.hpp>
#include <Pace/Protocol/FingerprintInputsWire.hpp>
#include <Pace/Protocol/NodeStatusView.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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

    /// What `demand_note` says when `demand` is present: what its figures are and are not.
    constexpr std::string_view DemandNoteRead =
        "`demand` is the leader's toolchain-demand history over the window named, counted only for the minutes it was "
        "counting (`observed_minutes` of `window_minutes`, since `coverage_since`): a total over part of a window is "
        "that part's. Compiles folded into `other` are named by no fingerprint, so they count toward no toolchain's "
        "share and no recommendation";

    /// @return The history range the upgrade advice weighs demand over.
    [[nodiscard]] Distributed::FleetRangeRow const& DemandRange() noexcept
    {
        return Distributed::FleetRangeTable[static_cast<std::size_t>(Distributed::FleetRange::Week)];
    }

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
            // Likewise `node-id`: a leader that renders none names no member, which is null, never a refusal.
            auto memberId = row.Find("node-id") == nullptr ? std::optional<std::string> { std::string {} }
                                                           : StringOrNullAt(row, "node-id");
            if (!fingerprint.has_value() || !endpoint.has_value() || !label.has_value() || !family.has_value()
                || !memberId.has_value())
                return std::unexpected { std::string { "a \"workers\" row without a string toolchain and endpoint "
                                                       "and a string-or-null compiler, family and node-id" } };
            auto worker = Distributed::WorkerReport {};
            worker.info.fingerprint = std::move(*fingerprint);
            worker.info.endpoint = std::move(*endpoint);
            worker.info.toolchainLabel = std::move(*label);
            worker.info.toolchainFamily = std::move(*family);
            worker.memberId = std::move(*memberId);
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
            { "lacks_recommended", Strings(advice.nodesLackingRecommended) },
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
                           "toolchain_diff): {}; lacking the recommended fingerprint: {} ({})",
                           advice.family,
                           *advice.recommendedVersion,
                           ChoicesWords(advice),
                           MachinesWords(advice.nodesBehind),
                           MachinesWords(advice.nodesAhead),
                           MachinesWords(advice.nodesSameVersionOtherFingerprint),
                           MachinesWords(advice.nodesLackingRecommended),
                           advice.basis);
    }

    /// How many minutes of a window the leader must have counted before demand may DECIDE a recommendation: 24 hours,
    /// within the 7-day window asked for.
    ///
    /// A recommendation sends an operator to install a compiler, and a day is the shortest span that holds one full
    /// working cycle of a team's builds: an hour of counts is whatever one person happened to be building, and a
    /// leader that took office minutes ago has seen almost nothing. Below this the figures are still SHOWN, but the
    /// report recommends on versions alone and says how much was observed against how much is required.
    constexpr std::uint64_t MinDemandCoverageMinutes = 1440;

    /// What the leader's `toolchain_demand` section said, read for the upgrade report.
    struct DemandReading
    {
        std::vector<Distributed::DemandRow> rows; ///< One per named fingerprint; empty unless `weighed`.
        std::uint64_t unattributable {};          ///< Compiles in `other` and `unattributed`: no fingerprint.
        bool weighed { false };                   ///< Whether `rows` may decide a recommendation.
        std::string notWeighedBecause;            ///< Why demand was read but must not decide; empty when it may.
        bool available { false };                 ///< Whether there is a covered window to show.
        std::string basis;                        ///< Words appended to every ordered family's basis: what was used.
        Json::Value json;                         ///< The `demand` member: the figures used, or `null`.
    };

    /// @param demand What was read.
    /// @return The reading in one sentence, with the reason it does not decide when it does not.
    [[nodiscard]] std::string Summary(DemandReading const& demand)
    {
        return demand.notWeighedBecause.empty()
                   ? demand.basis
                   : std::format("{}; demand was read but does not decide ({})", demand.basis, demand.notWeighedBecause);
    }

    /// @param section A `toolchain_demand` section.
    /// @param key A member.
    /// @return The member as a count, or nothing when it is absent or not one.
    [[nodiscard]] std::optional<std::uint64_t> UintAt(Json::Value const& section, std::string_view key)
    {
        auto const* const member = section.Find(key);
        return member != nullptr ? member->AsUint() : std::nullopt;
    }

    /// Read the demand the upgrade report weighs from the fleet document.
    ///
    /// **Absent is not zero.** A section that is `null` (the answering node is no leader, or keeps no such history), one
    /// this build cannot read, or one that counted for no minute at all is UNAVAILABLE, and the report then recommends
    /// on versions alone and says so; it never reads those as "no toolchain was asked for".
    /// @param root The fleet document.
    /// @param range The window the document was asked for.
    /// @return What was read.
    [[nodiscard]] DemandReading ReadDemand(Json::Value const& root, std::string_view range)
    {
        auto const unavailable = [&](std::string_view why) {
            return DemandReading { .rows = {},
                                   .unattributable = 0,
                                   .weighed = false,
                                   .notWeighedBecause = {},
                                   .available = false,
                                   .basis = std::format("demand over {} was unavailable ({}), so this recommendation is "
                                                        "made on versions alone -- absence of demand data is not "
                                                        "zero demand",
                                                        range,
                                                        why),
                                   .json = Null() };
        };
        auto const* const section = root.Find("toolchain_demand");
        if (section == nullptr || section->IsNull())
            return unavailable("the answering node keeps no toolchain-demand history; a leader counts only while it leads");
        auto const* const keysMember = section->Find("keys");
        auto const* const keys = keysMember != nullptr ? keysMember->AsArray() : nullptr;
        auto const observed = UintAt(*section, "observed_minutes");
        auto const window = UintAt(*section, "window_minutes");
        auto const total = UintAt(*section, "total");
        if (keys == nullptr || !observed.has_value() || !window.has_value() || !total.has_value())
            return unavailable("the section is not one this build reads");
        if (*observed == 0)
            return unavailable("the leader counted for none of the window's minutes");
        auto const sufficient = *observed >= MinDemandCoverageMinutes;
        // What no row names. A member that is absent or malformed makes its figure UNKNOWN, never zero: demand then
        // does not decide, rather than deciding as if nothing were folded.
        auto const* const other = section->Find("other");
        auto const* const unattributed = section->Find("unattributed");
        auto const otherOk =
            other != nullptr
            && (other->IsNull() || (UintAt(*other, "keys_folded").has_value() && UintAt(*other, "total").has_value()));
        auto const unattributedOk =
            unattributed != nullptr && (unattributed->IsNull() || UintAt(*unattributed, "total").has_value());

        auto reading = DemandReading {};
        auto named = std::uint64_t { 0 };
        auto rows = Json::Array {};
        for (auto const& row: *keys)
        {
            auto fingerprint = StringAt(row, "fingerprint");
            auto const compiles = UintAt(row, "total");
            if (!fingerprint.has_value() || !compiles.has_value())
                return unavailable("a row of the section is not one this build reads");
            named = SaturatingAdd(named, *compiles);
            // A key that was folded away and came back shows only what it did since: its count is a floor.
            auto const earlier = StringAt(row, "earlier_counts");
            auto const atLeast = earlier.value_or("unknown") != "none";
            if (sufficient && otherOk && unattributedOk)
                reading.rows.push_back(
                    Distributed::DemandRow { .fingerprint = *fingerprint, .compiles = *compiles, .atLeast = atLeast });
            rows.push_back(Obj(Json::Object { { "fingerprint", Str(*fingerprint) },
                                              { "compiles", Int(static_cast<std::int64_t>(*compiles)) },
                                              { "at_least", Bool(atLeast) } }));
        }
        auto const* const sinceMember = section->Find("coverage_since_ms");
        auto const since = sinceMember != nullptr ? sinceMember->AsInt() : std::nullopt;
        auto const foldedKeys = otherOk && !other->IsNull() ? UintAt(*other, "keys_folded") : std::nullopt;
        auto const folded = otherOk && !other->IsNull() ? UintAt(*other, "total") : std::nullopt;
        auto const unnamed = unattributedOk && !unattributed->IsNull() ? UintAt(*unattributed, "total") : std::nullopt;

        reading.available = true;
        reading.weighed = sufficient && otherOk && unattributedOk;
        reading.unattributable = SaturatingAdd(folded.value_or(0), unnamed.value_or(0));
        if (!sufficient)
            reading.notWeighedBecause = std::format("coverage is insufficient ({} minute(s) observed, {} required); the "
                                                    "figures are shown but do not decide",
                                                    *observed,
                                                    MinDemandCoverageMinutes);
        else if (!otherOk || !unattributedOk)
            reading.notWeighedBecause = std::format("the section's `{}` member is missing or malformed, so the compiles it "
                                                    "holds are unknown, not zero",
                                                    !otherOk ? "other" : "unattributed");
        reading.basis = std::format(
            "demand over {}: counted for {} of the window's {} minute(s), since {}{}; {} compile(s) "
            "named a toolchain of {} counted{}",
            range,
            *observed,
            *window,
            since.has_value() ? Rfc3339(*since).value_or("an out-of-range instant") : std::string { "an unknown instant" },
            *observed < *window ? " -- PARTIAL coverage, these counts are of that part only" : std::string {},
            named,
            *total,
            folded.has_value() && foldedKeys.has_value()
                ? std::format("; {} more are in `other ({} toolchain{})` and are attributed to no "
                              "toolchain, so named counts are \"at least\" figures and demand decides a "
                              "family only where its lead exceeds them",
                              *folded,
                              *foldedKeys,
                              *foldedKeys == 1 ? "" : "s")
                : std::string {});
        if (auto const carryNone = unnamed.value_or(0); carryNone > 0)
            reading.basis += std::format("; {} more carry no fingerprint (`unattributed`)", carryNone);
        auto figures =
            Json::Object { { "range", Str(std::string { range }) },
                           { "observed_minutes", Int(static_cast<std::int64_t>(*observed)) },
                           { "window_minutes", Int(static_cast<std::int64_t>(*window)) },
                           { "coverage_since", since.has_value() ? StrOrNull(Rfc3339(*since)) : Null() },
                           { "partial_coverage", Bool(*observed < *window) },
                           { "total", Int(static_cast<std::int64_t>(*total)) },
                           { "toolchains", Arr(std::move(rows)) },
                           { "other",
                             folded.has_value() && foldedKeys.has_value()
                                 ? Obj(Json::Object { { "toolchains_folded", Int(static_cast<std::int64_t>(*foldedKeys)) },
                                                      { "compiles", Int(static_cast<std::int64_t>(*folded)) } })
                                 : Null() } };
        reading.json = Obj(std::move(figures));
        return reading;
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
        {
            auto const& named = toolchains.at(fingerprint).families;
            families.insert(named.begin(), named.end());
        }
        if (families.empty())
            return std::nullopt;
        auto unserved = std::vector<std::string> {};
        for (auto const& [fingerprint, row]: toolchains)
            if (!served.contains(fingerprint)
                && std::ranges::any_of(row.families, [&](std::string const& family) { return families.contains(family); }))
                unserved.push_back(fingerprint);
        return unserved;
    }

    [[nodiscard]] Mcp::ToolResult Toolchains(IMcpUpstream& upstream,
                                             IAppliedClusterView const& cluster,
                                             Mcp::ToolCall const& /*call*/)
    {
        // The demand window the advice weighs: a week, so one quiet afternoon does not pick the compiler. The
        // `workers` rows do not depend on the range.
        auto const demandRange = DemandRange().key;
        auto document = McpFleet::AskFleetDocument(upstream, demandRange);
        if (!document.has_value())
            return std::move(document.error());
        auto workers = WorkersOf(document->root);
        if (!workers.has_value())
            return Error(std::format("the fleet document {} answered carries {}; nothing it said is reported",
                                     document->reply.answeredBy,
                                     workers.error()));

        auto toolchains = std::map<std::string, ToolchainRow> {};
        auto machines = std::map<std::string, std::set<std::string>> {};
        // The member ids the LEADER joined each registration to, by the key its connection PROVED (`node-id`), per
        // endpoint. Two registrations claiming one endpoint under different keys name nobody: a guess either way.
        auto memberIds = std::map<std::string, std::set<std::string>> {};
        for (auto const& worker: *workers)
        {
            auto& row = toolchains[worker.info.fingerprint];
            row.endpoints.insert(worker.info.endpoint);
            if (!worker.info.toolchainLabel.empty())
                row.labels.insert(worker.info.toolchainLabel);
            if (!worker.info.toolchainFamily.empty())
                row.families.insert(worker.info.toolchainFamily);
            machines[worker.info.endpoint].insert(worker.info.fingerprint);
            memberIds[worker.info.endpoint].insert(worker.memberId);
        }
        auto familyless = std::set<std::string> {};
        for (auto const& [fingerprint, row]: toolchains)
            if (row.families.empty())
                familyless.insert(fingerprint);

        // Demand is the leader's own record over the window; where it is unavailable the report recommends on versions
        // alone, and says so in every basis it writes.
        auto const demand = ReadDemand(document->root, demandRange);
        auto advice =
            Distributed::AdviseUpgrades(*workers,
                                        demand.rows,
                                        Distributed::DemandContext { .unattributable = demand.unattributable,
                                                                     .weighed = demand.weighed,
                                                                     .notWeighedBecause = demand.notWeighedBecause });
        for (auto& family: advice)
            if (!family.orderingUnknown)
                family.basis += std::format("; {}", demand.basis);

        auto toolchainRows = Json::Array {};
        for (auto const& [fingerprint, row]: toolchains)
            toolchainRows.push_back(ToolchainJson(fingerprint, row));
        auto machineRows = Json::Array {};
        auto unservedWords = std::vector<std::string> {};
        auto const state = cluster.AppliedState();
        auto unplaced = std::size_t { 0 };
        for (auto const& [endpoint, served]: machines)
        {
            auto const unserved = UnservedBy(served, toolchains);
            if (unserved.has_value() && !unserved->empty())
                unservedWords.push_back(std::format("{} lacks {}", endpoint, Joined(*unserved, ", ")));
            // Named by the member id its registrations' PROVEN key joined to, when they agree on exactly one --
            // never by the endpoint, which a registration only claims.
            auto const& ids = memberIds[endpoint];
            auto const naming = NamingOfMember(state, ids.size() == 1 ? *ids.begin() : std::string {});
            unplaced += Unplaced(naming) ? 1U : 0U;
            auto row = Json::Object { { "endpoint", Str(endpoint) } };
            AppendNaming(row, naming);
            row.emplace_back("served", Strings(served));
            row.emplace_back("unserved", unserved.has_value() ? Strings(*unserved) : Null());
            machineRows.push_back(Obj(std::move(row)));
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
            "their families: {}; {}{}; {}",
            toolchains.size(),
            machines.size(),
            adviceWords.empty() ? std::string { "no toolchain of a known family" } : Joined(adviceWords, " | "),
            familyless.empty() ? std::string {}
                               : std::format("; {} registered no family and join none", Joined(familyless, ", ")),
            unservedWords.empty() ? std::string { "none" } : Joined(unservedWords, "; "),
            Summary(demand),
            UnplacedNote(unplaced),
            Provenance(document->reply));

        auto result = Envelope(document->reply);
        result.emplace_back("toolchain_count", Count(toolchains.size()));
        result.emplace_back("machine_count", Count(machines.size()));
        result.emplace_back("toolchains", Arr(std::move(toolchainRows)));
        result.emplace_back("nodes", Arr(std::move(machineRows)));
        result.emplace_back("unserved_note", Str(std::string { UnservedNote }));
        result.emplace_back("demand", demand.available ? demand.json : Null());
        result.emplace_back("demand_note", Str(demand.weighed ? std::string { DemandNoteRead } : Summary(demand)));
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
    using McpToolchainDiff::TotalBudget;
    static_assert(TotalBudget == std::chrono::seconds { 30 },
                  "toolchain_diff's description states its budget as 30 seconds");

    /// One `toolchain_diff` call's upstream: every question is put to @p inner until `TotalBudget` has passed since
    /// the call began, and refused `BudgetSpent` after it, without asking anyone.
    ///
    /// A decorator rather than a check at each of the three places the tool asks, so no question can be added that
    /// the budget does not see.
    class BudgetedUpstream final: public IMcpUpstream
    {
      public:
        /// @param inner Where questions go; must outlive this.
        /// @param clock What the budget is measured on; must outlive this.
        BudgetedUpstream(IMcpUpstream& inner, core::platform::IClock const& clock) noexcept:
            _inner { inner },
            _clock { clock },
            _began { clock.now() }
        {
        }

        /// @copydoc IMcpUpstream::Ask
        [[nodiscard]] UpstreamResult Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame) override
        {
            auto const spent = std::chrono::duration_cast<std::chrono::milliseconds>(_clock.now() - _began);
            if (spent >= TotalBudget)
                return std::unexpected { std::pair {
                    UpstreamFailure::BudgetSpent,
                    std::format("{}: toolchain_diff starts no question more than {} ms after it began, and question "
                                "{} was the first past that; the {} question(s) before it took that long between them, "
                                "which does not say which node was slow -- ask again",
                                UpstreamFailureRowOf(UpstreamFailure::BudgetSpent).sentence,
                                TotalBudget.count(),
                                _asked + 1,
                                _asked) } };
            ++_asked;
            return _inner.Ask(target, nodeIdOrEmpty, std::move(frame));
        }

      private:
        IMcpUpstream& _inner;
        core::platform::IClock const& _clock;
        core::platform::SteadyTimePoint _began;
        std::size_t _asked { 0 };
    };

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
        // A spent budget is no fault of the node this question was for, so it is named as the question refused,
        // never as the node that failed.
        if (!answer.has_value() && answer.error().first == UpstreamFailure::BudgetSpent)
            return std::unexpected { Error(std::format("side {}'s question to {} was not asked: {}: {}",
                                                       side.key,
                                                       NodeWords(side),
                                                       UpstreamFailureRowOf(answer.error().first).name,
                                                       answer.error().second)) };
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
            // The member RESOLVED, never the text as asked: `node` echoes the caller, these name the machine.
            { "node_id", StrOrNull(side.target.naming.nodeId) },
            { "display_name", StrOrNull(side.target.naming.displayName) },
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

    [[nodiscard]] Mcp::ToolResult ToolchainDiff(IMcpUpstream& unbudgeted,
                                                IAppliedClusterView const& cluster,
                                                core::platform::IClock const& clock,
                                                std::string const& selfId,
                                                std::string const& thisVersion,
                                                Mcp::ToolCall const& call)
    {
        // Every question below goes through the one budget, the resolves included.
        auto upstream = BudgetedUpstream { unbudgeted, clock };
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
        R"("a":{"type":"object","description":"One side: node (a member's id or display name, matched exactly; omit for this node) and exactly one of fingerprint or toolchain (a served toolchain's label, e.g. \"gcc 13.2.0\", or its compiler path, matched exactly).","properties":{"node":{"type":"string"},"fingerprint":{"type":"string"},"toolchain":{"type":"string"}},"additionalProperties":false},)"
        R"("b":{"type":"object","description":"The other side, in the same shape as a.","properties":{"node":{"type":"string"},"fingerprint":{"type":"string"},"toolchain":{"type":"string"}},"additionalProperties":false}},"additionalProperties":false})";

    constexpr std::string_view ToolchainDiffOutput =
        R"({"type":"object","required":["term","term_scope","a","b","identical","same_fingerprint","recompute","complete","incomplete","skew","schema_differs","banner","grammar","only_a","only_b","content_differs","verdict","inferred"],"properties":{)"
        R"("term":{"type":["integer","null"]},"term_scope":{"const":"this-node"},)"
        R"("a":{"type":"object","description":"node (the name as asked; null: this node), node_id (the member resolved, null when this node has no id), display_name (what the cluster records for it, or null), fingerprint, toolchain (what it was resolved from, or null), answered_by, version (its pace-node), schema, grammar, total_files."},"b":{"type":"object"},)"
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
        R"("nodes":{"type":"array","description":"Per machine: endpoint; node_id and display_name, of the member whose identity key the machine's registrations PROVED, as the leader's node-id column joins it -- never by the endpoint, which a registration only claims (both null when no member holds that key or its registrations disagree -- absent, never guessed); the fingerprints it serves, and unserved -- the fleet's fingerprints in the same families it does not serve; null when it serves no toolchain of a known family (see unserved_note)."},)"
        R"("unserved_note":{"type":"string"},"demand":{"type":["object","null"],"description":"The leader's toolchain-demand over 7d, which the upgrade report weighs only once at least 1440 minutes (24 h) were observed and only where a lead exceeds the folded other compiles: range, observed_minutes of window_minutes, coverage_since, partial_coverage, total, toolchains (fingerprint, compiles) and other (toolchains_folded, compiles -- attributed to no toolchain). Null (absent, NOT zero) when the answering node keeps no such history or counted for no minute; demand_note then says why and the report is on versions alone."},"demand_note":{"type":"string"},)"
        R"("upgrade_report":{"type":"array","description":"Per banner-derived compiler family: recommended_version, and recommended_fingerprints -- an object from DRIVER (gcc, cc, g++: one install answers to several) to fingerprint, a driver absent when its builds of the version cannot be told apart, null when the ordering is unknown; basis (the versions compared and any demand), and machines placed by their highest version of the family: behind (lower: should upgrade), ahead (higher), same_version_other_fingerprint (the recommended version under another fingerprint for the SAME driver: ask toolchain_diff); lacks_recommended (every machine that does not serve the recommended fingerprint of a driver that has one, whatever its version: these should change); ordering_unknown."}}})";

} // namespace

std::vector<Mcp::ToolRow> ToolchainTools(IMcpUpstream& upstream,
                                         IAppliedClusterView const& cluster,
                                         core::platform::IClock const& clock,
                                         std::string selfId,
                                         std::string thisVersion)
{
    auto* const to = &upstream;
    auto const* const in = &cluster;
    auto const* const timer = &clock;
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
            .handler = [to, in](Mcp::ToolCall const& call) { return Toolchains(*to, *in, call); },
        },
        Mcp::ToolRow {
            .name = "toolchain_diff",
            .title = "Toolchain diff",
            .description =
                "Why two toolchain fingerprints differ, even when the name and version are the same. Each side is a "
                "node (a member's id or display name, matched exactly; this node when omitted) and a fingerprint, or "
                "a toolchain label or compiler path that names exactly one toolchain that node serves -- an "
                "ambiguous one is an error listing the candidates. Each node sends the inputs its survey digested -- "
                "the schema, the compiler's banner, the driver grammar and every include-tree file with its content "
                "digest -- and THIS node recomputes each fingerprint from them before comparing (recompute: passed), "
                "refusing a record that no longer reproduces. The result names what differs: schema, both banners, "
                "both grammars, files only on a, only on b, and on both with different content, each with a count. "
                "The verdict states only what the comparison shows; anything more is marked 'inferred:' and inferred "
                "is true. A side whose record does not reproduce its fingerprint is refused as stale; identical "
                "inputs reproducing neither fingerprint, or a side digested under another schema, are reported as a "
                "skew naming both versions and schemas. A side past 200000 files, its byte bound or its page bound "
                "is compared on schema, banner and grammar only (complete: false), and the verdict says the files "
                "were not compared. A call whose nodes answer too slowly to finish within 30 seconds is an error "
                "saying so, never a partial diff. `term` is the term THIS node holds.",
            .inputSchema = ToolchainDiffInput,
            .outputSchema = ToolchainDiffOutput,
            .effect = Mcp::ToolEffect::ReadOnly,
            .handler = [to, in, timer, self = std::move(selfId), version = std::move(thisVersion)](
                           Mcp::ToolCall const& call) { return ToolchainDiff(*to, *in, *timer, self, version, call); },
        },
    };
}

} // namespace Pace::Node
