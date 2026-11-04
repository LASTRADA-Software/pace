// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/CompileCache/KeyDigest.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file ToolchainFingerprint.hpp
/// What a toolchain fingerprint is made of, and the digest over it.
///
/// **Header-only and dependency-free** beyond `KeyDigest.hpp`, because two binaries that do not share a library
/// compute it: `pace-cc` (which does not link `Pace`) keys a dispatch on it, and a `pace-node` recomputes ANOTHER
/// node's fingerprint from the inputs record that node sent, before it explains how two fingerprints differ
/// (`toolchain_diff`). One function compiled into both, so the recomputation is the launcher's digest rather than a
/// second implementation that agrees with it today.

namespace Pace::Cc
{

/// One header in the toolchain's own include tree, as the fingerprint sees it.
struct ToolchainFile
{
    /// Path **relative to the include root it was found under**, `/`-separated.
    ///
    /// Relative, not absolute, and that is the whole design. Two machines running
    /// the same toolchain at different install prefixes — `/usr/lib/gcc/...` and
    /// `/opt/toolchains/gcc-13/...`, or a vendored SDK checked out at two depths —
    /// must produce the *same* fingerprint, or distribution is disabled between
    /// exactly the machines it exists to connect. An absolute path would make the
    /// prefix part of the identity and split the fleet by install location.
    std::string relativePath;

    /// Digest of the file's contents.
    ///
    /// Contents, never mtime or size. Install times differ on every machine, so an
    /// mtime-derived fingerprint would make every pair of machines mismatch — and
    /// it would do so *silently*, presenting as "distribution never finds a
    /// worker" rather than as an error. Size alone is far too weak: two headers
    /// differing by one character are the case that matters.
    std::string contentHash;
};

/// Schema tag for the fingerprint digest.
///
/// Versioned like the cache keys are, and for the same reason: if the *inputs*
/// ever change shape — a different set of roots, a different hash — old and new
/// launchers must not agree by accident on a value that means different things.
/// A fingerprint mismatch merely disables distribution, so a bump here is cheap;
/// a false *match* would dispatch to the wrong toolchain, which is the failure
/// this whole mechanism exists to prevent.
///
/// v3 (#1656): `cl` folds no include tree and a clang targeting MSVC folds only its
/// own resource directory (`FingerprintTree`), so the same toolchain digests
/// differently under v2 and v3. The fingerprint cache's stamp folds this tag, so an
/// entry written under v2 is a miss rather than a v2 value served as a v3 one.
inline constexpr std::string_view FingerprintSchema = "toolchain-v3";

/// Everything a toolchain fingerprint is computed from, as the digest saw it.
///
/// **The record IS the digest's argument list**, never a description gathered beside it: `CaptureFingerprintInputs`
/// builds it from the very values a caller would pass, and `ComputeToolchainFingerprint` digests nothing else, so a
/// node that kept the record it fingerprinted from can hand it to another node, and that node can recompute the
/// same digest from it. A record re-gathered after the fact could describe a tree that moved since, and a diff of
/// it would explain a fingerprint the node no longer has.
struct FingerprintInputs
{
    /// The schema the digest was taken under -- `FingerprintSchema` for a record this build captured.
    ///
    /// Carried rather than assumed, so a record from a build of another schema is told apart by NAME rather than
    /// by a digest that merely fails to match.
    std::string schema;

    /// The compiler's own version line, verbatim.
    std::string banner;

    /// `DriverGrammarName` of the driver's family; empty for an unrecognised driver.
    std::string driverGrammar;

