// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

/// @file OomVictimExec.hpp
/// Layer A of the compile-OOM defence: every compiler a node spawns starts as the kernel's first OOM victim.
///
/// `posix_spawn` runs no code of ours between creating the child and executing the compiler, and writing
/// `/proc/<pid>/oom_score_adj` after the spawn races the driver forking `cc1plus`: for that window a compiler
/// would exist at the node's own score. So the runner spawns the NODE'S OWN BINARY under a second name, the
/// shim, which raises its own score and then `exec`s the compiler. The score is a property of the process
/// that `exec` keeps and `fork` copies, so the compiler and every child it starts hold it from their first
/// instruction, and there is no window at all. The node's own score is never changed.
///
/// The shim is selected by `argv[0]`, not by a flag: it is no `CliOptions` row, so it can never reach a
/// service registration, and nothing an operator types can select it.
///
/// Header-only dependencies, std only: `pace-cc` compiles this file without linking the `Pace` library.
namespace Pace::Cc
{

/// The name the shim is invoked under: `argv[0]`'s last path component.
inline constexpr std::string_view OomVictimExecName = "pace-oom-victim-exec";

/// The score every compiler starts with: the kernel's largest, so its OOM killer takes a compile first.
inline constexpr std::string_view OomVictimScore = "1000";

/// The exit status of a shim that could not run its compiler, as a shell reports a command it could not run.
inline constexpr int OomVictimExecFailed = 127;

/// What a runner spawns its compilers through to make each the kernel's first OOM victim.
struct OomVictim
{
    /// The binary that serves as the shim: one whose `main` begins with `RunOomVictimExecIfInvoked`.
    std::filesystem::path self;
};

/// Where the shim writes its own OOM score.
///
/// The seam over `/proc/self/oom_score_adj`, so the shim's decision can be tested without a test process
/// raising its own score.
class IOomScoreWriter
{
  public:
    IOomScoreWriter() = default;
    virtual ~IOomScoreWriter() = default;
    IOomScoreWriter(IOomScoreWriter const&) = delete;
    IOomScoreWriter& operator=(IOomScoreWriter const&) = delete;
    IOomScoreWriter(IOomScoreWriter&&) = delete;
    IOomScoreWriter& operator=(IOomScoreWriter&&) = delete;

    /// Set this process's OOM score adjustment.
    ///
    /// **Must leave no descriptor open**: the shim `exec`s the compiler next, and `exec` keeps every
    /// descriptor that is not close-on-exec.
    /// @param score The value, as the kernel's text interface takes it.
    /// @return Nothing on success; the error that refused the write.
    [[nodiscard]] virtual std::expected<void, std::error_code> Write(std::string_view score) = 0;
};

/// The production writer: `/proc/self/oom_score_adj`, opened close-on-exec and closed before it returns.
///
/// Raising one's own score needs no privilege on Linux; it fails only where `/proc` is not mounted. On any
/// other platform every write answers `std::errc::not_supported`.
class ProcSelfOomScoreWriter final: public IOomScoreWriter
{
  public:
    /// @copydoc IOomScoreWriter::Write
    [[nodiscard]] std::expected<void, std::error_code> Write(std::string_view score) override;
};

/// Run as the shim when this process was invoked as one; `main`'s FIRST statement in `pace-node`, before any
/// configuration is parsed, any logger made or any thread started.
///
/// When `argv[0]`'s last path component is `OomVictimExecName`: writes `OomVictimScore` through @p writer, then
/// `execvp(argv[1], argv + 1)`. A write that fails is one warning on stderr and the compiler still runs -- the
/// compile matters more than its kill priority. Returns only when the compiler could not be run, with one
/// stderr line naming it and why.
///
/// On a platform other than Linux this is never the shim and always returns disengaged.
/// @param argv The process's arguments, `argv[0]` first.
/// @param writer Where the score is written.
/// @return Disengaged when this process is not the shim; otherwise `OomVictimExecFailed`.
[[nodiscard]] std::optional<int> RunOomVictimExecIfInvoked(std::span<char const* const> argv, IOomScoreWriter& writer);

/// The shim this process can spawn its compilers through, resolved ONCE, at a composition root.
///
/// The binary is named as `/proc/self/exe` rather than the path that link resolves to: a child spawned through
/// it executes the image its parent runs, so a package upgrade that replaces the file under a running node
/// leaves its compiles running rather than every one failing to spawn. The link is read once here, to learn
/// that `/proc` is there to name it.
/// @return The shim on Linux with `/proc` mounted; disengaged elsewhere, which leaves compilers at the
///         node's own score.
[[nodiscard]] std::optional<OomVictim> ThisProcessOomVictim();

} // namespace Pace::Cc
