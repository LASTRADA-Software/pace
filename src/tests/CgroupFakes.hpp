// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Platform/CgroupV2.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// @file CgroupFakes.hpp
/// A cgroup tree in memory, for the cases that decide what the node does with the one it finds.
namespace Pace::Testing
{

/// A cgroup tree held in memory: files with text, directories, and the refusals a case scripts.
class FakeCgroupHost final: public Platform::ICgroupHost
{
  public:
    std::map<std::string, std::string> files;       ///< Readable files and their text.
    mutable std::set<std::string> directories;      ///< Directories that exist.
    std::set<std::string> unmakeable;               ///< Directories `mkdir` is refused for.
    std::set<std::string> unwritable;               ///< Files `IsWritable` denies.
    std::map<std::string, std::errc> writeRefusals; ///< Files whose write is refused.
    std::set<std::string> busyRemovals;             ///< Directories `rmdir` answers busy for, this many times.
    std::uint32_t busyFor { 0 };                    ///< How many `rmdir`s of a busy directory fail.
    /// What probing a spawn into a cgroup answers: `no_such_file_or_directory` is success (see the real probe).
    std::errc spawnProbe { std::errc::no_such_file_or_directory };
    mutable std::vector<std::string> trail;      ///< Every mutation,
    mutable std::uint32_t pauses { 0 };          ///< `Pause` calls.
    mutable std::uint32_t removalAttempts { 0 }; ///< `RemoveCgroupDirectory` calls.
    mutable int nextDescriptor { 1000 };         ///< The next descriptor `OpenDirectory` hands out.
    mutable std::vector<int> closed;             ///< Descriptors closed,

    [[nodiscard]] std::expected<std::string, std::error_code> ReadFile(std::filesystem::path const& path) const override
    {
        if (auto const found = files.find(path.generic_string()); found != files.end())
            return found->second;
        return std::unexpected { std::make_error_code(std::errc::no_such_file_or_directory) };
    }

    [[nodiscard]] std::expected<void, std::error_code> WriteFile(std::filesystem::path const& path,
                                                                 std::string_view text) const override
    {
        if (auto const refused = writeRefusals.find(path.generic_string()); refused != writeRefusals.end())
            return std::unexpected { std::make_error_code(refused->second) };
        trail.push_back("write " + path.generic_string() + " " + std::string { text });
        return {};
    }

    [[nodiscard]] std::expected<void, std::error_code> MakeDirectory(std::filesystem::path const& path) const override
    {
        if (directories.contains(path.generic_string()))
            return std::unexpected { std::make_error_code(std::errc::file_exists) };
        if (unmakeable.contains(path.generic_string()))
            return std::unexpected { std::make_error_code(std::errc::permission_denied) };
        trail.push_back("mkdir " + path.generic_string());
        directories.insert(path.generic_string());
        return {};
    }

    [[nodiscard]] std::expected<void, std::error_code> RemoveCgroupDirectory(
        std::filesystem::path const& path) const override
    {
        ++removalAttempts;
        if (busyRemovals.contains(path.generic_string()) && removalAttempts <= busyFor)
            return std::unexpected { std::make_error_code(std::errc::device_or_resource_busy) };
        trail.push_back("rmdir " + path.generic_string());
        directories.erase(path.generic_string());
        return {};
    }

    [[nodiscard]] std::expected<std::vector<std::string>, std::error_code> ListDirectory(
        std::filesystem::path const& path) const override
    {
        auto names = std::vector<std::string> {};
        for (auto const& directory: directories)
            if (std::filesystem::path { directory }.parent_path() == path)
                names.push_back(std::filesystem::path { directory }.filename().generic_string());
        return names;
    }

    [[nodiscard]] std::expected<int, std::error_code> OpenDirectory(std::filesystem::path const& /*path*/) const override
    {
        return nextDescriptor++;
    }

    void CloseDescriptor(int descriptor) const noexcept override
    {
        closed.push_back(descriptor);
    }

    [[nodiscard]] bool IsWritable(std::filesystem::path const& path) const override
    {
        return !unwritable.contains(path.generic_string());
    }

    [[nodiscard]] std::expected<void, std::error_code> ProbeSpawnInto(int /*directory*/) const override
    {
        if (spawnProbe == std::errc::no_such_file_or_directory)
            return {};
        return std::unexpected { std::make_error_code(spawnProbe) };
    }

    [[nodiscard]] std::uint32_t OwnProcessId() const noexcept override
    {
        return 4242;
    }

    void Pause(std::chrono::microseconds /*duration*/) const override
    {
        ++pauses;
    }
};

} // namespace Pace::Testing
