// SPDX-License-Identifier: Apache-2.0
#include "AdmissionTestUtils.hpp"
#include "FleetTextResponder.hpp"
#include "McpEndpoint.hpp"
#include "McpUpstream.hpp"
#include "NodeIoLoop.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/LiveStream.hpp>
#include <Pace/Server/AdminCredential.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace Pace;
using namespace Pace::Node;
using namespace std::chrono_literals;
using Pace::Testing::Unwrap;
using Pace::Testing::UnwrapMutable;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// A platform reactor on a thread of its own, as `NodeIoLoop` runs one: the flows `NodeIoUpstream` posts
/// run there, and the case's thread is the MCP thread that waits on them.
///
/// Stopped and joined by the destructor, so a failed `REQUIRE` unwinds through it rather than leaving a
/// reactor thread nobody stops; a flow still parked then is freed with the reactor.
class RunningLoop
{
  public:
    RunningLoop()
    {
        _thread = std::jthread { [this] { _reactor.run(); } };
    }

    RunningLoop(RunningLoop const&) = delete;
    RunningLoop& operator=(RunningLoop const&) = delete;
    RunningLoop(RunningLoop&&) = delete;
    RunningLoop& operator=(RunningLoop&&) = delete;

    ~RunningLoop()
    {
        _reactor.stop();
        _thread.join();
    }

    /// @return The reactor.
    [[nodiscard]] core::net::EventLoop& Loop() noexcept
    {
        return _reactor;
    }

    /// Run @p work on the loop's thread and hand its answer back, bounded.
    /// @param work What to run there.
    /// @return Its answer, or nothing when the loop did not run it within the bound.
    template <typename Work>
    [[nodiscard]] auto OnLoop(Work work) -> std::optional<decltype(work())>
    {
        auto answered = std::make_shared<std::promise<decltype(work())>>();
        auto future = answered->get_future();
        _reactor.post([answered, work = std::move(work)] { answered->set_value(work()); });
        if (future.wait_for(Testing::WaitHangGuard) != std::future_status::ready)
            return std::nullopt;
        return future.get();
    }

    /// @return The loop thread's id.
    [[nodiscard]] std::thread::id ThreadId()
    {
        return Unwrap(OnLoop([] { return std::this_thread::get_id(); }));
    }

    /// @return How many posted flows the loop still holds -- a parked one is a frame nobody has freed.
    [[nodiscard]] std::size_t FlowsHeld()
    {
        return Unwrap(OnLoop([this] { return _reactor.spawnedCount(); }));
    }

    /// @return How many deadlines the loop still has armed.
    [[nodiscard]] std::size_t TimersHeld()
    {
        return Unwrap(OnLoop([this] { return _reactor.pendingTimerCount(); }));
    }

  private:
    core::platform::SteadyClock _clock;
    core::net::PlatformLoop _reactor { _clock };
    std::jthread _thread;
};

/// The connection a flow is handed: every operation forwarded to an in-memory end the dialer keeps, and
/// every `close` counted -- which is how the CANCELLATION is told apart from the flow merely ending, since
/// a flow that ends drops its connection without closing it here.
class CloseCountingSocket final: public core::net::testing::SocketDecorator
{
  public:
    /// @param inner The end operations go to; must outlive this.
    /// @param closes Where each close is counted; must outlive this.
    CloseCountingSocket(core::net::ISocket& inner, std::atomic<std::size_t>& closes) noexcept:
        SocketDecorator { inner },
        _closes { &closes }
    {
    }

    /// Counts, then forwards LAST: forwarding completes the parked read inline, and the flow it resumes
    /// destroys this before the call returns.
    void close() noexcept override
    {
        _closes->fetch_add(1);
        SocketDecorator::close();
    }

  private:
    std::atomic<std::size_t>* _closes;
};

/// What a scripted endpoint does when it is dialled.
enum class Script : std::uint8_t
{
    Answer, ///< Connects, and answers the first request with the scripted reply.
    Silent, ///< Connects, and never answers.
    Fails,  ///< The dial fails with the scripted fault and reason.
    Hangs,  ///< Connects, and closes before it answers; the seal reports `Scripted::seal` as the reason, if any.
};

/// One endpoint's script.
struct Scripted
{
    Script script { Script::Answer };
    std::vector<std::byte> reply;
    std::string reason;
    UpstreamOpeningFault fault { UpstreamOpeningFault::Unreached }; ///< For `Fails`.
    std::optional<SealFault> seal {};                               ///< For `Hangs`: what the seal says stopped it.
};

