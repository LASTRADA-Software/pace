// SPDX-License-Identifier: Apache-2.0
#include "BudgetOverrideApplier.hpp"
#include "CacheTier.hpp"
#include "McpUpstream.hpp"

#include <Pace/Config/ByteSize.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// One row per tier: the configuration field that sizes it -- the flag `BudgetTierTable` names.
    struct ConfiguredBudgetRow
    {
        StorageTier tier;                  ///< The tier.
        std::uint64_t NodeConfig::* field; ///< Its budget in the configuration.
    };

    /// Every tier, in enumerator order.
    constexpr EnumTable<StorageTier, ConfiguredBudgetRow> ConfiguredBudgetTable { {
        { .tier = StorageTier::Memory, .field = &NodeConfig::cacheMemoryBytes },
        { .tier = StorageTier::Disk, .field = &NodeConfig::cacheDiskBytes },
    } };
    static_assert(RowsInEnumeratorOrder(ConfiguredBudgetTable, &ConfiguredBudgetRow::tier),
                  "ConfiguredBudgetTable must hold one row per StorageTier, in enumerator order");

    /// @param state An applied state.
    /// @param id A member id, matched exactly.
    /// @return Its record, or null when the state records no such member.
    [[nodiscard]] Cluster::ClusterMember const* MemberOf(Cluster::ClusterState const& state, std::string_view id)
    {
        for (auto const& member: state.members)
            if (member.id == id)
                return &member;
        return nullptr;
    }

    /// A budget as a person reads it: a size, or `unbounded` for zero.
    /// @param bytes A budget.
    /// @return Its text.
    [[nodiscard]] std::string BudgetText(std::uint64_t bytes)
    {
        return bytes == 0 ? std::string { "unbounded" } : FormatByteSize(static_cast<std::size_t>(bytes));
    }

    /// Put one MEMBER-REPORT to the leader and read whether it took it.
    /// @param upstream The way to the leader.
    /// @param verb The report.
    /// @param value Its record.
    /// @return Nothing once accepted, or why not.
    [[nodiscard]] std::expected<void, std::string> Report(IMcpUpstream& upstream,
                                                          Wire::MemberReportVerb verb,
                                                          std::vector<std::byte> const& value)
    {
        auto const frame = Wire::EncodeMemberReport(Wire::MemberReportRequest { .verb = verb, .value = value });
        auto reply = upstream.Ask(UpstreamTarget::Leader, {}, std::string { Wire::AsStringView(frame) });
        if (!reply.has_value())
            return std::unexpected(
                std::format("{}: {}", UpstreamFailureRowOf(reply.error().first).name, reply.error().second));
        return {};
    }
} // namespace

OverrideAction DecideOverride(std::optional<Cluster::BudgetOverride> const& recorded,
                              std::uint64_t configured,
                              bool alreadyApplied) noexcept
{
    if (!recorded.has_value())
        return OverrideAction::None;
    if (!recorded->baseline.has_value())
        return OverrideAction::ReportBaselineAndApply;
    if (*recorded->baseline != configured)
        return OverrideAction::DropLocalNewer;
    return alreadyApplied ? OverrideAction::None : OverrideAction::Apply;
}

bool CacheTierBudgets::HasTier(StorageTier tier) const
{
    return _tier != nullptr && _tier->SnapshotTiers()[static_cast<std::size_t>(tier)].has_value();
}

std::expected<void, std::string> CacheTierBudgets::ApplyOverride(StorageTier tier, std::uint64_t bytes)
{
    if (_tier == nullptr)
        return std::unexpected(std::format("no {} tier on this node", TraitsFor(tier).name));
    if (!std::in_range<std::size_t>(bytes))
        return std::unexpected(std::format("a budget of {} bytes is more than this process can address", bytes));
    return _tier->ApplyTierBudget(tier, static_cast<std::size_t>(bytes));
}

std::expected<void, std::string> CacheTierBudgets::RestoreConfigured(StorageTier tier, std::uint64_t bytes)
{
    if (_tier == nullptr)
        return std::unexpected(std::format("no {} tier on this node", TraitsFor(tier).name));
    return _tier->RestoreTierBudget(tier, bytes);
}

std::expected<void, std::string> UpstreamMemberReportClient::RecordBaseline(StorageTier tier,
                                                                            std::uint64_t baseline,
                                                                            std::uint64_t overrideId)
{
    return Report(_upstream,
                  Wire::MemberReportVerb::RecordBaseline,
                  Wire::EncodeBudgetBaselineValue(Wire::BudgetBaselineValue {
                      .tier = static_cast<std::uint8_t>(tier), .baseline = baseline, .overrideId = overrideId }));
}

std::expected<void, std::string> UpstreamMemberReportClient::DropOverride(StorageTier tier, std::uint64_t overrideId)
{
    return Report(_upstream,
                  Wire::MemberReportVerb::DropOverride,
                  Wire::EncodeBudgetDropValue(
                      Wire::BudgetDropValue { .tier = static_cast<std::uint8_t>(tier), .overrideId = overrideId }));
}

