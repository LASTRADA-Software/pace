// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/CgroupV2.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <ranges>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/wait.h>

    #include <cerrno>

    #include <fcntl.h>
    #include <spawn.h>
    #include <unistd.h>
#endif

namespace Pace::Platform
{

namespace
{

    /// The failure with its cause and a detail.
    /// @param cause Why.
    /// @param detail What was observed.
    /// @return The unexpected value to return.
    [[nodiscard]] std::unexpected<CgroupFailure> Failed(CgroupUnavailable cause, std::string detail)
    {
        return std::unexpected { CgroupFailure { .cause = cause, .detail = std::move(detail) } };
    }

    /// `<path>: <message>` for a detail.
    /// @param path What was being touched.
    /// @param error What the platform said.
    /// @return The text.
    [[nodiscard]] std::string Because(std::filesystem::path const& path, std::error_code const& error)
    {
        return std::format("{}: {}", path.string(), error.message());
    }

    /// Whether @p text lists @p word as one of its whitespace-separated words.
    /// @param text A control file's text.
    /// @param word The word.
    /// @return True when present.
    [[nodiscard]] bool ListsWord(std::string_view text, std::string_view word)
    {
        return std::ranges::any_of(text | std::views::split(' ') | std::views::transform([](auto part) {
                                       return std::string_view { part.begin(), part.end() };
                                   }),
                                   [word](std::string_view candidate) {
                                       while (!candidate.empty() && (candidate.back() == '\n' || candidate.back() == '\r'))
                                           candidate.remove_suffix(1);
                                       return candidate == word;
                                   });
    }

    /// A decimal number read from a control file.
    /// @param text The file's text.
    /// @return The value; disengaged for anything but digits (and a trailing newline), and for 0.
    [[nodiscard]] std::optional<std::uint64_t> ParseDecimal(std::string_view text)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
            text.remove_suffix(1);
        auto value = std::uint64_t { 0 };
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc {} || end != text.data() + text.size() || value == 0)
            return std::nullopt;
        return value;
    }

    /// One job's cgroup directory, open.
    class CgroupJob final: public IJobCgroup
    {
      public:
        CgroupJob(std::shared_ptr<ICgroupHost const> host,
                  std::filesystem::path directory,
                  int descriptor,
                  CgroupTeardownPolicy teardown):
            _host { std::move(host) },
            _directory { std::move(directory) },
            _descriptor { descriptor },
            _teardown { teardown }
        {
        }

        CgroupJob(CgroupJob const&) = delete;
        CgroupJob(CgroupJob&&) = delete;
        CgroupJob& operator=(CgroupJob const&) = delete;
        CgroupJob& operator=(CgroupJob&&) = delete;

        /// Kill what is left in the cgroup and remove it (`RemoveCgroup`): `cgroup.kill` is
        /// asynchronous, so the `rmdir` is retried while it answers busy, for at most the policy's
        /// `attempts` x `pause`. A directory still there after that is left for the service manager,
        /// which removes the subtree when the service stops.
        ~CgroupJob() override
        {
            _host->CloseDescriptor(_descriptor);
            static_cast<void>(RemoveCgroup(*_host, _directory, _teardown));
        }

        /// @copydoc IJobCgroup::DirectoryDescriptor
        [[nodiscard]] int DirectoryDescriptor() const noexcept override
        {
            return _descriptor;
        }

        /// @copydoc IJobCgroup::PeakBytes
        [[nodiscard]] std::optional<std::uint64_t> PeakBytes() const override
        {
            auto const text = _host->ReadFile(_directory / "memory.peak");
            if (!text.has_value())
                return std::nullopt;
            return ParseDecimal(*text);
        }

      private:
        std::shared_ptr<ICgroupHost const> _host;
        std::filesystem::path _directory;
        int _descriptor;
        CgroupTeardownPolicy _teardown;
    };

} // namespace

