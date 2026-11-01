// SPDX-License-Identifier: Apache-2.0
#include "FleetTextResponder.hpp"
#include "Responders.hpp"

#include <Pace/Core/WireFrame.hpp>
#include <Pace/Distributed/MembershipOracle.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Metrics/MetricsCatalog.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/LiveStream.hpp>
#include <Pace/Server/AdminCredential.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::ListedMembership;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// The member every request below arrives from, unless a case says otherwise.
constexpr std::string_view Reader = "10.0.0.7";

/// Where the leader a follower names answers.
constexpr std::string_view LeaderEndpoint = "10.0.0.2:6674";

/// Sources whose leadership and document a case scripts, and which record what was asked.
class ScriptedFleet final: public ILiveStatsSources
{
  public:
    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject /*subject*/) const override
    {
        return std::nullopt;
    }

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return leadership;
    }

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "n1.test:6674";
    }

    /// @copydoc ILiveStatsSources::FleetText
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(
        std::string_view section, std::string_view range, CompileCacheWire::FleetTextFormat format) const override
    {
        ++reads;
        askedSection = section;
        askedRange = range;
        askedFormat = format;
        return answer;
    }

    /// Who leads; a leader by default, so a case states only what it changes.
    std::optional<LiveLeadership> leadership { LiveLeadership { .leads = true, .leaderEndpoint = {} } };

    /// What a read answers; a leader's document by default.
    std::expected<FleetTextDocument, FleetTextDeclined> answer { FleetTextDocument {
        .leads = true, .leaderEndpoint = {}, .body = "# kpi\nkey\tvalue\n" } };

    mutable std::size_t reads { 0 };     ///< How many reads reached the sources.
    mutable std::string askedSection {}; ///< The section the last read named.
    mutable std::string askedRange {};   ///< The range the last read named.
    /// The format the last read named.
    mutable Wire::FleetTextFormat askedFormat { Wire::FleetTextFormat::Text };
};

/// A FLEET-TEXT frame.
[[nodiscard]] std::vector<std::byte> FleetTextFrame(std::string section = {},
                                                    std::string range = {},
                                                    std::string token = {},
                                                    Wire::FleetTextFormat format = Wire::FleetTextFormat::Text)
{
    return Wire::EncodeFleetTextRequest(Wire::FleetTextRequest {
        .section = std::move(section), .range = std::move(range), .dashboardToken = std::move(token), .format = format });
}

/// What a refusal said, beside its code.
[[nodiscard]] std::string DetailOf(std::span<std::byte const> reply)
{
    auto const refusal = Wire::DecodeErrorPayload(Testing::PayloadOf(reply));
    REQUIRE(refusal.has_value());
    return std::string { Unwrap(refusal).second };
}

/// @return Whether no counter in the table has moved.
[[nodiscard]] bool NothingCounted(IMetricsSink const& metrics)
{
    return std::ranges::all_of(CounterTable, [&metrics](auto const& row) { return metrics.Read(row.counter) == 0; });
}

/// Everything a case needs: a leader's sources, members, and a responder with no token file.
struct Rig
{
    AtomicMetricsSink metrics;
    ScriptedFleet sources;
    ListedMembership membership { { std::string { Reader }, "127.0.0.1" }, Distributed::MembershipParticipant::OpenPolicy };
    FleetTextResponder responder { sources, membership, AdminCredential {}, metrics };

    /// Ask @p on, from @p peer.
    [[nodiscard]] std::vector<std::byte> Ask(FleetTextResponder& on,
                                             std::vector<std::byte> const& frame,
                                             std::string_view peer)
    {
        return core::async::syncRun(on.Answer(frame, PeerIdentity { .host = std::string { peer } })).bytes;
    }

    /// Ask the rig's responder from this machine, which a node with no token file admits.
    [[nodiscard]] std::vector<std::byte> AskLocally(std::vector<std::byte> const& frame)
    {
        return Ask(responder, frame, "127.0.0.1");
    }
};

} // namespace

TEST_CASE("A fleet read is answered with the document the sources rendered for the words asked", "[node][fleettext]")
{
    Rig rig;
    auto const reply = rig.AskLocally(FleetTextFrame("series", "7d"));

    REQUIRE(Testing::StatusOf(reply) == Wire::Status::Ok);
    CHECK(Wire::AsStringView(Testing::PayloadOf(reply)) == "# kpi\nkey\tvalue\n");

    // The words reach the sources as typed: the component parses no key of its own, so a section
    // it could refuse here would be a second table of what exists.
    CHECK(rig.sources.askedSection == "series");
    CHECK(rig.sources.askedRange == "7d");
    CHECK(NothingCounted(rig.metrics));
}

