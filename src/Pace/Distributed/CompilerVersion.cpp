// SPDX-License-Identifier: Apache-2.0
#include <Pace/CompileCache/VersionName.hpp>
#include <Pace/Core/SaturatingAdd.hpp>
#include <Pace/Distributed/CompilerVersion.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <system_error>
#include <utility>

namespace Pace::Distributed
{

namespace
{
    /// @param items Some texts.
    /// @return Them joined by ", ", in their order: a basis is read by a person, so no brackets or quotes.
    template <typename Range>
    [[nodiscard]] std::string Joined(Range const& items)
    {
        auto text = std::string {};
        for (auto const& item: items)
            text += std::format("{}{}", text.empty() ? "" : ", ", item);
        return text;
    }

    /// @param text A version: digits and dots.
    /// @return Its parts, or nothing when a part is empty, is not all digits, has a leading zero, or does not fit.
    [[nodiscard]] std::optional<std::vector<std::uint32_t>> VersionParts(std::string_view text)
    {
        // Splitting nothing yields no pieces, not one empty piece, so an empty version is refused here.
        if (text.empty())
            return std::nullopt;
        auto parts = std::vector<std::uint32_t> {};
        for (auto const piece: std::views::split(text, '.'))
        {
            auto const part = std::string_view { piece.begin(), piece.end() };
            if (part.empty() || !std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; }))
                return std::nullopt;
            // `13.02` is `13.2` to one reader and something else to another; a version this report cannot read one
            // way only is not read at all.
            if (part.size() > 1 && part.front() == '0')
                return std::nullopt;
            auto value = std::uint32_t {};
            auto const [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
            if (error != std::errc {} || end != part.data() + part.size())
                return std::nullopt;
            parts.push_back(value);
        }
        return parts;
    }

    /// One fingerprint of a family, as its labels state it.
    struct FamilyMember
    {
        std::set<std::string> labels;          ///< Every distinct label its entries carry in this family.
        std::set<std::string> endpoints;       ///< The machines serving it.
        std::optional<CompilerVersion> parsed; ///< Its version, when every label parsed to one version.
        bool unordered {};                     ///< A label did not parse, or two labels disagree.
        bool unlabelled {};                    ///< An entry stated no label at all, so its version is unknown.
        /// The drivers its labels name, version suffix dropped (`CompileCache::DriverWithoutVersion`): `gcc` and
        /// `cc` are one install under two names, and a fingerprint is chosen per driver, never across two.
        std::set<std::string> drivers;
    };

    /// One family, keyed by fingerprint.
    using Family = std::map<std::string, FamilyMember>;

    /// @param members A family's fingerprints.
    /// @return Why the family cannot be ordered, or nothing when it can.
    [[nodiscard]] std::optional<std::string> UnorderedBecause(Family const& members)
    {
        auto unlabelled = std::vector<std::string> {};
        for (auto const& [fingerprint, member]: members)
            if (member.unlabelled)
                unlabelled.push_back(fingerprint);
        if (!unlabelled.empty())
            return std::format("ordering unknown: {} carry no label, so their version is unknown and no version is "
                               "compared",
                               Joined(unlabelled));

        auto unparsed = std::vector<std::string> {};
        for (auto const& member: members | std::views::values)
            if (member.unordered)
                unparsed.insert(unparsed.end(), member.labels.begin(), member.labels.end());
        if (!unparsed.empty())
        {
            std::ranges::sort(unparsed);
            auto const [first, last] = std::ranges::unique(unparsed);
            unparsed.erase(first, last);
            return std::format("ordering unknown: the label(s) {} do not state one `<tool> <version>` per "
                               "toolchain, so no version is compared",
                               Joined(unparsed));
        }

        auto lengths = std::set<std::size_t> {};
        for (auto const& member: members | std::views::values)
            if (member.parsed.has_value())
                lengths.insert(member.parsed->parts.size());
        if (lengths.size() > 1)
        {
            auto versions = std::set<std::string> {};
            for (auto const& member: members | std::views::values)
                if (member.parsed.has_value())
                    versions.insert(VersionText(member.parsed->parts));
            return std::format(
                "ordering unknown: versions {} have different numbers of parts, which do not order unambiguously",
                Joined(versions));
        }
        return std::nullopt;
    }

