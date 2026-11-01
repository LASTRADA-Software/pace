// SPDX-License-Identifier: Apache-2.0
#include "FingerprintInputsResponder.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeToolchains.hpp"

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/FingerprintInputsWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::Node;
using Pace::Testing::Unwrap;

namespace Wire = Pace::CompileCacheWire;

namespace
{

/// The version every case's responder names.
constexpr std::string_view Version = "9.8.7";

/// A source answering one published survey, or none.
class FixedServed final: public IServedInputsSource
{
  public:
    /// @param served What the survey published; null for a survey that has not concluded.
    explicit FixedServed(std::shared_ptr<ServedInputs const> served):
        _served { std::move(served) }
    {
    }

    /// @copydoc IServedInputsSource::ServedInputsNow
    [[nodiscard]] std::shared_ptr<ServedInputs const> ServedInputsNow() const override
    {
        return _served;
    }

  private:
    std::shared_ptr<ServedInputs const> _served;
};

/// @param count How many files.
/// @param pathBytes How long each path is, at least.
/// @return A record of that many files, captured as a survey captures one.
[[nodiscard]] Cc::FingerprintInputs RecordOf(std::size_t count, std::size_t pathBytes = 0)
{
    auto files = std::vector<Cc::ToolchainFile> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
    {
        auto path = std::format("include/c++/13/bits/h{:05}.h", index);
        if (path.size() < pathBytes)
            path.append(pathBytes - path.size(), 'p');
        files.push_back(Cc::ToolchainFile { .relativePath = std::move(path), .contentHash = std::format("{:032x}", index) });
    }
    return Cc::CaptureFingerprintInputs("gcc (GCC) 13.2.0", "gnu", std::move(files));
}

/// @param inputs A record.
/// @return A survey serving it under its own fingerprint, plus a pinned toolchain with no record.
[[nodiscard]] std::shared_ptr<ServedInputs const> SurveyOf(Cc::FingerprintInputs inputs)
{
    auto const fingerprint = Cc::ComputeToolchainFingerprint(inputs);
    return std::make_shared<ServedInputs const>(ServedInputs {
        ServedToolchainInputs { .fingerprint = fingerprint,
                                .label = "gcc 13.2.0",
                                .compiler = "/usr/bin/gcc",
                                .inputs = std::make_shared<Cc::FingerprintInputs const>(std::move(inputs)) },
        ServedToolchainInputs { .fingerprint = "pinned00000000000000000000000000",
                                .label = {},
                                .compiler = "/opt/pinned/g++",
                                .inputs = nullptr },
    });
}

/// @param fingerprint What to ask about; empty for the index.
/// @param cursor Where the page starts.
/// @return The request payload, as the tool sends it.
[[nodiscard]] std::vector<std::byte> RequestPayload(std::string fingerprint, std::uint64_t cursor)
{
    auto const frame = Wire::EncodeFingerprintInputsRequest(
        Wire::FingerprintInputsRequest { .fingerprint = std::move(fingerprint), .cursor = cursor });
    return { frame.begin() + static_cast<std::ptrdiff_t>(Wire::RequestHeaderSize), frame.end() };
}

/// A reply, read back: its status, its payload, and the refusal it carries if any.
struct Reply
{
    Wire::Status status {};                 ///< What it said.
    std::vector<std::byte> payload;         ///< Its body.
    std::optional<Wire::ErrorCode> code {}; ///< The refusal code, for an error.
    std::string detail;                     ///< The refusal's words.
};

/// @param frame A reply frame.
/// @return It read back.
[[nodiscard]] Reply ReadReply(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    REQUIRE(header.has_value());
    auto reply = Reply {};
    reply.status = Unwrap(header).status;
    reply.payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(Wire::ReplyHeaderSize), frame.end());
    REQUIRE(reply.payload.size() == Unwrap(header).payloadLength);
    if (reply.status == Wire::Status::Error)
    {
        auto const refusal = Wire::DecodeErrorPayload(reply.payload);
        REQUIRE(refusal.has_value());
        reply.code = Unwrap(refusal).first;
        reply.detail = std::string { Unwrap(refusal).second };
    }
    return reply;
}

