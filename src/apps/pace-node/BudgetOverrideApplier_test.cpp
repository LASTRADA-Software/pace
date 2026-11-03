// SPDX-License-Identifier: Apache-2.0
#include "BudgetOverrideApplier.hpp"
#include "McpUpstream.hpp"
#include "NodeConditions.hpp"
#include "Responders.hpp"

#include <Pace/Cluster/BudgetOverride.hpp>
#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/IClusterAdmin.hpp>
#include <Pace/Distributed/SchedulerProtocol.hpp>
#include <Pace/Distributed/SchedulerService.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/WireReply.hpp>

using namespace Pace;
using namespace Pace::Node;

namespace
{
namespace Wire = CompileCacheWire;

constexpr auto MiB = std::uint64_t { 1024 } * 1024;
constexpr std::string_view SelfId = "node-a";

/// The configuration in force, changed by a case the way an accepted reload changes the reloader's snapshot.
class SwappableConfig final: public INodeConfigSource
{
  public:
    explicit SwappableConfig(NodeConfig cfg):
        _cfg { std::move(cfg) }
    {
    }

    [[nodiscard]] NodeConfig Current() const override
    {
        std::scoped_lock const lock { _mutex };
        return _cfg;
    }

    /// Put @p memory in force for the memory tier.
    /// @param memory The configured budget.
    void SetMemory(std::uint64_t memory)
    {
        std::scoped_lock const lock { _mutex };
        _cfg.cacheMemoryBytes = memory;
    }

  private:
    mutable std::mutex _mutex;
    NodeConfig _cfg;
};

/// Tiers that record every budget put in force.
class RecordingTiers final: public ITierBudgets
{
  public:
    [[nodiscard]] bool HasTier(StorageTier tier) const override
    {
        return tier == StorageTier::Memory || hasDisk;
    }

    [[nodiscard]] std::expected<void, std::string> ApplyOverride(StorageTier tier, std::uint64_t bytes) override
    {
        if (refuse.has_value())
            return std::unexpected(*refuse);
        applied.emplace_back(tier, bytes, BudgetSource::Cluster);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> RestoreConfigured(StorageTier tier, std::uint64_t bytes) override
    {
        applied.emplace_back(tier, bytes, BudgetSource::Configured);
        return {};
    }

    bool hasDisk { false };                                                    ///< Whether a disk tier exists.
    std::optional<std::string> refuse {};                                      ///< Refuse every override with this.
    std::vector<std::tuple<StorageTier, std::uint64_t, BudgetSource>> applied; ///< Every budget put in force.
};

/// A leader that takes every report, or is unreachable.
class RecordingReports final: public IMemberReportClient
{
  public:
    [[nodiscard]] std::expected<void, std::string> RecordBaseline(StorageTier tier,
                                                                  std::uint64_t baseline,
                                                                  std::uint64_t overrideId) override
    {
        if (!reachable)
            return std::unexpected(std::string { "unreachable: nothing answered" });
        baselines.emplace_back(tier, baseline, overrideId);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> DropOverride(StorageTier tier, std::uint64_t overrideId) override
    {
        if (!reachable)
            return std::unexpected(std::string { "unreachable: nothing answered" });
        drops.emplace_back(tier, overrideId);
        return {};
    }

    bool reachable { true };                                                      ///< Whether the leader answers.
    std::vector<std::tuple<StorageTier, std::uint64_t, std::uint64_t>> baselines; ///< Every baseline taken.
    std::vector<std::pair<StorageTier, std::uint64_t>> drops;                     ///< Every drop taken.
};

/// A state recording this node with @p memory as its memory override, or none.
/// @param memory The override, or nothing.
/// @return The state.
[[nodiscard]] Cluster::ClusterState StateWith(std::optional<Cluster::BudgetOverride> memory)
{
    auto state = Cluster::ClusterState {};
    auto member = Cluster::ClusterMember {};
    member.id = std::string { SelfId };
    member.raftEndpoint = "10.0.0.1:6675";
    member.seat = Cluster::MemberSeat::Learner;
    member.publicKey = Testing::TestKeyPair(std::string { SelfId }).PublicKey();
    member.memoryOverride = std::move(memory);
    state.members.push_back(std::move(member));
    return state;
}

/// An override of @p bytes recorded at entry @p id, with @p baseline.
[[nodiscard]] Cluster::BudgetOverride Override(std::uint64_t bytes, std::optional<std::uint64_t> baseline, std::uint64_t id)
{
    return Cluster::BudgetOverride {
        .bytes = bytes, .baseline = baseline, .setAtUnixSeconds = 1'767'225'600, .id = id, .setBy = "voter-1"
    };
}

/// @param memory The memory tier's configured budget.
/// @return A configuration with it.
[[nodiscard]] NodeConfig ConfigWith(std::uint64_t memory)
{
    auto cfg = NodeConfig {};
    cfg.cacheMemoryBytes = memory;
    cfg.cacheDiskBytes = 0;
    return cfg;
}

/// One node's applier and everything it reaches, reconciled only when a case says so.
struct Rig
{
    explicit Rig(std::uint64_t memory):
        config { ConfigWith(memory) }
    {
    }

