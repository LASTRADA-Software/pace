// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/EnumTable.hpp>
#include <Pace/Distributed/MachineStanding.hpp>
#include <Pace/Distributed/SchedulerService.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Pace::Distributed
{

/// How the admission vocabulary travels, in ONE place (#1471).
///
/// `CompileCacheWire.hpp` must stay dependency-free, so it MIRRORS `Membership` and
/// `MembershipParticipant` rather than carrying them; the two halves are joined by the tables
/// below. They live in the library rather than in the node app because a mirror needs a reader at
/// each end -- the node ENCODES an answer and `pacectl` RENDERS one -- and two copies of a
/// mapping are two things that can disagree about which bit means which route, in a verb whose
/// whole subject is attributing a decision correctly.

/// One row of `MembershipWireVerdicts`.
struct MembershipVerdictRow
{
    Membership verdict;                   ///< What the fold calls it.
    CompileCacheWire::WireMembership tag; ///< What the wire calls it.

    /// What a person reading an answer calls it -- `pacectl explain-admission` and the MCP `explain_admission` tool
    /// alike. The word an operator would use, not the enumerator: `refused` rather than `Outsider`, which reads as a
    /// status a client invented.
    std::string_view name;
};

/// Which wire tag each admission verdict travels as.
///
/// A table rather than a cast: `Membership`'s order is load-bearing to `PrecedenceOf`, so
/// transmitting the ordinal directly would make a precedence change a wire change.
constexpr EnumTable<Membership, MembershipVerdictRow> MembershipWireVerdicts { {
    { .verdict = Membership::Outsider, .tag = CompileCacheWire::WireMembership::Outsider, .name = "refused" },
    { .verdict = Membership::Member, .tag = CompileCacheWire::WireMembership::Member, .name = "admitted" },
    { .verdict = Membership::Forgotten, .tag = CompileCacheWire::WireMembership::Forgotten, .name = "forgotten" },
} };

static_assert(RowsInEnumeratorOrder(MembershipWireVerdicts, &MembershipVerdictRow::verdict),
              "MembershipWireVerdicts must hold one row per Membership, in enumerator order");

/// One row of `MembershipWireRoutes`.
struct MembershipRouteRow
{
    MembershipParticipant route; ///< What the fold calls it.
    std::uint32_t bit;           ///< The bit it travels as, a `CompileCacheWire::WireMembershipRoute` value; 0 for none.

    /// What a person reading an answer calls it: **the setting or the act, never the enumerator.** An operator told
    /// `fleet-open` knows which setting to change; one told `OpenPolicy` has to go and look it up.
    ///
    /// A column of THIS table, keyed on the participant rather than the bit, and that is the guard rather than a
    /// preference: a bitmask has no `Last`, so a table keyed on one cannot be checked for completeness, while here
    /// `RowsInEnumeratorOrder` fails the build when a route is added and not spelled -- where a bit-keyed list would
    /// compile and render the new route as one this client cannot name, blaming the node's version for this
    /// client's omission.
    std::string_view name;
};

/// Which bit each admission route travels as.
///
/// A SET on the wire, so these are powers of two rather than ordinals: an operator asking why a
/// caller is served needs to know when BOTH a proof and a ticket admit it, and a single value could
/// not say so. `Reserved` travels as NO bit, and two things keep it off the wire: its row here
/// carries zero, so OR-ing it in changes nothing, and `MembershipParticipantSet::Add` never records
/// it, so no decision names it in the first place.
constexpr EnumTable<MembershipParticipant, MembershipRouteRow> MembershipWireRoutes { {
    { .route = MembershipParticipant::Reserved, .bit = 0, .name = "reserved" },
    { .route = MembershipParticipant::Loopback, .bit = CompileCacheWire::WireMembershipRoute::Loopback, .name = "loopback" },
    { .route = MembershipParticipant::OpenPolicy,
      .bit = CompileCacheWire::WireMembershipRoute::OpenPolicy,
      .name = "fleet-open" },
    { .route = MembershipParticipant::ProvenIdentity,
      .bit = CompileCacheWire::WireMembershipRoute::ProvenIdentity,
      .name = "proven-key" },
    { .route = MembershipParticipant::MachineTicket,
      .bit = CompileCacheWire::WireMembershipRoute::MachineTicket,
      .name = "ticket" },
    { .route = MembershipParticipant::KeyTombstone,
      .bit = CompileCacheWire::WireMembershipRoute::KeyTombstone,
      .name = "key-revoked" },
} };

static_assert(RowsInEnumeratorOrder(MembershipWireRoutes, &MembershipRouteRow::route),
              "MembershipWireRoutes must hold one row per MembershipParticipant, in enumerator order: a route with "
              "no bit would travel as silence, and this verb exists to stop a silence being read as an answer");

/// @return Whether no row of the table that TRANSMITS carries a retired route bit.
///
/// Asked here rather than only of the wire header's named constants, because this table is what
/// puts bits on the wire: a row spelled with a literal, or a new participant given a retired bit,
/// passes a check over the names and is caught only here.
[[nodiscard]] constexpr bool NoTransmittedRouteIsRetired() noexcept
{
    return std::ranges::none_of(
        MembershipWireRoutes, [](MembershipRouteRow const& row) { return CompileCacheWire::CarriesRetiredRoute(row.bit); });
}

static_assert(NoTransmittedRouteIsRetired(),
              "a retired route bit is reserved: a deployed client reads it by its old name, so no route may travel as one");

/// One row of `MachineStandingWire`.
struct MachineStandingRow
{
    MachineStanding standing;                  ///< What the roster says.
    CompileCacheWire::WireMachineStanding tag; ///< What the wire calls it.
    std::string_view name;                     ///< What a person reading an answer calls it: the seat, or why none.
};

/// Which wire tag each standing travels as: a table rather than a cast, so the private enum's order
/// is not a wire contract.
constexpr EnumTable<MachineStanding, MachineStandingRow> MachineStandingWire { {
    { .standing = MachineStanding::Voter, .tag = CompileCacheWire::WireMachineStanding::Voter, .name = "voter" },
    { .standing = MachineStanding::Learner, .tag = CompileCacheWire::WireMachineStanding::Learner, .name = "learner" },
    { .standing = MachineStanding::Pending, .tag = CompileCacheWire::WireMachineStanding::Pending, .name = "pending" },
    { .standing = MachineStanding::Revoked, .tag = CompileCacheWire::WireMachineStanding::Revoked, .name = "revoked" },
    { .standing = MachineStanding::Unknown, .tag = CompileCacheWire::WireMachineStanding::Unknown, .name = "unknown" },
} };

static_assert(RowsInEnumeratorOrder(MachineStandingWire, &MachineStandingRow::standing),
              "MachineStandingWire must hold one row per MachineStanding, in enumerator order");

/// @param standing Where a machine stands.
/// @return Its wire tag.
[[nodiscard]] constexpr CompileCacheWire::WireMachineStanding OnTheWire(MachineStanding standing) noexcept
{
    return MachineStandingWire[static_cast<std::size_t>(standing)].tag;
}

/// @param tag A standing as the wire carries it.
/// @return The standing, or nullopt for a tag this table does not hold.
[[nodiscard]] constexpr std::optional<MachineStanding> StandingOnTheWire(CompileCacheWire::WireMachineStanding tag) noexcept
{
    for (auto const& row: MachineStandingWire)
        if (row.tag == tag)
            return row.standing;
    return std::nullopt;
}

/// @p decision as the wire carries it.
/// @param decision What the oracle concluded.
/// @return The same answer, in the wire's vocabulary.
[[nodiscard]] constexpr CompileCacheWire::AdmissionExplanationFields OnTheWire(MembershipDecision decision) noexcept
{
    auto bits = std::uint32_t { 0 };
    for (auto const& row: MembershipWireRoutes)
        if (decision.decidedBy.Has(row.route))
            bits |= row.bit;

    return { .verdict = MembershipWireVerdicts[static_cast<std::size_t>(decision.verdict)].tag, .decidedBy = bits };
}

/// What to call one admission verdict, as a person reads an answer.
/// @param verdict The wire tag.
/// @return Its `MembershipWireVerdicts` row's name; `unknown` for a tag no row holds, which
///         `DecodeAdmissionExplanation` makes unreachable for a decoded reply -- it REFUSES a verdict this build
///         cannot name, because a verdict is the answer itself.
[[nodiscard]] constexpr std::string_view VerdictName(CompileCacheWire::WireMembership verdict) noexcept
{
    for (auto const& row: MembershipWireVerdicts)
        if (row.tag == verdict)
            return row.name;
    return "unknown";
}

/// @param standing Where a machine stands.
/// @return What a person calls it.
[[nodiscard]] constexpr std::string_view StandingName(MachineStanding standing) noexcept
{
    return MachineStandingWire[static_cast<std::size_t>(standing)].name;
}

/// Every route in @p decidedBy, as a person reads them: the ONE derivation `pacectl explain-admission` and the MCP
/// `explain_admission` tool render from.
///
/// **A route this build cannot name is reported by its NUMBER, never dropped.** The node may be newer, and
/// under-reporting who decided during an upgrade would say fewer things decided than did; the number lets an
/// operator ask a client of the node's own version which route it is, which a count alone would not.
/// @param decidedBy The bitmask the node sent.
/// @return The named routes in table order, then each unnamed bit as `0x...`; empty when nothing decided.
[[nodiscard]] inline std::vector<std::string> AdmissionRouteNames(std::uint32_t decidedBy)
{
    auto names = std::vector<std::string> {};
    auto named = std::uint32_t { 0 };
    for (auto const& row: MembershipWireRoutes)
    {
        if ((decidedBy & row.bit) == 0)
            continue;
        named |= row.bit;
        names.emplace_back(row.name);
    }
    for (auto const bit: std::views::iota(0U, 32U))
    {
        auto const value = std::uint32_t { 1 } << bit;
        if ((decidedBy & ~named & value) != 0)
            names.push_back(std::format("{:#x}", value));
    }
    return names;
}

} // namespace Pace::Distributed