    /// Who lacks what is recommended, whichever way their version lies: a machine that does not serve the recommended
    /// fingerprint of some driver that has one. The three placement lists describe; this one instructs.
    /// @param servedBy Each machine's fingerprints.
    /// @param recommended The fingerprint recommended per driver.
    /// @return The machines, sorted.
    [[nodiscard]] std::vector<std::string> LackingRecommended(
        std::map<std::string, std::vector<std::string>> const& servedBy,
        std::map<std::string, std::string> const& recommended)
    {
        auto lacking = std::vector<std::string> {};
        for (auto const& [endpoint, fingerprints]: servedBy)
            if (std::ranges::any_of(recommended,
                                    [&](auto const& choice) { return !std::ranges::contains(fingerprints, choice.second); }))
                lacking.push_back(endpoint);
        return lacking;
    }

    /// Per version: its fingerprints, and the demand they carry together.
    using VersionDemand = std::map<std::vector<std::uint32_t>, std::pair<std::vector<std::string>, std::uint64_t>>;

    /// What a family's basis is made of.
    struct BasisFacts
    {
        std::uint64_t familyDemand {}; ///< Named compiles across the family's versions.
        bool demandDecides {};         ///< Whether demand chose the target.
        bool undecidedByFolded {};     ///< Whether unattributable compiles alone kept demand from deciding.
        bool singleVersion {};         ///< Whether the family serves one version only.
        bool targetFloor {};           ///< Whether the target's count is a lower bound.
        std::uint64_t targetCount {};  ///< The target version's named compiles.
        std::string targetText;        ///< The target version.
        std::string compared;          ///< The versions compared, with their counts.
    };

    /// @param count A number of compiles.
    /// @return "1 compile" or "N compiles".
    [[nodiscard]] std::string CompilesWords(std::uint64_t count)
    {
        return std::format("{} compile{}", count, count == 1 ? "" : "s");
    }

    /// One family's basis. **Each case opens with its own words**, and none opens with "no demand data" once data was
    /// read: not recorded, read but not weighed (with why), a covered window with no compiles for the family, demand
    /// that decides, and demand that does not because unattributable compiles could change the order.
    /// @param facts What the family's choice was made of.
    /// @param context What is known about the demand.
    /// @return The basis.
    [[nodiscard]] std::string BasisOf(BasisFacts const& facts, DemandContext const& context)
    {
        auto const tail =
            std::format("highest labelled version served: {}; versions compared: {}", facts.targetText, facts.compared);
        if (!context.notWeighedBecause.empty())
            return std::format("demand was read but does not decide ({}); {}", context.notWeighedBecause, tail);
        if (facts.familyDemand == 0)
            return context.weighed ? std::format("no compiles were recorded for this family in the window; {}", tail)
                                   : std::format("no demand data; {}", tail);
        if (facts.demandDecides)
            return std::format("highest version the most demand uses: {} ({}{} of {} named); versions compared: {}{}",
                               facts.targetText,
                               facts.targetFloor ? "at least " : "",
                               CompilesWords(facts.targetCount),
                               CompilesWords(facts.familyDemand),
                               facts.compared,
                               context.unattributable > 0
                                   ? std::format("; {} folded or unattributed (named for no toolchain) cannot "
                                                 "reverse that lead",
                                                 CompilesWords(context.unattributable))
                                   : std::string {});
        if (facts.undecidedByFolded)
            return std::format("demand does not decide this family: unattributable demand (folded or unnamed) of {} "
                               "could change the order; {}",
                               CompilesWords(context.unattributable),
                               tail);
        return std::format("demand does not decide this family: {}; {}",
                           facts.singleVersion ? "one version is served, so there is nothing to choose"
                                               : "no version leads the others",
                           tail);
    }

    /// A count is exact only when nothing was folded away and none of its keys ever was; otherwise it is a floor.
    /// @param fingerprints A version's fingerprints.
    /// @param atLeastOf Fingerprints whose own count is a lower bound.
    /// @param folded Compiles no fingerprint is named for.
    /// @return Whether the version's count must be read as "at least".
    [[nodiscard]] bool IsFloor(std::vector<std::string> const& fingerprints,
                               std::set<std::string> const& atLeastOf,
                               std::uint64_t folded)
    {
        return folded > 0 || std::ranges::any_of(fingerprints, [&](std::string const& fingerprint) {
                   return atLeastOf.contains(fingerprint);
               });
    }