    /// @return The memory tier's row.
    [[nodiscard]] TierBudgetStatus Memory() const
    {
        for (auto const& row: applier.BudgetStatus())
            if (row.tier == StorageTier::Memory)
                return row;
        FAIL("no memory row");
        return {};
    }

    SwappableConfig config;
    RecordingTiers tiers;
    RecordingReports reports;
    CapturingLogger logger;
    NodeConditions conditions;
    BudgetOverrideApplier applier {
        tiers, reports, config, logger, &conditions, std::string { SelfId }, ReconcileOn::Caller
    };
};

/// @param logger A capturing logger.
/// @return Every line it took at Warn.
[[nodiscard]] std::vector<std::string> WarnLines(CapturingLogger const& logger)
{
    auto lines = std::vector<std::string> {};
    for (auto const& record: logger.Snapshot())
        if (record.level == LogLevel::Warn)
            lines.push_back(record.message);
    return lines;
}

/// @param conditions A registry.
/// @return The dropped row's state.
[[nodiscard]] Wire::ConditionState DroppedState(NodeConditions const& conditions)
{
    return conditions.StateOf(NodeCondition::CacheBudgetOverrideDropped);
}

} // namespace

TEST_CASE("DecideOverride: each row of the table", "[node][budget][budget-applier]")
{
    struct Row
    {
        std::optional<Cluster::BudgetOverride> recorded {};
        std::uint64_t configured { 0 };
        bool alreadyApplied { false };
        OverrideAction expected { OverrideAction::Last };
    };
    auto const rows = std::array {
        // No override: nothing to do -- the configured value stands.
        Row { .recorded = std::nullopt, .configured = 512 * MiB, .alreadyApplied = false, .expected = OverrideAction::None },
        // Baseline unset: report it first, whether or not applied -- a report is idempotent at the leader.
        Row { .recorded = Override(64 * MiB, std::nullopt, 7),
              .configured = 512 * MiB,
              .alreadyApplied = false,
              .expected = OverrideAction::ReportBaselineAndApply },
        Row { .recorded = Override(64 * MiB, std::nullopt, 7),
              .configured = 512 * MiB,
              .alreadyApplied = true,
              .expected = OverrideAction::ReportBaselineAndApply },
        // Baseline equal to configured: apply, or nothing once applied.
        Row { .recorded = Override(64 * MiB, 512 * MiB, 7),
              .configured = 512 * MiB,
              .alreadyApplied = false,
              .expected = OverrideAction::Apply },
        Row { .recorded = Override(64 * MiB, 512 * MiB, 7),
              .configured = 512 * MiB,
              .alreadyApplied = true,
              .expected = OverrideAction::None },
        // A zero baseline is a real configuration (unbounded), never an absence.
        Row { .recorded = Override(64 * MiB, 0, 7),
              .configured = 0,
              .alreadyApplied = false,
              .expected = OverrideAction::Apply },
        // Baseline different: the local edit is later and wins, applied or not.
        Row { .recorded = Override(64 * MiB, 512 * MiB, 7),
              .configured = 1024 * MiB,
              .alreadyApplied = false,
              .expected = OverrideAction::DropLocalNewer },
        Row { .recorded = Override(64 * MiB, 512 * MiB, 7),
              .configured = 1024 * MiB,
              .alreadyApplied = true,
              .expected = OverrideAction::DropLocalNewer },
        Row { .recorded = Override(64 * MiB, 0, 7),
              .configured = 512 * MiB,
              .alreadyApplied = false,
              .expected = OverrideAction::DropLocalNewer },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.configured, row.alreadyApplied, row.recorded.has_value());
        CHECK(DecideOverride(row.recorded, row.configured, row.alreadyApplied) == row.expected);
    }
}

TEST_CASE("The applier never compares clocks: it is built with no clock, and decides by baseline alone",
          "[node][budget][budget-applier]")
{
    // The constructor takes no clock of any kind -- the whole of "the later change wins" is the configuration moving
    // since the baseline -- so a clock cannot be injected and cannot be read.
    STATIC_CHECK_FALSE(std::is_constructible_v<BudgetOverrideApplier,
                                               ITierBudgets&,
                                               IMemberReportClient&,
                                               INodeConfigSource const&,
                                               core::platform::IClock&,
                                               ILogger&,
                                               NodeConditions*,
                                               std::string,
                                               ReconcileOn>);
    STATIC_CHECK(std::is_constructible_v<BudgetOverrideApplier,
                                         ITierBudgets&,
                                         IMemberReportClient&,
                                         INodeConfigSource const&,
                                         ILogger&,
                                         NodeConditions*,
                                         std::string,
                                         ReconcileOn>);

    // And a leader whose clock is far behind or ahead changes nothing: the stamp is display only.
    Rig rig { 512 * MiB };
    auto early = Override(64 * MiB, 512 * MiB, 9);
    early.setAtUnixSeconds = 0;
    rig.applier.OnState(StateWith(early));
    rig.applier.Reconcile();
    CHECK(rig.Memory().source == BudgetSource::Cluster);
    CHECK(rig.Memory().effective == std::optional { 64 * MiB });
}

TEST_CASE("Set while down, node starts: the baseline is recorded, then the override applied",
          "[node][budget][budget-applier]")
{
    Rig rig { 512 * MiB };
    // Before any state the node runs what it is configured with, and says so.
    CHECK(rig.Memory().source == BudgetSource::Configured);
    CHECK(rig.Memory().effective == std::optional { 512 * MiB });
    CHECK(DroppedState(rig.conditions) == Wire::ConditionState::Clear);

    // The override an operator set while this node was down: no baseline yet.
    rig.applier.OnState(StateWith(Override(64 * MiB, std::nullopt, 7)));
    rig.applier.Reconcile();
    REQUIRE(rig.reports.baselines.size() == 1);
    CHECK(rig.reports.baselines[0] == std::tuple { StorageTier::Memory, 512 * MiB, std::uint64_t { 7 } });
    REQUIRE(rig.tiers.applied.size() == 1);
    CHECK(rig.tiers.applied[0] == std::tuple { StorageTier::Memory, 64 * MiB, BudgetSource::Cluster });
    CHECK(rig.Memory().source == BudgetSource::Cluster);
    CHECK(rig.Memory().note.empty());

    // The leader's record of the baseline comes back: nothing more to do, and nothing applied twice.
    rig.applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
    rig.applier.Reconcile();
    CHECK(rig.reports.baselines.size() == 1);
    CHECK(rig.tiers.applied.size() == 1);
    CHECK(rig.Memory().recorded.and_then([](Cluster::BudgetOverride const& o) { return o.baseline; })
          == std::optional { 512 * MiB });
    CHECK(DroppedState(rig.conditions) == Wire::ConditionState::Clear);
}

TEST_CASE("A baseline the leader did not take leaves the override unapplied, and is asked again",
          "[node][budget][budget-applier]")
{
    Rig rig { 512 * MiB };
    rig.reports.reachable = false;
    rig.applier.OnState(StateWith(Override(64 * MiB, std::nullopt, 7)));
    rig.applier.Reconcile();
    // Never applied against a baseline nobody recorded.
    CHECK(rig.tiers.applied.empty());
    CHECK(rig.Memory().source == BudgetSource::Configured);
    CHECK(rig.Memory().note.contains("could not be reported"));

    // The next trigger -- here a pass the retry would run -- asks again, and once taken the override applies.
    rig.reports.reachable = true;
    rig.applier.Reconcile();
    CHECK(rig.reports.baselines.size() == 1);
    CHECK(rig.Memory().source == BudgetSource::Cluster);
    CHECK(rig.Memory().note.empty());
}

TEST_CASE("Admin edits the file: the reload drops the override and raises the condition",
          "[node][budget][budget-applier][conditions]")
{
    Rig rig { 512 * MiB };
    rig.applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
    rig.applier.Reconcile();
    REQUIRE(rig.Memory().source == BudgetSource::Cluster);

    // The operator edits `cache_memory` and the reload is accepted: the configuration in force moved since the
    // baseline, so the local edit is the later change.
    rig.config.SetMemory(1024 * MiB);
    rig.applier.ReloadAccepted();
    rig.applier.Reconcile();

    REQUIRE(rig.reports.drops.size() == 1);
    CHECK(rig.reports.drops[0] == std::pair { StorageTier::Memory, std::uint64_t { 7 } });
    CHECK(rig.tiers.applied.back() == std::tuple { StorageTier::Memory, 1024 * MiB, BudgetSource::Configured });
    CHECK(rig.Memory().source == BudgetSource::Configured);
    CHECK(rig.Memory().effective == std::optional { 1024 * MiB });
    CHECK(DroppedState(rig.conditions) == Wire::ConditionState::Raised);
    // Said at Warn, naming both values.
    auto const warned = WarnLines(rig.logger);
    REQUIRE(warned.size() == 1);
    CHECK(warned[0].contains("1G"));
    CHECK(warned[0].contains("512M"));

    // The leader's removal arrives: the row stays raised (latched), nothing is said or sent again.
    rig.applier.OnState(StateWith(std::nullopt));
    rig.applier.Reconcile();
    CHECK(rig.reports.drops.size() == 1);
    CHECK(WarnLines(rig.logger).size() == 1);
    CHECK(DroppedState(rig.conditions) == Wire::ConditionState::Raised);
    CHECK(rig.Memory().source == BudgetSource::Configured);
}

TEST_CASE("A drop the leader did not take is sent again, and said once", "[node][budget][budget-applier]")
{
    Rig rig { 1024 * MiB };
    rig.reports.reachable = false;
    rig.applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
    rig.applier.Reconcile();
    rig.applier.Reconcile();
    CHECK(rig.reports.drops.empty());
    CHECK(WarnLines(rig.logger).size() == 1);
    CHECK(rig.Memory().note.contains("retrying"));
    // Never applied, since the configuration outranks it.
    CHECK(rig.tiers.applied.empty());

    rig.reports.reachable = true;
    rig.applier.Reconcile();
    CHECK(rig.reports.drops.size() == 1);
    CHECK(WarnLines(rig.logger).size() == 1);
}

TEST_CASE("File unchanged across a restart keeps the override", "[node][budget][budget-applier]")
{
    // A restart is a new applier over the same configuration and the same recorded state.
    for (auto const restart: { 1, 2 })
    {
        CAPTURE(restart);
        Rig rig { 512 * MiB };
        rig.applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
        rig.applier.Reconcile();
        CHECK(rig.reports.drops.empty());
        CHECK(rig.reports.baselines.empty());
        CHECK(rig.Memory().source == BudgetSource::Cluster);
        CHECK(rig.Memory().effective == std::optional { 64 * MiB });
        CHECK(DroppedState(rig.conditions) == Wire::ConditionState::Clear);
    }
}

TEST_CASE("A cleared override gives the tier its configured budget back", "[node][budget][budget-applier]")
{
    Rig rig { 512 * MiB };
    rig.applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
    rig.applier.Reconcile();
    rig.applier.OnState(StateWith(std::nullopt));
    rig.applier.Reconcile();
    CHECK(rig.tiers.applied.back() == std::tuple { StorageTier::Memory, 512 * MiB, BudgetSource::Configured });
    CHECK(rig.Memory().source == BudgetSource::Configured);
    CHECK_FALSE(rig.Memory().recorded.has_value());
    // A clear is no drop.
    CHECK(DroppedState(rig.conditions) == Wire::ConditionState::Clear);
}

TEST_CASE("A reloaded budget with no override is put in force", "[node][budget][budget-applier]")
{
    Rig rig { 512 * MiB };
    rig.config.SetMemory(256 * MiB);
    rig.applier.ReloadAccepted();
    rig.applier.Reconcile();
    CHECK(rig.tiers.applied.back() == std::tuple { StorageTier::Memory, 256 * MiB, BudgetSource::Configured });
    CHECK(rig.Memory().effective == std::optional { 256 * MiB });
}

TEST_CASE("An override for a tier this node lacks is not applied, and says why", "[node][budget][budget-applier]")
{
    Rig rig { 512 * MiB };
    auto state = StateWith(std::nullopt);
    state.members[0].diskOverride = Override(4096 * MiB, std::nullopt, 8);
    rig.applier.OnState(state);
    rig.applier.Reconcile();
    CHECK(rig.reports.baselines.empty());
    CHECK(rig.tiers.applied.empty());
    auto const rows = rig.applier.BudgetStatus();
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].tier == StorageTier::Disk);
    CHECK_FALSE(rows[1].effective.has_value());
    CHECK(rows[1].note.contains("no disk tier"));
}

