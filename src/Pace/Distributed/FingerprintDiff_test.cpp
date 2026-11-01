// SPDX-License-Identifier: Apache-2.0
#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/Distributed/FingerprintDiff.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace Pace;
using Pace::Testing::Unwrap;
using namespace Pace::Distributed;

namespace
{

/// The tree both sides of a case start from.
[[nodiscard]] std::vector<Cc::ToolchainFile> BaseTree()
{
    return {
        Cc::ToolchainFile { .relativePath = "c++/13/bits/stl_vector.h", .contentHash = "a1" },
        Cc::ToolchainFile { .relativePath = "c++/13/bits/stl_map.h", .contentHash = "a2" },
        Cc::ToolchainFile { .relativePath = "c++/13/bits/stl_list.h", .contentHash = "a3" },
        Cc::ToolchainFile { .relativePath = "c++/13/vector", .contentHash = "a4" },
        Cc::ToolchainFile { .relativePath = "stdio.h", .contentHash = "a5" },
    };
}

/// @param banner The banner.
/// @param files The tree.
/// @return A record captured as a survey captures one, under the GNU grammar.
[[nodiscard]] Cc::FingerprintInputs Record(std::string const& banner, std::vector<Cc::ToolchainFile> files)
{
    return Cc::CaptureFingerprintInputs(banner, "gnu", std::move(files));
}

/// @param files A tree.
/// @param path A file in it.
/// @param hash Its new content digest.
/// @return The tree with that one file's content changed.
[[nodiscard]] std::vector<Cc::ToolchainFile> WithContent(std::vector<Cc::ToolchainFile> files,
                                                         std::string const& path,
                                                         std::string const& hash)
{
    for (auto& file: files)
        if (file.relativePath == path)
            file.contentHash = hash;
    return files;
}

} // namespace

TEST_CASE("Captured inputs recompute to the fingerprint", "[distributed][fingerprint-diff]")
{
    // One derivation: the record IS the digest's argument list, so it recomputes to exactly what the three-argument
    // spelling -- the launcher's -- computes from the same values, in any order the caller listed the files.
    auto tree = BaseTree();
    auto const record = Cc::CaptureFingerprintInputs("gcc (GCC) 13.2.0", "gnu", tree);
    auto const fingerprint = Cc::ComputeToolchainFingerprint("gcc (GCC) 13.2.0", "gnu", tree);
    CHECK(Cc::ComputeToolchainFingerprint(record) == fingerprint);
    CHECK(record.schema == Cc::FingerprintSchema);
    CHECK(std::ranges::is_sorted(record.files, Cc::FoldsBefore));

    // A record whose files arrive unsorted -- one decoded from a peer that did not sort -- recomputes alike.
    auto unsorted = record;
    std::ranges::reverse(unsorted.files);
    CHECK(Cc::ComputeToolchainFingerprint(unsorted) == fingerprint);

    // And every field is folded: a record differing in any one of them is another digest.
    auto schema = record;
    schema.schema = "toolchain-v2";
    auto banner = record;
    banner.banner = "gcc (GCC) 13.2.1";
    auto grammar = record;
    grammar.driverGrammar = "msvc";
    auto content = record;
    content.files.front().contentHash = "ff";
    for (auto const& changed: { schema, banner, grammar, content })
        CHECK(Cc::ComputeToolchainFingerprint(changed) != fingerprint);
}

TEST_CASE("Diff reports a banner change only", "[distributed][fingerprint-diff]")
{
    auto const diff = DiffFingerprintInputs(Record("gcc (GCC) 13.2.0", BaseTree()), Record("gcc (GCC) 13.3.0", BaseTree()));
    REQUIRE(diff.banner.has_value());
    CHECK(Unwrap(diff.banner).first == "gcc (GCC) 13.2.0");
    CHECK(Unwrap(diff.banner).second == "gcc (GCC) 13.3.0");
    CHECK_FALSE(diff.schemaDiffers);
    CHECK_FALSE(diff.grammar.has_value());
    CHECK(diff.onlyA.empty());
    CHECK(diff.onlyB.empty());
    CHECK(diff.contentDiffers.empty());
    CHECK(diff.verdict == "the compiler banners differ");
    // A different banner already names the compilers as different; nothing past it is inferred.
    CHECK_FALSE(diff.inferred);
    CHECK_FALSE(diff.Identical());
}

