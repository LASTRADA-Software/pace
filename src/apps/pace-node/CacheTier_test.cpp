// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "LiveStatsSources.hpp"
#include "MachineBudget.hpp"
#include "MachineStandingTestUtils.hpp"
#include "NodeIoLoop.hpp"
#include "NodeProofClient.hpp"
#include "NodeStatusResponder.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheStatus.hpp"
#include "StatsSource.hpp"

#include <Pace/Core/Compression.hpp>
#include <Pace/Core/Ed25519.hpp>
#include <Pace/Core/ISecureRandom.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Distributed/MembershipOracle.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Metrics/StatsReadingCodec.hpp>
#include <Pace/Platform/LocalAddresses.hpp>
#include <Pace/Platform/LocalAddressesTestUtils.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Server/AdminHttpServer.hpp>
#include <Pace/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/HalfClose.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/ScriptedHostFacts.hpp>
#include <tests/SharedTierFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::StatusOf;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// Everything `StartCacheTierOrExplain` needs, over one clock.
///
/// The reactor is declared LAST, so among the members it is destroyed FIRST -- and
/// that is fine here only because no member holds a tier. A case keeps its tier in
/// a local declared AFTER the fixture, which is therefore destroyed BEFORE it, so
/// the tier's destructor posts its listener close onto a reactor that is still
/// there. Reversing the two is not a compile error and would present as a close
/// posted onto a reactor that has gone.
struct Fixture
{
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    CapturingLogger logger;

    /// A machine with no addresses of its own, which is not a limitation here: the
    /// tier is dialled over loopback, and loopback is answered before the set is
    /// consulted at all. Cases about WHO is admitted live in `CacheProxy_test`,
    /// where the peer address is an argument rather than whatever the kernel says.
    Testing::ScriptedHostAddresses hostAddresses;
    CachedLocalityOracle locality { hostAddresses, clock };
    NodeIoLoop io;

    /// A config bound to a port found at run time, so cases never collide.
    ///
    /// Not a number somebody picked: `catch_discover_tests` runs every case as its
    /// own process and the suite runs in parallel, so a fixed port is a failure
    /// that appears only under `ctest -j`. And not `0` either -- `ParseTcpPort`
    /// rejects it, correctly, since as a CLI value it names no port an operator
    /// could dial -- so a probe is bound and released to find one.
    [[nodiscard]] static NodeConfig BaseConfig()
    {
        auto probe = BlockingListener::Bind("127.0.0.1", 0);
        REQUIRE(probe);
        // Asked of the SOCKET, not of the pointer: `Bind` returns a listener in an
        // errored state rather than nothing, so a null check passes on a bind that
        // failed and the port below comes back 0.
        REQUIRE(probe->IsBound());
        auto const port = probe->boundPort();
        probe.reset();

        NodeConfig cfg;
        cfg.nodeListen = std::format("127.0.0.1:{}", port);
        return cfg;
    }

    /// Start a tier over @p cfg.
    /// @param cfg What the node was told to be.
    /// @return The tier, a null tier meaning "carry on without one", or the reason.
    /// What the tier's upstream client presents. These cases are about WHICH tier is
    /// built and what it reports, never about the credential -- so this is production's
    /// own "no configuration file, no secret" source rather than a fake.
    NodeConfig unauthenticated;
    ConfiguredCredential credential { unauthenticated, nullptr };
    /// Where the fleet's setting points: nowhere, and these nodes hold no identity to prove with, so
    /// the upstream is `--upstream`'s or none -- the cases here are about the TIER.
    SharedCacheDirectory directory { "pc-7", {} };

    [[nodiscard]] std::expected<std::unique_ptr<CacheTier>, NodeRefusal> Start(NodeConfig const& cfg)
    {
        UpstreamParts const parts { .upstream = cfg.upstream,
                                    .credential = credential,
                                    .directory = directory,
                                    .prover = nullptr,
                                    .io = io,
                                    .clock = clock,
                                    .metrics = metrics,
                                    .conditions = nullptr,
                                    .logger = logger,
                                    .host = nullptr };
        return StartCacheTierOrExplain(cfg, parts, locality, clock, metrics, logger);
    }
};

/// A trust that admits no server. The upstream never asks it -- the shared cache's dialer passes a
/// trust naming the one machine per operation -- so what it answers is never read.
class RefusingTrust final: public IServerTrust
{
  public:
    [[nodiscard]] ServerStanding StandingOf(std::string_view /*serverId*/,
                                            Ed25519PublicKey const& /*serverKey*/) const override
    {
        return ServerStanding::NotVoter;
    }

    [[nodiscard]] std::string_view Expected() const override
    {
        return "nobody";
    }
};

/// Everything `MakeCacheUpstream` chooses between, for a node `pc-7` with an identity and a host.
struct UpstreamFixture
{
    NodeIoLoop io;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    NodeConfig unauthenticated;
    ConfiguredCredential credential { unauthenticated, nullptr };
    SharedCacheDirectory directory { "pc-7", {} };
    Ed25519KeyPair key = Testing::TestKeyPair("pc-7");
    RefusingTrust trust;
    SystemSecureRandom random;
    NodeProofClient prover { "pc-7", key, trust, nullptr, nullptr, random };
    Testing::MemoryOpener opener;
    SharedCacheHost host { "pc-7", opener, nullptr, logger, ReconcileOn::Caller };

    /// @param upstream `--upstream`, or empty.
    /// @return The parts, with this node's identity and host.
    [[nodiscard]] UpstreamParts Parts(std::string_view upstream)
    {
        return UpstreamParts { .upstream = upstream,
                               .credential = credential,
                               .directory = directory,
                               .prover = &prover,
                               .io = io,
                               .clock = clock,
                               .metrics = metrics,
                               .conditions = nullptr,
                               .logger = logger,
                               .host = &host };
    }

    /// Both the directory and the host apply a state naming THIS node, and the host opens its tier.
    void NameThisMachine()
    {
        auto const state = Testing::NamingSharedCache("pc-7");
        directory.Applied(state);
        host.Applied(state);
        host.Reconcile();
    }
};

/// Whether any captured line contains @p needle.
/// @param logger Where the tier reported.
/// @param needle Text to look for.
/// @return True when some line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view needle)
{
    auto const records = logger.Snapshot();
    return std::ranges::any_of(records, [needle](CapturingLogger::Record const& r) { return r.message.contains(needle); });
}

/// One tier's entry, or nullopt when the cache has no such tier.
///
/// A template over the table, because the two this file reads -- `SnapshotTiers()`'s
/// statistics and `CacheCapacityOf`'s budgets -- are both an `EnumTable` keyed by
/// tier, and two helpers with one body is two things to keep in step.
/// @param tiers Any per-tier table.
/// @param tier Which tier to read.
/// @return That tier's entry.
template <typename Table>
[[nodiscard]] auto const& At(Table const& tiers, StorageTier tier)
{
    return tiers[static_cast<std::size_t>(tier)];
}

/// A payload that compresses hugely, so a codec effect cannot be mistaken for noise.
/// @param bytes How long.
/// @return That many identical bytes.
[[nodiscard]] std::vector<std::byte> Compressible(std::size_t bytes)
{
    return std::vector<std::byte>(bytes, std::byte { 0x41 });
}

/// Write all of @p bytes.
/// @param socket Where; must outlive the task.
/// @param bytes What; must outlive the task.
/// @return Whether every byte was taken.
[[nodiscard]] core::async::Task<bool> WriteWhole(core::net::ISocket* socket, std::span<std::byte const> bytes)
{
    auto const written = co_await socket->write(bytes);
    co_return written.has_value() && *written == bytes.size();
}

/// Everything @p socket delivers until its peer has finished sending.
/// @param socket Where; must outlive the task, and its peer must already be closed.
/// @return The bytes, as text.
[[nodiscard]] core::async::Task<std::string> ReadToEnd(core::net::ISocket* socket)
{
    std::string text;
    std::vector<std::byte> chunk(64 * 1024);
    while (true)
    {
        auto const read = co_await socket->read(chunk);
        if (!read.has_value() || *read == 0)
            co_return text;
        for (auto const byte: std::span<std::byte const> { chunk }.first(*read))
            text.push_back(static_cast<char>(byte));
    }
}

