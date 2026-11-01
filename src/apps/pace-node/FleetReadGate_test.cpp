// SPDX-License-Identifier: Apache-2.0
#include "FleetReadGate.hpp"

#include <Pace/Protocol/LiveStream.hpp>
#include <Pace/Server/AdminCredential.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace Pace;
using namespace Pace::Node;

namespace
{

/// Where the leader a follower names answers.
constexpr std::string_view LeaderEndpoint = "10.0.0.2:6674";

/// @return A node that leads.
[[nodiscard]] LiveLeadership Leading()
{
    return LiveLeadership { .leads = true, .leaderEndpoint = "10.0.0.1:6674" };
}

/// @return A node that follows the leader at `LeaderEndpoint`.
[[nodiscard]] LiveLeadership Following()
{
    return LiveLeadership { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint } };
}

} // namespace

TEST_CASE("A fleet read at a node with no scheduler is sent elsewhere before anything is checked", "[node][fleettext]")
{
    // Nothing to guard: a wrong credential from a stranger's host is still told the fleet is
    // served elsewhere, because there is no fleet here to be refused.
    auto const verdict =
        DecideFleetRead(std::nullopt, AdminCredential { "s3cret" }, "guess", "192.0.2.1", FleetReader::ByAddress);
    CHECK(verdict.decision == FleetReadDecision::NoScheduler);
    CHECK(verdict.detail.contains("no scheduler"));
}

TEST_CASE("With no token file the fleet is read from this machine only", "[node][fleettext]")
{
    AdminCredential const none {};
    CHECK(DecideFleetRead(Leading(), none, {}, "127.0.0.1", FleetReader::ByAddress).decision == FleetReadDecision::Admitted);

    auto const remote = DecideFleetRead(Leading(), none, {}, "10.0.0.7", FleetReader::ByAddress);
    CHECK(remote.decision == FleetReadDecision::Unauthenticated);
    CHECK(remote.detail.contains("--dashboard-token-file"));
}

TEST_CASE("With a token file the token decides from anywhere and loopback earns nothing", "[node][fleettext]")
{
    AdminCredential const guarded { "s3cret" };
    CHECK(DecideFleetRead(Leading(), guarded, "s3cret", "10.0.0.7", FleetReader::ByAddress).decision
          == FleetReadDecision::Admitted);
    CHECK(DecideFleetRead(Leading(), guarded, "guess", "127.0.0.1", FleetReader::ByAddress).decision
          == FleetReadDecision::Unauthenticated);
    CHECK(DecideFleetRead(Leading(), guarded, {}, "127.0.0.1", FleetReader::ByAddress).decision
          == FleetReadDecision::Unauthenticated);
}

TEST_CASE("The credential is asked before leadership so a caller without it learns no leader", "[node][fleettext]")
{
    auto const refused =
        DecideFleetRead(Following(), AdminCredential { "s3cret" }, "guess", "10.0.0.7", FleetReader::ByAddress);
    CHECK(refused.decision == FleetReadDecision::Unauthenticated);
    CHECK_FALSE(refused.detail.contains(LeaderEndpoint));

    SECTION("and a caller with it is redirected, the words being the leader's endpoint alone")
    {
        auto const redirected =
            DecideFleetRead(Following(), AdminCredential { "s3cret" }, "s3cret", "10.0.0.7", FleetReader::ByAddress);
        CHECK(redirected.decision == FleetReadDecision::NotLeader);
        CHECK(redirected.detail == LeaderEndpoint);
    }

    SECTION("and during an election the redirect names nobody rather than a guess")
    {
        auto const electing = LiveLeadership { .leads = false, .leaderEndpoint = {} };
        auto const verdict = DecideFleetRead(electing, AdminCredential {}, {}, "127.0.0.1", FleetReader::ByAddress);
        CHECK(verdict.decision == FleetReadDecision::NotLeader);
        CHECK(verdict.detail.empty());
    }
}

TEST_CASE("A proven member reads the fleet without the dashboard credential, from any address", "[node][fleettext]")
{
    // What a member's MCP fleet tools rest on: a node holds no dashboard credential, and the proof it made on
    // the sealed connection stands in for one. Both credential regimes, and a remote host in each, since the
    // address is not what admits it.
    for (auto const& dashboard: { AdminCredential {}, AdminCredential { "s3cret" } })
    {
        CHECK(DecideFleetRead(Leading(), dashboard, {}, "10.0.0.7", FleetReader::ProvenMember).decision
              == FleetReadDecision::Admitted);
        // The control: the same call by address alone is refused, so the reader is what admitted it.
        CHECK(DecideFleetRead(Leading(), dashboard, {}, "10.0.0.7", FleetReader::ByAddress).decision
              == FleetReadDecision::Unauthenticated);
    }

    SECTION("and at a follower it is told the leader, which a proven member may know")
    {
        auto const redirected =
            DecideFleetRead(Following(), AdminCredential { "s3cret" }, {}, "10.0.0.7", FleetReader::ProvenMember);
        CHECK(redirected.decision == FleetReadDecision::NotLeader);
        CHECK(redirected.detail == LeaderEndpoint);
    }

    SECTION("and a proof finds no fleet where there is none")
    {
        CHECK(DecideFleetRead(std::nullopt, AdminCredential {}, {}, "10.0.0.7", FleetReader::ProvenMember).decision
              == FleetReadDecision::NoScheduler);
    }
}
