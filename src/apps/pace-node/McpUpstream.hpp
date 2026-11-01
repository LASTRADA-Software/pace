// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdmitForwarder.hpp"
#include "EndpointDialer.hpp"

#include <Pace/Cluster/ClusterState.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/ProvenIdentity.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

namespace Pace::Cc
{
class IAdvertisedEndpointSource;
} // namespace Pace::Cc

namespace Pace::Node
{

class IFrameResponder;
class ISchedulingLeaderSource;

/// @file McpUpstream.hpp
/// The MCP surface's way to the rest of the fleet: one `0xFC` request frame put to the Raft leader or to
/// a named node, over the same proven and sealed connection the admission relay opens
/// (`ProvenSchedulerDialer`), and the answer handed back to the MCP thread that asked.
///
/// **The MCP thread blocks and the dial does not.** The MCP surface is served on a blocking thread of its
/// own (`McpEndpoint`), so hostile or slow MCP input can only ever stall MCP. A dial, though, belongs on a
/// reactor (`wire-and-protocol.md`: a loop thread dials through `makeConnector`, never
/// `BlockingConnector`), so the request is posted onto the node's I/O loop as a flow of its own and the
/// MCP thread waits for its answer on a condition variable, bounded by `McpUpstream::CallDeadline`.
///
/// **A wait that runs out cancels what it waited for.** The MCP thread does not merely stop listening: it
/// posts the cancellation onto the loop, which closes the connection the flow is parked on, so the flow
/// unwinds and its frame is freed there rather than left parked for a reply nobody will read. Every other
/// wait inside the flow is bounded on its own -- the dial by `ForwardDialBudget`, the exchange by an idle
/// window as long as the call deadline -- so a flow ends even if the cancellation is never needed.

namespace McpUpstream
{
    /// How long the MCP thread waits for a forwarded answer before it reports `UpstreamFailure::Timeout`.
    ///
    /// The whole of one tool call's network leg, redirects included: four dials of at most
    /// `ForwardDialBudget`'s connect and handshake would exceed it, and that is deliberate -- an MCP
    /// client is a person waiting on a model waiting on this, and an answer later than this is worth less
    /// than the sentence saying who could not be reached.
    inline constexpr std::chrono::milliseconds CallDeadline { std::chrono::seconds { 10 } };

    /// The largest reply payload a forwarded call accepts, in bytes.
    ///
    /// Larger than the admission relay's ceiling (`MaxForwardReplyBytes`, a grant's few hundred bytes)
    /// because what MCP forwards is a REPORT -- the fleet document among them -- and smaller than the
    /// wire's report ceiling (`CompileCacheWire::MaxReportReply`), because the answer is held whole in
    /// this process and re-rendered into one MCP response.
    inline constexpr std::size_t MaxReplyBytes = std::size_t { 16 } * 1024U * 1024U;
} // namespace McpUpstream

/// Whom a forwarded frame is put to.
///
/// **PRIVATE: persisted and transmitted nowhere.** No explicit values, so nothing reads an ordinal as a
/// contract; `Last` is the length of a table keyed by one.
enum class UpstreamTarget : std::uint8_t
{
    Leader, ///< Whoever leads the fleet now, `NotLeader` followed (fleet-scope tools).
    Node,   ///< One named member, never redirected (node-scope tools); empty name means this node.
    Last,   ///< Not a target.
};

/// What a forwarded frame was answered with.
struct UpstreamReply
{
    /// The reply's terminal status: `Ok` or `Miss`. A refusal is `UpstreamFailure::Refused`, never a reply.
    CompileCacheWire::Status status { CompileCacheWire::Status::Ok };

    /// The reply's payload, as the wire carried it.
    std::string payload;

    /// The `0xFC` endpoint that answered -- the redirect's target when one was followed, and this node's
    /// own advertised endpoint (or, with none advertised, its id) when it answered in-process.
    std::string answeredBy;