TEST_CASE("A budget the tier refuses is shown with the reason, said once, and never retried hot",
          "[node][budget][budget-applier]")
{
    Rig rig { 512 * MiB };
    rig.tiers.refuse = "a budget of 18446744073709551615 bytes is more than this process can address";
    rig.applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
    rig.applier.Reconcile();
    rig.applier.Reconcile();
    CHECK(rig.Memory().source == BudgetSource::Configured);
    CHECK(rig.Memory().note.contains("could not be applied"));
    CHECK(WarnLines(rig.logger).size() == 1);
}

TEST_CASE("An applied state reaches the tiers on the applier's own thread, never the caller's",
          "[node][budget][budget-applier]")
{
    // Production's shape: `OnState` is the consensus apply callback, which must not apply or report.
    SwappableConfig config { ConfigWith(512 * MiB) };
    struct ThreadTiers final: ITierBudgets
    {
        [[nodiscard]] bool HasTier(StorageTier tier) const override
        {
            return tier == StorageTier::Memory;
        }
        [[nodiscard]] std::expected<void, std::string> ApplyOverride(StorageTier /*tier*/, std::uint64_t /*bytes*/) override
        {
            std::scoped_lock const lock { mutex };
            appliedOn = std::this_thread::get_id();
            return {};
        }
        [[nodiscard]] std::expected<void, std::string> RestoreConfigured(StorageTier /*tier*/,
                                                                         std::uint64_t /*bytes*/) override
        {
            return {};
        }
        mutable std::mutex mutex;
        std::optional<std::thread::id> appliedOn;
    } tiers;
    RecordingReports reports;
    NullLogger logger;
    BudgetOverrideApplier applier {
        tiers, reports, config, logger, nullptr, std::string { SelfId }, ReconcileOn::OwnThread
    };
    applier.OnState(StateWith(Override(64 * MiB, 512 * MiB, 7)));
    auto const reached = [&tiers] {
        std::scoped_lock const lock { tiers.mutex };
        return tiers.appliedOn.has_value();
    };
    auto const applied = Testing::WaitUntil("the applier's thread to apply the override the state recorded",
                                            reached,
                                            [&reached] { return std::format("applied {}", reached()); });
    REQUIRE(applied);
    std::scoped_lock const lock { tiers.mutex };
    CHECK(tiers.appliedOn != std::optional { std::this_thread::get_id() });
}