std::string CgroupFailure::Describe() const
{
    auto const& row = RowFor(cause);
    return detail.empty() ? std::string { row.reason } : std::format("{}: {}", row.reason, detail);
}

std::expected<void, std::error_code> RemoveCgroup(ICgroupHost const& host,
                                                  std::filesystem::path const& directory,
                                                  CgroupTeardownPolicy const& policy)
{
    // `cgroup.kill` is asynchronous (and absent before Linux 5.14, where the `rmdir` simply has to wait or fail).
    static_cast<void>(host.WriteFile(directory / "cgroup.kill", "1"));
    auto removed = std::expected<void, std::error_code> {};
    for (auto const attempt: std::views::iota(std::uint32_t { 0 }, policy.attempts))
    {
        removed = host.RemoveCgroupDirectory(directory);
        if (removed.has_value() || removed.error() == std::errc::no_such_file_or_directory)
            return {};
        if (removed.error() != std::errc::device_or_resource_busy)
            break;
        if (attempt + 1 < policy.attempts)
            host.Pause(policy.pause);
    }
    // A directory still there is left for the service manager, which removes the subtree at stop.
    return removed;
}

std::expected<std::string, CgroupFailure> ParseUnifiedCgroupPath(std::string_view document)
{
    for (auto const line: document | std::views::split('\n'))
    {
        std::string_view const text { line.begin(), line.end() };
        // `<hierarchy-id>:<controllers>:<path>`; the unified hierarchy is id 0 with no controllers.
        if (!text.starts_with("0::"))
            continue;
        auto path = std::string { text.substr(3) };
        // A cgroup removed under a live process is spelled with this suffix.
        if (path.ends_with(" (deleted)"))
            path.resize(path.size() - std::string_view { " (deleted)" }.size());
        if (path.empty() || path.front() != '/')
            break;
        if (std::ranges::any_of(std::filesystem::path { path }, [](auto const& part) { return part == ".."; }))
            break;
        return path;
    }
    return Failed(CgroupUnavailable::NoUnifiedHierarchy, "/proc/self/cgroup names no cgroup v2 path (no '0::' line)");
}

