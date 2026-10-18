// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pace
{

/// Read a variable from the process environment.
///
/// The single place the process environment is read. Every caller previously
/// spelled its own `#if defined(_WIN32)` / `getenv_s` / `std::getenv` dance,
/// which was that many chances to get the Windows length convention wrong
/// (getenv_s reports the length *including* the NUL, so 0 means absent and 1
/// means present but empty).
///
/// `src/apps/pace-cc` is included in "every caller" despite not linking
/// this library: it compiles this file straight into the binary through the
/// `_fc_cc_core` list, which is how the launcher stays free of vcpkg
/// dependencies and CRT-static while still sharing the one implementation.
///
/// Anything with a decision to test reaches the environment through an injected
/// seam instead — `SystemConfigPathProbe::GetEnv` is this function wrapped in
/// one. Calling it directly is for leaf probes that have no logic worth faking:
/// `NoColorRequested` in Terminal.cpp, `MetricsPortFromEnv` in the daemon's
/// main.cpp, and the launcher's own `EnvOr` and `StateDirectory` are the whole
/// set — each a read plus a comparison.
///
/// Deliberately NOT named `GetEnvironmentVariable`: `<windows.h>` defines that
/// as a macro expanding to `GetEnvironmentVariableA`/`W`, so any translation
/// unit including both headers would fail to compile.
///
/// @param name Variable name.
/// @return The value; an empty string when the variable is set but empty; and
///         `std::nullopt` when it is not set at all. The distinction matters:
///         an unset variable means "this location does not apply", while a set
///         but empty one is a deliberate value.
[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(std::string_view name);

/// The NAMES of every variable in the process environment, in the environment's order.
///
/// Read beside `ReadEnvironmentVariable` because it is the same source: a caller that
/// must notice a name it does not know in advance (`RetiredEnvironmentNames`) cannot ask
/// for it by name. Windows' hidden per-drive entries (`=C:=...`) name no variable and are
/// left out.
/// @return The names; empty when the environment is empty or cannot be read.
[[nodiscard]] std::vector<std::string> EnvironmentVariableNames();

} // namespace Pace