namespace
{
/// The leader's cluster: this node recorded under its own key, with an override awaiting its baseline.
class OneMemberCluster final: public Distributed::IClusterAdmin
{
  public:
    std::vector<Cluster::Command> proposed; ///< Every proposal, in order.

    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        return StateWith(Override(64 * MiB, std::nullopt, 7));
    }

    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
    {
        proposed.push_back(command);
        return {};
    }

    void NoteAnnouncedEndpoint(Consensus::NodeId const& /*member*/, std::string /*endpoint*/) override {}
};

/// The upstream's in-process path on the LEADER, exactly: a frame put to this node is answered by its own router
/// through `FrameRouterAnswerer`, which presents -- or, without an identity, does not present -- this node's proof.
struct SelfAnswering final: IMcpUpstream
{
    explicit SelfAnswering(FrameRouterAnswerer& answerer):
        target { answerer }
    {
    }

    [[nodiscard]] UpstreamResult Ask(UpstreamTarget /*target*/, std::string_view /*node*/, std::string frame) override
    {
        auto const bytes = std::as_bytes(std::span { frame.data(), frame.size() });
        auto const reply = core::async::syncRun(target.Answer(std::vector<std::byte> { bytes.begin(), bytes.end() }));
        if (Testing::StatusOf(reply) == Wire::Status::Ok)
            return UpstreamReply { .status = Wire::Status::Ok, .payload = {}, .answeredBy = "self", .term = 1 };
        lastError = Testing::ErrorOf(reply);
        return std::unexpected { std::pair { UpstreamFailure::Refused, std::string { "refused" } } };
    }