BudgetOverrideApplier::BudgetOverrideApplier(ITierBudgets& tiers,
                                             IMemberReportClient& reports,
                                             INodeConfigSource const& config,
                                             ILogger& logger,
                                             NodeConditions* conditions,
                                             std::string selfId,
                                             ReconcileOn on):
    _tiers { tiers },
    _reports { reports },
    _config { config },
    _logger { logger },
    _conditions { conditions },
    _selfId { std::move(selfId) }
{
    // What the tiers were built with: the configuration in force as this is constructed, which is the one the cache
    // tier was started from -- no override can be in force before the first pass.
    auto const cfg = _config.Current();
    for (auto const& row: ConfiguredBudgetTable)
        _inForce[static_cast<std::size_t>(row.tier)] =
            InForce { .bytes = cfg.*row.field, .source = BudgetSource::Configured, .overrideId = 0 };

    // Answered at construction, so the row is never `Undecided` on a node that runs consensus: nothing has been dropped
    // until a pass finds a baseline the configuration has moved away from.
    if (_conditions != nullptr)
        _conditions->Clear(NodeCondition::CacheBudgetOverrideDropped);

    // The first pass runs with no state yet, so the status says what is configured from the first `node-status`.
    if (on == ReconcileOn::OwnThread)
        _thread = std::jthread { [this](std::stop_token const& stop) { Run(stop); } };
    else
        Reconcile();
}

BudgetOverrideApplier::~BudgetOverrideApplier()
{
    if (_thread.joinable())
    {
        _thread.request_stop();
        _thread.join();
    }
}

void BudgetOverrideApplier::OnState(Cluster::ClusterState const& state)
{
    {
        std::scoped_lock const lock { _mutex };
        _pending = state;
        _dirty = true;
    }
    _wake.notify_one();
}

void BudgetOverrideApplier::ReloadAccepted()
{
    {
        std::scoped_lock const lock { _mutex };
        _dirty = true;
    }
    _wake.notify_one();
}

void BudgetOverrideApplier::Run(std::stop_token const& stop)
{
    while (!stop.stop_requested())
    {
        Reconcile();
        std::unique_lock lock { _mutex };
        // A report still owed is asked again on the retry interval; otherwise only a state, a reload or the stop wakes.
        if (_retry)
            std::ignore = _wake.wait_for(lock, stop, BudgetReportRetryInterval, [this] { return _dirty; });
        else
            std::ignore = _wake.wait(lock, stop, [this] { return _dirty; });
    }
}

void BudgetOverrideApplier::Reconcile()
{
    std::scoped_lock const pass { _reconcileMutex };
    auto state = std::optional<Cluster::ClusterState> {};
    {
        std::scoped_lock const lock { _mutex };
        state = _pending;
        _dirty = false;
    }
    auto const cfg = _config.Current();
    auto const* member = state.has_value() ? MemberOf(*state, _selfId) : nullptr;

    auto status = std::vector<TierBudgetStatus> {};
    auto owed = false;
    for (auto const& row: ConfiguredBudgetTable)
    {
        auto const recorded =
            member != nullptr ? Cluster::BudgetOf(*member, row.tier) : std::optional<Cluster::BudgetOverride> {};
        auto [tierStatus, reportOwed] = ReconcileTier(row.tier, recorded, cfg.*row.field);
        owed = owed || reportOwed;
        // A tier this node lacks is reported only where the cluster records an override for it -- said, not hidden.
        if (tierStatus.effective.has_value() || tierStatus.recorded.has_value())
            status.push_back(std::move(tierStatus));
    }

    {
        std::scoped_lock const lock { _mutex };
        _retry = owed;
    }
    std::scoped_lock const lock { _statusMutex };
    _status = std::move(status);
}