/// A dialer over in-memory pairs: each dial runs on the loop (it is a coroutine the flow awaits there),
/// is recorded with the thread it ran on, and is answered by the endpoint's script.
///
/// Records and asserts nothing on the loop thread: what it saw is read on the case's thread.
class ScriptedDialer final: public IUpstreamDialer
{
  public:
    /// Script @p endpoint; called before any `Ask`.
    /// @param endpoint Where.
    /// @param scripted What it does.
    void Place(std::string endpoint, Scripted scripted)
    {
        auto const guard = std::scoped_lock { _mutex };
        _scripts.insert_or_assign(std::move(endpoint), std::move(scripted));
    }

    /// @copydoc IUpstreamDialer::Open
    [[nodiscard]] core::async::Task<UpstreamOpening> Open(std::string endpoint) override
    {
        auto scripted = Scripted { .script = Script::Fails, .reply = {}, .reason = "nothing is scripted there" };
        {
            auto const guard = std::scoped_lock { _mutex };
            _dialled.push_back(endpoint);
            _threads.push_back(std::this_thread::get_id());
            if (auto const found = _scripts.find(endpoint); found != _scripts.end())
                scripted = found->second;
        }
        switch (scripted.script)
        {
            case Script::Fails:
                co_return UpstreamOpening { .socket = nullptr, .reason = scripted.reason, .fault = scripted.fault };
            case Script::Answer:
            case Script::Silent:
            case Script::Hangs:
                break;
        }
        auto pair = core::net::testing::InMemorySocketPair::create();
        // Closed before the request arrives: the exchange breaks as it does when a peer goes away -- or, in
        // production, when the seal refuses a reply and ends the stream, which is what `seal` stands for.
        if (scripted.script == Script::Hangs)
            pair.server->close();
        // Written before the request arrives: an in-memory pipe buffers it, and the exchange reads it once
        // its own request is sent, exactly as it would read a reply that came back over the wire.
        if (scripted.script == Script::Answer)
            std::ignore = co_await pair.server->write(scripted.reply);
        auto handed = std::make_unique<CloseCountingSocket>(*pair.client, _clientCloses);
        {
            auto const guard = std::scoped_lock { _mutex };
            _servers.push_back(std::move(pair.server));
            _clients.push_back(std::move(pair.client));
        }
        auto sealFault = std::function<std::optional<SealFault>()> {};
        if (scripted.seal.has_value())
            sealFault = [fault = *scripted.seal] {
                return std::optional { fault };
            };
        co_return UpstreamOpening { .socket = std::move(handed),
                                    .reason = {},
                                    .fault = UpstreamOpeningFault::Unreached,
                                    .sealFault = std::move(sealFault) };
    }

    /// @return Every endpoint dialled, in order.
    [[nodiscard]] std::vector<std::string> Dialled() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _dialled;
    }

    /// @return The thread each dial ran on, in order.
    [[nodiscard]] std::vector<std::thread::id> Threads() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _threads;
    }

    /// @return How many times a flow closed a connection this dialer handed it.
    [[nodiscard]] std::size_t ClientCloses() const noexcept
    {
        return _clientCloses.load();
    }

    /// The server end of the @p index-th connection; read on the loop thread only.
    /// @param index Which.
    /// @return Its socket.
    [[nodiscard]] core::net::testing::InMemorySocket& Server(std::size_t index)
    {
        auto const guard = std::scoped_lock { _mutex };
        return *_servers.at(index);
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, Scripted, std::less<>> _scripts;
    std::vector<std::string> _dialled;
    std::vector<std::thread::id> _threads;
    std::vector<std::unique_ptr<core::net::testing::InMemorySocket>> _servers;
    /// The dialling ends, kept here so a flow's decorator can never outlive what it forwards to.
    std::vector<std::unique_ptr<core::net::testing::InMemorySocket>> _clients;
    std::atomic<std::size_t> _clientCloses { 0 };
};

/// A fleet a case places: who leads, where the members answer, who this node is and its term. Set
/// before the first `Ask`, which posts -- so every read on the loop happens after.
struct PlacedFleet final: public IUpstreamDirectory
{
    [[nodiscard]] std::string LeaderEndpoint() const override
    {
        return leader;
    }

    [[nodiscard]] std::optional<std::string> EndpointOf(std::string_view nodeId) const override
    {
        if (auto const found = members.find(nodeId); found != members.end())
            return found->second;
        return std::nullopt;
    }

    [[nodiscard]] std::string SelfId() const override
    {
        return selfId;
    }

    [[nodiscard]] std::string SelfEndpoint() const override
    {
        return selfEndpoint;
    }

    [[nodiscard]] std::optional<std::uint64_t> Term() const override
    {
        return term;
    }

    std::string leader;
    std::map<std::string, std::string, std::less<>> members;
    std::string selfId { "self" };
    std::string selfEndpoint { "10.0.0.100:6674" };
    std::optional<std::uint64_t> term { 7 };
};

