// SPDX-License-Identifier: Apache-2.0
//
// Test helper for ProcessRunner_test.cpp: allocates `argv[1]` MiB and writes to every page, so a
// parent that waits for it can be shown to have measured a DESCENDANT's peak memory.

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>

int main(int argc, char** argv)
{
    auto const args = std::span { argv, static_cast<std::size_t>(argc) };
    if (args.size() < 2)
        return 2;

    std::size_t mebibytes = 0;
    std::string_view const text { args[1] };
    if (std::from_chars(text.data(), text.data() + text.size(), mebibytes).ec != std::errc {})
        return 2;

    constexpr std::size_t PageBytes = 4096;
    auto const total = mebibytes << 20;
    auto const block = std::make_unique_for_overwrite<unsigned char[]>(total);
    for (auto const offset: std::views::iota(std::size_t { 0 }, total) | std::views::stride(PageBytes))
        block[offset] = static_cast<unsigned char>(offset / PageBytes);

    // Read it back through a volatile so the stores cannot be elided.
    unsigned char volatile sink = block[(total / 2) & ~(PageBytes - 1)];
    (void) sink;
    return 0;
}