std::pair<TierBudgetStatus, bool> BudgetOverrideApplier::ReconcileTier(
    StorageTier tier, std::optional<Cluster::BudgetOverride> const& recorded, std::uint64_t configured)
{
    auto status = TierBudgetStatus { .tier = tier, .configured = configured, .recorded = recorded };
    if (!_tiers.HasTier(tier))
    {
        if (recorded.has_value())
            status.note = std::format("not applied: this node has no {} tier", TraitsFor(tier).name);
        return { std::move(status), false };
    }

    // What the tier should run with, and whether a report is still owed: the configured value unless the override is
    // to be in force.
    auto target = InForce { .bytes = configured, .source = BudgetSource::Configured, .overrideId = 0 };
    auto owed = false;
    if (recorded.has_value())
    {
        auto const& over = *recorded;
        auto const& inForce = _inForce[static_cast<std::size_t>(tier)];
        auto const alreadyApplied = inForce.source == BudgetSource::Cluster && inForce.overrideId == over.id;
        auto const asOverride = InForce { .bytes = over.bytes, .source = BudgetSource::Cluster, .overrideId = over.id };
        switch (DecideOverride(recorded, configured, alreadyApplied))
        {
            case OverrideAction::None:
            case OverrideAction::Apply:
                target = asOverride;
                break;
            case OverrideAction::ReportBaselineAndApply:
                // Applied only once the leader has taken the baseline: an override applied against a baseline nobody
                // recorded could not be told from a local edit made meanwhile.
                if (auto const reported = _reports.RecordBaseline(tier, configured, over.id); reported.has_value())
                    target = asOverride;
                else
                {
                    owed = true;
                    status.note = std::format("not applied yet: this node's configured {} could not be reported to the "
                                              "leader as the override's baseline ({}); retrying",
                                              BudgetText(configured),
                                              reported.error());
                }
                break;
            case OverrideAction::DropLocalNewer: {
                auto const baseline = over.baseline.value_or(0);
                auto const dropped = DropLocalNewer(tier, over, baseline, configured);
                owed = !dropped.has_value();
                status.note = std::format(
                    "dropped: this node's configuration changed from {} to {} after the override "
                    "was set{}",
                    BudgetText(baseline),
                    BudgetText(configured),
                    dropped.has_value() ? std::string {}
                                        : std::format("; asking the leader to remove it ({}); retrying", dropped.error()));
                break;
            }
            case OverrideAction::Last:
                break;
        }
    }

    if (auto const enforced = Enforce(tier, target.bytes, target.source, target.overrideId); !enforced.has_value())
        status.note = std::format("{} {} could not be applied: {}",
                                  target.source == BudgetSource::Cluster ? "the override of" : "the configured",
                                  BudgetText(target.bytes),
                                  enforced.error());

    auto const& now = _inForce[static_cast<std::size_t>(tier)];
    status.effective = now.bytes;
    status.source = now.source;
    return { std::move(status), owed };
}

std::expected<void, std::string> BudgetOverrideApplier::DropLocalNewer(StorageTier tier,
                                                                       Cluster::BudgetOverride const& recorded,
                                                                       std::uint64_t baseline,
                                                                       std::uint64_t configured)
{
    // Said once per override, however many passes it takes the leader to remove it: a setback that repeats is said on
    // its transition, and the latched row keeps it visible after the line has scrolled away.
    if (_droppedSaid.insert(recorded.id).second)
    {
        auto const detail = std::format("{}: configured {} now, the override of {} was set against {}; this node's later "
                                        "edit wins and the override is dropped",
                                        TraitsFor(tier).name,
                                        BudgetText(configured),
                                        BudgetText(recorded.bytes),
                                        BudgetText(baseline));
        _logger.Logf(LogLevel::Warn, "cache budget override dropped -- {}", detail);
        if (_conditions != nullptr)
            _conditions->Raise(NodeCondition::CacheBudgetOverrideDropped, detail);
    }
    return _reports.DropOverride(tier, recorded.id);
}

std::expected<void, std::string> BudgetOverrideApplier::Enforce(StorageTier tier,
                                                                std::uint64_t bytes,
                                                                BudgetSource source,
                                                                std::uint64_t overrideId)
{
    auto& inForce = _inForce[static_cast<std::size_t>(tier)];
    if (inForce.bytes == bytes && inForce.source == source && inForce.overrideId == overrideId)
        return {};
    // The bytes on the tier are what matter; a change of source alone (an override equal to the configured value) moves
    // no budget.
    auto applied = std::expected<void, std::string> {};
    if (inForce.bytes != bytes)
        applied =
            source == BudgetSource::Cluster ? _tiers.ApplyOverride(tier, bytes) : _tiers.RestoreConfigured(tier, bytes);
    if (!applied.has_value())
    {
        // Said once per refused figure, never per pass: a tier refusing the same budget again is the same news.
        if (_failuresSaid.emplace(tier, applied.error()).second)
            _logger.Logf(LogLevel::Warn,
                         "cache budget of {} for the {} tier was not applied: {}",
                         BudgetText(bytes),
                         TraitsFor(tier).name,
                         applied.error());
        return applied;
    }
    inForce = InForce { .bytes = bytes, .source = source, .overrideId = overrideId };
    // A tier that took a budget again may refuse again later, and that is news once more.
    std::erase_if(_failuresSaid, [tier](auto const& said) { return said.first == tier; });
    _logger.Logf(LogLevel::Info,
                 "cache budget for the {} tier is now {}, from {}",
                 TraitsFor(tier).name,
                 BudgetText(bytes),
                 BudgetSourceName(source));
    return {};
}

std::vector<TierBudgetStatus> BudgetOverrideApplier::BudgetStatus() const
{
    std::scoped_lock const lock { _statusMutex };
    return _status;
}

} // namespace Pace::Node