/// This node's own surface, answering every frame with one scripted reply and recording where it ran.
class ScriptedSelf final: public IInProcessAnswerer
{
  public:
    /// @param reply The whole reply frame.
    explicit ScriptedSelf(std::vector<std::byte> reply):
        _reply { std::move(reply) }
    {
    }

    /// @copydoc IInProcessAnswerer::Answer
    [[nodiscard]] core::async::Task<std::vector<std::byte>> Answer(std::vector<std::byte> frame) override
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _asked.push_back(std::move(frame));
            _threads.push_back(std::this_thread::get_id());
        }
        co_return _reply;
    }

    /// @return Every frame it was asked, in order.
    [[nodiscard]] std::vector<std::vector<std::byte>> Asked() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _asked;
    }

    /// @return The thread each answer ran on.
    [[nodiscard]] std::vector<std::thread::id> Threads() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _threads;
    }

  private:
    std::vector<std::byte> _reply;
    mutable std::mutex _mutex;
    std::vector<std::vector<std::byte>> _asked;
    std::vector<std::thread::id> _threads;
};

/// @param text Bytes.
/// @return The same bytes, as a reply payload.
[[nodiscard]] std::vector<std::byte> OkReply(std::string_view text)
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes(text));
}

/// @param endpoint Whom it names.
/// @return `NotLeader` naming @p endpoint.
[[nodiscard]] std::vector<std::byte> NotLeaderNaming(std::string_view endpoint)
{
    return Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, endpoint);
}

/// @return The request every case forwards: one a node answers, a NODE-STATUS.
[[nodiscard]] std::string RequestFrame()
{
    auto const bytes = Wire::EncodeNodeStatusRequest();
    auto frame = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, frame.begin(), [](std::byte const b) { return static_cast<char>(b); });
    return frame;
}

/// One read, as a task a case can drive to completion.
/// @param socket What to read from; its bytes are already buffered.
/// @param buffer Where they go.
/// @return How many arrived; zero for none or a failure.
core::async::Task<std::size_t> ReadSome(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    auto const read = co_await socket->read(buffer);
    co_return read.has_value() ? *read : 0;
}

/// The pieces every case wires, declared so the loop goes FIRST: its flows reach everything above it.
struct Bridge
{
    explicit Bridge(UpstreamPolicy policy = {}):
        upstream { loop.Loop(), dialer, fleet, self, policy }
    {
    }

    ScriptedDialer dialer;
    PlacedFleet fleet;
    ScriptedSelf self { OkReply("self-status") };
    RunningLoop loop;
    NodeIoUpstream upstream;
};

constexpr std::string_view LeaderA = "10.0.0.1:6674";
constexpr std::string_view NodeB = "10.0.0.2:6674";

} // namespace

TEST_CASE("mcp-upstream: forwards to the leader", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA },
                        Scripted { .script = Script::Answer, .reply = OkReply("fleet"), .reason = {} });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE(answer.has_value());
    CHECK(answer->status == Wire::Status::Ok);
    CHECK(answer->payload == "fleet");
    CHECK(answer->answeredBy == LeaderA);
    CHECK(answer->term == std::optional<std::uint64_t> { 7 });
    CHECK(bridge.dialer.Dialled() == std::vector<std::string> { std::string { LeaderA } });

    // The leader received the frame exactly as the MCP thread handed it over.
    auto const received = bridge.loop.OnLoop([&] {
        auto buffer = std::vector<std::byte>(RequestFrame().size() + 1);
        buffer.resize(core::async::syncRun(ReadSome(&bridge.dialer.Server(0), buffer)));
        return buffer;
    });
    auto const expected = Wire::EncodeNodeStatusRequest();
    CHECK(Unwrap(received) == expected);
    // Control for the deadline case: a flow that ends by answering closes nothing through the dialer's
    // count, so a close counted there is the cancellation's.
    CHECK(bridge.dialer.ClientCloses() == 0);
}

TEST_CASE("mcp-upstream: the dial runs on the loop thread and never on the asking thread", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA }, Scripted { .script = Script::Answer, .reply = OkReply("x"), .reason = {} });

    REQUIRE(bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame()).has_value());
    auto const threads = bridge.dialer.Threads();
    REQUIRE(threads.size() == 1);
    CHECK(threads.front() == bridge.loop.ThreadId());
    CHECK(threads.front() != std::this_thread::get_id());
}

TEST_CASE("mcp-upstream: a redirect mid-call is followed to the leader it names", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA },
                        Scripted { .script = Script::Answer, .reply = NotLeaderNaming(NodeB), .reason = {} });
    bridge.dialer.Place(std::string { NodeB },
                        Scripted { .script = Script::Answer, .reply = OkReply("from-b"), .reason = {} });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE(answer.has_value());
    CHECK(answer->answeredBy == NodeB);
    CHECK(answer->payload == "from-b");
    CHECK(bridge.dialer.Dialled() == std::vector<std::string> { std::string { LeaderA }, std::string { NodeB } });
}