/// The two doors a node's figures leave by -- `/metrics` and `NodeMetrics` -- assembled over ONE
/// snapshot provider the way `main.cpp` assembles them: the provider `MakeNodeSnapshotProvider`
/// builds, handed to the admin route and, through the live-stats sources, to the responder.
///
/// Over a scripted machine and held load counters, so the two doors asked in turn describe one
/// machine: the system's figures move between two reads, which would be a difference neither door
/// has. The tier is the case's own, started by `StartCacheTierOrExplain`.
class MetricsDoors
{
  public:
    /// @param tier The node's cache; must outlive this.
    /// @param metrics The node's counters; must outlive this.
    MetricsDoors(CacheTier const& tier, IMetricsSink& metrics):
        _metrics { metrics },
        _provider { MakeNodeSnapshotProvider(NodeScrapeSources { .host = &_host,
                                                                 .load = &_load,
                                                                 .busySlots = [] { return std::size_t { 2 }; },
                                                                 .cache = &tier,
                                                                 .slots = 4,
                                                                 .scratchRoot = {},
                                                                 .consensus = {} },
                                             std::chrono::steady_clock::now()) },
        _sources { NodeLiveStatsParts { .metrics = &metrics,
                                        .snapshot = _provider,
                                        .identity = &_identity,
                                        .fleet = {},
                                        .history = nullptr,
                                        .endpoint = {},
                                        .surfaces = _served } },
        _responder { _identity, _sources, _membership, _standing, metrics }
    {
    }

    MetricsDoors(MetricsDoors const&) = delete;
    MetricsDoors(MetricsDoors&&) = delete;
    MetricsDoors& operator=(MetricsDoors const&) = delete;
    MetricsDoors& operator=(MetricsDoors&&) = delete;
    ~MetricsDoors() = default;

    /// The body `GET /metrics` is served, through the admin surface's own request handler.
    /// @return The Prometheus text, headers stripped.
    [[nodiscard]] std::string MetricsRoute()
    {
        auto pair = core::net::testing::InMemorySocketPair::create();
        auto const request = std::string_view { "GET /metrics HTTP/1.1\r\n\r\n" };
        REQUIRE(core::async::syncRun(WriteWhole(pair.client.get(), Wire::AsBytes(request))));
        REQUIRE(Pace::Testing::ShutdownWrite(*pair.client).has_value());
        core::platform::SteadyClock clock;
        // `_served`, the SAME set the `node-metrics` door reads through `_sources` -- which is
        // the whole point of this fixture. The two doors are meant to differ in nothing but how
        // they are asked, so a surface set either one owns privately would make them disagree
        // about which rows are absent and read as a figure one door drops (#1484, #1501).
        core::async::syncRun(ServeAdminHttp(pair.server.get(), &_metrics, _provider, &clock, {}, _served.Span()));
        pair.server->close();

        auto const response = core::async::syncRun(ReadToEnd(pair.client.get()));
        REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
        auto const bodyAt = response.find("\r\n\r\n");
        REQUIRE(bodyAt != std::string::npos);
        return response.substr(bodyAt + 4);
    }

    /// The reading `NodeMetrics` answers, as the bytes after the reply header.
    /// @return The reply body.
    [[nodiscard]] std::vector<std::byte> NodeMetricsBody()
    {
        auto const reply =
            core::async::syncRun(_responder.Answer(Wire::EncodeNodeMetricsRequest(), PeerIdentity { .host = "127.0.0.1" }))
                .bytes;
        REQUIRE(StatusOf(reply) == Wire::Status::Ok);
        auto const body = Testing::PayloadOf(reply);
        return { body.begin(), body.end() };
    }

  private:
    Testing::ScriptedHostFacts _host;
    Testing::FixedHostCounters _load;
    /// Nothing reads it: `NodeMetrics` answers the cache subject, which is the counters and the
    /// snapshot only.
    Testing::SilentNodeStatus _identity;
    Distributed::OpenMembership _membership;
    IMetricsSink& _metrics;
    AdminHttpServer::SnapshotProvider _provider;
    /// One surface set for BOTH doors, declared before `_sources`, which copies it.
    ///
    /// Left at the default -- every surface -- because this fixture is about whether the two
    /// doors agree, not about which rows a particular node can write. Narrowing it here would
    /// make the case assert absences as well, which is `NodeStatusResponder_test`'s subject.
    ServedSurfaces _served {};
    NodeLiveStatsSources _sources;
    /// No roster: this fixture asks no machine question.
    Testing::FixedStanding _standing {};
    NodeStatusResponder _responder;
};

} // namespace

TEST_CASE("A node told to hold nothing serves no cache tier", "[node][cache-tier]")
{
    // `--cache-memory 0` used to build an InMemoryLruStorage with a zero budget,
    // which is how that class spells UNBOUNDED -- so the one flag an operator can
    // switch this off with was the one that removed the limit, and a node told to
    // hold nothing grew until the machine ran out of memory. Silent throughout:
    // the cache works, and works better and better.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    CHECK(*started == nullptr);
}

TEST_CASE("A node with no cache port serves no cache tier", "[node][cache-tier]")
{
    // `--listen-node=` is the documented way to turn the local cache off, and it
    // turns it off WITHOUT touching `--cache-memory` -- whose default is a share of
    // host RAM, so it is left non-zero here rather than relying on the runner's.
    // That pairing, a budget asked for and no tier built, is what made sizing the
    // machine's reservation from the flag reserve a quarter of RAM for nothing
    // (#167).
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.nodeListen.clear();
    cfg.cacheMemoryBytes = 8ULL * 1024 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier == nullptr);

    // Absent, not zero, and all the way down: this is the record `NodeCapacityOf`
    // derives the reservation from, so a memory tier reported here for a tier that
    // was never built is memory the fleet would never get back.
    auto const budget = CacheCapacityOf(tier.get());
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Memory).has_value());
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Disk).has_value());

    // And it says so. This was the one route to "no cache tier" that logged
    // nothing at all, so an operator had no line anywhere telling them this node
    // was not caching.
    CHECK(Logged(fixture.logger, "serving no local cache tier"));
}

TEST_CASE("A cache budget with no port to serve it is called out", "[node][cache-tier]")
{
    // `--cache-memory` and `--cache-dir` do nothing at all without a port, and a
    // flag silently doing nothing is the shape this codebase keeps a list about.
    // Named only when the operator actually set one -- a default nobody typed is
    // not something to warn them about.
    Testing::ScratchDirectory const scratch { "node-cache-no-port" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.nodeListen.clear();
    cfg.cacheDir = scratch.Path();

    auto const started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    CHECK(*started == nullptr);
    CHECK(Logged(fixture.logger, "--cache-memory/--cache-dir have no effect"));
}

TEST_CASE("A memory-only node reports its budget and no disk tier", "[node][cache-tier]")
{
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
    // Absent rather than a disk tier holding nothing: an operator who did not ask
    // for `--cache-dir` has no disk tier to be empty.
    CHECK_FALSE(At(tiers, StorageTier::Disk).has_value());

    // And the announced record says the same, because that is the one
    // `NodeCapacityOf` subtracts from this machine's RAM (#167). Asserted here
    // rather than in a case of its own: `CacheCapacityOf` is a projection of the
    // snapshot above, and this file's fixture costs a process and a port probe.
    auto const budget = CacheCapacityOf(tier.get());
    REQUIRE(At(budget.tierBytesLimit, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(budget.tierBytesLimit, StorageTier::Memory)) == cfg.cacheMemoryBytes);
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Disk).has_value());
}

// The name may not START with `--`: `catch_discover_tests` passes it to the
// runner as an argument, and Catch2 reads a leading double dash as a flag and
// exits with "Unrecognised token" -- which ctest reports as a failing test that
// passes when run by hand.
TEST_CASE("Naming --cache-dir gives the node an on-disk tier", "[node][cache-tier]")
{
    // The flag was parsed and round-tripped by the config writer and read by
    // nothing: `CacheTier::Start` built a bare in-memory store and never looked at
    // the path. A node configured with a disk cache had none, and said so nowhere.
    Testing::ScratchDirectory const scratch { "node-cache-dir" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 4 * 1024 * 1024;
    cfg.cacheDir = scratch.Path();

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    REQUIRE(At(tiers, StorageTier::Disk).has_value());
    // Distinct budgets, so a report that named one tier's figure under the other's
    // label could not pass: the memory half is capped and the disk half is not.
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).bytesLimit == 0);
}