std::expected<CgroupLayout, CgroupFailure> PrepareJobCgroups(ICgroupHost const& host,
                                                             std::filesystem::path const& mount,
                                                             CgroupTeardownPolicy const& teardown)
{
    auto const document = host.ReadFile("/proc/self/cgroup");
    if (!document.has_value())
        return Failed(CgroupUnavailable::NoUnifiedHierarchy, Because("/proc/self/cgroup", document.error()));
    auto own = ParseUnifiedCgroupPath(*document);
    if (!own.has_value())
        return std::unexpected { std::move(own).error() };

    // `/` is the root of THIS process's cgroup namespace (a service with `ProtectControlGroups=private`
    // sees its own subtree as the mount's root), so it joins like any other path.
    auto layout = CgroupLayout { .delegated = own->size() > 1 ? mount / own->substr(1) : mount, .leaf = {} };
    layout.leaf = layout.delegated / NodeLeafName;

    // Read once: its absence is how a v1 or hybrid host shows (the mount holds no such file), and its
    // contents say whether the memory controller can be handed down at all.
    auto const controllers = host.ReadFile(layout.delegated / "cgroup.controllers");
    if (!controllers.has_value())
        return Failed(CgroupUnavailable::NoUnifiedHierarchy,
                      Because(layout.delegated / "cgroup.controllers", controllers.error()));
    if (!ListsWord(*controllers, "memory"))
        return Failed(
            CgroupUnavailable::NoMemoryController,
            std::format("{} offers: {}", layout.delegated.string(), std::string_view { *controllers }.substr(0, 200)));

    auto const subtreeControl = layout.delegated / "cgroup.subtree_control";
    if (!host.IsWritable(subtreeControl))
        return Failed(CgroupUnavailable::NotDelegated, subtreeControl.string());

    // A terminal's scope, say, delegates to its user and is writable -- and holds a shell. Enabling the
    // controller there is refused ("no internal processes") AFTER this process has been moved out of the
    // cgroup the user put it in, so the refusal is anticipated rather than discovered halfway. An
    // unreadable list proceeds: the kernel's own refusal below is still the last word.
    if (auto const members = host.ReadFile(layout.delegated / "cgroup.procs"); members.has_value())
    {
        auto const ownProcess = std::to_string(host.OwnProcessId());
        for (auto const line: *members | std::views::split('\n'))
        {
            std::string_view const member { line.begin(), line.end() };
            if (!member.empty() && member != ownProcess)
                return Failed(CgroupUnavailable::SharedCgroup,
                              std::format("{} holds process {}", layout.delegated.string(), member));
        }
    }

    // Before anything is created or moved, so a refusal leaves the tree exactly as it was. Probed
    // against the node's own cgroup: the question is about the system call, which any cgroup answers.
    auto const delegatedDirectory = host.OpenDirectory(layout.delegated);
    if (!delegatedDirectory.has_value())
        return Failed(CgroupUnavailable::SpawnIntoRefused, Because(layout.delegated, delegatedDirectory.error()));
    auto const probed = host.ProbeSpawnInto(*delegatedDirectory);
    host.CloseDescriptor(*delegatedDirectory);
    if (!probed.has_value())
        return Failed(CgroupUnavailable::SpawnIntoRefused, probed.error().message());

    if (auto const made = host.MakeDirectory(layout.leaf); !made.has_value() && made.error() != std::errc::file_exists)
        return Failed(CgroupUnavailable::LeafNotCreated, Because(layout.leaf, made.error()));

    // The pid, not a thread id: `cgroup.procs` moves the whole thread group, which is every thread of the node.
    if (auto const moved = host.WriteFile(layout.leaf / "cgroup.procs", std::to_string(host.OwnProcessId()));
        !moved.has_value())
        return Failed(CgroupUnavailable::ProcessNotMoved, Because(layout.leaf / "cgroup.procs", moved.error()));

    // After the move: enabling a controller for the children of a cgroup that still holds processes is
    // refused ("no internal processes"), which is why the node steps into a leaf first.
    if (auto const enabled = host.WriteFile(subtreeControl, "+memory"); !enabled.has_value())
        return Failed(CgroupUnavailable::MemoryNotEnabled, Because(subtreeControl, enabled.error()));

    // Written is not enabled: read it back, since a service manager that does not consider this cgroup
    // delegated is free to rewrite the file, and a node that believes it measures while it does not is
    // the failure this exists to end.
    if (auto const now = host.ReadFile(subtreeControl); now.has_value() && !ListsWord(*now, "memory"))
        return Failed(CgroupUnavailable::MemoryNotEnabled,
                      subtreeControl.string() + " does not list memory after it was enabled");

    // The leaf is a child, so it has the file exactly when the kernel offers it.
    if (auto const peak = host.ReadFile(layout.leaf / "memory.peak"); !peak.has_value())
        return Failed(CgroupUnavailable::PeakNotReadable, Because(layout.leaf / "memory.peak", peak.error()));

    // A node killed before it could remove its jobs (nothing cleans a hand-delegated subtree) leaves
    // `job-*` behind. Swept now, so `Create` never has to step over a long run of them; a name it still
    // meets is skipped there as a backstop.
    if (auto const entries = host.ListDirectory(layout.delegated); entries.has_value())
    {
        for (auto const& name: *entries)
        {
            if (!name.starts_with("job-"))
                continue;
            if (auto const removed = RemoveCgroup(host, layout.delegated / name, teardown); !removed.has_value())
                return Failed(CgroupUnavailable::StaleJobCgroups, Because(layout.delegated / name, removed.error()));
        }
    }

    return layout;
}

CgroupJobSource::CgroupJobSource(std::shared_ptr<ICgroupHost const> host,
                                 CgroupLayout layout,
                                 CgroupTeardownPolicy teardown):
    _host { std::move(host) },
    _layout { std::move(layout) },
    _teardown { teardown }
{
}