TEST_CASE("The format a fleet read names reaches the sources, and a json read is gated like any other", "[node][fleettext]")
{
    Rig rig;
    rig.sources.answer = FleetTextDocument { .leads = true, .leaderEndpoint = {}, .body = R"({"schema":2})" };
    auto const reply = rig.AskLocally(FleetTextFrame({}, "24h", {}, Wire::FleetTextFormat::Json));
    REQUIRE(Testing::StatusOf(reply) == Wire::Status::Ok);
    CHECK(Wire::AsStringView(Testing::PayloadOf(reply)) == R"({"schema":2})");
    CHECK(rig.sources.askedFormat == Wire::FleetTextFormat::Json);

    // A follower redirects a json read exactly as it does a text one.
    rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = "10.0.0.2:6674" };
    auto const followed = rig.AskLocally(FleetTextFrame({}, {}, {}, Wire::FleetTextFormat::Json));
    CHECK(Testing::ErrorOf(followed) == Wire::ErrorCode::NotLeader);

    // And a stranger gets nothing, whatever the format.
    auto const stranger = rig.Ask(rig.responder, FleetTextFrame({}, {}, {}, Wire::FleetTextFormat::Json), "192.0.2.1");
    CHECK(Testing::ErrorOf(stranger) == Wire::ErrorCode::NotAMember);
}

TEST_CASE("A version 16 FLEET-TEXT is refused UnsupportedVersion, not as a malformed frame", "[node][fleettext]")
{
    Rig rig;
    // The version-16 grammar: three fields, no format. Hand-built, because the encoder only speaks 17.
    auto const old = Wire::Detail::EncodeRequest(
        16, Wire::Op::FleetText, { Wire::AsBytes("series"), Wire::AsBytes("7d"), Wire::AsBytes("") });
    auto const reply = rig.AskLocally(old);
    CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::UnsupportedVersion);
    CHECK(DetailOf(reply).contains("17..17"));
    CHECK(rig.sources.reads == 0);
    // Not mistaken for a malformed frame, which would have moved this counter.
    CHECK(NothingCounted(rig.metrics));

    // And a version nobody speaks yet is the same answer.
    auto const future = Wire::Detail::EncodeRequest(
        18, Wire::Op::FleetText, { Wire::AsBytes(""), Wire::AsBytes(""), Wire::AsBytes(""), Wire::AsBytes("") });
    CHECK(Testing::ErrorOf(rig.AskLocally(future)) == Wire::ErrorCode::UnsupportedVersion);
}

TEST_CASE("A stranger is refused a fleet read by name and counted once whichever door it used", "[node][fleettext]")
{
    Rig rig;
    auto const reply = rig.Ask(rig.responder, FleetTextFrame(), "192.0.2.1");
    CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::NotAMember);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember) == 1);
    CHECK(rig.sources.reads == 0);

    // `RefusePeer` is the one implementation; the door asking it counts once more, not twice.
    CHECK(
        rig.responder
            .RefusePeer(PeerIdentity { .host = std::string { "192.0.2.1" } }, static_cast<std::uint8_t>(Wire::Op::FleetText))
            .has_value());
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember) == 2);
    CHECK_FALSE(
        rig.responder
            .RefusePeer(PeerIdentity { .host = std::string { Reader } }, static_cast<std::uint8_t>(Wire::Op::FleetText))
            .has_value());
}

TEST_CASE("The dashboard credential decides a fleet read and is counted against the fleet-text series", "[node][fleettext]")
{
    Rig rig;

    SECTION("with no token file a remote member is refused and this machine is not")
    {
        auto const remote = rig.Ask(rig.responder, FleetTextFrame(), Reader);
        CHECK(Testing::ErrorOf(remote) == Wire::ErrorCode::Unauthenticated);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedUnauthenticated) == 1);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated) == 0);
        CHECK(rig.sources.reads == 0);

        CHECK(Testing::StatusOf(rig.AskLocally(FleetTextFrame())) == Wire::Status::Ok);
    }

    SECTION("with a token file the token decides from anywhere")
    {
        FleetTextResponder guarded { rig.sources, rig.membership, AdminCredential { "s3cret" }, rig.metrics };
        CHECK(Testing::ErrorOf(rig.Ask(guarded, FleetTextFrame({}, {}, "guess"), "127.0.0.1"))
              == Wire::ErrorCode::Unauthenticated);
        CHECK(Testing::StatusOf(rig.Ask(guarded, FleetTextFrame({}, {}, "s3cret"), Reader)) == Wire::Status::Ok);
    }

    SECTION("and a follower asked without it names no leader")
    {
        rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint } };
        auto const reply = rig.Ask(rig.responder, FleetTextFrame(), Reader);
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::Unauthenticated);
        CHECK_FALSE(DetailOf(reply).contains(LeaderEndpoint));
    }
}

