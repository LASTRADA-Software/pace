// SPDX-License-Identifier: Apache-2.0
#include <Pace/Consensus/DurableFile.hpp>
#include <Pace/Core/ByteAppender.hpp>
#include <Pace/Core/Crc32c.hpp>
#include <Pace/Core/Endian.hpp>
#include <Pace/Distributed/HistoryEnvelope.hpp>

#include <algorithm>
#include <fstream>
#include <span>
#include <utility>

namespace Pace::Distributed::HistoryEnvelope
{

void AppendU64(std::string& out, std::uint64_t value)
{
    // Big-endian, because that is what every other encoder in this tree writes; the choice is arbitrary for a
    // private file and consistency is not. These records are `[u64 length][bytes]`, which is NOT the
    // `[u32 length][bytes]` grammar `AppendField` writes, so the framing stays here and only the integer is shared.
    ByteAppender { out }.AppendU64(value);
}

std::uint64_t ReadU64(std::string_view bytes, std::size_t offset) noexcept
{
    return ReadBigEndian<std::uint64_t>(
        std::span { reinterpret_cast<std::byte const*>(bytes.data()) + offset, sizeof(std::uint64_t) });
}

std::string FramedBuffer()
{
    return std::string(EnvelopeSize, char { 0 });
}

bool WriteFramed(std::filesystem::path const& path, FileEnvelope const& envelope, std::string& file, StateFile which)
{
    std::string_view const body { file.data() + EnvelopeSize, file.size() - EnvelopeSize };

    std::string header;
    header.reserve(EnvelopeSize);
    header.append(envelope.magic.data(), envelope.magic.size());
    header.push_back(static_cast<char>(envelope.newest));
    AppendU64(header, body.size());
    AppendU64(header, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    std::ranges::copy(header, file.begin());

    // Through the node's one durable writer: a crash leaves the previous file whole rather than
    // half of this one, and what lands is synced and its close checked, as every state file is.
    return Consensus::ReplaceFileAtomically(path, std::as_bytes(std::span { file }), which).has_value();
}

FramedFile ReadFramed(std::filesystem::path const& path, FileEnvelope const& envelope)
{
    std::ifstream in { path, std::ios::binary };
    if (!in)
        return {};

    // Sized and read in one go rather than through `istreambuf_iterator`: GCC at
    // -O3 inlines that far enough to trip `-Werror=null-dereference`, which this
    // codebase has already worked around twice.
    in.seekg(0, std::ios::end);
    auto const end = in.tellg();
    if (end < 0)
        return {};
    in.seekg(0, std::ios::beg);

    std::string raw(static_cast<std::size_t>(end), char { 0 });
    in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (in.gcount() != static_cast<std::streamsize>(raw.size()))
        return {};

    if (raw.size() < EnvelopeSize)
        return {};
    if (!std::equal(envelope.magic.begin(), envelope.magic.end(), raw.begin()))
        return {};

    auto const version = static_cast<std::uint8_t>(raw[envelope.magic.size()]);
    // The magic matched, so this IS one of ours -- and a version above the newest
    // this build writes was written by a LATER one. Everything else here is
    // recoverable by waiting; overwriting a year of readings that the newer build
    // could still have read is not.
    if (version > envelope.newest)
        return FramedFile { .outcome = FramedOutcome::Newer, .version = version, .body = {} };

    auto const declared = ReadU64(raw, envelope.magic.size() + 1);
    auto const checksum = ReadU64(raw, envelope.magic.size() + 1 + sizeof(std::uint64_t));
    // The header is dropped rather than copied past: what is left IS the body.
    raw.erase(0, EnvelopeSize);
    if (raw.size() != declared)
        return {};
    if (Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(raw.data()), raw.size() }) != checksum)
        return {};

    return FramedFile { .outcome = FramedOutcome::Ok, .version = version, .body = std::move(raw) };
}

} // namespace Pace::Distributed::HistoryEnvelope
