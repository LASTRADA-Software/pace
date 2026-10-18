// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/Environment.hpp>

#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    // On macOS `environ` is not available to anything but the main executable;
    // _NSGetEnviron() is the documented way to reach it.
    #include <crt_externs.h>
#else
    #include <unistd.h>
#endif

namespace Pace
{

std::optional<std::string> ReadEnvironmentVariable(std::string_view name)
{
    // Both platform APIs take a NUL-terminated name, and string_view is not.
    std::string const key { name };

#if defined(_WIN32)
    // The secure CRT form, so the build stays warning-clean under /WX.
    // getenv_s reports the length INCLUDING the NUL: 0 means not present.
    std::size_t size = 0;
    if (::getenv_s(&size, nullptr, 0, key.c_str()) != 0 || size == 0)
        return std::nullopt;

    // The size check again, on the fetching call: getenv_s reports 0 for a
    // variable it did not find, and between the two calls it can genuinely go
    // missing. Without this, `size - 1` underflows and resize() is asked for
    // the whole address space.
    std::string value(size, '\0');
    if (::getenv_s(&size, value.data(), size, key.c_str()) != 0 || size == 0)
        return std::nullopt;
    value.resize(size - 1);
    return value;
#else
    char const* const value = std::getenv(key.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string { value };
#endif
}

std::vector<std::string> EnvironmentVariableNames()
{
    std::vector<std::string> names;
    // An entry is NAME=VALUE. A leading `=` is one of Windows' hidden per-drive current
    // directories (`=C:=C:\path`), which names no variable anybody set.
    auto const append = [&names](std::string_view entry) {
        if (entry.empty() || entry.starts_with('='))
            return;
        names.emplace_back(entry.substr(0, entry.find('=')));
    };
#if defined(_WIN32)
    char* const block = ::GetEnvironmentStringsA();
    if (block == nullptr)
        return names;
    char const* cursor = block;
    while (*cursor != '\0')
    {
        std::string_view const entry { cursor };
        append(entry);
        cursor += entry.size() + 1;
    }
    ::FreeEnvironmentStringsA(block);
#else
    #if defined(__APPLE__)
    char** cursor = *::_NSGetEnviron();
    #else
    char** cursor = environ;
    #endif
    while (cursor != nullptr && *cursor != nullptr)
    {
        append(*cursor);
        ++cursor;
    }
#endif
    return names;
}

} // namespace Pace
