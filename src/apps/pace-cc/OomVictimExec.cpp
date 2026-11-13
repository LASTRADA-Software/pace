// SPDX-License-Identifier: Apache-2.0
#include "OomVictimExec.hpp"

#include <cstddef>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#if defined(__linux__)
    #include <cerrno>
    #include <cstring>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace Pace::Cc
{

#if defined(__linux__)
namespace
{

    /// `argv[0]`'s last path component.
    /// @param invokedAs `argv[0]`.
    /// @return What follows its last `/`; all of it when there is none.
    [[nodiscard]] std::string_view BaseNameOf(std::string_view invokedAs) noexcept
    {
        auto const slash = invokedAs.find_last_of('/');
        return slash == std::string_view::npos ? invokedAs : invokedAs.substr(slash + 1);
    }

} // namespace
#endif

std::expected<void, std::error_code> ProcSelfOomScoreWriter::Write([[maybe_unused]] std::string_view score)
{
#if defined(__linux__)
    // Close-on-exec as well as closed below: the shim `exec`s the compiler next, and a descriptor that survived
    // the exec would be the compiler's to inherit.
    int const fd = ::open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected { std::error_code { errno, std::generic_category() } };
    auto const written = ::write(fd, score.data(), score.size());
    auto const writeError = errno;
    ::close(fd);
    if (written < 0)
        return std::unexpected { std::error_code { writeError, std::generic_category() } };
    if (static_cast<std::size_t>(written) != score.size())
        return std::unexpected { std::make_error_code(std::errc::io_error) };
    return {};
#else
    return std::unexpected { std::make_error_code(std::errc::not_supported) };
#endif
}

std::optional<int> RunOomVictimExecIfInvoked([[maybe_unused]] std::span<char const* const> argv,
                                             [[maybe_unused]] IOomScoreWriter& writer)
{
#if defined(__linux__)
    if (argv.empty() || argv.front() == nullptr || BaseNameOf(argv.front()) != OomVictimExecName)
        return std::nullopt;

    auto const compiler = argv.subspan(1);
    if (compiler.empty() || compiler.front() == nullptr)
    {
        std::cerr << OomVictimExecName << ": no compiler to run\n";
        return OomVictimExecFailed;
    }

    if (auto const written = writer.Write(OomVictimScore); !written.has_value())
        std::cerr << std::format("{}: warning: the OOM score could not be raised to {} ({}); the compile runs at the "
                                 "node's own score\n",
                                 OomVictimExecName,
                                 OomVictimScore,
                                 written.error().message());

    // `execvp` takes a null-terminated, non-const array; `argv`'s span stops before `main`'s terminator.
    std::vector<char*> arguments;
    arguments.reserve(compiler.size() + 1);
    for (char const* argument: compiler)
        arguments.push_back(const_cast<char*>(argument));
    arguments.push_back(nullptr);
    ::execvp(arguments.front(), arguments.data());

    auto const why = errno;
    std::cerr << std::format("{}: cannot run {}: {}\n", OomVictimExecName, compiler.front(), std::strerror(why));
    return OomVictimExecFailed;
#else
    return std::nullopt;
#endif
}

std::expected<OomVictim, std::string> ThisProcessOomVictim()
{
#if defined(__linux__)
    auto const link = std::filesystem::path { "/proc/self/exe" };
    auto failure = std::error_code {};
    if (auto const target = std::filesystem::read_symlink(link, failure); failure || target.empty())
        return std::unexpected { std::format("{} cannot be read ({}), so this binary cannot be spawned as the shim",
                                             link.string(),
                                             failure ? failure.message() : std::string { "it names nothing" }) };
    return OomVictim { .self = link };
#else
    return std::unexpected { std::string { "this platform has no oom_score_adj" } };
#endif
}

} // namespace Pace::Cc