TEST_CASE("A disk-only node keeps its disk tier", "[node][cache-tier]")
{
    // `--cache-memory 0 --cache-dir <path>` turns off the in-memory half and
    // nothing else. Reading it as "no cache at all" would silently discard a tier
    // the operator named a path for.
    Testing::ScratchDirectory const scratch { "node-cache-disk-only" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir = scratch.Path();

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    CHECK_FALSE(At(tiers, StorageTier::Memory).has_value());
    CHECK(At(tiers, StorageTier::Disk).has_value());
}

namespace
{

/// Bytes that no codec shrinks, so a disk tier's footprint grows by what the case wrote.
///
/// The leading byte is pinned outside `[1, MaxCompileValueGeneration]`, so the tier stores the
/// value verbatim: a random leading byte inside that range reads as a compile value of some
/// generation, and one this build does not implement is refused (#483).
/// @param bytes How long; at least one.
/// @param seed Varies the content between values.
/// @return That many bytes of a linear congruential sequence after the pinned first.
[[nodiscard]] std::vector<std::byte> Incompressible(std::size_t bytes, std::uint64_t seed)
{
    std::vector<std::byte> out(bytes);
    auto state = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    for (auto& byte: out)
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        byte = static_cast<std::byte>(state >> 56U);
    }
    out.front() = std::byte { 0x5A };
    return out;
}

/// What the tier's STORE verb answers, over the loopback peer every cache verb admits.
/// @param tier The tier.
/// @param frame The request.
/// @return The reply's status, or nullopt when the reply carried none.
[[nodiscard]] std::optional<Wire::Status> Ask(CacheTier& tier, std::vector<std::byte> const& frame)
{
    return StatusOf(core::async::syncRun(tier.Responder().Answer(frame, PeerIdentity { .host = "127.0.0.1" })).bytes);
}

/// A STORE request for @p key carrying @p value.
/// @param key The object key.
/// @param value The object.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> StoreFrame(std::string const& key, std::vector<std::byte> value)
{
    return Wire::EncodeStore(Wire::StoreRequest {
        .key = key, .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = std::move(value) });
}

} // namespace