TEST_CASE("Diff reports three headers differing under one directory with the inferred verdict",
          "[distributed][fingerprint-diff]")
{
    auto changed = BaseTree();
    changed = WithContent(changed, "c++/13/bits/stl_vector.h", "b1");
    changed = WithContent(changed, "c++/13/bits/stl_map.h", "b2");
    changed = WithContent(changed, "c++/13/bits/stl_list.h", "b3");
    auto const diff = DiffFingerprintInputs(Record("gcc (GCC) 13.2.0", BaseTree()), Record("gcc (GCC) 13.2.0", changed));

    CHECK_FALSE(diff.banner.has_value());
    CHECK_FALSE(diff.grammar.has_value());
    CHECK(diff.contentDiffers
          == std::vector<std::string> { "c++/13/bits/stl_list.h", "c++/13/bits/stl_map.h", "c++/13/bits/stl_vector.h" });
    // The fact, then the inference -- labelled as one.
    CHECK(diff.verdict.starts_with("same banner and grammar; 3 files under `c++/13/bits/` differ"));
    CHECK(diff.verdict.contains("inferred: likely a different libstdc++ package build"));
    CHECK(diff.inferred);
}

TEST_CASE("Diff lists the files only on A and only on B", "[distributed][fingerprint-diff]")
{
    auto left = BaseTree();
    left.push_back(Cc::ToolchainFile { .relativePath = "c++/13/bits/only_left.h", .contentHash = "c1" });
    auto right = BaseTree();
    right.push_back(Cc::ToolchainFile { .relativePath = "c++/13/bits/only_right.h", .contentHash = "c2" });
    right.push_back(Cc::ToolchainFile { .relativePath = "c++/13/bits/also_right.h", .contentHash = "c3" });
    auto const diff = DiffFingerprintInputs(Record("gcc 13", left), Record("gcc 13", right));

    CHECK(diff.onlyA == std::vector<std::string> { "c++/13/bits/only_left.h" });
    CHECK(diff.onlyB == std::vector<std::string> { "c++/13/bits/also_right.h", "c++/13/bits/only_right.h" });
    CHECK(diff.contentDiffers.empty());
    CHECK(diff.verdict.contains("3 files under `c++/13/bits/` differ (1 only on A, 2 only on B, 0 with different "
                                "content)"));
}

TEST_CASE("Diff of identical inputs says so and infers nothing", "[distributed][fingerprint-diff]")
{
    auto const diff = DiffFingerprintInputs(Record("gcc 13", BaseTree()), Record("gcc 13", BaseTree()));
    CHECK(diff.Identical());
    CHECK(diff.verdict.starts_with("the inputs are identical"));
    CHECK_FALSE(diff.inferred);
}

TEST_CASE("Diff names a schema and a grammar difference as facts", "[distributed][fingerprint-diff]")
{
    auto left = Record("clang version 22", BaseTree());
    auto right = left;
    right.schema = "toolchain-v2";
    right.driverGrammar = "msvc";
    auto const diff = DiffFingerprintInputs(left, right);
    CHECK(diff.schemaDiffers);
    REQUIRE(diff.grammar.has_value());
    CHECK(Unwrap(diff.grammar).second == "msvc");
    CHECK(diff.verdict.contains("the fingerprint schemas differ (toolchain-v3 vs toolchain-v2)"));
    CHECK(diff.verdict.contains("the driver grammars differ (gnu vs msvc)"));
    CHECK_FALSE(diff.inferred);
}

TEST_CASE("The inference reads where in the tree the differences sit, most specific row first",
          "[distributed][fingerprint-diff]")
{
    auto const verdictFor = [](std::string const& path) {
        auto left = BaseTree();
        left.push_back(Cc::ToolchainFile { .relativePath = path, .contentHash = "d1" });
        return DiffFingerprintInputs(Record("clang 22", left), Record("clang 22", BaseTree())).verdict;
    };
    CHECK(verdictFor("c++/v1/__config").contains("likely a different libc++ build"));
    CHECK(verdictFor("c++/13/vector2").contains("likely a different libstdc++ package build"));
    CHECK(verdictFor("lib/clang/22/include/stddef.h").contains("compiler's own resource headers"));
    CHECK(verdictFor("sys/types2.h").contains("likely a different build of the package that installs these headers"));
    // A component, never a substring: `libc++abi/` is not `c++/`.
    CHECK_FALSE(verdictFor("libc++abi/cxxabi.h").contains("libstdc++"));
}

TEST_CASE("The common directory is cut at whole path components", "[distributed][fingerprint-diff]")
{
    CHECK(CommonDirectory(std::vector<std::string> {}).empty());
    CHECK(CommonDirectory(std::vector<std::string> { "c++/13/bits/a.h" }) == "c++/13/bits/");
    CHECK(CommonDirectory(std::vector<std::string> { "c++/13/a.h", "c++/14/b.h" }) == "c++/");
    CHECK(CommonDirectory(std::vector<std::string> { "c++/13/bits/a.h", "c++/13/b.h" }) == "c++/13/");
    CHECK(CommonDirectory(std::vector<std::string> { "stdio.h", "c++/13/b.h" }).empty());
}
