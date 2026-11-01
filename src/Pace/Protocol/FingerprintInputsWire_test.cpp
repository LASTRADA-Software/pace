// SPDX-License-Identifier: Apache-2.0
#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>
#include <Pace/Protocol/FingerprintInputsWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using namespace Pace::CompileCacheWire;
using Pace::Testing::Unwrap;

namespace
{

/// A path longer than any short-string buffer, so a decoded value that borrowed from the payload would read freed
/// memory once the payload is gone -- the arrangement a view-returning decoder fails on.
constexpr std::size_t LongPathBytes = 200;
static_assert(LongPathBytes > sizeof(std::string), "a path that fits a short-string buffer proves nothing about ownership");

/// @return A page of two files, one with a long path.
[[nodiscard]] FingerprintInputsPage SamplePage()
{
    return FingerprintInputsPage {
        .fingerprint = "0123456789abcdef0123456789abcdef",
        .version = "1.2.3",
        .cursor = 3,
        .totalFiles = 9,
        .nextCursor = 5,
        .inputs =
            Cc::FingerprintInputs {
                .schema = std::string { Cc::FingerprintSchema },
                .banner = "gcc (GCC) 13.2.0",
                .driverGrammar = "gnu",
                .files = { Cc::ToolchainFile { .relativePath = std::string(LongPathBytes, 'p'), .contentHash = "aa" },
                           Cc::ToolchainFile { .relativePath = "c++/13/vector", .contentHash = "bb" } } },
    };
}

} // namespace

TEST_CASE("FINGERPRINT-INPUTS has its specified opcode byte", "[wire][fingerprint-inputs]")
{
    // The BYTE, not the symbol, and the raw enumerator beside it: every in-tree caller spells the enumerator, so a
    // consistent renumbering leaves the suite green while every deployed peer reads another verb.
    CHECK(static_cast<std::uint8_t>(Op::FingerprintInputs) == 0x25);
    REQUIRE(FindOp(0x25) != nullptr);
    CHECK(FindOp(0x25)->code == Op::FingerprintInputs);
}

TEST_CASE("FINGERPRINT-INPUTS is a gated member read in the node family, bounded both ways", "[wire][fingerprint-inputs]")
{
    auto const* const row = FindOp(static_cast<std::uint8_t>(Op::FingerprintInputs));
    REQUIRE(row != nullptr);
    CHECK(row->name == "fingerprint-inputs");
    CHECK(row->fieldCount == 2);
    CHECK(row->family == VerbFamily::Node);
    CHECK_FALSE(row->preAuth.Allowed());
    CHECK(row->identity == IdentityRequirement::AddressAdmits);
    CHECK(row->maxPayload.Bytes() == MaxControlPayload);
    CHECK(row->maxReply.Bytes() == FingerprintInputsReplyCeiling);
    CHECK(FingerprintInputsReplyCeiling == 4U * 1024U * 1024U);
    CHECK(row->legalStatuses == static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)));
}

TEST_CASE("A FINGERPRINT-INPUTS request round-trips, and a malformed one is refused", "[wire][fingerprint-inputs]")
{
    auto const frame = EncodeFingerprintInputsRequest(
        FingerprintInputsRequest { .fingerprint = "0123456789abcdef0123456789abcdef", .cursor = 0x0102030405060708ULL });
    auto const header = DecodeRequestHeader(frame);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == 0x25);
    auto const decoded = DecodeFingerprintInputsRequest(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).fingerprint == "0123456789abcdef0123456789abcdef");
    CHECK(Unwrap(decoded).cursor == 0x0102030405060708ULL);

    // A fingerprint past the ceiling, and a cursor of the wrong width, are refused.
    auto const tooLong = EncodeFingerprintInputsRequest(
        FingerprintInputsRequest { .fingerprint = std::string(MaxToolchainFingerprintBytes + 1, 'f'), .cursor = 0 });
    CHECK_FALSE(
        DecodeFingerprintInputsRequest(std::span<std::byte const> { tooLong }.subspan(RequestHeaderSize)).has_value());
    auto const shortCursor = WireFields::Encode({ AsBytes("ab"), AsBytes("1234") });
    CHECK_FALSE(DecodeFingerprintInputsRequest(shortCursor).has_value());
}

TEST_CASE("A FINGERPRINT-INPUTS page round-trips and owns what it decoded", "[wire][fingerprint-inputs]")
{
    auto const page = SamplePage();
    auto decoded = std::optional<FingerprintInputsPage> {};
    {
        // Decoded from a buffer that is then dropped and its storage churned: a borrowed field reads garbage.
        auto payload = EncodeFingerprintInputsPage(page);
        CHECK(FingerprintInputsReplyKindOf(payload) == FingerprintInputsReplyKind::Inputs);
        decoded = DecodeFingerprintInputsPage(payload);
        std::ranges::fill(payload, std::byte { 0xEE });
    }
    auto churn = std::vector<std::byte>(4096, std::byte { 0xCD });
    static_cast<void>(churn);
    REQUIRE(decoded.has_value());
    auto const& got = Unwrap(decoded);
    CHECK(got.fingerprint == page.fingerprint);
    CHECK(got.version == page.version);
    CHECK(got.cursor == 3);
    CHECK(got.totalFiles == 9);
    CHECK(got.nextCursor == std::optional<std::uint64_t> { 5 });
    CHECK(got.inputs.schema == page.inputs.schema);
    CHECK(got.inputs.banner == page.inputs.banner);
    CHECK(got.inputs.driverGrammar == page.inputs.driverGrammar);
    REQUIRE(got.inputs.files.size() == 2);
    CHECK(got.inputs.files[0].relativePath == std::string(LongPathBytes, 'p'));
    CHECK(got.inputs.files[1].contentHash == "bb");

    // The last page carries no next cursor at all.
    auto last = page;
    last.nextCursor.reset();
    auto const lastDecoded = DecodeFingerprintInputsPage(EncodeFingerprintInputsPage(last));
    REQUIRE(lastDecoded.has_value());
    CHECK_FALSE(Unwrap(lastDecoded).nextCursor.has_value());
}