TEST_CASE("mcp-upstream: a redirect loop stops at MaxLeaderRedirects with TooManyRedirects", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA },
                        Scripted { .script = Script::Answer, .reply = NotLeaderNaming(NodeB), .reason = {} });
    bridge.dialer.Place(std::string { NodeB },
                        Scripted { .script = Script::Answer, .reply = NotLeaderNaming(LeaderA), .reason = {} });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::TooManyRedirects);
    // Three redirects followed means four asks, and not one more.
    CHECK(std::cmp_equal(bridge.dialer.Dialled().size(), MaxLeaderRedirects + 1));
}

TEST_CASE("mcp-upstream: a NotLeader naming nobody is NoLeaderKnown", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(
        std::string { LeaderA },
        Scripted { .script = Script::Answer, .reply = NotLeaderNaming("no leader: try again"), .reason = {} });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::NoLeaderKnown);
    CHECK(answer.error().second.contains(LeaderA));
    CHECK(bridge.dialer.Dialled().size() == 1);
}

TEST_CASE("mcp-upstream: no leader known is answered without dialling", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::NoLeaderKnown);
    CHECK(bridge.dialer.Dialled().empty());
}

TEST_CASE("mcp-upstream: an unknown node id is UnknownNode and a prefix is not a match", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.members.emplace("node-abc", std::string { NodeB });

    auto const unknown = bridge.upstream.Ask(UpstreamTarget::Node, "node-zzz", RequestFrame());
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().first == UpstreamFailure::UnknownNode);
    CHECK(unknown.error().second.contains("node-zzz"));

    auto const prefix = bridge.upstream.Ask(UpstreamTarget::Node, "node-a", RequestFrame());
    REQUIRE_FALSE(prefix.has_value());
    CHECK(prefix.error().first == UpstreamFailure::UnknownNode);
    CHECK(bridge.dialer.Dialled().empty());
}

TEST_CASE("mcp-upstream: a named node is asked at its recorded endpoint and never redirected", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.members.emplace("node-b", std::string { NodeB });

    SECTION("it answers")
    {
        bridge.dialer.Place(std::string { NodeB },
                            Scripted { .script = Script::Answer, .reply = OkReply("b-status"), .reason = {} });
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Node, "node-b", RequestFrame());
        REQUIRE(answer.has_value());
        CHECK(answer->payload == "b-status");
        CHECK(answer->answeredBy == NodeB);
    }

    SECTION("a NotLeader from it is its answer, reported Refused")
    {
        bridge.dialer.Place(std::string { NodeB },
                            Scripted { .script = Script::Answer, .reply = NotLeaderNaming(LeaderA), .reason = {} });
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Node, "node-b", RequestFrame());
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().first == UpstreamFailure::Refused);
        CHECK(bridge.dialer.Dialled() == std::vector<std::string> { std::string { NodeB } });
    }

    SECTION("a member that announced no endpoint is Unreachable, and nothing is dialled")
    {
        bridge.fleet.members.insert_or_assign("node-b", std::string {});
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Node, "node-b", RequestFrame());
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().first == UpstreamFailure::Unreachable);
        CHECK(bridge.dialer.Dialled().empty());
    }
}

TEST_CASE("mcp-upstream: a leader that refuses is Refused with its words", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(
        std::string { LeaderA },
        Scripted { .script = Script::Answer,
                   .reply = Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "this caller is not a member"),
                   .reason = {} });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::Refused);
    CHECK(answer.error().second.contains("this caller is not a member"));
}

TEST_CASE("mcp-upstream: a refusal of this node as the caller is classified by its code, one failure per code",
          "[node][mcp][mcp-upstream]")
{
    // Both codes the standing gate answers, each its OWN failure -- the wire says a caller refused for want of an
    // identity must not be sent looking for a voter -- and a control: any other refusal stays `Refused`, whatever its
    // words say.
    struct Case
    {
        Wire::ErrorCode code;
        std::string_view words;
        UpstreamFailure expected;
    };
    for (auto const& [code, words, expected]: std::array {
             Case { .code = Wire::ErrorCode::OperatorStandingRequired,
                    .words = "a learner's ticket decides nothing",
                    .expected = UpstreamFailure::StandingRequired },
             Case { .code = Wire::ErrorCode::IdentifiedCallerRequired,
                    .words = "say who you are",
                    .expected = UpstreamFailure::NotIdentified },
             Case { .code = Wire::ErrorCode::NotAMember,
                    .words = "operator standing required",
                    .expected = UpstreamFailure::Refused },
         })
    {
        CAPTURE(words);
        Bridge bridge;
        bridge.fleet.leader = LeaderA;
        bridge.dialer.Place(std::string { LeaderA },
                            Scripted { .script = Script::Answer,
                                       .reply = Wire::EncodeErrorReply(code, std::string { words }),
                                       .reason = {} });
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().first == expected);
        CHECK(answer.error().second.contains(words));
    }
}

