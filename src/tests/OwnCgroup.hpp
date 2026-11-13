// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#if defined(__linux__)
    #include <fcntl.h>
    #include <unistd.h>
#endif

/// @file OwnCgroup.hpp
/// A real cgroup a test may start a child in WITHOUT changing the machine's cgroup tree: the test process's own.
///
/// A runner case that needs the child to run inside a cgroup -- so the runner keeps the job and classifies a kill
/// by it -- cannot use a fake descriptor (the spawn is refused and the runner drops the job), and must not create
/// cgroups on the developer's machine. Spawning into the cgroup the process already lives in moves nothing. Where
/// the system refuses even that (no cgroup v2, a container, a seccomp filter), the runner reports the spawn
/// refused and a case SKIPs: the property is untestable there, not false.
namespace Pace::Testing
{

/// An open descriptor of this process's own cgroup v2 directory, closed on destruction.
class OwnCgroupDirectory
{
  public:
    /// Open it. Never fails loudly: `Descriptor()` is -1 when it could not be found or opened.
    OwnCgroupDirectory()
    {
#if defined(__linux__)
        auto file = std::ifstream { "/proc/self/cgroup" };
        auto line = std::string {};
        while (std::getline(file, line))
        {
            constexpr auto Unified = std::string_view { "0::" };
            if (!line.starts_with(Unified))
                continue;
            auto const path = std::filesystem::path { "/sys/fs/cgroup" } / line.substr(Unified.size() + 1);
            _descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            return;
        }
#endif
    }

    OwnCgroupDirectory(OwnCgroupDirectory const&) = delete;
    OwnCgroupDirectory(OwnCgroupDirectory&&) = delete;
    OwnCgroupDirectory& operator=(OwnCgroupDirectory const&) = delete;
    OwnCgroupDirectory& operator=(OwnCgroupDirectory&&) = delete;

    ~OwnCgroupDirectory()
    {
#if defined(__linux__)
        if (_descriptor >= 0)
            ::close(_descriptor);
#endif
    }

    /// @return The descriptor, or -1.
    [[nodiscard]] int Descriptor() const noexcept
    {
        return _descriptor;
    }

  private:
    int _descriptor { -1 };
};

} // namespace Pace::Testing