TEST_CASE("A member that proved its key reads the fleet without the dashboard credential", "[node][fleettext]")
{
    // What a member's MCP fleet tools reach the leader by: the proven, sealed connection, from a node that
    // holds no dashboard credential. The production fold, with the key PRESENTED on the connection -- a
    // host-answering fake cannot stand for a proof.
    AtomicMetricsSink metrics;
    ScriptedFleet sources;
    Testing::RosterFold const roster { { "pc-07" }, { "gone-01" } };
    FleetTextResponder guarded { sources, roster.admitted, AdminCredential { "s3cret" }, metrics };
    auto const ask = [&guarded](PeerIdentity peer) {
        return core::async::syncRun(guarded.Answer(FleetTextFrame({}, {}, {}, Wire::FleetTextFormat::Json), std::move(peer)))
            .bytes;
    };

    auto const proven = ask(PeerIdentity { .host = "10.0.0.7", .proven = Testing::IdentityOf("pc-07") });
    CHECK(Testing::StatusOf(proven) == Wire::Status::Ok);
    CHECK(sources.reads == 1);
    CHECK(sources.askedFormat == Wire::FleetTextFormat::Json);

    SECTION("this machine without a proof is still asked for the credential")
    {
        // The control: loopback is admitted to the surface, and earns no fleet read under a token file.
        CHECK(Testing::ErrorOf(ask(PeerIdentity { .host = "127.0.0.1" })) == Wire::ErrorCode::Unauthenticated);
        CHECK(metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedUnauthenticated) == 1);
    }

    SECTION("a revoked key's proof reads nothing, and is refused as a stranger")
    {
        auto const refused = ask(PeerIdentity { .host = "127.0.0.1", .proven = Testing::IdentityOf("gone-01") });
        CHECK(Testing::ErrorOf(refused) == Wire::ErrorCode::NotAMember);
        CHECK(sources.reads == 1);
    }
}

TEST_CASE("A follower redirects a fleet read to the leader and a worker says the fleet is elsewhere", "[node][fleettext]")
{
    Rig rig;

    SECTION("a follower: NotLeader whose words are the leader's endpoint, uncounted")
    {
        rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint } };
        auto const reply = rig.AskLocally(FleetTextFrame("kpi"));
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::NotLeader);
        CHECK(DetailOf(reply) == LeaderEndpoint);
        CHECK(rig.sources.reads == 0);
        CHECK(NothingCounted(rig.metrics));
    }

    SECTION("a node with no scheduler: served elsewhere, which is not unimplemented, uncounted")
    {
        rig.sources.leadership = std::nullopt;
        auto const reply = rig.AskLocally(FleetTextFrame("kpi"));
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::DispatchNotPermitted);
        CHECK(rig.sources.reads == 0);
        CHECK(NothingCounted(rig.metrics));
    }

    SECTION("a document rendered after the leader stepped down is judged by that document")
    {
        // The gate read `leads` a moment before the render; the snapshot the body came from is the
        // later reading, and a follower's registry is a fraction presented as the whole.
        rig.sources.answer =
            FleetTextDocument { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint }, .body = "# kpi\n" };
        auto const reply = rig.AskLocally(FleetTextFrame("kpi"));
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::NotLeader);
        CHECK(DetailOf(reply) == LeaderEndpoint);
        CHECK(NothingCounted(rig.metrics));
    }
}

TEST_CASE("A section or a range this build does not serve is refused by its own code and not counted", "[node][fleettext]")
{
    Rig rig;
    rig.sources.answer = std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::UnknownSelector,
                                                             .detail = "unknown range; this build serves: 1h, 2h\n" });

    auto const reply = rig.AskLocally(FleetTextFrame("kpi", "1fortnight"));
    CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::UnknownFleetSelector);
    CHECK(DetailOf(reply) == "unknown range; this build serves: 1h, 2h\n");
    CHECK(NothingCounted(rig.metrics));

    SECTION("while sources detached mid-read say the fleet is not served here")
    {
        rig.sources.answer =
            std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = "stopping" });
        CHECK(Testing::ErrorOf(rig.AskLocally(FleetTextFrame())) == Wire::ErrorCode::DispatchNotPermitted);
        CHECK(NothingCounted(rig.metrics));
    }
}

