// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "DirectManifest.hpp"

#include <Pace/CompileCache/PathCanon.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pace::Cc
{

/// Whether a header the compiler did NOT find last time would be found now (#1754).
///
/// ## The defect
///
/// A direct-mode manifest re-hashes the headers a compile FOUND. It says nothing about the places
/// searched BEFORE each one, so a header created there later -- `src\inc0\a.hpp` in front of
/// `src\inc\a.hpp`, or an `a.hpp` beside the includer, which `#include "..."` searches first --
/// changes what the compiler would read while every recorded hash still matches. The manifest
/// validated and served the object built from the shadowed header. MSVC code bases repeat
/// `pch.h`, `Resource.h` and `stdafx.h` across modules, which is how a new file becomes a shadow.
///
/// ## The census
///
/// The launcher does not see an `#include` spelling, only the path the compiler resolved. So it
/// over-approximates, which is the safe direction: every recorded header `H` lying under a
/// search directory `D` gives a TAIL, `H` relative to `D`; every search directory `S` joined to
/// every tail is a CANDIDATE a compiler might have found instead. The census is the set of
/// candidates that EXIST. A manifest records it, and a later census that names a candidate the
/// recorded one did not is a shadow that appeared -- the manifest is refused, and the ordinary
/// preprocessed key decides.
///
/// Over-approximation costs a manifest miss when a same-named file appears in a directory the
/// compiler would in fact have searched LATER. It never costs a wrong hit, because every place
/// the compiler can search before the recorded header is a candidate:
/// - the command line's include directories (`PathValueRole::IncludeDir`),
/// - the include-search environment (`INCLUDE`, `CPATH`, ...),
/// - and the directory of every recorded header and of the translation unit, which is where a
///   quoted include is looked up first (MSVC searches every open includer's directory).
///
/// ## What it does not see
///
/// A header reached through a spelling that climbs out of its search directory
/// (`#include "../common/x.h"`) has no tail under any search directory but its own, so a shadow
/// of THAT spelling is not a candidate. Recorded rather than solved: closing it needs the
/// `#include` text, which the launcher does not read.
///
/// Directories inside a compiler's own install are not searched for shadows: their contents
/// change only with the compiler, which the toolchain stamp already keys.

/// Lists one directory's entry names. Injected so the census is testable without a disk.
class IDirectoryListing
{
  public:
    virtual ~IDirectoryListing() = default;

    /// @param directory An absolute directory path.
    /// @return Its entries' names (not paths), or nothing when it does not exist or cannot be read.
    [[nodiscard]] virtual std::optional<std::vector<std::string>> Names(std::string const& directory) const = 0;

  protected:
    IDirectoryListing() = default;
    IDirectoryListing(IDirectoryListing const&) = default;
    IDirectoryListing& operator=(IDirectoryListing const&) = default;
    IDirectoryListing(IDirectoryListing&&) = default;
    IDirectoryListing& operator=(IDirectoryListing&&) = default;
};

/// The production listing, over `std::filesystem`.
/// @return A listing of this machine's disk.
[[nodiscard]] std::unique_ptr<IDirectoryListing> MakeDiskDirectoryListing();

/// The directories a compile searches for headers, from its command line and its environment:
/// resolved against @p workingDirectory, in the layout's spelling, deduplicated. The includer
/// directories are not here -- they depend on the headers, and the census adds them.
/// @param args             The compile arguments, without the compiler.
/// @param family           The driver's family, deciding which spellings are include flags.
/// @param workingDirectory The compile's working directory.
/// @param layout           This build's roots.
/// @param includeSearch    `NAME=value` include-search variables (`CompilerEnvironment`).
/// @return The search directories.
[[nodiscard]] std::vector<std::string> HeaderSearchDirectories(std::span<std::string const> args,
                                                               DriverFamily family,
                                                               std::string_view workingDirectory,
                                                               PathCanon::Layout const& layout,
                                                               std::span<std::string const> includeSearch);

/// Every candidate shadow of @p headers that exists, in the form a manifest records it: a root
/// token where the candidate has one, else its absolute path. Sorted, deduplicated, and never
/// naming one of @p headers itself.
/// @param headers            The resolved paths a manifest records, the translation unit included.
/// @param searchDirectories  `HeaderSearchDirectories`' answer.
/// @param layout             This build's roots.
/// @param listing            The filesystem.
/// @return The census.
[[nodiscard]] std::vector<std::string> ShadowCensus(std::span<std::string const> headers,
                                                    std::span<std::string const> searchDirectories,
                                                    PathCanon::Layout const& layout,
                                                    IDirectoryListing const& listing);

/// The resolved paths a recorded manifest names, localized to this machine: what a census is
/// taken over when the manifest is VALIDATED, as `BuildManifest`'s inputs were when it was recorded.
/// @param manifest The manifest.
/// @param layout   This machine's roots.
/// @return One path per entry, in entry order.
[[nodiscard]] std::vector<std::string> LocalizedEntries(DirectManifest const& manifest, PathCanon::Layout const& layout);

} // namespace Pace::Cc