TEST_CASE("mcp-upstream: an endpoint nothing answers at is Unreachable", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA },
                        Scripted { .script = Script::Fails, .reply = {}, .reason = "connection refused" });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::Unreachable);
    CHECK(answer.error().second.contains(LeaderA));
    CHECK(answer.error().second.contains("connection refused"));
}

TEST_CASE("mcp-upstream: a reply the seal refused for its size is TooLarge and never a peer that went away",
          "[node][mcp][mcp-upstream]")
{
    // Both connections break the same way on the wire; only what the seal recorded tells them apart.
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA },
                        Scripted { .script = Script::Hangs, .reply = {}, .reason = {}, .seal = SealFault::Oversized });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::TooLarge);
    CHECK(answer.error().second.contains(LeaderA));
    CHECK(answer.error().second.contains(std::format("{} bytes", McpUpstream::MaxReplyBytes)));

    SECTION("the control: a peer that closes with no seal fault went away before it answered")
    {
        bridge.dialer.Place(std::string { NodeB }, Scripted { .script = Script::Hangs, .reply = {}, .reason = {} });
        bridge.fleet.leader = NodeB;
        auto const lost = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE_FALSE(lost.has_value());
        CHECK(lost.error().first == UpstreamFailure::Unreachable);
        CHECK(lost.error().second.contains("before it answered"));
    }

    SECTION("and a seal fault that is not a size is not reported as one")
    {
        bridge.dialer.Place(std::string { NodeB },
                            Scripted { .script = Script::Hangs, .reply = {}, .reason = {}, .seal = SealFault::BadTag });
        bridge.fleet.leader = NodeB;
        auto const tampered = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE_FALSE(tampered.has_value());
        CHECK(tampered.error().first == UpstreamFailure::Unreachable);
    }
}

TEST_CASE("mcp-upstream: a proof refusal surfaces ProofRefused with the peer's reason", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA },
                        Scripted { .script = Script::Fails,
                                   .reply = {},
                                   .reason = "key revoked by the cluster",
                                   .fault = UpstreamOpeningFault::ProofRefused });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::ProofRefused);
    CHECK(answer.error().second.contains("key revoked by the cluster"));
    CHECK(answer.error().second.contains(LeaderA));
}

TEST_CASE("mcp-upstream: the production dialer reports a refused proof as ProofRefused", "[node][mcp][mcp-upstream]")
{
    // The production dialer against the production fold: the leader admits a proof only from a key its
    // roster holds, and every connection to it reports another host, so loopback admits nothing.
    using namespace AdmissionFixture;
    ProvenLeader leader;
    auto const leaderPort = leader.Serve();

    PlacedFleet fleet;
    fleet.leader = EndpointOf(leaderPort);
    ScriptedSelf self { OkReply("unused") };

    SECTION("a machine the roster does not hold is refused its proof")
    {
        Follower node { EndpointOf(leaderPort), QuickPulse, std::string_view { "stranger" } };
        std::ignore = node.Serve(); // Runs the node's loop, which the bridge posts onto.
        NodeIoUpstream upstream { node.io->Loop(), UnwrapMutable(node.provenDialer), fleet, self };

        auto const answer = upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().first == UpstreamFailure::ProofRefused);
        // The LEADER's words, carried through: it names the key it does not hold.
        CHECK(answer.error().second.contains("node-key-unknown"));
        CHECK(answer.error().second.contains(fleet.leader));
    }

    SECTION("control: a machine the roster holds proves, and the leader's answer comes back")
    {
        leader.queue.ReportFor(Queue::ForwardingMachine);
        Follower node { EndpointOf(leaderPort), QuickPulse, Queue::ForwardingMachine };
        std::ignore = node.Serve();
        NodeIoUpstream upstream { node.io->Loop(), UnwrapMutable(node.provenDialer), fleet, self };

        auto const answer = upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        // The scheduler surface serves no NODE-STATUS, and says so over the sealed session: a refusal the
        // leader wrote is proof the handshake completed and the exchange was answered.
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().first == UpstreamFailure::Refused);
        CHECK(answer.error().second.contains("unknown-opcode"));
    }
}

