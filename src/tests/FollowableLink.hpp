// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace Pace::Testing
{

/// Create a symbolic link and prove THIS process can open through it, or say why not (#1735).
///
/// Creating a link is not the same fact as following one. On at least one Windows 11 build a
/// non-administrator creates links freely -- `create_symlink`, `mklink /D`, `mklink /J` all
/// succeed -- and every Win32 open through them then fails with error 448, "untrusted mount
/// point". A case that checked only the creation then failed DETERMINISTICALLY, on a clean
/// master, for a property of the host rather than of the code. So the caller gets one answer
/// for both questions: nothing when the link exists and opens, else the reason to SKIP with.
///
/// @param target What the link points at; a directory gets a directory link.
/// @param link   Where the link goes.
/// @return Nothing when the link was made and a read through it works; otherwise the reason,
///         worded for `SKIP`.
[[nodiscard]] inline std::optional<std::string> MakeFollowableLink(std::filesystem::path const& target,
                                                                   std::filesystem::path const& link)
{
    std::error_code error;
    bool const directory = std::filesystem::is_directory(target, error);
    if (directory)
        std::filesystem::create_directory_symlink(target, link, error);
    else
        std::filesystem::create_symlink(target, link, error);
    if (error)
        return std::format("this host lets this process create no symbolic link ({})", error.message());

    // FOLLOWED, never `symlink_status`: the question is whether an open through the link reaches
    // the target, and both reads below go through it.
    if (directory)
    {
        std::filesystem::directory_iterator const entries { link, error };
        if (error)
            return std::format("this host cannot follow a link this process created ({})", error.message());
        return std::nullopt;
    }
    std::ifstream const opened { link, std::ios::binary };
    if (!opened)
        return std::string { "this host cannot follow a link this process created (it would not open)" };
    return std::nullopt;
}

} // namespace Pace::Testing