TEST_CASE("A FINGERPRINT-INPUTS page whose next cursor would loop or overrun is refused", "[wire][fingerprint-inputs]")
{
    for (auto const next: { std::uint64_t { 3 }, std::uint64_t { 2 }, std::uint64_t { 10 } })
    {
        INFO("next " << next);
        auto page = SamplePage();
        page.nextCursor = next;
        CHECK_FALSE(DecodeFingerprintInputsPage(EncodeFingerprintInputsPage(page)).has_value());
    }
    // An index is not a page, and a page is not an index.
    auto const index = EncodeFingerprintIndex(FingerprintIndexFields { .version = "1", .total = 0, .entries = {} });
    CHECK_FALSE(DecodeFingerprintInputsPage(index).has_value());
    CHECK_FALSE(DecodeFingerprintIndex(EncodeFingerprintInputsPage(SamplePage())).has_value());
}

TEST_CASE("A FINGERPRINT-INPUTS index round-trips, and its entries cannot outnumber its total", "[wire][fingerprint-inputs]")
{
    auto const index = FingerprintIndexFields {
        .version = "1.2.3",
        .total = 3,
        .entries = { FingerprintIndexEntry { .fingerprint = "f1",
                                             .label = "gcc 13.2.0",
                                             .compiler = "/usr/bin/gcc",
                                             .state = FingerprintInputsState::Recorded },
                     FingerprintIndexEntry { .fingerprint = "f2",
                                             .label = {},
                                             .compiler = "/opt/pinned/g++",
                                             .state = FingerprintInputsState::NotYetCaptured } },
    };
    auto const payload = EncodeFingerprintIndex(index);
    CHECK(FingerprintInputsReplyKindOf(payload) == FingerprintInputsReplyKind::Index);
    auto const decoded = DecodeFingerprintIndex(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).total == 3);
    REQUIRE(Unwrap(decoded).entries.size() == 2);
    CHECK(Unwrap(decoded).entries[0].label == "gcc 13.2.0");
    CHECK(Unwrap(decoded).entries[0].state == FingerprintInputsState::Recorded);
    CHECK(Unwrap(decoded).entries[1].compiler == "/opt/pinned/g++");
    CHECK(Unwrap(decoded).entries[1].state == FingerprintInputsState::NotYetCaptured);

    auto overfull = index;
    overfull.total = 1;
    CHECK_FALSE(DecodeFingerprintIndex(EncodeFingerprintIndex(overfull)).has_value());
}

TEST_CASE("The reply kinds keep their bytes", "[wire][fingerprint-inputs]")
{
    // And the index's per-entry state: why a node holds no inputs is part of the answer, so its byte is pinned and a
    // byte past the last is refused rather than read as a reason it is not.
    CHECK(static_cast<std::uint8_t>(FingerprintInputsState::Recorded) == 0);
    CHECK(static_cast<std::uint8_t>(FingerprintInputsState::Pinned) == 1);
    CHECK(static_cast<std::uint8_t>(FingerprintInputsState::NotYetCaptured) == 2);
    CHECK(static_cast<std::uint8_t>(FingerprintInputsState::NotCaptured) == 3);
    CHECK(static_cast<std::uint8_t>(FingerprintInputsState::Last) == 4);
    auto const foreign = std::array { std::byte { 4 } };
    auto const entries =
        WireFields::Encode({ AsBytes("f1"), AsBytes("l"), AsBytes("/c"), std::span<std::byte const> { foreign } });
    auto const kind = std::array { std::byte { 0 } };
    auto const total = EncodeU32Field(1);
    auto const index = WireFields::Encode({ std::span<std::byte const> { kind },
                                            AsBytes("1"),
                                            std::span<std::byte const> { total },
                                            std::span<std::byte const> { entries } });
    CHECK_FALSE(DecodeFingerprintIndex(index).has_value());

    CHECK(static_cast<std::uint8_t>(FingerprintInputsReplyKind::Index) == 0);
    CHECK(static_cast<std::uint8_t>(FingerprintInputsReplyKind::Inputs) == 1);
    CHECK(static_cast<std::uint8_t>(FingerprintInputsReplyKind::Last) == 2);
}

TEST_CASE("A page's overhead and entry sizes are what the encoder writes", "[wire][fingerprint-inputs]")
{
    // The responder packs pages by these numbers; if they understate what the encoder writes, a page overruns the
    // ceiling it was packed under.
    auto const page = SamplePage();
    auto expected = FingerprintInputsPageOverhead(page.fingerprint, page.version, page.inputs);
    for (auto const& file: page.inputs.files)
        expected += EncodedFileBytes(file);
    CHECK(EncodeFingerprintInputsPage(page).size() == expected);

    auto const index = FingerprintIndexFields {
        .version = "1.2.3",
        .total = 1,
        .entries = { FingerprintIndexEntry {
            .fingerprint = "f1", .label = "l", .compiler = "/c", .state = FingerprintInputsState::Pinned } },
    };
    CHECK(EncodeFingerprintIndex(index).size()
          == FingerprintIndexOverhead(index.version) + EncodedIndexEntryBytes(index.entries[0]));
}