TEST_CASE("mcp-upstream: each way a dial fails is reported by its own remedy", "[node][mcp][mcp-upstream]")
{
    // One case per fault, each asserting WHICH failure comes back: a handshake nobody answered and a peer
    // that is not ready yet must never read as a refusal.
    for (auto const fault: Enumerators<UpstreamOpeningFault>())
    {
        CAPTURE(static_cast<int>(fault));
        Bridge bridge;
        bridge.fleet.leader = LeaderA;
        bridge.dialer.Place(
            std::string { LeaderA },
            Scripted { .script = Script::Fails, .reply = {}, .reason = "the dialer's words", .fault = fault });

        auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().first == OpeningFaultTable[static_cast<std::size_t>(fault)].failure);
        CHECK(answer.error().second.contains("the dialer's words"));
    }
    CHECK(OpeningFaultTable[static_cast<std::size_t>(UpstreamOpeningFault::HandshakeIncomplete)].failure
          == UpstreamFailure::Unreachable);
    CHECK(OpeningFaultTable[static_cast<std::size_t>(UpstreamOpeningFault::ProofDeferred)].failure
          == UpstreamFailure::NotReady);
    CHECK(OpeningFaultTable[static_cast<std::size_t>(UpstreamOpeningFault::ProofNotOffered)].failure
          == UpstreamFailure::NoProofOffered);
    CHECK(OpeningFaultTable[static_cast<std::size_t>(UpstreamOpeningFault::PeerUntrusted)].failure
          == UpstreamFailure::PeerUntrusted);
}

TEST_CASE("mcp-upstream: a proof handshake nobody answers is Unreachable and never ProofRefused",
          "[node][mcp][mcp-upstream]")
{
    // A listener that never accepts: the kernel completes the connect from its backlog, and nothing ever
    // answers the challenge -- an overloaded or wedged leader, as the production dialer meets it.
    using namespace AdmissionFixture;
    auto const silent = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(silent != nullptr);
    REQUIRE(silent->IsBound());
    auto const endpoint = EndpointOf(silent->boundPort());

    Follower node { endpoint, QuickPulse, Queue::ForwardingMachine };
    std::ignore = node.Serve();
    ProvenSchedulerDialer dialer {
        UnwrapMutable(node.proof),
        node.io->Connector(),
        node.io->Reactor(),
        ForwardDialBudget { .connectTimeout = 1s, .handshakeTimeout = 200ms, .keepAlive = core::net::KeepAlive::No }
    };
    PlacedFleet fleet;
    fleet.leader = endpoint;
    ScriptedSelf self { OkReply("unused") };
    NodeIoUpstream upstream { node.io->Loop(), dialer, fleet, self };

    auto const answer = upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::Unreachable);
    CHECK(answer.error().second.contains("did not complete within 200 ms"));
    CHECK(answer.error().second.contains(endpoint));
}

TEST_CASE("mcp-upstream: a leader that cannot judge a proof yet is NotReady and never ProofRefused",
          "[node][mcp][mcp-upstream]")
{
    // The production fold, behind: a key the roster lacks may only not be applied yet, so the leader
    // answers `RosterNotYetApplied` -- a restarting leader, which a moment later will judge it.
    using namespace AdmissionFixture;
    ProvenLeader leader;
    leader.consensus.Set(AppliedStateReading::Behind);
    auto const leaderPort = leader.Serve();

    Follower node { EndpointOf(leaderPort), QuickPulse, std::string_view { "stranger" } };
    std::ignore = node.Serve();
    PlacedFleet fleet;
    fleet.leader = EndpointOf(leaderPort);
    ScriptedSelf self { OkReply("unused") };
    NodeIoUpstream upstream { node.io->Loop(), UnwrapMutable(node.provenDialer), fleet, self };

    auto const answer = upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::NotReady);
    CHECK(answer.error().second.contains(fleet.leader));
}

TEST_CASE("mcp-upstream: a self target answers in-process without dialling", "[node][mcp][mcp-upstream]")
{
    Bridge bridge;

    SECTION("the node scope's default, an empty id")
    {
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Node, "", RequestFrame());
        REQUIRE(answer.has_value());
        CHECK(answer->payload == "self-status");
        CHECK(answer->answeredBy == bridge.fleet.selfEndpoint);
    }

    SECTION("this node's own id")
    {
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Node, bridge.fleet.selfId, RequestFrame());
        REQUIRE(answer.has_value());
        CHECK(answer->payload == "self-status");
    }

    SECTION("the leader, when this node leads")
    {
        bridge.fleet.leader = bridge.fleet.selfEndpoint;
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE(answer.has_value());
        CHECK(answer->payload == "self-status");
    }

    SECTION("a redirect naming this node")
    {
        bridge.fleet.leader = LeaderA;
        bridge.dialer.Place(
            std::string { LeaderA },
            Scripted { .script = Script::Answer, .reply = NotLeaderNaming(bridge.fleet.selfEndpoint), .reason = {} });
        auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
        REQUIRE(answer.has_value());
        CHECK(answer->payload == "self-status");
        CHECK(bridge.dialer.Dialled() == std::vector<std::string> { std::string { LeaderA } });
    }

    auto const threads = bridge.self.Threads();
    REQUIRE(threads.size() == 1);
    CHECK(threads.front() == bridge.loop.ThreadId());
    CHECK(bridge.self.Asked().front() == Wire::EncodeNodeStatusRequest());
}