    /// The consensus term this node held when the answer arrived; DISENGAGED when this node knows no
    /// term (it runs no consensus, or has observed no role yet).
    ///
    /// **This node's term, not a field of the reply**: no `0xFC` reply carries one. A node that follows
    /// a leader holds that leader's term, so for a fleet-scope answer the two agree once leadership has
    /// settled; during an election they may not, and a redirect followed to a node this one has not yet
    /// heard from is answered under a term this node has not yet adopted. Optional rather than zero,
    /// because zero is a term a reader would believe.
    std::optional<std::uint64_t> term;
};

/// Why a forwarded frame was not answered.
///
/// **PRIVATE: persisted and transmitted nowhere.** No explicit values; `Last` is the length of
/// `UpstreamFailureTable`.
enum class UpstreamFailure : std::uint8_t
{
    NoLeaderKnown,    ///< No leader is known here, or the leader asked answered that none is.
    UnknownNode,      ///< No member of the applied cluster state has the id asked for.
    Unreachable,      ///< Nothing answered, the proof handshake went unanswered, or the answer was lost.
    NotReady,         ///< The node asked cannot judge this node's proof yet: transient, ask again shortly.
    NoProofOffered,   ///< The node asked serves no proof: it runs no consensus, so it is no member scheduler.
    PeerUntrusted,    ///< THIS node declined to prove itself: the key the node asked presented is not trusted.
    ProofRefused,     ///< The node asked answered, and refused this node's proof of its identity.
    Timeout,          ///< No answer within `McpUpstream::CallDeadline`; the flow was cancelled on the loop.
    TooManyRedirects, ///< The leader moved more often than `MaxLeaderRedirects` follows.
    Refused,          ///< The authority answered, with a refusal.
    LoopNotRunning,   ///< This node's `0xFC` I/O loop is not turning, so nothing could be asked at all.
    Last,             ///< Not a failure.
};

/// One failure's row.
struct UpstreamFailureRow
{
    UpstreamFailure failure;   ///< The failure.
    std::string_view name;     ///< A stable lowercase name, for logs and structured results.
    std::string_view sentence; ///< What it means, for a person: the headline the detail follows.
};

/// What each failure is called and means.
inline constexpr EnumTable<UpstreamFailure, UpstreamFailureRow> UpstreamFailureTable { {
    { .failure = UpstreamFailure::NoLeaderKnown,
      .name = "no-leader-known",
      .sentence = "no leader is known to this node right now; an election may be in progress" },
    { .failure = UpstreamFailure::UnknownNode,
      .name = "unknown-node",
      .sentence = "no member of this fleet has that node id" },
    { .failure = UpstreamFailure::Unreachable,
      .name = "unreachable",
      .sentence = "the node asked could not be reached, did not complete the handshake, or was lost before it "
                  "answered" },
    { .failure = UpstreamFailure::NotReady,
      .name = "not-ready",
      .sentence = "the node asked cannot judge this node's identity yet, since its consensus has not caught up; "
                  "ask again shortly" },
    { .failure = UpstreamFailure::NoProofOffered,
      .name = "no-proof-offered",
      .sentence = "the node asked serves no identity proof: it runs no consensus, so it is not a scheduler of "
                  "this fleet" },
    { .failure = UpstreamFailure::PeerUntrusted,
      .name = "peer-untrusted",
      .sentence = "this node declined to prove its identity to the node asked, whose key it does not trust" },
    { .failure = UpstreamFailure::ProofRefused,
      .name = "proof-refused",
      .sentence = "the node asked did not accept this node's proof of its identity" },
    { .failure = UpstreamFailure::Timeout,
      .name = "timeout",
      .sentence = "the node asked did not answer in time, and the request was abandoned" },
    { .failure = UpstreamFailure::TooManyRedirects,
      .name = "too-many-redirects",
      .sentence = "the leader moved more often than this node follows it" },
    { .failure = UpstreamFailure::Refused, .name = "refused", .sentence = "the node asked refused the request" },
    { .failure = UpstreamFailure::LoopNotRunning,
      .name = "loop-not-running",
      .sentence = "this node's 0xFC I/O loop is not running -- it serves no 0xFC surface (an empty --listen-node), "
                  "has not finished starting, or is stopping -- so it cannot reach any node, itself included" },
} };
static_assert(RowsInEnumeratorOrder(UpstreamFailureTable, &UpstreamFailureRow::failure),
              "UpstreamFailureTable must hold one row per UpstreamFailure, in enumerator order");

/// @param failure A failure.
/// @return Its row.
[[nodiscard]] constexpr UpstreamFailureRow const& UpstreamFailureRowOf(UpstreamFailure failure) noexcept
{
    return UpstreamFailureTable[static_cast<std::size_t>(failure)];
}

/// How each way a dial can fail is reported: one row per `UpstreamOpeningFault`.
struct OpeningFaultRow
{
    UpstreamOpeningFault fault; ///< The dial's fault.
    UpstreamFailure failure;    ///< What `Ask` reports for it.
};

/// The dial's faults, as `Ask` reports them. **A handshake nobody answered is `Unreachable`, never
/// `ProofRefused`**: telling a person their node was refused, when the peer was only busy or restarting,
/// is a confident wrong signal.
inline constexpr EnumTable<UpstreamOpeningFault, OpeningFaultRow> OpeningFaultTable { {
    { .fault = UpstreamOpeningFault::Unreached, .failure = UpstreamFailure::Unreachable },
    { .fault = UpstreamOpeningFault::HandshakeIncomplete, .failure = UpstreamFailure::Unreachable },
    { .fault = UpstreamOpeningFault::ProofDeferred, .failure = UpstreamFailure::NotReady },
    { .fault = UpstreamOpeningFault::ProofNotOffered, .failure = UpstreamFailure::NoProofOffered },
    { .fault = UpstreamOpeningFault::PeerUntrusted, .failure = UpstreamFailure::PeerUntrusted },
    { .fault = UpstreamOpeningFault::ProofRefused, .failure = UpstreamFailure::ProofRefused },
} };
static_assert(RowsInEnumeratorOrder(OpeningFaultTable, &OpeningFaultRow::fault),
              "OpeningFaultTable must hold one row per UpstreamOpeningFault, in enumerator order");

/// What `IMcpUpstream::Ask` answers: the reply, or the failure and the words saying who and why.
using UpstreamResult = std::expected<UpstreamReply, std::pair<UpstreamFailure, std::string>>;

/// Puts one `0xFC` frame to the fleet's leader or a named node, and waits for the answer.
class IMcpUpstream
{
  public:
    IMcpUpstream() = default;
    IMcpUpstream(IMcpUpstream const&) = delete;
    IMcpUpstream& operator=(IMcpUpstream const&) = delete;
    IMcpUpstream(IMcpUpstream&&) = delete;
    IMcpUpstream& operator=(IMcpUpstream&&) = delete;
    virtual ~IMcpUpstream() = default;

