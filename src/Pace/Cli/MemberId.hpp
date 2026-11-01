// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Cli/Options.hpp>
#include <Pace/Core/Errors/ConfigError.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace Pace
{

/// @file MemberId.hpp
/// The one rule a member id typed by an operator is held to, wherever it is typed: `pace-node --node-id`, the
/// enrollment verbs and `--cluster-name` on one side, `pacectl cluster-name` on the other.

/// Why an id is refused for its length: the sentence every door that takes one says.
/// @param bytes How long the id was.
/// @return The reason, naming the bound.
[[nodiscard]] inline std::string MemberIdTooLong(std::size_t bytes)
{
    return std::format("is {} bytes, longer than the {} an id may be: every peer refuses a fleet summary naming "
                       "a longer one",
                       bytes,
                       CompileCacheWire::MaxIdBytes);
}

/// An id typed on a command line, held to the ONE bound every reader of an id holds it to
/// (`CompileCacheWire::MaxIdBytes`), and to being text.
///
/// Refused here, by name, because past it the id is no less valid to this process: it is every PEER that refuses the
/// fleet summary carrying it, silently, as malformed. Empty passes: an empty `--node-id` is how a node asks to mint one.
/// @param sv The value text.
/// @return The id, or why it is too long or not text. The error names no field; the caller's door stamps it.
[[nodiscard]] inline std::expected<std::string, ConfigError> ParseMemberId(std::string_view sv)
{
    if (sv.size() > CompileCacheWire::MaxIdBytes)
        return std::unexpected(ArgvError(ConfigErrorCode::ParseError, std::format("value {}", MemberIdTooLong(sv.size()))));
    return ParseUtf8Text(sv);
}

} // namespace Pace