/// Page through @p fingerprint's whole record the way the tool does.
/// @param responder What answers.
/// @param fingerprint The fingerprint.
/// @return Every page, in order.
[[nodiscard]] std::vector<Wire::FingerprintInputsPage> AllPages(FingerprintInputsResponder const& responder,
                                                                std::string const& fingerprint)
{
    auto pages = std::vector<Wire::FingerprintInputsPage> {};
    auto cursor = std::uint64_t { 0 };
    // Bounded by the record: a responder that stalled at one cursor would otherwise loop forever.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 10000))
    {
        auto const reply = ReadReply(responder.Answer(RequestPayload(fingerprint, cursor)));
        REQUIRE(reply.status == Wire::Status::Ok);
        CHECK(reply.payload.size() <= Wire::FingerprintInputsReplyCeiling);
        auto page = Wire::DecodeFingerprintInputsPage(reply.payload);
        REQUIRE(page.has_value());
        auto const next = Unwrap(page).nextCursor;
        pages.push_back(Unwrap(page));
        if (!next.has_value())
            return pages;
        cursor = Unwrap(next);
    }
    FAIL("the responder never answered a last page");
    return pages;
}

} // namespace

TEST_CASE("FINGERPRINT-INPUTS pages across cursor boundaries and loses nothing", "[node][fingerprint-inputs]")
{
    AtomicMetricsSink metrics;
    auto const record = RecordOf(10);
    FixedServed const source { SurveyOf(record) };
    FingerprintInputsResponder const responder { &source, metrics, std::string { Version }, 3 };
    auto const fingerprint = Cc::ComputeToolchainFingerprint(record);

    auto const pages = AllPages(responder, fingerprint);

    // Ten files at three a page: 3, 3, 3, 1 -- every boundary exercised, the last one short.
    REQUIRE(pages.size() == 4);
    auto gathered = std::vector<Cc::ToolchainFile> {};
    for (auto const& page: pages)
    {
        CHECK(page.fingerprint == fingerprint);
        CHECK(page.version == Version);
        CHECK(page.totalFiles == 10);
        CHECK(page.inputs.schema == record.schema);
        CHECK(page.inputs.banner == record.banner);
        CHECK(page.inputs.driverGrammar == record.driverGrammar);
        CHECK(page.cursor == gathered.size());
        gathered.insert(gathered.end(), page.inputs.files.begin(), page.inputs.files.end());
    }
    CHECK(pages[0].inputs.files.size() == 3);
    CHECK(pages[3].inputs.files.size() == 1);
    REQUIRE(gathered.size() == record.files.size());
    CHECK(std::ranges::equal(gathered, record.files, [](Cc::ToolchainFile const& a, Cc::ToolchainFile const& b) {
        return a.relativePath == b.relativePath && a.contentHash == b.contentHash;
    }));

    // And the pages, put back together, are the record: it recomputes to the fingerprint it was filed under.
    auto rebuilt = pages.front().inputs;
    rebuilt.files = gathered;
    CHECK(Cc::ComputeToolchainFingerprint(rebuilt) == fingerprint);
}

TEST_CASE("FINGERPRINT-INPUTS cuts a page to the reply ceiling by bytes", "[node][fingerprint-inputs]")
{
    // Large paths, the default page size: the BYTES decide, every page stays under the ceiling, and still nothing is
    // lost. 600 files of 16 KiB paths is ~9.4 MiB, so at least three pages.
    AtomicMetricsSink metrics;
    constexpr std::size_t PathBytes = std::size_t { 16 } * 1024U;
    auto const record = RecordOf(600, PathBytes);
    FixedServed const source { SurveyOf(record) };
    FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };

    auto const pages = AllPages(responder, Cc::ComputeToolchainFingerprint(record));
    CHECK(pages.size() >= 3);
    auto total = std::size_t { 0 };
    for (auto const& page: pages)
        total += page.inputs.files.size();
    CHECK(total == 600);
}