    /// Put @p frame to @p target and wait, blocking the calling thread, for the answer.
    ///
    /// Called on the MCP thread; never on the loop the answer is fetched on, which this would deadlock.
    /// @param target Whom to ask.
    /// @param nodeIdOrEmpty For `UpstreamTarget::Node`, the member's id, matched exactly -- no prefix
    ///        matching on ids; empty means this node. Ignored for `UpstreamTarget::Leader`.
    /// @param frame A whole `0xFC` request frame, header included.
    /// @return The answer, or why there is none.
    [[nodiscard]] virtual UpstreamResult Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame) = 0;
};

/// Who answers where: what `NodeIoUpstream` resolves a target through.
///
/// A seam rather than the cluster state itself, so a case can place the fleet: what a forwarded call does
/// is a property of who leads and where the members answer, and that is what a test has to be able to
/// say. Every member is read on the MCP thread and on the loop thread, so an implementation is
/// thread-safe.
class IUpstreamDirectory
{
  public:
    IUpstreamDirectory() = default;
    IUpstreamDirectory(IUpstreamDirectory const&) = delete;
    IUpstreamDirectory& operator=(IUpstreamDirectory const&) = delete;
    IUpstreamDirectory(IUpstreamDirectory&&) = delete;
    IUpstreamDirectory& operator=(IUpstreamDirectory&&) = delete;
    virtual ~IUpstreamDirectory() = default;

    /// @return The current leader's `0xFC` endpoint -- this node's own while it leads; empty when no leader is
    ///         known.
    [[nodiscard]] virtual std::string LeaderEndpoint() const = 0;

    /// @param nodeId A member's id, matched exactly.
    /// @return Its `0xFC` endpoint as the applied cluster state records it -- empty for a member that
    ///         has announced none -- or DISENGAGED when no member has that id.
    [[nodiscard]] virtual std::optional<std::string> EndpointOf(std::string_view nodeId) const = 0;

    /// @return This node's id.
    [[nodiscard]] virtual std::string SelfId() const = 0;

    /// @return The `0xFC` endpoint this node advertises; empty when it advertises none.
    [[nodiscard]] virtual std::string SelfEndpoint() const = 0;

    /// @return The consensus term this node holds now, or DISENGAGED when it knows none.
    [[nodiscard]] virtual std::optional<std::uint64_t> Term() const = 0;
};

/// The directory production uses: the leader as the scheduling redirect publishes it, the members as the
/// applied cluster state records them, the term as the consensus role observer reports it.
///
/// Thread-safe: told on the consensus apply thread and the role observer's, read on the MCP thread and
/// the I/O loop's -- the shape `AppliedSchedulers` has for the same reason.
class AppliedUpstreamDirectory final: public IUpstreamDirectory
{
  public:
    /// @param selfId This node's id.
    /// @param leader Where the leader answers, read per call; must outlive this.
    /// @param self The endpoint this node advertises, read per call; must outlive this.
    AppliedUpstreamDirectory(std::string selfId,
                             ISchedulingLeaderSource const& leader,
                             Cc::IAdvertisedEndpointSource const& self);