    std::optional<Wire::ErrorCode> lastError; ///< The code the last refusal carried.
    FrameRouterAnswerer& target;              ///< This node's in-process path.
};

/// A leading scheduler over `OneMemberCluster`, served through the production responder.
struct LeaderRig
{
    LeaderRig()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        service.AdministerWith(cluster);
    }

    OneMemberCluster cluster; ///< Declared before the service, which holds a reference to it.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, schedulerLogger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };
    Testing::RosterFold const roster { { std::string { SelfId } } };
    SchedulerResponder responder { protocol, roster.admitted, metrics };
};
} // namespace

TEST_CASE("The leader's own MEMBER-REPORT rides its in-process path under its own proof, and is refused without it",
          "[node][budget][budget-applier][member-report]")
{
    // No route that dials itself: on the leader `NodeIoUpstream` answers a frame to itself through
    // `FrameRouterAnswerer`, which presents this node's proof, so MEMBER-REPORT's `ProvenNodeOnly` door is passed and the
    // report is attributed to this machine.
    LeaderRig leader;
    FrameRouterAnswerer self { Testing::IdentityOf(std::string { SelfId }) };
    self.RouteThrough(&leader.responder);
    SelfAnswering upstream { self };
    UpstreamMemberReportClient reports { upstream };

    REQUIRE(reports.RecordBaseline(StorageTier::Memory, 512 * MiB, 7).has_value());
    REQUIRE(leader.cluster.proposed.size() == 1);
    auto const& command = leader.cluster.proposed[0];
    CHECK(command.kind == Cluster::CommandKind::RecordBaseline);
    CHECK(command.key == SelfId);
    CHECK(command.baselineBytes == std::optional { 512 * MiB });
    CHECK(command.overrideId == std::optional<std::uint64_t> { 7 });

    SECTION("without its identity the same report is refused by the door, and nothing is proposed")
    {
        FrameRouterAnswerer anonymous { std::nullopt };
        anonymous.RouteThrough(&leader.responder);
        SelfAnswering anonymousUpstream { anonymous };
        UpstreamMemberReportClient anonymousReports { anonymousUpstream };
        CHECK_FALSE(anonymousReports.DropOverride(StorageTier::Memory, 7).has_value());
        CHECK(anonymousUpstream.lastError == std::optional { Wire::ErrorCode::NodeIdentityRequired });
        CHECK(leader.cluster.proposed.size() == 1);
    }
}
