// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Core/StateFiles.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

/// @file HistoryEnvelope.hpp
/// The framing every history store's file shares: magic, version, body length, CRC32C, the one durable writer -- and
/// the refusal to write over a file a LATER build wrote.
///
/// **That rule belongs to the ENVELOPE rather than to each store** (`metrics-and-observability.md`): the second store
/// was written by copying the first and lost exactly that clause. Shared here, a store gets the durability, the
/// checksum and the never-overwrite-a-newer-file rule by naming a `FileEnvelope`, rather than by somebody remembering
/// to copy them. Internal to the history stores; nothing on a wire reads it.

namespace Pace::Distributed::HistoryEnvelope
{

/// What a store's file says about itself.
///
/// A file that could not say which build wrote it makes an upgrade a silent corruption. An OLDER version is not
/// necessarily an error: a store may carry a reader for one and inflate it forward.
struct FileEnvelope
{
    std::array<char, 4> magic; ///< What this file is.
    std::uint8_t newest;       ///< The newest version this build writes.
};

/// Magic, version byte, body length, body checksum.
inline constexpr std::size_t EnvelopeSize = 4 + 1 + (sizeof(std::uint64_t) * 2);

/// Append one big-endian `uint64`: this file family's one integer spelling.
/// @param out The buffer.
/// @param value Host-order value.
void AppendU64(std::string& out, std::uint64_t value);

/// Read one big-endian `uint64`.
/// @param bytes The buffer; at least `offset + 8` bytes long.
/// @param offset Where the word starts.
/// @return The value.
[[nodiscard]] std::uint64_t ReadU64(std::string_view bytes, std::size_t offset) noexcept;

/// A buffer with the header's bytes reserved in front, ready for a body.
///
/// The body is appended straight into the buffer that becomes the file, so a large store is not copied twice.
/// @return The buffer, `EnvelopeSize` placeholder bytes long.
[[nodiscard]] std::string FramedBuffer();

/// Stamp the header onto such a buffer and put it where a crash cannot leave half of it.
/// @param path Where the file belongs.
/// @param envelope What to stamp.
/// @param file The whole file: header space, then the body.
/// @param which Which state file it is, and so who may read it.
/// @return True when the file was replaced.
[[nodiscard]] bool WriteFramed(std::filesystem::path const& path,
                               FileEnvelope const& envelope,
                               std::string& file,
                               StateFile which);

/// What reading a framed file produced.
///
/// **Private: never transmitted or persisted.**
enum class FramedOutcome : std::uint8_t
{
    Ok,      ///< The body is there and its checksum matched.
    Missing, ///< Absent, short, not ours, or damaged: start empty, overwrite freely.
    Newer,   ///< Ours, and written by a LATER build: start empty and NEVER write.
};

/// A framed file's contents.
///
/// `body` OWNS its bytes rather than viewing a buffer this returned by value: a struct a decoder hands back must not
/// borrow from what it decoded, or the obvious spelling is a use-after-free.
struct FramedFile
{
    FramedOutcome outcome { FramedOutcome::Missing }; ///< Whether there is anything to read.
    std::uint8_t version { 0 };                       ///< Which build's shape the body is in.
    std::string body;                                 ///< The bytes after the header.
};

/// Read a framed file and check everything about it except the shape of its body.
/// @param path Where to look.
/// @param envelope What the file must claim to be.
/// @return The body, or why there is none.
[[nodiscard]] FramedFile ReadFramed(std::filesystem::path const& path, FileEnvelope const& envelope);

/// A cursor over a file's body, so a reader cannot walk off the end by omission.
class BodyReader
{
  public:
    /// @param body The bytes after the header.
    explicit BodyReader(std::string_view body) noexcept:
        _body { body }
    {
    }

    /// @param into Where the next big-endian `uint64` lands.
    /// @return False when the body is exhausted, leaving @p into untouched.
    bool Take(std::uint64_t& into) noexcept
    {
        if (_cursor + sizeof(std::uint64_t) > _body.size())
            return false;
        into = ReadU64(_body, _cursor);
        _cursor += sizeof(std::uint64_t);
        return true;
    }

    /// @param length How many bytes to take.
    /// @param into A view of them, valid as long as the body is.
    /// @return False when the body is exhausted, leaving @p into untouched.
    ///
    /// Written as a remaining-length comparison rather than `_cursor + length`, because a length read off a damaged
    /// file can overflow that sum and let a bounds check pass on a span that runs off the end.
    bool Take(std::uint64_t length, std::string_view& into) noexcept
    {
        if (length > _body.size() - _cursor)
            return false;
        into = _body.substr(_cursor, static_cast<std::size_t>(length));
        _cursor += static_cast<std::size_t>(length);
        return true;
    }

    /// @return Whether every byte of the body has been taken.
    [[nodiscard]] bool Exhausted() const noexcept
    {
        return _cursor == _body.size();
    }

  private:
    std::string_view _body;
    std::size_t _cursor { 0 };
};

} // namespace Pace::Distributed::HistoryEnvelope