    /// @param byVersion A family's versions with their demand.
    /// @param familyDemand The family's named demand.
    /// @param inexact Whether a version's count is a floor.
    /// @return One entry per version: its text, with its count when the family has any demand.
    template <typename Floor>
    [[nodiscard]] std::vector<std::string> ComparedWords(VersionDemand const& byVersion,
                                                         std::uint64_t familyDemand,
                                                         Floor const& inexact)
    {
        auto compared = std::vector<std::string> {};
        for (auto const& [parts, row]: byVersion)
            compared.push_back(familyDemand == 0 ? VersionText(parts)
                                                 : std::format("{} ({}{})",
                                                               VersionText(parts),
                                                               inexact(row.first) ? "at least " : "",
                                                               CompilesWords(row.second)));
        return compared;
    }

    /// Which version a family converges on, and whether demand is what chose it.
    struct TargetChoice
    {
        VersionDemand::value_type const* target {}; ///< The version chosen.
        bool demandDecides {};                      ///< Whether demand, rather than the versions rule, chose it.
        bool undecidedByFolded {};                  ///< Whether folded compiles alone kept demand from deciding.
    };

    /// Choose the target: the highest version the most demand uses -- but only where that demand LEADS by more than the
    /// compiles no row can attribute, since those could belong to any runner-up. Otherwise the highest served version.
    /// @param byVersion The family's versions, ascending, with their demand.
    /// @param folded Compiles attributed to no fingerprint.
    /// @return The choice.
    [[nodiscard]] TargetChoice ChooseTarget(VersionDemand const& byVersion, std::uint64_t folded)
    {
        // The highest version among those carrying the most demand: `byVersion` is ascending, so the last maximum
        // found walking it is the highest. With no demand every version ties at zero, which is the highest served.
        auto const* demandTarget = &*byVersion.begin();
        for (auto const& entry: byVersion)
            if (entry.second.second >= demandTarget->second.second)
                demandTarget = &entry;
        if (byVersion.size() == 1)
            return TargetChoice { .target = demandTarget, .demandDecides = false, .undecidedByFolded = false };
        auto runnerUp = std::uint64_t { 0 };
        for (auto const& entry: byVersion)
            if (&entry != demandTarget)
                runnerUp = std::max(runnerUp, entry.second.second);
        auto const leads = demandTarget->second.second > runnerUp;
        auto const decides = leads && !IsSaturated(folded) && !IsSaturated(runnerUp)
                             && !IsSaturated(demandTarget->second.second)
                             && demandTarget->second.second > SaturatingAdd(runnerUp, folded);
        return TargetChoice { .target = decides ? demandTarget : &*byVersion.rbegin(),
                              .demandDecides = decides,
                              .undecidedByFolded = leads && !decides && folded > 0 };
    }