TEST_CASE("mcp-upstream: the deadline expires as Timeout and the frame is cancelled on the loop",
          "[node][mcp][mcp-upstream]")
{
    // The loop's own idle bound is far past the case, so only the cancellation the MCP thread posts can
    // end the parked exchange: a build that merely stopped listening leaves the flow parked, and this
    // case reads that as a frame nobody freed.
    constexpr auto Deadline = 200ms;
    Bridge bridge { UpstreamPolicy { .deadline = Deadline, .idle = 1h } };
    bridge.fleet.leader = LeaderA;
    bridge.dialer.Place(std::string { LeaderA }, Scripted { .script = Script::Silent, .reply = {}, .reason = {} });

    auto const answer = bridge.upstream.Ask(UpstreamTarget::Leader, "", RequestFrame());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().first == UpstreamFailure::Timeout);
    CHECK(answer.error().second.contains(LeaderA));

    CHECK(Testing::WaitUntil(
        "the cancelled flow to unwind and its frame to be freed on the loop",
        [&] { return bridge.loop.FlowsHeld() == 0; },
        [&] { return std::format("{} flow(s) still held", bridge.loop.FlowsHeld()); }));
    CHECK(bridge.loop.TimersHeld() == 0);
    // And it unwound because the connection it was parked on was CLOSED -- by the cancellation, the
    // only thing in this case that closes one.
    CHECK(bridge.dialer.ClientCloses() == 1);
}

TEST_CASE("mcp-upstream: the applied directory reads members exactly and knows no term until told",
          "[node][mcp][mcp-upstream]")
{
    AdmissionFixture::StatedEndpoint self;
    self.Set("10.0.0.100:6674");
    KnownSchedulingLeader leader;
    leader.Publish(LeaderA);
    AppliedUpstreamDirectory directory { "self", leader, self };

    CHECK(directory.LeaderEndpoint() == LeaderA);
    CHECK(directory.SelfEndpoint() == "10.0.0.100:6674");
    CHECK(directory.SelfId() == "self");
    CHECK_FALSE(directory.Term().has_value());
    CHECK_FALSE(directory.EndpointOf("node-b").has_value());

    auto state = Cluster::ClusterState {};
    state.members.push_back(
        Cluster::ClusterMember { .id = "node-b", .raftEndpoint = {}, .schedulerEndpoint = std::string { NodeB } });
    state.members.push_back(Cluster::ClusterMember { .id = "node-c", .raftEndpoint = {}, .schedulerEndpoint = {} });
    directory.Applied(state);
    directory.ObservedRole(4, false);

    CHECK(directory.EndpointOf("node-b") == std::optional<std::string> { std::string { NodeB } });
    CHECK(directory.EndpointOf("node-c") == std::optional<std::string> { std::string {} });
    CHECK_FALSE(directory.EndpointOf("node").has_value());
    CHECK(directory.Term() == std::optional<std::uint64_t> { 4 });

    // A later state replaces the earlier one: a forgotten member is unknown again.
    directory.Applied(Cluster::ClusterState {});
    CHECK_FALSE(directory.EndpointOf("node-b").has_value());
}

TEST_CASE("mcp-upstream: while this node leads, the directory names its own endpoint as the leader's",
          "[node][mcp][mcp-upstream]")
{
    // The scheduling leader a redirect reads is EMPTY on the leader -- a leader redirects nobody to itself -- so a
    // directory that read only it would answer every fleet call on the leader with NoLeaderKnown.
    AdmissionFixture::StatedEndpoint self;
    self.Set("10.0.0.100:6674");
    KnownSchedulingLeader leader;
    AppliedUpstreamDirectory directory { "self", leader, self };
    CHECK(directory.LeaderEndpoint().empty());

    directory.ObservedRole(5, true);
    CHECK(directory.LeaderEndpoint() == "10.0.0.100:6674");
    CHECK(directory.Term() == std::optional<std::uint64_t> { 5 });

    // Deposed: back to the leader consensus names.
    leader.Publish(LeaderA);
    directory.ObservedRole(6, false);
    CHECK(directory.LeaderEndpoint() == LeaderA);
}

namespace
{

/// An upstream that counts what reached it, for the gate in front of it.
class CountingUpstream final: public IMcpUpstream
{
  public:
    /// @copydoc IMcpUpstream::Ask
    [[nodiscard]] UpstreamResult Ask(UpstreamTarget /*target*/,
                                     std::string_view /*nodeIdOrEmpty*/,
                                     std::string /*frame*/) override
    {
        ++asked;
        return UpstreamReply { .status = Wire::Status::Ok, .payload = "ok", .answeredBy = "self", .term = std::nullopt };
    }