TEST_CASE("zero refused as a live tier budget", "[node][cache-tier][resize]")
{
    // Zero is how both tiers spell UNBOUNDED, and `--cache-memory 0` how a node builds no
    // memory tier: a live store can do neither safely, so the override is refused by name.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const applied = tier->ApplyTierBudget(StorageTier::Memory, 0);
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error() == TierOverrideCannotRemoveTier);
    // And nothing moved.
    CHECK(Unwrap(At(tier->SnapshotTiers(), StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
}

TEST_CASE("absent tier refused as a live tier budget", "[node][cache-tier][resize]")
{
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const applied = tier->ApplyTierBudget(StorageTier::Disk, 64ULL * 1024 * 1024);
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error() == "no disk tier on this node");
    // Still no disk tier: refusing is not creating one.
    CHECK_FALSE(At(tier->SnapshotTiers(), StorageTier::Disk).has_value());
    CHECK(Unwrap(At(tier->SnapshotTiers(), StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
}

TEST_CASE("capacity recomputed after shrink", "[node][cache-tier][resize][capacity]")
{
    // What a node holds back from compiles is what its tier HOLDS (#167), so a shrink must
    // move the reserve, and through it the machine budget the presence loop reports next.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    constexpr std::uint64_t Before = 64ULL * 1024 * 1024;
    constexpr std::uint64_t After = 16ULL * 1024 * 1024;
    cfg.cacheMemoryBytes = Before;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);
    Testing::ScriptedHostFacts const host;
    LiveNodeCapacity live { cfg, host, tier.get() };

    auto const was = live.Current();
    CHECK(Unwrap(At(was.cache.tierBytesLimit, StorageTier::Memory)) == Before);
    auto const wasBudget = MachineBudgetOf(cfg, was, HeldCharges {});

    REQUIRE(tier->ApplyTierBudget(StorageTier::Memory, After).has_value());

    auto const now = live.Current();
    CHECK(Unwrap(At(now.cache.tierBytesLimit, StorageTier::Memory)) == After);
    // The same derivation the start makes, over the tier as it now stands -- not a patch of
    // the old figure by the difference.
    auto const rederived = NodeCapacityOf(cfg, host, CacheCapacityOf(tier.get()), IndexReserveBytesOf(tier.get()));
    CHECK(now.reservedMemoryBytes == rederived.reservedMemoryBytes);
    CHECK(now.cache.tierBytesLimit == rederived.cache.tierBytesLimit);
    CHECK(was.reservedMemoryBytes - now.reservedMemoryBytes == Before - After);
    auto const nowBudget = MachineBudgetOf(cfg, now, HeldCharges {});
    auto const usable = [&host](Distributed::NodeCapacity const& c) {
        return (host.TotalMemoryBytes() - c.reservedMemoryBytes) * MachineBudgetSharePercent / 100;
    };
    CHECK(wasBudget.memoryBudgetBytes == usable(was));
    CHECK(nowBudget.memoryBudgetBytes == usable(now));
    CHECK(nowBudget.memoryBudgetBytes > wasBudget.memoryBudgetBytes);

    // A refused apply tells nobody: the capacity stays what the tier holds.
    REQUIRE_FALSE(tier->ApplyTierBudget(StorageTier::Disk, After).has_value());
    CHECK(live.Current().reservedMemoryBytes == now.reservedMemoryBytes);
    CHECK(live.Current().cache.tierBytesLimit == now.cache.tierBytesLimit);
}

namespace
{
/// The concurrent-shrink cases' shape: objects seeded before the shrink, cache verbs on the
/// traffic thread, readings on the reader thread, applies on the shrinking thread, and the size
/// of every object -- each larger than a disk page, so every eviction frees pages.
constexpr int ShrinkSeeded = 96;
constexpr int ShrinkTrafficOps = 192;
constexpr int ShrinkReads = 192;
constexpr int ShrinkApplies = 32;
constexpr std::size_t ShrinkValueBytes = 24 * 1024;

/// @param tier The tier.
/// @param tierKind Which of its tiers.
/// @return How far that tier sits above its budget, through the one derivation every surface uses.
[[nodiscard]] std::size_t OverBudget(CacheTier const& tier, StorageTier tierKind)
{
    return IStorage::BytesOverBudget(At(tier.SnapshotTiers(), tierKind));
}

/// A store a driver test steps directly: the production composition (`OpenDiskStore` under
/// `ShareStorage`), seeded with objects each larger than a page, so every eviction frees pages.
/// @param directory Where the store lives.
/// @param logger Where the store reports.
/// @param objects How many objects to seed.
/// @return The store.
[[nodiscard]] std::unique_ptr<IStorage> SeededDiskStore(std::filesystem::path const& directory, ILogger& logger, int objects)
{
    auto disk = OpenDiskStore(DiskStoreSpec { .path = directory / DiskStoreFileName,
                                              .maxBytes = 0,
                                              .codec = CompressionCodec::Identity,
                                              .level = 0,
                                              .minBytes = 0,
                                              .inUse = "a test store is held already" },
                              logger);
    REQUIRE(disk.has_value());
    auto storage = ShareStorage(std::move(*disk), logger);
    for (auto const i: std::views::iota(0, objects))
        REQUIRE(storage
                    ->Set(std::format("k-{:04}", i),
                          Incompressible(ShrinkValueBytes, static_cast<std::uint64_t>(i)),
                          0,
                          core::platform::SteadyTimePoint::max())
                    .has_value());
    return storage;
}
} // namespace

TEST_CASE("A disk tier shrunk live keeps serving and converges in the background", "[node][cache-tier][resize]")
{
    Testing::ScratchDirectory const scratch { "node-cache-disk-shrink" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir = scratch.Path();
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    constexpr int Objects = 64;
    for (auto const i: std::views::iota(0, Objects))
        REQUIRE(Ask(*tier, StoreFrame(std::format("obj-{:03}", i), Incompressible(32 * 1024, static_cast<std::uint64_t>(i))))
                == Wire::Status::Ok);
    auto const held = Unwrap(At(tier->SnapshotTiers(), StorageTier::Disk)).bytesUsed;
    auto const budget = held / 4;

    REQUIRE(tier->ApplyTierBudget(StorageTier::Disk, budget).has_value());
    CHECK(Unwrap(At(tier->SnapshotTiers(), StorageTier::Disk)).bytesLimit == budget);
    // The tier's own driver carries it down, on its own thread; nothing here steps it.
    REQUIRE(Testing::WaitUntil(
        "the eviction driver brings the disk tier to its budget",
        [&tier] { return OverBudget(*tier, StorageTier::Disk) == 0 && tier->Eviction().Convergences() >= 1; },
        [&tier] {
            return std::format(
                "{} byte(s) over, {} slice(s)", OverBudget(*tier, StorageTier::Disk), tier->Eviction().Slices());
        }));
    CHECK(Unwrap(At(tier->SnapshotTiers(), StorageTier::Disk)).evictions > 0U);
    // The newest object is the warmest, and still served.
    CHECK(Ask(*tier, Wire::EncodeFetch(std::format("obj-{:03}", Objects - 1))) == Wire::Status::Ok);
}

TEST_CASE("The eviction driver releases the store between slices, so traffic completes during a shrink",
          "[node][cache-tier][resize][concurrency]")
{
    // The property the driver exists for: a shrink by most of the store never holds the store's
    // lock for the drain. Proved by construction rather than by timing -- between every two
    // slices the driver waits until a cache op on THIS thread has completed, which it can only
    // do with the lock released. A driver that held it across slices would never reach a second
    // slice boundary, or would wait out the bound with no op ever completing.
    Testing::ScratchDirectory const scratch { "node-eviction-interleave" };
    NullLogger logger;
    constexpr int Objects = 400;
    auto const storage = SeededDiskStore(scratch.Path(), logger, Objects);
    auto const budget = Unwrap(At(storage->SnapshotTiers(), StorageTier::Disk)).bytesUsed / 4;
    auto const resized = storage->ResizeTier(StorageTier::Disk, budget);
    REQUIRE(resized.has_value());
    REQUIRE(Unwrap(resized) > 0U);
    // The resize evicted nothing: the bulk is the driver's.
    CHECK(Unwrap(At(storage->SnapshotTiers(), StorageTier::Disk)).evictions == 0U);

    core::platform::ManualClock clock;
    std::atomic<std::uint64_t> trafficOps { 0 };
    std::atomic<std::uint64_t> boundaries { 0 };
    std::atomic<bool> converged { false };
    Testing::OffThreadWaits waits;
    auto pacing = DefaultEvictionPacing();
    pacing.betweenSlices = [&trafficOps, &boundaries, &waits](std::stop_token const& /*stop*/) {
        auto const seen = trafficOps.load(std::memory_order_acquire);
        boundaries.fetch_add(1, std::memory_order_acq_rel);
        (void) waits.Wait(
            "a cache op completing between two eviction slices",
            [&trafficOps, seen] { return trafficOps.load(std::memory_order_acquire) > seen; },
            [&trafficOps] { return std::format("{} op(s)", trafficOps.load(std::memory_order_acquire)); });
    };
    {
        TierEvictionDriver driver { *storage, clock, logger, [&converged] { converged.store(true); }, std::move(pacing) };
        driver.Kick();
        std::uint64_t next = 0;
        Testing::WaitOptions traffic;
        traffic.step = [&storage, &clock, &trafficOps, &next] {
            auto const key = std::format("k-{:04}", next % Objects);
            ++next;
            if (storage->Get(key, clock.now()).has_value())
                trafficOps.fetch_add(1, std::memory_order_acq_rel);
        };
        REQUIRE(Testing::WaitUntil(
            "the driver brings the store to its budget",
            [&converged] { return converged.load(); },
            [&driver, &trafficOps] { return std::format("{} slice(s), {} op(s)", driver.Slices(), trafficOps.load()); },
            traffic));
    }
    CHECK(waits.AllReached());
    // Several slices, and an op completed at every boundary between them.
    CHECK(boundaries.load() >= 2U);
    CHECK(trafficOps.load() >= boundaries.load());
    CHECK(IStorage::BytesOverBudget(At(storage->SnapshotTiers(), StorageTier::Disk)) == 0U);
}

TEST_CASE("An eviction driver stopped mid-shrink stops at the slice boundary and leaves a usable store",
          "[node][cache-tier][resize][concurrency]")
{
    // Parked between two slices of a run that has not converged -- deterministic, since the
    // hook waits for nothing but the stop -- and then destroyed: the stop must end the wait and
    // the join must return, with the store still serving and still able to converge.
    Testing::ScratchDirectory const scratch { "node-eviction-stop" };
    NullLogger logger;
    constexpr int Objects = 400;
    auto const storage = SeededDiskStore(scratch.Path(), logger, Objects);
    auto const budget = Unwrap(At(storage->SnapshotTiers(), StorageTier::Disk)).bytesUsed / 4;
    REQUIRE(storage->ResizeTier(StorageTier::Disk, budget).has_value());

    core::platform::ManualClock clock;
    std::atomic<bool> parked { false };
    auto pacing = DefaultEvictionPacing();
    pacing.betweenSlices = [&parked](std::stop_token const& stop) {
        parked.store(true, std::memory_order_release);
        std::mutex mutex;
        std::condition_variable_any stopped;
        std::unique_lock lock { mutex };
        (void) stopped.wait(lock, stop, [] { return false; });
    };
    std::uint64_t slices = 0;
    std::uint64_t convergences = 0;
    {
        TierEvictionDriver driver { *storage, clock, logger, {}, std::move(pacing) };
        driver.Kick();
        REQUIRE(Testing::WaitUntil(
            "the driver parked between two slices",
            [&parked] { return parked.load(std::memory_order_acquire); },
            [] { return std::string { "not yet" }; }));
        slices = driver.Slices();
        convergences = driver.Convergences();
    }
    CHECK(slices == 1U);
    CHECK(convergences == 0U);
    // Still over, and still a working store: the rest goes slice by slice on this thread.
    CHECK(IStorage::BytesOverBudget(At(storage->SnapshotTiers(), StorageTier::Disk)) > 0U);
    auto step = storage->EvictStep();
    for ([[maybe_unused]] auto const more: std::views::iota(0, Objects))
    {
        if (step.bytesOverBudget == 0 || step.evicted == 0)
            break;
        step = storage->EvictStep();
    }
    CHECK(step.bytesOverBudget == 0U);
    auto const newest = storage->Get(std::format("k-{:04}", Objects - 1), clock.now());
    REQUIRE(newest.has_value());
    CHECK(newest->found);
}

TEST_CASE("An eviction step that throws is reported and the driver keeps working", "[node][cache-tier][resize]")
{
    // An exception escaping the driver's thread would end the node. A run that throws is caught at
    // the loop, said once at Warn, counted -- and the next kick does the work.
    Testing::ScratchDirectory const scratch { "node-eviction-throw" };
    NullLogger storeLogger;
    constexpr int Objects = 64;
    std::function<void()> hook = [] {
        throw std::runtime_error { "injected eviction fault" };
    };
    Testing::HookedStorage storage { SeededDiskStore(scratch.Path(), storeLogger, Objects), hook };
    auto const budget = Unwrap(At(storage.SnapshotTiers(), StorageTier::Disk)).bytesUsed / 4;
    REQUIRE(storage.ResizeTier(StorageTier::Disk, budget).has_value());

    core::platform::ManualClock clock;
    CapturingLogger logger;
    TierEvictionDriver driver { storage, clock, logger, {}, DefaultEvictionPacing() };
    driver.Kick();
    REQUIRE(Testing::WaitUntil(
        "the failed run reported",
        [&driver] { return driver.FailedRuns() >= 1; },
        [&driver] { return std::format("{} failed run(s), {} slice(s)", driver.FailedRuns(), driver.Slices()); }));
    CHECK(Logged(logger, "injected eviction fault"));
    CHECK(Logged(logger, "the next kick retries"));
    CHECK(IStorage::BytesOverBudget(At(storage.SnapshotTiers(), StorageTier::Disk)) > 0U);

    // The hook is spent; the next kick converges.
    driver.Kick();
    REQUIRE(Testing::WaitUntil(
        "the next kick brings the store to its budget",
        [&driver] { return driver.Convergences() >= 1; },
        [&driver] { return std::format("{} convergence(s), {} slice(s)", driver.Convergences(), driver.Slices()); }));
    CHECK(driver.FailedRuns() == 1U);
    CHECK(IStorage::BytesOverBudget(At(storage.SnapshotTiers(), StorageTier::Disk)) == 0U);
}

TEST_CASE("A budget listener is told once as it registers", "[node][cache-tier][resize][capacity]")
{
    // So a drain the tier's startup kick finished before anybody listened is not lost: registering
    // derives, under the same lock as every later notification.
    class CountingListener final: public ITierBudgetListener
    {
      public:
        void TierBudgetApplied() override
        {
            ++told;
        }
        int told = 0;
    };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    CountingListener listener;
    tier->SetBudgetListener(&listener);
    CHECK(listener.told == 1);
    REQUIRE(tier->ApplyTierBudget(StorageTier::Memory, 4 * 1024 * 1024).has_value());
    CHECK(listener.told == 2);
    tier->SetBudgetListener(nullptr);
    REQUIRE(tier->ApplyTierBudget(StorageTier::Memory, 2 * 1024 * 1024).has_value());
    CHECK(listener.told == 2);
}

TEST_CASE("concurrent cache traffic during disk shrink", "[node][cache-tier][resize][concurrency]")
{
    // The production threads, each on its own: cache verbs (the reactor), the override being
    // applied (the admin surface), the tier's own eviction driver, and the readers of what the
    // tiers hold (the heartbeat, the presence loop, node-status). Both tiers are shrunk. Run under
    // TSan by `scripts/tsan-gate.sh`, whose pace-node-tests row selects every case.
    Testing::ScratchDirectory const scratch { "node-cache-concurrent-shrink" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 1024 * 1024;
    cfg.cacheDir = scratch.Path();
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);
    Testing::ScriptedHostFacts const host;
    LiveNodeCapacity live { cfg, host, tier.get() };

    for (auto const i: std::views::iota(0, ShrinkSeeded))
        REQUIRE(
            Ask(*tier,
                StoreFrame(std::format("seed-{:03}", i), Incompressible(ShrinkValueBytes, static_cast<std::uint64_t>(i))))
            == Wire::Status::Ok);
    auto const budget = Unwrap(At(tier->SnapshotTiers(), StorageTier::Disk)).bytesUsed / 3;
    constexpr std::size_t MemoryBudget = 256 * 1024;

    // Each thread RETURNS what it saw; every assertion runs on this thread after the joins.
    struct Traffic
    {
        int answered = 0;
        int failed = 0;
    };
    struct Shrink
    {
        bool diskApplied = false;
        bool memoryApplied = false;
    };
    struct Readings
    {
        int taken = 0;
        int inconsistent = 0;
    };
    Traffic traffic;
    Shrink shrink;
    Readings readings;
    {
        std::jthread const writer { [&tier, &traffic] {
            for (auto const i: std::views::iota(0, ShrinkTrafficOps))
            {
                auto const status =
                    i % 2 == 0 ? Ask(*tier,
                                     StoreFrame(std::format("new-{:03}", i),
                                                Incompressible(ShrinkValueBytes, 1000U + static_cast<std::uint64_t>(i))))
                               : Ask(*tier, Wire::EncodeFetch(std::format("seed-{:03}", i % ShrinkSeeded)));
                if (status == Wire::Status::Ok || status == Wire::Status::Miss)
                    ++traffic.answered;
                else
                    ++traffic.failed;
            }
        } };
        std::jthread const shrinker { [&tier, &shrink, budget] {
            shrink.diskApplied = tier->ApplyTierBudget(StorageTier::Disk, budget).has_value();
            shrink.memoryApplied = tier->ApplyTierBudget(StorageTier::Memory, MemoryBudget).has_value();
        } };
        std::jthread const reader { [&tier, &live, &readings] {
            for ([[maybe_unused]] auto const read: std::views::iota(0, ShrinkReads))
            {
                auto const capacity = live.Current();
                auto const tiers = tier->SnapshotTiers();
                auto const budgets = CacheCapacityOf(tier.get());
                (void) IndexReserveBytesOf(tier.get());
                ++readings.taken;
                if (!At(budgets.tierBytesLimit, StorageTier::Disk).has_value() || !At(tiers, StorageTier::Disk).has_value()
                    || !At(capacity.cache.tierBytesLimit, StorageTier::Disk).has_value())
                    ++readings.inconsistent;
            }
        } };
    }

    CHECK(shrink.diskApplied);
    CHECK(shrink.memoryApplied);
    CHECK(traffic.failed == 0);
    CHECK(traffic.answered == ShrinkTrafficOps);
    CHECK(readings.taken == ShrinkReads);
    CHECK(readings.inconsistent == 0);
    // The driver converges once the writes stop; a kick per poll covers a run that ended early.
    Testing::WaitOptions kicking;
    kicking.step = [&tier] {
        tier->KickEviction();
    };
    REQUIRE(Testing::WaitUntil(
        "the disk tier back at its budget",
        [&tier] { return OverBudget(*tier, StorageTier::Disk) == 0; },
        [&tier] { return std::format("{} byte(s) over", OverBudget(*tier, StorageTier::Disk)); },
        kicking));
    auto const disk = Unwrap(At(tier->SnapshotTiers(), StorageTier::Disk));
    CHECK(disk.bytesLimit == budget);
    auto const memory = Unwrap(At(tier->SnapshotTiers(), StorageTier::Memory));
    CHECK(memory.bytesLimit == MemoryBudget);
    CHECK(memory.bytesUsed <= MemoryBudget);
    CHECK(Unwrap(At(live.Current().cache.tierBytesLimit, StorageTier::Disk)) == budget);
    CHECK(Unwrap(At(live.Current().cache.tierBytesLimit, StorageTier::Memory)) == MemoryBudget);
}

TEST_CASE("concurrent cache traffic during memory shrink", "[node][cache-tier][resize][concurrency]")
{
    // The memory tier's own path -- `InMemoryLruStorage::ResizeTier`, evict-to-fit under the
    // shard lock -- against cache verbs and readers on their own threads, the budget moved down
    // and up repeatedly so every apply evicts or grows while traffic runs.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    constexpr std::size_t Large = 2 * 1024 * 1024;
    constexpr std::size_t Small = 128 * 1024;
    cfg.cacheMemoryBytes = Large;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);
    Testing::ScriptedHostFacts const host;
    LiveNodeCapacity live { cfg, host, tier.get() };

    struct Traffic
    {
        int answered = 0;
        int failed = 0;
    };
    struct Shrink
    {
        int applied = 0;
    };
    struct Readings
    {
        int taken = 0;
        int overBudget = 0;
    };
    Traffic traffic;
    Shrink shrink;
    Readings readings;
    {
        std::jthread const writer { [&tier, &traffic] {
            for (auto const i: std::views::iota(0, ShrinkTrafficOps))
            {
                auto const status = i % 2 == 0 ? Ask(*tier,
                                                     StoreFrame(std::format("mem-{:03}", i),
                                                                Incompressible(16 * 1024, static_cast<std::uint64_t>(i))))
                                               : Ask(*tier, Wire::EncodeFetch(std::format("mem-{:03}", i - 1)));
                if (status == Wire::Status::Ok || status == Wire::Status::Miss)
                    ++traffic.answered;
                else
                    ++traffic.failed;
            }
        } };
        std::jthread const shrinker { [&tier, &shrink] {
            for (auto const apply: std::views::iota(0, ShrinkApplies))
                if (tier->ApplyTierBudget(StorageTier::Memory, apply % 2 == 0 ? Small : Large).has_value())
                    ++shrink.applied;
            // Last word: the small budget.
            if (tier->ApplyTierBudget(StorageTier::Memory, Small).has_value())
                ++shrink.applied;
        } };
        std::jthread const reader { [&tier, &live, &readings] {
            for ([[maybe_unused]] auto const read: std::views::iota(0, ShrinkReads))
            {
                (void) live.Current();
                (void) CacheCapacityOf(tier.get());
                // The memory tier fits whenever its lock is free: an in-memory resize evicts to fit
                // before it releases it, so no reader can catch it over.
                if (OverBudget(*tier, StorageTier::Memory) != 0)
                    ++readings.overBudget;
                ++readings.taken;
            }
        } };
    }

    CHECK(shrink.applied == ShrinkApplies + 1);
    CHECK(traffic.failed == 0);
    CHECK(traffic.answered == ShrinkTrafficOps);
    CHECK(readings.taken == ShrinkReads);
    CHECK(readings.overBudget == 0);
    auto const memory = Unwrap(At(tier->SnapshotTiers(), StorageTier::Memory));
    CHECK(memory.bytesLimit == Small);
    CHECK(memory.bytesUsed <= Small);
    CHECK(Unwrap(At(live.Current().cache.tierBytesLimit, StorageTier::Memory)) == Small);
}

TEST_CASE("A cache directory that cannot be opened is fatal when it was named", "[node][cache-tier]")
{
    // The same rule `--listen-node` follows: an address the operator TYPED is a
    // promise, and a broken promise stops startup rather than being logged and
    // carried on from. `--cache-dir` has no default at all, so naming it is the
    // only way to reach this path.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 1024 * 1024;
    // A path under a regular file, which no platform will create a store beneath.
    Testing::ScratchDirectory const scratch { "node-cache-bad-dir" };
    auto const blocker = scratch / "not-a-directory";
    {
        std::ofstream const file { blocker };
    }
    cfg.cacheDir = blocker / "cache";

    auto const started = fixture.Start(cfg);
    REQUIRE_FALSE(started.has_value());
    // And it names the flag that failed. Both failures this function can report
    // leave through one string, so a directory it could not create used to reach
    // the operator as "--listen-node ..." and send them to check a port.
    CHECK(started.error().reason.contains("--cache-dir"));
    CHECK_FALSE(started.error().reason.contains("--listen-node"));
    // A directory that could not be created may be creatable at the next start -- a volume not yet
    // mounted -- so the cache tier's refusals are restarted.
    CHECK(started.error().cause == NodeRefusalCause::CacheStore);
}

// The provenance rule -- a NAMED address is a promise and a broken promise is fatal,
// a DEFAULTED one is warned past (#286) -- moved to `NodeFrameSurface_test` with the
// bind itself when the cache and scheduler surfaces merged (#290). This tier opens no
// listener any more, so a case here could only have asserted it through a function
// that no longer decides it.

TEST_CASE("A tier says whether it has a shared cache behind it", "[node][cache-tier]")
{
    // #214's wiring, asserted rather than assumed. The scrape's
    // `upstream_configured` gauge is read off the upstream that was BUILT, so this
    // is the join between the configuration and what a scrape reports -- and a
    // field nothing populates renders a confident `0` for every node, which is the
    // failure mode the gauge exists to prevent.
    Fixture fixture;

    SECTION("none configured")
    {
        auto cfg = Fixture::BaseConfig();
        cfg.upstream.clear();

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);
        CHECK_FALSE(tier->HasUpstream());
    }

    SECTION("one configured")
    {
        // Named, not reachable -- and that is the distinction. `Configured()` says an
        // operator asked for a shared cache; whether it answers is what the store
        // counters report, and conflating the two is how a laptop came to look like
        // a fleet whose cache is down.
        auto cfg = Fixture::BaseConfig();
        cfg.upstream = "127.0.0.1:1";

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);
        CHECK(tier->HasUpstream());
    }
}

// --- compression --------------------------------------------------------------
//
// These cases exist because a compression setting that reaches no tier is INERT and
// indistinguishable, from every other surface, from one that works: the object still
// stores, still reads back byte-for-byte, and every counter moves exactly as before.
// So what is asserted is the DIFFERENCE between a tier told to compress and one told
// not to -- never the round trip alone, which passes under the bug.

TEST_CASE("MemoryCompressionOf carries what the node was told", "[node][cache-tier][compression]")
{
    NodeConfig cfg;
    cfg.memoryCompression = CompressionCodec::Zstd;
    cfg.memoryCompressionLevel = 9;
    cfg.memoryCompressionMinBytes = 1234;

    auto const options = MemoryCompressionOf(cfg);
    CHECK(options.level == 9);
    CHECK(options.minBytes == 1234);

    // The codec is the EFFECTIVE one, so this is not `== Zstd`: on a build without
    // `PACE_ENABLE_COMPRESSION` the tier stores plaintext and an assertion
    // naming the configured value would fail there for being right. The level and
    // the floor above are carried verbatim, which is why they are spelled as
    // literals and this is not.
    CHECK(options.codec == Compression::EffectiveCodec(CompressionCodec::Zstd));
    CHECK(options.codec
          == (Compression::IsAvailable(CompressionCodec::Zstd) ? CompressionCodec::Zstd : CompressionCodec::Identity));

    // The default is OFF, stated here rather than assumed: turning it on by default
    // would change the CPU cost of every existing deployment on upgrade.
    CHECK(MemoryCompressionOf(NodeConfig {}).codec == CompressionCodec::Identity);
}

TEST_CASE("The memory tier compresses only when a codec is named", "[node][cache-tier][compression]")
{
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        SKIP("this build has no zstd, so there is no codec to tell apart from none");

    constexpr std::size_t PayloadBytes = 64 * 1024;
    auto const payload = Compressible(PayloadBytes);

    // One tier per codec, each storing the same object, each asked what it now holds.
    auto residentBytes = [&payload](CompressionCodec codec) {
        Fixture fixture;
        auto cfg = Fixture::BaseConfig();
        cfg.cacheMemoryBytes = 8 * 1024 * 1024;
        cfg.memoryCompression = codec;
        cfg.memoryCompressionMinBytes = 1024;

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);

        auto const stored =
            core::async::syncRun(
                tier->Responder().Answer(
                    Wire::EncodeStore(Wire::StoreRequest {
                        .key = "object", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = payload }),
                    PeerIdentity { .host = "127.0.0.1" }))
                .bytes;
        REQUIRE(StatusOf(stored) == Wire::Status::Ok);

        // And it must still be READABLE: a tier that compressed and could not decode
        // would shrink exactly as convincingly.
        auto const fetched =
            core::async::syncRun(tier->Responder().Answer(Wire::EncodeFetch("object"), PeerIdentity { .host = "127.0.0.1" }))
                .bytes;
        REQUIRE(StatusOf(fetched) == Wire::Status::Ok);

        auto const tiers = tier->SnapshotTiers();
        REQUIRE(At(tiers, StorageTier::Memory).has_value());
        return Unwrap(At(tiers, StorageTier::Memory)).bytesUsed;
    };

    auto const plain = residentBytes(CompressionCodec::Identity);
    auto const packed = residentBytes(CompressionCodec::Zstd);

    // The budget counts STORED bytes, which is the whole point: an uncompressed tier
    // is charged the object, a compressed one a fraction of it. A ratio rather than a
    // constant, because how well zstd does on this payload is for zstd to decide.
    CHECK(plain >= PayloadBytes);
    CHECK(packed < plain / 2);
}