TEST_CASE("A malformed fleet read and a frame-ceiling probe are counted and an unknown opcode is not", "[node][fleettext]")
{
    Rig rig;

    auto const fields = WireFields::Encode({ Wire::AsBytes("kpi"), Wire::AsBytes("1h") });
    std::vector<std::byte> frame(Wire::RequestHeaderSize);
    WireFrame::PutHeader(frame,
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::FleetText),
                         static_cast<std::uint32_t>(fields.size()));
    frame.insert(frame.end(), fields.begin(), fields.end());

    CHECK(Testing::ErrorOf(rig.AskLocally(frame)) == Wire::ErrorCode::MalformedFrame);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedMalformed) == 1);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed) == 0);
    CHECK(rig.sources.reads == 0);

    auto const opRaw = static_cast<std::uint8_t>(Wire::Op::FleetText);
    CHECK(Testing::ErrorOf(rig.responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, opRaw, "512 MiB"))
          == Wire::ErrorCode::PayloadTooLarge);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge) == 1);

    CHECK(Testing::ErrorOf(rig.responder.RefusalReply(Wire::PrePayloadDecision::UnknownOpcode, opRaw, {}))
          == Wire::ErrorCode::UnknownOpcode);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge) == 1);

    SECTION("and a busy surface says so against the fleet-text series, not the operator verbs'")
    {
        CHECK(Testing::ErrorOf(rig.responder.EndpointRefusalReply(EndpointRefusal::InFlightBudget, opRaw, {}))
              == Wire::ErrorCode::EndpointBusy);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedEndpointBusy) == 1);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedEndpointBusy) == 0);
    }
}

TEST_CASE("A fleet read through the slot is the attached sources' and a detached slot has no fleet", "[node][fleettext]")
{
    // The node's responder is built against the slot before the fleet exists, so the slot is the
    // one door the document is read through; a slot answering from nothing would serve every
    // reader "no fleet" on a leader.
    ScriptedFleet sources;
    LiveStatsSourceSlot slot;
    {
        auto const attached = slot.Attach(sources);
        auto const read = slot.FleetText("series", "7d", Wire::FleetTextFormat::Json);
        REQUIRE(read.has_value());
        CHECK(read->body == "# kpi\nkey\tvalue\n");
        CHECK(sources.askedSection == "series");
        CHECK(sources.askedRange == "7d");
        CHECK(sources.askedFormat == Wire::FleetTextFormat::Json);
    }

    auto const detached = slot.FleetText("series", "7d", Wire::FleetTextFormat::Json);
    REQUIRE_FALSE(detached.has_value());
    CHECK(detached.error().refusal == FleetTextRefusal::NoFleet);
    CHECK(sources.reads == 1);
}

TEST_CASE("MergedResponder routes the Fleet family to the fleet-text component and nowhere else", "[node][fleettext]")
{
    Rig rig;
    MergedResponder merged { SurfaceComponents { .fleet = &rig.responder } };

    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::FleetText)) == &rig.responder);
    CHECK(
        Testing::StatusOf(core::async::syncRun(merged.Answer(FleetTextFrame(), PeerIdentity { .host = "127.0.0.1" })).bytes)
        == Wire::Status::Ok);

    // Routes rather than catching all: no other verb reaches it.
    for (auto const& row: Wire::OpTable)
        if (row.code != Wire::Op::FleetText)
        {
            INFO("verb " << row.name);
            CHECK(merged.OwnerOf(static_cast<std::uint8_t>(row.code)) != &rig.responder);
        }

    SECTION("and a node with no fleet-text component answers served-nowhere rather than crashing")
    {
        MergedResponder without { SurfaceComponents {} };
        CHECK(Testing::ErrorOf(
                  core::async::syncRun(without.Answer(FleetTextFrame(), PeerIdentity { .host = "127.0.0.1" })).bytes)
              == Wire::UnimplementedVerb);
    }

    SECTION("and a verb routed to it directly that it does not own is stepped over, uncounted")
    {
        std::vector<std::byte> fetch(Wire::RequestHeaderSize);
        WireFrame::PutHeader(fetch, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Fetch), 0);
        CHECK(Testing::ErrorOf(rig.AskLocally(fetch)) == Wire::UnimplementedVerb);
        CHECK(NothingCounted(rig.metrics));
    }
}