std::expected<std::unique_ptr<IJobCgroup>, std::error_code> CgroupJobSource::Create()
{
    // A name left behind by an earlier node (killed before it could remove its jobs) is skipped, not
    // reused: a stale cgroup's `memory.peak` is somebody else's.
    constexpr auto Attempts = std::uint32_t { 16 };
    auto lastError = std::error_code {};
    for ([[maybe_unused]] auto const attempt: std::views::iota(std::uint32_t { 0 }, Attempts))
    {
        auto const directory = _layout.delegated / std::format("job-{}", _next.fetch_add(1, std::memory_order_relaxed));
        auto const made = _host->MakeDirectory(directory);
        if (!made.has_value())
        {
            lastError = made.error();
            if (lastError == std::errc::file_exists)
                continue;
            return std::unexpected { lastError };
        }
        auto const descriptor = _host->OpenDirectory(directory);
        if (!descriptor.has_value())
        {
            static_cast<void>(_host->RemoveCgroupDirectory(directory));
            return std::unexpected { descriptor.error() };
        }
        return std::make_unique<CgroupJob>(_host, directory, *descriptor, _teardown);
    }
    return std::unexpected { lastError };
}

// ---------------------------------------------------------------------------------------------------
// The real cgroup tree.
// ---------------------------------------------------------------------------------------------------

#if defined(__linux__)

namespace
{

    /// The current `errno` as an error code.
    [[nodiscard]] std::error_code LastError() noexcept
    {
        return std::error_code { errno, std::generic_category() };
    }

    /// A descriptor closed on scope exit.
    class OwnedDescriptor
    {
      public:
        explicit OwnedDescriptor(int descriptor) noexcept:
            _descriptor { descriptor }
        {
        }
        OwnedDescriptor(OwnedDescriptor const&) = delete;
        OwnedDescriptor(OwnedDescriptor&&) = delete;
        OwnedDescriptor& operator=(OwnedDescriptor const&) = delete;
        OwnedDescriptor& operator=(OwnedDescriptor&&) = delete;
        ~OwnedDescriptor()
        {
            if (_descriptor >= 0)
                ::close(_descriptor);
        }
        [[nodiscard]] int Get() const noexcept
        {
            return _descriptor;
        }

      private:
        int _descriptor;
    };

} // namespace

std::expected<std::string, std::error_code> SystemCgroupHost::ReadFile(std::filesystem::path const& path) const
{
    auto const file = OwnedDescriptor { ::open(path.c_str(), O_RDONLY | O_CLOEXEC) };
    if (file.Get() < 0)
        return std::unexpected { LastError() };
    auto text = std::string {};
    auto buffer = std::array<char, 4096> {};
    while (true)
    {
        auto const got = ::read(file.Get(), buffer.data(), buffer.size());
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected { LastError() };
        }
        if (got == 0)
            return text;
        text.append(buffer.data(), static_cast<std::size_t>(got));
    }
}

std::expected<void, std::error_code> SystemCgroupHost::WriteFile(std::filesystem::path const& path,
                                                                 std::string_view text) const
{
    auto const file = OwnedDescriptor { ::open(path.c_str(), O_WRONLY | O_CLOEXEC) };
    if (file.Get() < 0)
        return std::unexpected { LastError() };
    // One `write`: a control file parses what a single call hands it, and its error is that call's.
    while (::write(file.Get(), text.data(), text.size()) < 0)
    {
        if (errno != EINTR)
            return std::unexpected { LastError() };
    }
    return {};
}

std::expected<void, std::error_code> SystemCgroupHost::MakeDirectory(std::filesystem::path const& path) const
{
    if (::mkdir(path.c_str(), 0755) != 0)
        return std::unexpected { LastError() };
    return {};
}

std::expected<void, std::error_code> SystemCgroupHost::RemoveCgroupDirectory(std::filesystem::path const& path) const
{
    if (::rmdir(path.c_str()) != 0)
        return std::unexpected { LastError() };
    return {};
}