TEST_CASE("The disk tier compresses with the codec the node names", "[node][cache-tier][compression]")
{
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        SKIP("this build has no zstd, so there is no codec to tell apart from none");

    constexpr std::size_t PayloadBytes = 256 * 1024;
    auto const payload = Compressible(PayloadBytes);

    // Measured on the STORE FILE and on `bytesUsed`, and both must follow the codec.
    //
    // `bytesUsed` used to be blind to it: `CowTreeStorage` charged `originalLen`, the
    // pre-compression size, so a compressed disk tier reported exactly what an
    // uncompressed one did, and `--cache-disk` bounded logical bytes. It now charges
    // the store's page footprint, so a value costs its compressed size against the
    // budget -- which is what lets compression make the same budget hold more.
    struct Measured
    {
        std::size_t file;      ///< The store file's length once closed.
        std::size_t footprint; ///< The disk tier's `bytesUsed` while open.
    };
    auto measure = [&payload](CompressionCodec codec, std::string_view scratchName) {
        Testing::ScratchDirectory const scratch { scratchName };
        auto const store = scratch.Path() / DiskStoreFileName;
        std::size_t footprint = 0;
        {
            Fixture fixture;
            auto cfg = Fixture::BaseConfig();
            // No memory tier, so nothing mirrors the value into an L1 instead.
            cfg.cacheMemoryBytes = 0;
            cfg.cacheDir = scratch.Path();
            cfg.compression = codec;
            cfg.compressionMinBytes = 1024;

            auto started = fixture.Start(cfg);
            REQUIRE(started.has_value());
            auto const tier = std::move(*started);
            REQUIRE(tier != nullptr);

            auto const stored =
                core::async::syncRun(tier->Responder().Answer(Wire::EncodeStore(Wire::StoreRequest { .key = "object",
                                                                                                     .prefetchGroup = {},
                                                                                                     .srcRoot = "/src",
                                                                                                     .buildTree = "/build",
                                                                                                     .value = payload }),
                                                              PeerIdentity { .host = "127.0.0.1" }))
                    .bytes;
            REQUIRE(StatusOf(stored) == Wire::Status::Ok);

            // Still readable: a tier that compressed and could not decode would
            // shrink the file exactly as convincingly.
            auto const fetched = core::async::syncRun(tier->Responder().Answer(Wire::EncodeFetch("object"),
                                                                               PeerIdentity { .host = "127.0.0.1" }))
                                     .bytes;
            REQUIRE(StatusOf(fetched) == Wire::Status::Ok);

            auto const tiers = tier->SnapshotTiers();
            REQUIRE(At(tiers, StorageTier::Disk).has_value());
            footprint = Unwrap(At(tiers, StorageTier::Disk)).bytesUsed;
        }
        // Sized after the tier is closed, so the last commit has certainly landed.
        std::error_code error;
        auto const size = std::filesystem::file_size(store, error);
        REQUIRE_FALSE(error);
        return Measured { .file = static_cast<std::size_t>(size), .footprint = footprint };
    };

    auto const plain = measure(CompressionCodec::Identity, "node-disk-codec-none");
    auto const packed = measure(CompressionCodec::Zstd, "node-disk-codec-zstd");

    // A ratio rather than a constant: how well zstd does on this payload is for zstd
    // to decide, and the file carries pages of tree overhead either way.
    CHECK(plain.file >= PayloadBytes);
    CHECK(packed.file < plain.file / 2);
    // And the budget's figure sees the codec too. Uncompressed, it is at least the
    // payload; compressed, well under half of that.
    CHECK(plain.footprint >= PayloadBytes);
    CHECK(packed.footprint < plain.footprint / 2);
}

