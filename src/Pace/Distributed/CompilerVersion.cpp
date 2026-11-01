// SPDX-License-Identifier: Apache-2.0
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

    /// @param family The family's name.
    /// @param members A family's fingerprints, every one parsed to a version of the same length.
    /// @param demandOf Compiles per fingerprint.
    /// @param anyDemandData Whether any demand was handed in at all, for any family.
    /// @return The advice for a family whose versions order.
    [[nodiscard]] UpgradeAdvice AdviseOrdered(std::string family,
                                              Family const& members,
                                              std::map<std::string, std::uint64_t> const& demandOf,
                                              bool anyDemandData)
    {
        auto const demandFor = [&](std::string const& fingerprint) {
            auto const found = demandOf.find(fingerprint);
            return found != demandOf.end() ? found->second : std::uint64_t { 0 };
        };

        // Per version: its fingerprints, and the demand they carry together.
        auto byVersion = std::map<std::vector<std::uint32_t>, std::pair<std::vector<std::string>, std::uint64_t>> {};
        for (auto const& [fingerprint, member]: members)
        {
            if (!member.parsed.has_value())
                continue;
            auto& [fingerprints, compiles] = byVersion[member.parsed->parts];
            fingerprints.push_back(fingerprint);
            compiles += demandFor(fingerprint);
        }
        if (byVersion.empty())
            return UpgradeAdvice { .family = std::move(family),
                                   .recommendedVersion = std::nullopt,
                                   .recommendedFingerprint = std::nullopt,
                                   .basis = "ordering unknown: no version parsed",
                                   .nodesBehind = {},
                                   .nodesAhead = {},
                                   .nodesSameVersionOtherFingerprint = {},
                                   .orderingUnknown = true };

        auto familyDemand = std::uint64_t { 0 };
        for (auto const& row: byVersion | std::views::values)
            familyDemand += row.second;

        auto compared = std::vector<std::string> {};
        for (auto const& [parts, row]: byVersion)
            compared.push_back(familyDemand == 0 ? VersionText(parts)
                                                 : std::format("{} ({} compiles)", VersionText(parts), row.second));

        // The highest version among those carrying the most demand: `byVersion` is ascending, so the last maximum
        // found walking it is the highest. With no demand every version ties at zero, which is the highest served.
        auto const* target = &*byVersion.begin();
        for (auto const& entry: byVersion)
            if (entry.second.second >= target->second.second)
                target = &entry;
        auto const targetText = VersionText(target->first);

        auto advice = UpgradeAdvice { .family = std::move(family),
                                      .recommendedVersion = targetText,
                                      .recommendedFingerprint = std::nullopt,
                                      .basis = {},
                                      .nodesBehind = {},
                                      .nodesAhead = {},
                                      .nodesSameVersionOtherFingerprint = {},
                                      .orderingUnknown = false };
        // "Labelled": a toolchain whose node stated no label has no version to compare and leaves the whole family
        // unordered before this point, so every version named here is one a label stated.
        if (familyDemand == 0)
            advice.basis = std::format("{}; highest labelled version served: {}; versions compared: {}",
                                       anyDemandData ? "no demand data for this family" : "no demand data",
                                       targetText,
                                       Joined(compared));
        else
            advice.basis = std::format("highest version the most demand uses: {} ({} of {} compiles); versions "
                                       "compared: {}",
                                       targetText,
                                       target->second.second,
                                       familyDemand,
                                       Joined(compared));

        auto const& candidates = target->second.first;
        if (candidates.size() == 1)
            advice.recommendedFingerprint = candidates.front();
        else
        {
            // One version under several fingerprints: the same label, different toolchains. Demand settles it only
            // when one of them carries strictly more than every other.
            auto ranked = candidates;
            std::ranges::sort(ranked, std::ranges::greater {}, demandFor);
            auto const clear = demandFor(ranked[0]) > demandFor(ranked[1]);
            auto shares = std::vector<std::string> {};
            for (auto const& fingerprint: candidates)
                shares.push_back(std::format("{} ({} compiles)", fingerprint, demandFor(fingerprint)));
            if (clear)
            {
                advice.recommendedFingerprint = ranked[0];
                advice.basis += std::format("; {} is served under {} fingerprints and demand favours {}: {}",
                                            targetText,
                                            candidates.size(),
                                            ranked[0],
                                            Joined(shares));
            }
            else
                advice.basis += std::format("; {} is served under {} fingerprints ({}) with nothing in demand to tell "
                                            "them apart, so none is recommended -- ask toolchain_diff why they differ",
                                            targetText,
                                            candidates.size(),
                                            Joined(candidates));
        }

        // Each machine is placed by the highest version of the family it serves, against the target: below is behind,
        // above is ahead -- never "behind" for a machine that already runs something newer -- and at the target under
        // another fingerprint only is the same version built differently.
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
                advice.nodesBehind.push_back(endpoint);
            else if (highest > target->first)
                advice.nodesAhead.push_back(endpoint);
            else if (advice.recommendedFingerprint.has_value()
                     && !std::ranges::contains(fingerprints, *advice.recommendedFingerprint))
                advice.nodesSameVersionOtherFingerprint.push_back(endpoint);
        }
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

std::vector<UpgradeAdvice> AdviseUpgrades(std::span<WorkerReport const> workers, std::span<DemandRow const> demand)
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
        if (!parsed.has_value() || (member.parsed.has_value() && member.parsed->parts != parsed->parts))
            member.unordered = true;
        else if (!member.parsed.has_value())
            member.parsed = std::move(parsed);
    }

    auto demandOf = std::map<std::string, std::uint64_t> {};
    for (auto const& row: demand)
        demandOf[row.fingerprint] += row.compiles;

    auto advice = std::vector<UpgradeAdvice> {};
    advice.reserve(families.size());
    for (auto const& [family, members]: families)
    {
        if (auto because = UnorderedBecause(members); because.has_value())
        {
            advice.push_back(UpgradeAdvice { .family = family,
                                             .recommendedVersion = std::nullopt,
                                             .recommendedFingerprint = std::nullopt,
                                             .basis = std::move(*because),
                                             .nodesBehind = {},
                                             .nodesAhead = {},
                                             .nodesSameVersionOtherFingerprint = {},
                                             .orderingUnknown = true });
            continue;
        }
        advice.push_back(AdviseOrdered(family, members, demandOf, !demand.empty()));
    }
    return advice;
}

} // namespace Pace::Distributed