    std::size_t asked { 0 }; ///< How many calls reached it.
};

/// A leader's fleet sources, for the router a call to this node is answered by.
class LeadingFleet final: public ILiveStatsSources
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
        return LiveLeadership { .leads = true, .leaderEndpoint = {} };
    }

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "n1.test:6674";
    }

    /// @copydoc ILiveStatsSources::FleetText
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/,
                                                                                Wire::FleetTextFormat /*format*/,
                                                                                std::string_view /*node*/) const override
    {
        return FleetTextDocument { .leads = true, .leaderEndpoint = {}, .body = R"({"schema":3})" };
    }
};

} // namespace

TEST_CASE("mcp-upstream: a loop that is not turning is refused at once, and nothing is asked", "[node][mcp][mcp-upstream]")
{
    // Without the gate a call posted to a loop with no thread is never run, and the tool answers Timeout ten
    // seconds later naming the node asked. The gate answers at once, naming this node's own loop.
    CountingUpstream inner;
    auto turning = false;
    LoopGatedUpstream gated { inner, [&turning] { return turning; }, [] { return false; } };

    auto const refused = gated.Ask(UpstreamTarget::Leader, {}, "frame");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().first == UpstreamFailure::LoopNotRunning);
    CHECK(refused.error().second.contains("--listen-node"));
    CHECK(inner.asked == 0);

    // The control: a turning loop is asked.
    turning = true;
    CHECK(gated.Ask(UpstreamTarget::Leader, {}, "frame").has_value());
    CHECK(inner.asked == 1);
}

TEST_CASE("mcp-upstream: a stopping MCP surface is refused at once ahead of the loop and nothing is asked",
          "[node][mcp][mcp-upstream]")
{
    // A tool that asks many questions -- toolchain_diff -- must not go on asking while the surface waits to stop.
    CountingUpstream inner;
    McpStopFlag stopping;
    LoopGatedUpstream gated { inner, [] { return true; }, [&stopping] { return stopping.Raised(); } };

    // The control: before the stop the call is asked.
    CHECK(gated.Ask(UpstreamTarget::Leader, {}, "frame").has_value());
    CHECK(inner.asked == 1);

    stopping.Raise();
    auto const refused = gated.Ask(UpstreamTarget::Leader, {}, "frame");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().first == UpstreamFailure::Stopping);
    CHECK(inner.asked == 1);

    SECTION("and stopping is named even when the loop has stopped too")
    {
        LoopGatedUpstream both { inner, [] { return false; }, [&stopping] { return stopping.Raised(); } };
        auto const named = both.Ask(UpstreamTarget::Leader, {}, "frame");
        REQUIRE_FALSE(named.has_value());
        CHECK(named.error().first == UpstreamFailure::Stopping);
    }
}

TEST_CASE("mcp-upstream: a node loop that adopted no surface is not turning, even once started", "[node][mcp][mcp-upstream]")
{
    // The production predicate the gate is handed: `NodeIoLoop::Start` starts no thread when nothing was adopted.
    NodeIoLoop io;
    CHECK_FALSE(io.Turning());
    io.Start();
    CHECK_FALSE(io.Turning());
}

TEST_CASE("mcp-upstream: a call to this node is answered by its own router as this node's proven identity",
          "[node][mcp][mcp-upstream]")
{
    // The leader asking itself holds no dashboard credential; the proof of its own key stands in for it, as the
    // proof over a dialled connection does for any other member. The production fold, the key presented.
    AtomicMetricsSink metrics;
    LeadingFleet sources;
    Testing::RosterFold const roster { { "n1" } };
    FleetTextResponder router { sources, roster.admitted, AdminCredential { "s3cret" }, metrics };
    auto const frame = Wire::EncodeFleetTextRequest(Wire::FleetTextRequest { .format = Wire::FleetTextFormat::Json });

    FrameRouterAnswerer self { Testing::IdentityOf("n1") };
    self.RouteThrough(&router);
    CHECK(Testing::StatusOf(core::async::syncRun(self.Answer(frame))) == Wire::Status::Ok);

    SECTION("without its identity this machine is asked for the credential like any loopback caller")
    {
        FrameRouterAnswerer anonymous { std::nullopt };
        anonymous.RouteThrough(&router);
        CHECK(Testing::ErrorOf(core::async::syncRun(anonymous.Answer(frame))) == Wire::ErrorCode::Unauthenticated);
    }

    SECTION("and with no router -- a node serving no 0xFC surface -- there is no reply")
    {
        FrameRouterAnswerer unrouted { Testing::IdentityOf("n1") };
        CHECK(core::async::syncRun(unrouted.Answer(frame)).empty());
    }
}