TEST_CASE("The startup line names a present tier's codec and an absent tier's nothing", "[node][cache-tier][compression]")
{
    // Nothing else reports it -- no metric, no fleet.json field -- so without this an
    // operator cannot tell a compressed tier from an uncompressed one.
    //
    // Both halves present, so BOTH `NameOf` calls run. The first version of this case
    // left the disk half off and pinned the memory codec to its default, so it
    // asserted `memory 8M none` and `disk off` -- text a build with the codec dropped
    // from either arm produces just as happily. Assert what tells the two apart.
    Testing::ScratchDirectory const scratch { "node-startup-line-codecs" };

    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    cfg.cacheDir = scratch.Path();
    cfg.cacheDiskBytes = 16 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    REQUIRE(*started != nullptr);

    // The disk arm renders a codec beside its size and path. Deleting
    // `Compression::NameOf` from that arm leaves `16M at <path>`, which this refuses.
    CHECK(Logged(fixture.logger,
                 std::format("disk 16M {} at {}",
                             Compression::NameOf(Compression::EffectiveCodec(cfg.compression)),
                             scratch.Path().string())));
    CHECK(Logged(fixture.logger, "memory 8M none"));
}

TEST_CASE("The startup line names each half's OWN codec", "[node][cache-tier][compression]")
{
    // Two different codecs, so a literal, a swap and a dropped call each fail. One
    // codec in both halves passes under a line that reads either half twice, which is
    // exactly the mistake a format string with two identical-looking arms invites.
    if (!Compression::IsAvailable(CompressionCodec::Zstd) || !Compression::IsAvailable(CompressionCodec::Lz4))
        SKIP("this build has no lz4/zstd, so the two halves cannot be given different codecs");

    Testing::ScratchDirectory const scratch { "node-startup-line-two-codecs" };

    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    cfg.memoryCompression = CompressionCodec::Zstd;
    cfg.cacheDir = scratch.Path();
    cfg.cacheDiskBytes = 16 * 1024 * 1024;
    cfg.compression = CompressionCodec::Lz4;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    REQUIRE(*started != nullptr);

    CHECK(Logged(fixture.logger, "memory 8M zstd"));
    CHECK(Logged(fixture.logger, "disk 16M lz4 at"));

    // And neither half wandered into the other's. `contains` is a substring test, so
    // the two checks above both pass on a line naming one codec twice.
    CHECK_FALSE(Logged(fixture.logger, "memory 8M lz4"));
    CHECK_FALSE(Logged(fixture.logger, "disk 16M zstd"));
}