std::expected<std::vector<std::string>, std::error_code> SystemCgroupHost::ListDirectory(
    std::filesystem::path const& path) const
{
    auto error = std::error_code {};
    auto names = std::vector<std::string> {};
    for (auto const& entry: std::filesystem::directory_iterator { path, error })
        names.push_back(entry.path().filename().string());
    if (error)
        return std::unexpected { error };
    return names;
}

std::expected<int, std::error_code> SystemCgroupHost::OpenDirectory(std::filesystem::path const& path) const
{
    auto const descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        return std::unexpected { LastError() };
    return descriptor;
}

void SystemCgroupHost::CloseDescriptor(int descriptor) const noexcept
{
    ::close(descriptor);
}

bool SystemCgroupHost::IsWritable(std::filesystem::path const& path) const
{
    return ::access(path.c_str(), W_OK) == 0;
}

std::expected<void, std::error_code> SystemCgroupHost::ProbeSpawnInto(int directory) const
{
    #if defined(POSIX_SPAWN_SETCGROUP)
    ::posix_spawnattr_t attributes {};
    if (auto const failure = ::posix_spawnattr_init(&attributes); failure != 0)
        return std::unexpected { std::error_code { failure, std::generic_category() } };
    ::posix_spawnattr_setcgroup_np(&attributes, directory);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETCGROUP);
    // A program that does not exist: a child created INSIDE the cgroup then fails to exec, which a
    // spawn reports as ENOENT. EBUSY is the kernel's own "this cgroup holds a controller for its
    // children" -- it got as far as judging the cgroup, so the call is allowed. Anything else is the
    // process creation itself being refused.
    ::pid_t child = -1;
    char* const noArguments[] = { nullptr };
    auto const answer =
        ::posix_spawn(&child, "/nonexistent/pace-cgroup-probe", nullptr, &attributes, noArguments, noArguments);
    ::posix_spawnattr_destroy(&attributes);
    if (answer == 0)
    {
        // Not expected, but a child exists and must not be left a zombie.
        ::waitpid(child, nullptr, 0);
        return std::unexpected { std::make_error_code(std::errc::invalid_argument) };
    }
    if (answer == ENOENT || answer == EBUSY)
        return {};
    return std::unexpected { std::error_code { answer, std::generic_category() } };
    #else
    static_cast<void>(directory);
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
    #endif
}

std::uint32_t SystemCgroupHost::OwnProcessId() const noexcept
{
    return static_cast<std::uint32_t>(::getpid());
}

#else // not Linux

std::expected<std::string, std::error_code> SystemCgroupHost::ReadFile(std::filesystem::path const&) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

std::expected<void, std::error_code> SystemCgroupHost::WriteFile(std::filesystem::path const&, std::string_view) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

std::expected<void, std::error_code> SystemCgroupHost::MakeDirectory(std::filesystem::path const&) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

std::expected<void, std::error_code> SystemCgroupHost::RemoveCgroupDirectory(std::filesystem::path const&) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

std::expected<std::vector<std::string>, std::error_code> SystemCgroupHost::ListDirectory(std::filesystem::path const&) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

std::expected<int, std::error_code> SystemCgroupHost::OpenDirectory(std::filesystem::path const&) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

void SystemCgroupHost::CloseDescriptor(int) const noexcept {}

bool SystemCgroupHost::IsWritable(std::filesystem::path const&) const
{
    return false;
}

std::expected<void, std::error_code> SystemCgroupHost::ProbeSpawnInto(int) const
{
    return std::unexpected { std::make_error_code(std::errc::operation_not_supported) };
}

std::uint32_t SystemCgroupHost::OwnProcessId() const noexcept
{
    return 0;
}

#endif

void SystemCgroupHost::Pause(std::chrono::microseconds duration) const
{
    std::this_thread::sleep_for(duration);
}

} // namespace Pace::Platform