    /// Take the members @p state records.
    /// @param state The state the cluster just applied.
    void Applied(Cluster::ClusterState const& state);

    /// Take what the consensus role observer just reported: the term, and whether THIS node leads in it.
    ///
    /// **Whether this node leads is told here, because no other source says it**: the scheduling leader this
    /// directory otherwise reads (`ISchedulingLeaderSource`) is empty while this node leads -- it is the
    /// endpoint a redirect names, and a leader redirects nobody to itself. Without it a fleet call on the
    /// leader would answer `NoLeaderKnown` from the one node that knows best.
    /// @param term The term this node now holds.
    /// @param selfLeads Whether this node leads in @p term.
    void ObservedRole(std::uint64_t term, bool selfLeads);

    /// @copydoc IUpstreamDirectory::LeaderEndpoint
    [[nodiscard]] std::string LeaderEndpoint() const override;

    /// @copydoc IUpstreamDirectory::EndpointOf
    [[nodiscard]] std::optional<std::string> EndpointOf(std::string_view nodeId) const override;

    /// @copydoc IUpstreamDirectory::SelfId
    [[nodiscard]] std::string SelfId() const override;

    /// @copydoc IUpstreamDirectory::SelfEndpoint
    [[nodiscard]] std::string SelfEndpoint() const override;

    /// @copydoc IUpstreamDirectory::Term
    [[nodiscard]] std::optional<std::uint64_t> Term() const override;

  private:
    std::string _selfId;                                        ///< This node's id.
    ISchedulingLeaderSource const& _leader;                     ///< Where the leader answers.
    Cc::IAdvertisedEndpointSource const& _self;                 ///< This node's advertised endpoint.
    mutable std::mutex _lock;                                   ///< Guards the three below.
    std::map<std::string, std::string, std::less<>> _endpoints; ///< Member id to `0xFC` endpoint.
    std::optional<std::uint64_t> _term;                         ///< The last term observed.
    bool _selfLeads { false };                                  ///< Whether this node leads in `_term`.
};

/// Answers a frame on this node, without dialling: the self-target path.
///
/// Runs on the I/O loop, as the node's own `0xFC` surfaces do. Production hands it the node's frame
/// router, asked as a caller on this machine; a test hands it a scripted reply.
class IInProcessAnswerer
{
  public:
    IInProcessAnswerer() = default;
    IInProcessAnswerer(IInProcessAnswerer const&) = delete;
    IInProcessAnswerer& operator=(IInProcessAnswerer const&) = delete;
    IInProcessAnswerer(IInProcessAnswerer&&) = delete;
    IInProcessAnswerer& operator=(IInProcessAnswerer&&) = delete;
    virtual ~IInProcessAnswerer() = default;

    /// Answer @p frame as this node's `0xFC` surface would.
    /// @param frame A whole request frame; by value, for the coroutine frame.
    /// @return A whole reply frame; empty when the surface would close without answering.
    [[nodiscard]] virtual core::async::Task<std::vector<std::byte>> Answer(std::vector<std::byte> frame) = 0;
};

/// What a `NodeIoUpstream` is built with beyond its collaborators.
struct UpstreamPolicy
{
    /// How long the MCP thread waits; a test shortens it to place a deadline inside a case.
    std::chrono::milliseconds deadline { McpUpstream::CallDeadline };

    /// How long a remote node may say nothing during one exchange before the flow gives up on it by
    /// itself: the loop's own bound, so a flow ends even when no cancellation reaches it. As long as the
    /// call deadline by default; a test lengthens it so that only the cancellation can end a flow.
    std::chrono::milliseconds idle { McpUpstream::CallDeadline };
};