TEST_CASE("An absent tier is named without a codec", "[node][cache-tier][compression]")
{
    // "off zstd" would describe a tier this node does not run. Its own case rather
    // than an assertion tacked onto a present-tier one, because it is the only thing
    // here that a build with no codecs at all can still tell apart.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    REQUIRE(*started != nullptr);

    CHECK(Logged(fixture.logger, "disk off,"));
    CHECK_FALSE(Logged(fixture.logger, "disk off none"));
}

TEST_CASE("The startup line reports the codec the tier was BUILT with", "[node][cache-tier][compression]")
{
    // The report and the store read one function, so they cannot disagree about a
    // build. That is the property; what makes it worth a case is that the failure it
    // prevents is invisible everywhere else -- on a build without
    // `PACE_ENABLE_COMPRESSION` the tier stores plaintext and a report taken
    // from the configuration says `zstd`, with every counter normal and every object
    // correct.
    //
    // Asserted against `MemoryCompressionOf`, which is what `BuildStorage` hands the
    // tier. A literal here would be this case authoring a second answer, which is the
    // defect rather than the test for it.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    cfg.memoryCompression = CompressionCodec::Zstd;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    REQUIRE(*started != nullptr);

    CHECK(Logged(fixture.logger, std::format("memory 8M {}", Compression::NameOf(MemoryCompressionOf(cfg).codec))));

    // And on a build that HAS zstd the effective codec is zstd, so the line above is
    // not quietly asserting `none` against `none`. On a build without it, both sides
    // read `none` and the case still says something true.
    CHECK(MemoryCompressionOf(cfg).codec
          == (Compression::IsAvailable(CompressionCodec::Zstd) ? CompressionCodec::Zstd : CompressionCodec::Identity));
}

// --- cache-drop (#1276) ------------------------------------------------------------

TEST_CASE("A dropped key is gone from every tier a fetch consults, and stays gone", "[node][cache-tier][cache-drop]")
{
    // Through the PRODUCTION composition -- `StartCacheTierOrExplain`'s shard, its
    // write-error decorator, the in-memory L1 over the on-disk L2 -- and not a bare tier,
    // because "gone" is a claim about what a FETCH reads. An L1 mirror left behind would
    // serve the object from memory after the disk forgot it, and a removal that never
    // reached the store file would come back when the node restarts. Both are asserted.
    Testing::ScratchDirectory const scratch { "node-cache-drop" };
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 4 * 1024 * 1024;
    cfg.cacheDir = scratch.Path();

    auto const ask = [](CacheTier& tier, std::vector<std::byte> const& frame) {
        return StatusOf(core::async::syncRun(tier.Responder().Answer(frame, PeerIdentity { .host = "127.0.0.1" })).bytes);
    };

    {
        Fixture fixture;
        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);

        std::vector<std::byte> const value(1024, std::byte { 0x5A });
        REQUIRE(ask(*tier,
                    Wire::EncodeStore(Wire::StoreRequest {
                        .key = "dropped", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = value }))
                == Wire::Status::Ok);
        // A fetch BEFORE the drop, so the object is resident in L1 as well as on disk: the
        // drop has two copies to remove, not one.
        REQUIRE(ask(*tier, Wire::EncodeFetch("dropped")) == Wire::Status::Ok);

        CHECK(ask(*tier, Wire::EncodeCacheDrop("dropped")) == Wire::Status::Ok);
        CHECK(ask(*tier, Wire::EncodeFetch("dropped")) == Wire::Status::Miss);
        CHECK(ask(*tier, Wire::EncodeCacheDrop("dropped")) == Wire::Status::Miss);

        auto const tiers = tier->SnapshotTiers();
        REQUIRE(At(tiers, StorageTier::Disk).has_value());
        CHECK(Unwrap(At(tiers, StorageTier::Disk)).deleteHits == 1);
        CHECK(Unwrap(At(tiers, StorageTier::Disk)).deleteMisses == 1);
    }

    // The same store file, reopened: the removal was persisted, not only forgotten in memory.
    Fixture fixture;
    auto reopened = fixture.Start(cfg);
    REQUIRE(reopened.has_value());
    auto const tier = std::move(*reopened);
    REQUIRE(tier != nullptr);
    CHECK(ask(*tier, Wire::EncodeFetch("dropped")) == Wire::Status::Miss);
}

TEST_CASE("NodeMetrics reads back as the series the node's own /metrics route serves", "[node][cache-tier][node-metrics]")
{
    // #1406. The two PRODUCTION acquisitions of one node, compared: the admin route's body as the
    // `/metrics` rung parses it, and the `NodeMetrics` verb as the CLI decodes it. Each is derived
    // from the node, never from a reading the case made, so a figure one door carries and the other
    // does not is a difference here rather than a gap both sides share.
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 4 * 1024 * 1024;
    cfg.upstream = "127.0.0.1:1";

    Fixture fixture;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);
    fixture.metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted, 7);

    MetricsDoors doors { *tier, fixture.metrics };
    auto const secondsOf = [](std::string_view lexical) {
        auto seconds = std::uint64_t { 0 };
        REQUIRE(std::from_chars(lexical.data(), lexical.data() + lexical.size(), seconds).ec == std::errc {});
        return seconds;
    };
    auto const scraped = Cli::ParsePrometheus(doors.MetricsRoute());
    auto const asked = Cli::DecodeNodeMetrics(doors.NodeMetricsBody());
    REQUIRE(asked.has_value());

    // The figures that were missing from this verb, and a counter, required present on BOTH sides:
    // two empty records agree perfectly, and so do two that lost the same block.
    constexpr auto Required =
        std::array<std::string_view, 7> { "pace_worker_jobs_completed_total", "pace_items",
                                          "pace_delete_hits_total",           "pace_tier_items{tier=\"memory\"}",
                                          "pace_node_logical_cores",          "pace_node_cpu_busy_ticks_total",
                                          "pace_node_upstream_configured" };
    for (auto const name: Required)
    {
        INFO(name);
        CHECK(Cli::FindField(scraped, name) != nullptr);
        CHECK(Cli::FindField(*asked, name) != nullptr);
    }

    // Then the whole set, both directions, value for value.
    CHECK(asked->fields.size() == scraped.fields.size());
    for (auto const& field: scraped.fields)
    {
        INFO(field.name);
        auto const* ours = Cli::FindField(*asked, field.name);
        REQUIRE(ours != nullptr);
        // The uptime is read off the process clock per call, so a second acquisition can be a second
        // later: the one figure whose difference is the MOMENT asked and not the door.
        if (field.name == "pace_uptime_seconds")
            CHECK(secondsOf(field.value.lexical) <= secondsOf(ours->value.lexical));
        else
            CHECK(ours->value.lexical == field.value.lexical);
    }
    for (auto const& field: asked->fields)
    {
        INFO(field.name);
        CHECK(Cli::FindField(scraped, field.name) != nullptr);
    }
}