    /// The include-tree files the driver row's `FingerprintTree` selected, SORTED by path and then by hash -- the
    /// order the digest folds them in, so two records compare file by file.
    std::vector<ToolchainFile> files;
};

/// Order two files the way the digest folds them: by path, then by hash.
///
/// By path, then by hash, so the order is total even if a caller somehow reports one path twice. Leaving it
/// partial would make the result depend on the sort's stability, which is exactly the kind of coupling the sort
/// exists to remove.
/// @param a One file.
/// @param b Another.
/// @return True when @p a folds before @p b.
[[nodiscard]] inline bool FoldsBefore(ToolchainFile const& a, ToolchainFile const& b) noexcept
{
    return a.relativePath != b.relativePath ? a.relativePath < b.relativePath : a.contentHash < b.contentHash;
}

/// Capture the inputs a fingerprint is about to be computed from, under this build's schema.
///
/// The one place a record is made: a caller that fingerprints passes the result straight to
/// `ComputeToolchainFingerprint`, so what it keeps and what it digested are one value.
///
/// The files are sorted here rather than trusted from the caller. The caller's order comes from a directory
/// traversal, which is a property of the filesystem -- two machines with byte-identical toolchains can enumerate
/// them differently, and an order-sensitive digest would report that as two different toolchains.
/// @param compilerBanner The compiler's own version line, verbatim.
/// @param driverGrammar `DriverGrammarName` of the driver's family; empty identifies nothing.
/// @param files The toolchain's include tree; order does not matter.
/// @return The record, its files sorted.
[[nodiscard]] inline FingerprintInputs CaptureFingerprintInputs(std::string_view compilerBanner,
                                                                std::string_view driverGrammar,
                                                                std::vector<ToolchainFile> files)
{
    std::ranges::sort(files, FoldsBefore);
    return FingerprintInputs { .schema = std::string { FingerprintSchema },
                               .banner = std::string { compilerBanner },
                               .driverGrammar = std::string { driverGrammar },
                               .files = std::move(files) };
}

/// Compute a toolchain fingerprint from a compiler identity and its include tree.
///
/// **Pure**: it touches no filesystem and reads no clock. Gathering the file list
/// is the caller's job (see `ProbeToolchainFiles`), which is what lets every rule
/// below be a unit test rather than a fixture with a real compiler installed.
///
/// The banner is folded in as well as the files, because the two answer different
/// questions and neither subsumes the other -- WHERE the toolchain owns a tree. Which
/// files are passed is the driver row's `FingerprintTree`: a tree the driver merely
/// BORROWS (the VC toolset and the Windows SDK, under `cl` and under any clang
/// targeting MSVC) is never passed, because a worker opens no header from it for a
/// preprocessed compile, and passing it split machines on an installed VS language
/// pack's IntelliSense XML (#1656). A banner alone is what the cache key
/// already uses, and it is too weak here: two machines can print an identical
/// `--version` while resolving different libstdc++ headers, and for *distribution*
/// that produces a silently wrong object rather than the stale path a replay guard
/// can probe. The headers alone are too weak in the other direction: two compilers
/// can share a header tree and generate different code from it.
///
/// The file list is sorted before digesting, so the caller's traversal order — a
/// directory iteration order, which is a property of the filesystem rather than of
/// the toolchain — cannot change the answer.
///
/// **The driver GRAMMAR is folded in too**, and it is the third independent question:
/// `clang-cl`, `clang++` and `clang` from one LLVM install print the same banner and
/// own one include tree, so the two inputs above cannot tell them apart — and a worker
/// looks a toolchain up by fingerprint, runs *its* driver, and appends the client's
/// arguments verbatim, so a GNU driver serving an MSVC-spelled job reads `/std:c++20`
/// as a filename ([#226](https://github.com/LASTRADA-Software/pace/issues/226)).
/// The name comes from `DriverGrammarName`, never the enumerator's value, for the
/// reason that function gives.
///
/// **The one digest.** Every other spelling forwards here, so a record captured by `CaptureFingerprintInputs` and
/// sent to another machine recomputes to exactly the value its sender registered. The schema folded is the
/// RECORD's: a record this build captured carries `FingerprintSchema`, and a caller handed a record of another
/// schema has to decide for itself whether this build's digest describes it.
///
/// A record whose files are not in `FoldsBefore` order -- one decoded from a peer that did not sort -- is sorted
/// on a copy first, so the answer never depends on who built the record.
/// @param inputs What to digest.
/// @return An opaque fingerprint, stable across machines with the same toolchain.
[[nodiscard]] inline std::string ComputeToolchainFingerprint(FingerprintInputs const& inputs)
{
    auto const fold = [&inputs](std::vector<ToolchainFile> const& files) {
        KeyDigest digest { inputs.schema };
        digest.Field(inputs.banner);
        // A separate length-prefixed field, never appended to the banner: two pieces
        // concatenated are not a framing, and the whole reason this value exists is that
        // one banner names three drivers.
        digest.Field(inputs.driverGrammar);
        for (auto const& file: files)
        {
            // Path and hash are folded as separate length-prefixed pieces, not
            // concatenated. Concatenation is not a framing: `{"ab", "c"}` and
            // `{"a", "bc"}` would digest identically, so a header renamed in a way that
            // shifted a character into its hash would fingerprint the same as the
            // original -- a false MATCH, which is the one error direction that
            // dispatches to the wrong toolchain.
            digest.Path(file.relativePath);
            digest.Item(file.contentHash);
        }
        return digest.ToHex();
    };
    if (std::ranges::is_sorted(inputs.files, FoldsBefore))
        return fold(inputs.files);
    auto sorted = inputs.files;
    std::ranges::sort(sorted, FoldsBefore);
    return fold(sorted);
}

/// Compute a toolchain fingerprint from a compiler identity and its include tree, under this build's schema.
///
/// `ComputeToolchainFingerprint(CaptureFingerprintInputs(...))`, spelled once for the callers that keep no record.
/// @param compilerBanner The compiler's own version line, verbatim.
/// @param driverGrammar `DriverGrammarName` of the driver's family; empty identifies
///        nothing, which is what an unrecognised driver gets.
/// @param files The toolchain's include tree; order does not matter.
/// @return An opaque fingerprint, stable across machines with the same toolchain.
[[nodiscard]] inline std::string ComputeToolchainFingerprint(std::string_view compilerBanner,
                                                             std::string_view driverGrammar,
                                                             std::vector<ToolchainFile> files)
{
    return ComputeToolchainFingerprint(CaptureFingerprintInputs(compilerBanner, driverGrammar, std::move(files)));
}

} // namespace Pace::Cc