    /// @param family The family's name.
    /// @param members A family's fingerprints, every one parsed to a version of the same length.
    /// @param demandOf Compiles per fingerprint.
    /// @param atLeastOf Fingerprints whose count is a lower bound.
    /// @param context What else is known about the demand: its unattributable compiles and whether it was weighed.
    /// @return The advice for a family whose versions order.
    [[nodiscard]] UpgradeAdvice AdviseOrdered(std::string family,
                                              Family const& members,
                                              std::map<std::string, std::uint64_t> const& demandOf,
                                              std::set<std::string> const& atLeastOf,
                                              DemandContext const& context)
    {
        auto const folded = context.unattributable;
        auto const demandFor = [&](std::string const& fingerprint) {
            auto const found = demandOf.find(fingerprint);
            return found != demandOf.end() ? found->second : std::uint64_t { 0 };
        };

        auto byVersion = VersionDemand {};
        for (auto const& [fingerprint, member]: members)
        {
            if (!member.parsed.has_value())
                continue;
            auto& [fingerprints, compiles] = byVersion[member.parsed->parts];
            fingerprints.push_back(fingerprint);
            compiles = SaturatingAdd(compiles, demandFor(fingerprint));
        }
        if (byVersion.empty())
            return UpgradeAdvice { .family = std::move(family),
                                   .recommendedVersion = std::nullopt,
                                   .recommendedFingerprints = {},
                                   .basis = "ordering unknown: no version parsed",
                                   .nodesBehind = {},
                                   .nodesAhead = {},
                                   .nodesSameVersionOtherFingerprint = {},
                                   .nodesLackingRecommended = {},
                                   .orderingUnknown = true };

        auto familyDemand = std::uint64_t { 0 };
        for (auto const& row: byVersion | std::views::values)
            familyDemand = SaturatingAdd(familyDemand, row.second);

        auto const inexact = [&](std::vector<std::string> const& fingerprints) {
            return IsFloor(fingerprints, atLeastOf, folded);
        };
        auto const compared = ComparedWords(byVersion, familyDemand, inexact);

        auto const choice = ChooseTarget(byVersion, folded);
        auto const* const target = choice.target;
        auto const demandDecides = choice.demandDecides && familyDemand > 0;
        auto const undecidedBecauseFolded = choice.undecidedByFolded && familyDemand > 0;
        auto const targetText = VersionText(target->first);

        auto advice = UpgradeAdvice { .family = std::move(family),
                                      .recommendedVersion = targetText,
                                      .recommendedFingerprints = {},
                                      .basis = {},
                                      .nodesBehind = {},
                                      .nodesAhead = {},
                                      .nodesSameVersionOtherFingerprint = {},
                                      .nodesLackingRecommended = {},
                                      .orderingUnknown = false };
        // "Labelled": a toolchain whose node stated no label has no version to compare and leaves the whole family
        // unordered before this point, so every version named here is one a label stated.
        advice.basis = BasisOf(BasisFacts { .familyDemand = familyDemand,
                                            .demandDecides = demandDecides,
                                            .undecidedByFolded = undecidedBecauseFolded,
                                            .singleVersion = byVersion.size() == 1,
                                            .targetFloor = inexact(target->second.first),
                                            .targetCount = target->second.second,
                                            .targetText = targetText,
                                            .compared = Joined(compared) },
                               context);

        // The FINGERPRINT is chosen per DRIVER, never across two: `gcc` and `cc` at one version are one install under
        // two names, two fingerprints because each banner names its own driver, and neither is "the other build" of
        // the other. Two fingerprints under ONE driver at the target are; demand settles that only when one carries
        // strictly more than every other, and otherwise the basis names them and points at toolchain_diff.
        auto const& candidates = target->second.first;
        auto drivers = std::set<std::string> {};
        for (auto const& fingerprint: candidates)
        {
            auto const& named = members.at(fingerprint).drivers;
            drivers.insert(named.begin(), named.end());
        }
        for (auto const& driver: drivers)
        {
            auto underDriver = std::vector<std::string> {};
            std::ranges::copy_if(candidates, std::back_inserter(underDriver), [&](std::string const& fingerprint) {
                return members.at(fingerprint).drivers.contains(driver);
            });
            if (underDriver.size() == 1)
            {
                advice.recommendedFingerprints.emplace(driver, underDriver.front());
                continue;
            }
            auto ranked = underDriver;
            std::ranges::sort(ranked, std::ranges::greater {}, demandFor);
            auto shares = std::vector<std::string> {};
            for (auto const& fingerprint: underDriver)
                shares.push_back(std::format("{} ({}{})",
                                             fingerprint,
                                             folded > 0 || atLeastOf.contains(fingerprint) ? "at least " : "",
                                             CompilesWords(demandFor(fingerprint))));
            if (!IsSaturated(folded) && demandFor(ranked[0]) > SaturatingAdd(demandFor(ranked[1]), folded))
            {
                advice.recommendedFingerprints.emplace(driver, ranked[0]);
                advice.basis += std::format("; {} under `{}` is served by {} fingerprints and demand favours {}: {}",
                                            targetText,
                                            driver,
                                            underDriver.size(),
                                            ranked[0],
                                            Joined(shares));
            }
            else
                advice.basis += std::format("; {} under `{}` is served by {} fingerprints ({}) with nothing in demand "
                                            "to tell them apart, so none is recommended for it -- ask toolchain_diff "
                                            "why they differ",
                                            targetText,
                                            driver,
                                            underDriver.size(),
                                            Joined(underDriver));
        }

        // Each machine is placed by the highest version of the family it serves, against the target: below is behind,
        // above is ahead -- never "behind" for a machine that already runs something newer. At the target, a machine
        // is the same version built differently when, for a driver it serves there, a fingerprint is recommended and
        // the machine serves that driver only under another one.
        auto servedBy = std::map<std::string, std::vector<std::string>> {};
        for (auto const& [fingerprint, member]: members)
            for (auto const& endpoint: member.endpoints)
                servedBy[endpoint].push_back(fingerprint);
        for (auto const& [endpoint, fingerprints]: servedBy)
        {
            auto highest = std::vector<std::uint32_t> {};
            for (auto const& fingerprint: fingerprints)
                if (auto const& parsed = members.at(fingerprint).parsed; parsed.has_value() && parsed->parts > highest)
                    highest = parsed->parts;
            if (highest < target->first)
            {
                advice.nodesBehind.push_back(endpoint);
                continue;
            }
            if (highest > target->first)
            {
                advice.nodesAhead.push_back(endpoint);
                continue;
            }
            auto const otherBuild = std::ranges::any_of(fingerprints, [&](std::string const& fingerprint) {
                auto const& member = members.at(fingerprint);
                if (!member.parsed.has_value() || member.parsed->parts != target->first)
                    return false;
                return std::ranges::any_of(member.drivers, [&](std::string const& driver) {
                    auto const chosen = advice.recommendedFingerprints.find(driver);
                    return chosen != advice.recommendedFingerprints.end() && chosen->second != fingerprint
                           && !std::ranges::contains(fingerprints, chosen->second);
                });
            });
            if (otherBuild)
                advice.nodesSameVersionOtherFingerprint.push_back(endpoint);
        }

        advice.nodesLackingRecommended = LackingRecommended(servedBy, advice.recommendedFingerprints);
        return advice;
    }
} // namespace