TEST_CASE("A dropped key moves the delete figures NodeMetrics reads, with no admin surface in between",
          "[node][cache-tier][cache-drop][node-metrics]")
{
    // #1406, through the reading a node answers: `cache-drop` answers `Ok` or `Miss` and nothing else,
    // so this verb is how an operator on the `0xFC` port alone sees that a drop landed; a reply of the
    // counter catalogue alone has no delete figures to show it.
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 4 * 1024 * 1024;

    Fixture fixture;
    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);
    MetricsDoors doors { *tier, fixture.metrics };

    auto const deleteFigures = [&doors] {
        auto const reading = DecodeStatsReading(doors.NodeMetricsBody());
        REQUIRE(reading.has_value());
        REQUIRE(reading->snapshot.storage.has_value());
        return std::pair { reading->snapshot.storage->deleteHits, reading->snapshot.storage->deleteMisses };
    };
    auto const ask = [&tier](std::vector<std::byte> const& frame) {
        return StatusOf(core::async::syncRun(tier->Responder().Answer(frame, PeerIdentity { .host = "127.0.0.1" })).bytes);
    };

    // The control: nothing dropped yet reads zero, so the figures below are the drops' and not a
    // number the reading always carried.
    CHECK(deleteFigures() == std::pair { std::uint64_t { 0 }, std::uint64_t { 0 } });

    std::vector<std::byte> const value(64, std::byte { 0x5A });
    REQUIRE(ask(Wire::EncodeStore(Wire::StoreRequest {
                .key = "dropped", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = value }))
            == Wire::Status::Ok);
    REQUIRE(ask(Wire::EncodeCacheDrop("dropped")) == Wire::Status::Ok);
    CHECK(deleteFigures() == std::pair { std::uint64_t { 1 }, std::uint64_t { 0 } });

    REQUIRE(ask(Wire::EncodeCacheDrop("dropped")) == Wire::Status::Miss);
    CHECK(deleteFigures() == std::pair { std::uint64_t { 1 }, std::uint64_t { 1 } });
}

TEST_CASE("A local upstream wins over the fleet's shared cache", "[node][cache-tier][shared-cache]")
{
    // A node with an identity AND an `--upstream`: the operator typed that address on this machine,
    // and it is fixed for the process, so it wins over whatever the fleet's setting names.
    UpstreamFixture fix;
    CHECK(UpstreamKindOf(fix.Parts("cache-old.office.example:6674")) == UpstreamKind::Daemon);
    CHECK(MakeCacheUpstream(fix.Parts("cache-old.office.example:6674"))->Configured());
}

TEST_CASE("With no local upstream a node with an identity reads through to the fleet setting",
          "[node][cache-tier][shared-cache]")
{
    UpstreamFixture fix;
    CHECK(UpstreamKindOf(fix.Parts("")) == UpstreamKind::FleetSharedCache);
    auto const chosen = MakeCacheUpstream(fix.Parts(""));
    // And an unset setting is honest: not configured.
    CHECK_FALSE(chosen->Configured());
}

TEST_CASE("Each upstream kind speaks its own verb pair", "[node][cache-tier][shared-cache]")
{
    CHECK(UpstreamKindTable[static_cast<std::size_t>(UpstreamKind::Daemon)].verbs
          == std::optional { Wire::DaemonCacheVerbs });
    CHECK(UpstreamKindTable[static_cast<std::size_t>(UpstreamKind::FleetSharedCache)].verbs
          == std::optional { Wire::FleetSharedCacheVerbs });
    CHECK_FALSE(UpstreamKindTable[static_cast<std::size_t>(UpstreamKind::InProcessShared)].verbs.has_value());
    CHECK_FALSE(UpstreamKindTable[static_cast<std::size_t>(UpstreamKind::None)].verbs.has_value());
}

TEST_CASE("The named machine's private tier reads through in process, dialling nothing", "[node][cache-tier][shared-cache]")
{
    // The same upstream every node with an identity builds, on the machine the setting names: the
    // switch hands the operation to the in-process half. Had it chosen the proven session, the
    // target -- this machine -- is not one a session is dialled for, so the store would have come
    // back `NotConfigured` rather than stored; and nothing on the dialling side may have moved.
    UpstreamFixture fix;
    fix.NameThisMachine();
    auto const chosen = MakeCacheUpstream(fix.Parts(""));
    REQUIRE(chosen->Configured());
    auto const value = Testing::AStoredObject();
    CHECK(core::async::syncRun(chosen->Store("k", value)) == UpstreamStore::Stored);
    auto const fetched = core::async::syncRun(chosen->Fetch("k"));
    REQUIRE(fetched.has_value());
    CHECK(Unwrap(fetched) == value);
    for (auto const counter: { IMetricsSink::Counter::NodeSharedCacheSessionsOpened,
                               IMetricsSink::Counter::NodeSharedCacheProofsFailed,
                               IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey,
                               IMetricsSink::Counter::NodeSharedCacheStaleHints,
                               IMetricsSink::Counter::NodeSharedCacheUnresolved })
        CHECK(fix.metrics.Read(counter) == 0);
    // The shared tier's own series moved: it was read and written in process.
    CHECK(fix.opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
}

TEST_CASE("A node with no identity and no upstream reads through to nothing", "[node][cache-tier][shared-cache]")
{
    UpstreamFixture fix;
    auto parts = fix.Parts("");
    parts.prover = nullptr;
    CHECK(UpstreamKindOf(parts) == UpstreamKind::None);
    CHECK_FALSE(MakeCacheUpstream(parts)->Configured());
}

TEST_CASE("The tier hands out the fleet half's status only where it reads through to the fleet",
          "[node][cache-tier][shared-cache][status]")
{
    // What `--node-status` is handed, reached the way `main` reaches it: through the tier, past the
    // switch and the proven wrapper `MakeCacheUpstream` builds, to the one half that keeps a verdict.
    Fixture fixture;
    UpstreamFixture fix;
    auto const cfg = Fixture::BaseConfig();
    SECTION("the fleet's setting: the live verdict of the half that dials it")
    {
        auto started =
            StartCacheTierOrExplain(cfg, fix.Parts(""), fixture.locality, fixture.clock, fixture.metrics, fixture.logger);
        REQUIRE(started.has_value());
        REQUIRE(*started != nullptr);
        auto const* const status = (*started)->SharedCacheStatus();
        REQUIRE(status != nullptr);
        // Live, and the fleet half's: it follows the directory it re-judges from.
        CHECK(status->Report().source == Wire::WireSharedCacheSource::None);
        fix.directory.Applied(Testing::NamingSharedCache("cache-c"));
        CHECK(status->Report().source == Wire::WireSharedCacheSource::Setting);
        CHECK(status->Report().machineId == "cache-c");
    }
    SECTION("--upstream: nothing proves anything on that leg")
    {
        auto started = StartCacheTierOrExplain(
            cfg, fix.Parts("127.0.0.1:1"), fixture.locality, fixture.clock, fixture.metrics, fixture.logger);
        REQUIRE(started.has_value());
        REQUIRE(*started != nullptr);
        CHECK((*started)->SharedCacheStatus() == nullptr);
    }
    SECTION("no identity: nothing reaches the fleet")
    {
        auto parts = fix.Parts("");
        parts.prover = nullptr;
        auto started = StartCacheTierOrExplain(cfg, parts, fixture.locality, fixture.clock, fixture.metrics, fixture.logger);
        REQUIRE(started.has_value());
        REQUIRE(*started != nullptr);
        CHECK((*started)->SharedCacheStatus() == nullptr);
    }
}

TEST_CASE("The startup line names the upstream by its kind", "[node][cache-tier][shared-cache]")
{
    // The line reads the SAME kind the tier was built from, so it cannot name another choice.
    Fixture fixture;
    UpstreamFixture fix;
    auto const cfg = Fixture::BaseConfig();
    SECTION("the fleet's setting, for a node with an identity")
    {
        auto started =
            StartCacheTierOrExplain(cfg, fix.Parts(""), fixture.locality, fixture.clock, fixture.metrics, fixture.logger);
        REQUIRE(started.has_value());
        CHECK(Logged(fixture.logger, "upstream shared-cache (the fleet setting; resolved at every apply))"));
    }
    SECTION("--upstream, which wins")
    {
        auto started = StartCacheTierOrExplain(
            cfg, fix.Parts("127.0.0.1:1"), fixture.locality, fixture.clock, fixture.metrics, fixture.logger);
        REQUIRE(started.has_value());
        CHECK(Logged(fixture.logger, "upstream 127.0.0.1:1 (override, pace-kv verbs))"));
    }
    SECTION("nothing, for a node with neither")
    {
        auto parts = fix.Parts("");
        parts.prover = nullptr;
        auto started = StartCacheTierOrExplain(cfg, parts, fixture.locality, fixture.clock, fixture.metrics, fixture.logger);
        REQUIRE(started.has_value());
        CHECK(Logged(fixture.logger, "upstream none)"));
    }
}