/// The production `IMcpUpstream`: the call is a flow on the node's I/O loop, awaited on a condition
/// variable.
///
/// **One hop loop of its own, and why not the admission relay's.** `AdmitForwarder::RelayAdmit` follows
/// `NotLeader` too, but its loop is shaped by what only it has -- a connection kept on the client's scope
/// after a grant, a client that can leave mid-dial, a hop bound of `MaxForwardHops` because a launcher
/// is waiting -- and `AskTheLeader` (`EndpointDialer`) is the one-shot verbs' loop, which blocks on a
/// thread it owns and so cannot run on a reactor. What all three share is the RULE, and that is shared:
/// what counts as a redirect is `Cc::RedirectTarget` / `LeaderRedirectTarget`, the bound is
/// `MaxLeaderRedirects`, and the dial is `IUpstreamDialer`.
///
/// **Never to itself.** A target that is this node -- the leader endpoint, a redirect, or the node id
/// naming it -- is answered in-process (`IInProcessAnswerer`) rather than dialled.
///
/// **Only `Leader` follows redirects.** A node-scope frame is put to the node named, and a `NotLeader` it
/// answers is its answer, reported `Refused`.
///
/// Thread model: `Ask` runs on the caller's thread; everything that touches a socket runs on @p loop.
class NodeIoUpstream final: public IMcpUpstream
{
  public:
    /// @param loop The node's I/O loop, which the dialer's sockets belong to; must outlive every flow
    ///        this posts, which is to say must stop before the collaborators below go.
    /// @param dialer How a remote node is reached; must outlive the loop's last turn.
    /// @param directory Who answers where; must outlive the loop's last turn.
    /// @param local How this node answers itself; must outlive the loop's last turn.
    /// @param policy The deadline.
    NodeIoUpstream(core::net::EventLoop& loop,
                   IUpstreamDialer& dialer,
                   IUpstreamDirectory const& directory,
                   IInProcessAnswerer& local,
                   UpstreamPolicy policy = {}) noexcept;

    /// @copydoc IMcpUpstream::Ask
    [[nodiscard]] UpstreamResult Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame) override;

  private:
    core::net::EventLoop& _loop;
    IUpstreamDialer& _dialer;
    IUpstreamDirectory const& _directory;
    IInProcessAnswerer& _local;
    UpstreamPolicy _policy;
};

/// Refuses at once, rather than after `McpUpstream::CallDeadline`, when the loop a forwarded call would run on
/// is not turning.
///
/// **Why this exists.** `NodeIoLoop` starts no thread on a node that adopted no framed surface, and its reactor
/// stops once the last adopted loop ends. A call posted there is never run, so every tool would answer
/// `Timeout` ten seconds later -- a wait that tells a person nothing, ending in a sentence that blames the node
/// asked. Asked per call, so a loop that stops while the node shuts down is reported the same way.
class LoopGatedUpstream final: public IMcpUpstream
{
  public:
    /// @param inner What answers when the loop turns; must outlive this.
    /// @param loopTurning Whether the loop a call would run on is turning now; asked on the calling thread, so
    ///        it is thread-safe.
    LoopGatedUpstream(IMcpUpstream& inner, std::function<bool()> loopTurning) noexcept;

    /// @copydoc IMcpUpstream::Ask
    [[nodiscard]] UpstreamResult Ask(UpstreamTarget target, std::string_view nodeIdOrEmpty, std::string frame) override;

  private:
    IMcpUpstream& _inner;
    std::function<bool()> _loopTurning;
};

/// The production `IInProcessAnswerer`: this node's own `0xFC` frame router, asked as a caller on this machine
/// that proved this node's own identity.
///
/// **Why the caller carries this node's identity.** A fleet read is admitted on the credential or on a proof
/// (`FleetReader::ProvenMember`), and a node asking itself holds no dashboard credential; the proof it would
/// make over a dialled connection is the one it states here, without a handshake to itself. It is true by
/// construction -- nothing but this process reaches this object -- and it admits nothing the roster does not:
/// `ExplainConnection` asks the roster about the key on every verb, so a node whose own key its cluster has not
/// recorded is answered as the loopback caller it also is.
///
/// Runs on the I/O loop, as the router does for a dialled connection.
class FrameRouterAnswerer final: public IInProcessAnswerer
{
  public:
    /// @param self This node's id and public identity key, or nothing when it holds no identity key.
    explicit FrameRouterAnswerer(std::optional<ProvenIdentity> self) noexcept;

    /// Name the router frames are answered by. Called before the loop starts and never after, so the loop
    /// thread reads what was written before it was created.
    /// @param router The router, or null on a node that serves no `0xFC` surface; must outlive the loop's
    ///        last turn.
    void RouteThrough(IFrameResponder* router) noexcept;

    /// @copydoc IInProcessAnswerer::Answer
    [[nodiscard]] core::async::Task<std::vector<std::byte>> Answer(std::vector<std::byte> frame) override;

  private:
    std::optional<ProvenIdentity> _self;
    IFrameResponder* _router { nullptr };
};

} // namespace Pace::Node