std::optional<CompilerVersion> ParseToolchainLabel(std::string_view label)
{
    auto const space = label.find(' ');
    if (space == std::string_view::npos || space == 0)
        return std::nullopt;
    auto parts = VersionParts(label.substr(space + 1));
    if (!parts.has_value())
        return std::nullopt;
    return CompilerVersion { .tool = std::string { label.substr(0, space) }, .parts = std::move(*parts) };
}

std::string VersionText(std::span<std::uint32_t const> parts)
{
    auto text = std::string {};
    for (auto const part: parts)
        text += std::format("{}{}", text.empty() ? "" : ".", part);
    return text;
}

std::vector<UpgradeAdvice> AdviseUpgrades(std::span<WorkerReport const> workers,
                                          std::span<DemandRow const> demand,
                                          DemandContext const& context)
{
    auto families = std::map<std::string, Family> {};
    for (auto const& worker: workers)
    {
        // The family the node read off the banner, never the label's tool name; none said, no family joined.
        auto const& family = worker.info.toolchainFamily;
        if (family.empty())
            continue;
        auto const& label = worker.info.toolchainLabel;
        auto& member = families[family][worker.info.fingerprint];
        member.endpoints.insert(worker.info.endpoint);
        if (label.empty())
        {
            member.unlabelled = true;
            continue;
        }
        member.labels.insert(label);

        // A label that does not parse, or a second label stating another version for the same fingerprint, leaves
        // the fingerprint -- and so its family -- unordered.
        auto parsed = ParseToolchainLabel(label);
        if (parsed.has_value())
            member.drivers.insert(std::string { CompileCache::DriverWithoutVersion(parsed->tool) });
        if (!parsed.has_value() || (member.parsed.has_value() && member.parsed->parts != parsed->parts))
            member.unordered = true;
        else if (!member.parsed.has_value())
            member.parsed = std::move(parsed);
    }

    auto demandOf = std::map<std::string, std::uint64_t> {};
    auto atLeastOf = std::set<std::string> {};
    // Read but not weighed: the rows are ignored whoever handed them, so nothing can decide on them by accident.
    for (auto const& row: context.notWeighedBecause.empty() ? demand : std::span<DemandRow const> {})
    {
        demandOf[row.fingerprint] = SaturatingAdd(demandOf[row.fingerprint], row.compiles);
        if (row.atLeast)
            atLeastOf.insert(row.fingerprint);
    }

    auto advice = std::vector<UpgradeAdvice> {};
    advice.reserve(families.size());
    for (auto const& [family, members]: families)
    {
        if (auto because = UnorderedBecause(members); because.has_value())
        {
            advice.push_back(UpgradeAdvice { .family = family,
                                             .recommendedVersion = std::nullopt,
                                             .recommendedFingerprints = {},
                                             .basis = std::move(*because),
                                             .nodesBehind = {},
                                             .nodesAhead = {},
                                             .nodesSameVersionOtherFingerprint = {},
                                             .nodesLackingRecommended = {},
                                             .orderingUnknown = true });
            continue;
        }
        advice.push_back(AdviseOrdered(family, members, demandOf, atLeastOf, context));
    }
    return advice;
}

} // namespace Pace::Distributed