TEST_CASE("FINGERPRINT-INPUTS answers a cursor past the end with an empty last page", "[node][fingerprint-inputs]")
{
    AtomicMetricsSink metrics;
    auto const record = RecordOf(4);
    FixedServed const source { SurveyOf(record) };
    FingerprintInputsResponder const responder { &source, metrics, std::string { Version }, 3 };

    for (auto const cursor: { std::uint64_t { 4 }, std::uint64_t { 5 }, std::uint64_t { 0xFFFF'FFFF'FFFF'FFFFULL } })
    {
        INFO("cursor " << cursor);
        auto const reply = ReadReply(responder.Answer(RequestPayload(Cc::ComputeToolchainFingerprint(record), cursor)));
        REQUIRE(reply.status == Wire::Status::Ok);
        auto const page = Wire::DecodeFingerprintInputsPage(reply.payload);
        REQUIRE(page.has_value());
        CHECK(Unwrap(page).inputs.files.empty());
        CHECK_FALSE(Unwrap(page).nextCursor.has_value());
        CHECK(Unwrap(page).totalFiles == 4);
        CHECK(Unwrap(page).cursor == 4);
    }
}

TEST_CASE("FINGERPRINT-INPUTS refuses an entry no reply can carry, rather than overflowing the ceiling",
          "[node][fingerprint-inputs]")
{
    AtomicMetricsSink metrics;
    auto files = std::vector<Cc::ToolchainFile> {
        Cc::ToolchainFile { .relativePath = "a.h", .contentHash = "00" },
        Cc::ToolchainFile { .relativePath = std::string(Wire::FingerprintInputsReplyCeiling + 1, 'z'), .contentHash = "11" },
    };
    auto const record = Cc::CaptureFingerprintInputs("gcc 13", "gnu", std::move(files));
    FixedServed const source { SurveyOf(record) };
    FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };
    auto const fingerprint = Cc::ComputeToolchainFingerprint(record);

    // The first page holds the small file and stops before the huge one.
    auto const first = ReadReply(responder.Answer(RequestPayload(fingerprint, 0)));
    REQUIRE(first.status == Wire::Status::Ok);
    auto const page = Wire::DecodeFingerprintInputsPage(first.payload);
    REQUIRE(page.has_value());
    CHECK(Unwrap(page).inputs.files.size() == 1);
    REQUIRE(Unwrap(page).nextCursor == std::optional<std::uint64_t> { 1 });

    // The page that would hold the huge one alone is refused by name, and the reply is a refusal, never an overflow.
    auto const second = ReadReply(responder.Answer(RequestPayload(fingerprint, 1)));
    CHECK(second.status == Wire::Status::Error);
    CHECK(second.code == Wire::ErrorCode::PayloadTooLarge);
    CHECK(second.detail.contains("file 1"));
    CHECK(second.payload.size() < Wire::FingerprintInputsReplyCeiling);
}

TEST_CASE("FINGERPRINT-INPUTS lists the served toolchains, and says which hold a record", "[node][fingerprint-inputs]")
{
    AtomicMetricsSink metrics;
    auto const record = RecordOf(2);
    FixedServed const source { SurveyOf(record) };
    FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };

    auto const reply = ReadReply(responder.Answer(RequestPayload({}, 0)));
    REQUIRE(reply.status == Wire::Status::Ok);
    REQUIRE(Wire::FingerprintInputsReplyKindOf(reply.payload) == Wire::FingerprintInputsReplyKind::Index);
    auto const index = Wire::DecodeFingerprintIndex(reply.payload);
    REQUIRE(index.has_value());
    CHECK(Unwrap(index).version == Version);
    CHECK(Unwrap(index).total == 2);
    REQUIRE(Unwrap(index).entries.size() == 2);
    CHECK(Unwrap(index).entries[0].fingerprint == Cc::ComputeToolchainFingerprint(record));
    CHECK(Unwrap(index).entries[0].label == "gcc 13.2.0");
    CHECK(Unwrap(index).entries[0].compiler == "/usr/bin/gcc");
    CHECK(Unwrap(index).entries[0].recorded);
    CHECK_FALSE(Unwrap(index).entries[1].recorded);
}

TEST_CASE("FINGERPRINT-INPUTS says why there is no record, by which refusal", "[node][fingerprint-inputs]")
{
    AtomicMetricsSink metrics;
    auto const record = RecordOf(2);

    SECTION("a fingerprint this node does not serve")
    {
        FixedServed const source { SurveyOf(record) };
        FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };
        auto const reply = ReadReply(responder.Answer(RequestPayload("ffffffffffffffffffffffffffffffff", 0)));
        CHECK(reply.code == Wire::ErrorCode::NoWorker);
        CHECK(reply.detail.contains("serves no toolchain with fingerprint ffffffffffffffffffffffffffffffff"));
    }
    SECTION("an operator's pin, never probed")
    {
        FixedServed const source { SurveyOf(record) };
        FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };
        auto const reply = ReadReply(responder.Answer(RequestPayload("pinned00000000000000000000000000", 0)));
        CHECK(reply.code == Wire::ErrorCode::NoWorker);
        CHECK(reply.detail.contains("never probed"));
    }
    SECTION("a survey that has not concluded")
    {
        FixedServed const source { nullptr };
        FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };
        auto const reply = ReadReply(responder.Answer(RequestPayload({}, 0)));
        CHECK(reply.code == Wire::ErrorCode::WorkerToolchainSurveyInFlight);
    }
    SECTION("a node that runs no worker")
    {
        FingerprintInputsResponder const responder { nullptr, metrics, std::string { Version } };
        auto const reply = ReadReply(responder.Answer(RequestPayload({}, 0)));
        CHECK(reply.code == Wire::NoCompileWorker::Code);
        CHECK(Wire::SaysNoCompileWorker(Wire::NoCompileWorker::Code, reply.detail));
    }
    // None of these is an event an operator acts on.
    CHECK(metrics.Read(IMetricsSink::Counter::NodeFingerprintInputsRefusedMalformed) == 0);
}

