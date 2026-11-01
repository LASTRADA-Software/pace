// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/CompileCache/ToolchainFingerprint.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file FingerprintDiff.hpp
/// Why two toolchain fingerprints differ: the inputs each was computed from (`Cc::FingerprintInputs`), compared
/// field by field and file by file.
///
/// **It states what the comparison shows, and labels anything else.** A schema, a banner, a grammar and a file list
/// either match or they do not, and the verdict says which. What that MEANS -- "a different libstdc++ package build"
/// -- is not something two lists of digests can show, so it is read off a small table of where in the tree the
/// differences sit, said only when the compiler identity itself agrees, and marked `inferred` every time.

namespace Pace::Distributed
{

/// How two fingerprints' inputs differ.
struct FingerprintDiff
{
    bool schemaDiffers { false }; ///< The two records were digested under different schemas.

    /// Both banners, verbatim, when they differ; nothing when they are the same.
    std::optional<std::pair<std::string, std::string>> banner;

    /// Both driver grammar names when they differ; nothing when they are the same.
    std::optional<std::pair<std::string, std::string>> grammar;

    std::vector<std::string> onlyA;          ///< Relative paths only the first record holds, sorted.
    std::vector<std::string> onlyB;          ///< Relative paths only the second record holds, sorted.
    std::vector<std::string> contentDiffers; ///< Relative paths both hold with different content, sorted.

    /// A short plain-language verdict: what differs, where, and -- marked `inferred` -- what that likely means.
    std::string verdict;

    /// Whether the verdict says anything the comparison does not show by itself.
    bool inferred { false };

    /// @return True when nothing differs: same schema, banner, grammar and files.
    [[nodiscard]] bool Identical() const noexcept
    {
        return !schemaDiffers && !banner.has_value() && !grammar.has_value() && onlyA.empty() && onlyB.empty()
               && contentDiffers.empty();
    }
};

/// One reading of WHERE in a toolchain's tree a set of differing files sits.
///
/// A row, so the next reading is a row rather than a branch: `needle` is matched against the common directory as
/// whole path components (`/c++/` inside `/<dir>/`), the first row that matches decides, and the empty needle --
/// last -- matches anything.
struct TreeInference
{
    std::string_view needle; ///< A path-component run, slash-delimited; empty matches every directory.
    std::string_view likely; ///< What a difference there likely is, completing "likely ...".
};

/// The readings, most specific first.
inline constexpr std::array TreeInferences {
    TreeInference { .needle = "/c++/v1/", .likely = "a different libc++ build" },
    TreeInference { .needle = "/c++/", .likely = "a different libstdc++ package build" },
    TreeInference { .needle = "/clang/", .likely = "a different build of the compiler's own resource headers" },
    TreeInference { .needle = "", .likely = "a different build of the package that installs these headers" },
};

/// The deepest directory every path sits under, `/`-terminated; empty when they share none.
/// @param paths Relative paths, `/`-separated.
/// @return The common directory, e.g. `c++/13/bits/`.
[[nodiscard]] std::string CommonDirectory(std::span<std::string const> paths);

/// Compare the inputs two fingerprints were computed from.
///
/// Neither record is assumed sorted: each is compared by path, so the answer does not depend on who built it.
/// @param a The first record.
/// @param b The second record.
/// @return What differs, and the verdict.
[[nodiscard]] FingerprintDiff DiffFingerprintInputs(Cc::FingerprintInputs const& a, Cc::FingerprintInputs const& b);

} // namespace Pace::Distributed