TEST_CASE("FINGERPRINT-INPUTS counts a malformed request, and only that", "[node][fingerprint-inputs]")
{
    AtomicMetricsSink metrics;
    FixedServed const source { SurveyOf(RecordOf(1)) };
    FingerprintInputsResponder const responder { &source, metrics, std::string { Version } };

    auto const tooLong = RequestPayload(std::string(Wire::MaxToolchainFingerprintBytes + 1, 'f'), 0);
    for (auto const& payload: { std::vector<std::byte> {}, tooLong })
    {
        auto const reply = ReadReply(responder.Answer(payload));
        CHECK(reply.code == Wire::ErrorCode::MalformedFrame);
    }
    // A frame that did not carry all of its payload is malformed too.
    CHECK(ReadReply(responder.Answer(std::nullopt)).code == Wire::ErrorCode::MalformedFrame);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeFingerprintInputsRefusedMalformed) == 3);
}

TEST_CASE("A served map publishes each toolchain's inputs, and a later survey keeps an unchanged one's",
          "[node][fingerprint-inputs]")
{
    // The record is the one the survey captured, shared rather than copied; a later survey that took the cache for
    // the same fingerprint brought none, and keeps the earlier one under the same key -- never under another.
    auto const record = std::make_shared<Cc::FingerprintInputs const>(RecordOf(3));
    auto previous = std::map<std::string, ServedToolchain> {
        { "fp-a",
          ServedToolchain {
              .compiler = "/usr/bin/gcc", .label = "gcc 13", .family = "gcc", .witness = std::nullopt, .inputs = record } },
    };
    auto next = std::map<std::string, ServedToolchain> {
        { "fp-a",
          ServedToolchain {
              .compiler = "/usr/bin/gcc", .label = "gcc 13", .family = "gcc", .witness = std::nullopt, .inputs = nullptr } },
        { "fp-b",
          ServedToolchain {
              .compiler = "/usr/bin/g++", .label = "g++ 13", .family = "g++", .witness = std::nullopt, .inputs = nullptr } },
    };
    CarryInputsForward(previous, next);
    CHECK(next.at("fp-a").inputs == record);
    CHECK(next.at("fp-b").inputs == nullptr);

    auto const published = ServedInputsOf(next);
    REQUIRE(published->size() == 2);
    CHECK((*published)[0].fingerprint == "fp-a");
    CHECK((*published)[0].inputs == record);
    CHECK((*published)[1].compiler == "/usr/bin/g++");

    // Through the runtime state the worker publishes into: nothing before a survey concludes, then exactly that.
    auto runtime = NodeRuntimeState { ToolchainReading {} };
    CHECK(runtime.ServedInputsNow() == nullptr);
    runtime.PublishServedInputs(published);
    CHECK(runtime.ServedInputsNow() == published);
}
