// SPDX-License-Identifier: Apache-2.0
#include "ShadowCensus.hpp"

#include <Pace/Platform/NarrowText.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>

namespace Pace::Cc
{

namespace
{
    /// The production listing.
    class DiskDirectoryListing final: public IDirectoryListing
    {
      public:
        [[nodiscard]] std::optional<std::vector<std::string>> Names(std::string const& directory) const override
        {
            auto const path = PathFromNarrowText(directory);
            if (!path.has_value())
                return std::nullopt;
            std::error_code error;
            std::filesystem::directory_iterator entries { *path, error };
            if (error)
                return std::nullopt;
            std::vector<std::string> names;
            for (auto const& entry: entries)
                names.push_back(PathBytes(entry.path().filename()));
            return names;
        }
    };

    /// One spelling for every comparison the census makes: `/`-separated, without a trailing
    /// separator, and lower-cased on a Windows layout, whose filesystems fold case.
    [[nodiscard]] std::string Folded(std::string_view path, bool foldCase)
    {
        std::string out { path };
        std::ranges::replace(out, '\\', '/');
        while (out.size() > 1 && out.back() == '/')
            out.pop_back();
        if (foldCase)
            out = PathCanon::AsciiLower(out);
        return out;
    }

    /// The directory part of a folded path; empty for a path with no separator.
    [[nodiscard]] std::string_view ParentOf(std::string_view folded)
    {
        auto const slash = folded.rfind('/');
        return slash == std::string_view::npos ? std::string_view {} : folded.substr(0, slash);
    }

    /// Answers "does this path exist" from directory listings, listing each directory once.
    class ListingCache
    {
      public:
        ListingCache(IDirectoryListing const& listing, bool foldCase):
            _listing { listing },
            _foldCase { foldCase }
        {
        }

        /// @param directory A folded directory.
        /// @param tail      A folded `/`-separated relative path.
        /// @return True when `directory/tail` exists, judged one component at a time.
        [[nodiscard]] bool Exists(std::string const& directory, std::string_view tail)
        {
            std::string current = directory;
            for (auto const component: tail | std::views::split('/'))
            {
                std::string_view const name { component.begin(), component.end() };
                if (!Contains(current, name))
                    return false;
                current += '/';
                current += name;
            }
            return true;
        }

      private:
        [[nodiscard]] bool Contains(std::string const& directory, std::string_view name)
        {
            auto [entry, inserted] = _names.try_emplace(directory);
            if (inserted)
            {
                if (auto listed = _listing.Names(directory))
                    for (auto const& found: *listed)
                        entry->second.insert(_foldCase ? PathCanon::AsciiLower(found) : found);
            }
            return entry->second.contains(std::string { name });
        }

        IDirectoryListing const& _listing;
        bool _foldCase;
        std::map<std::string, std::set<std::string>, std::less<>> _names;
    };

    /// The entries of a path-list variable's value: `;`-separated when it has one, else
    /// `:`-separated unless the value is a single drive-letter path.
    [[nodiscard]] std::vector<std::string_view> PathListEntries(std::string_view value)
    {
        auto const isDrivePath = value.size() >= 2 && PathCanon::IsDriveLetter(value[0]) && value[1] == ':';
        char const separator = value.contains(';') || isDrivePath ? ';' : ':';
        std::vector<std::string_view> entries;
        for (auto const piece: value | std::views::split(separator))
            if (!piece.empty())
                entries.emplace_back(piece.begin(), piece.end());
        return entries;
    }
} // namespace

std::unique_ptr<IDirectoryListing> MakeDiskDirectoryListing()
{
    return std::make_unique<DiskDirectoryListing>();
}

std::vector<std::string> HeaderSearchDirectories(std::span<std::string const> args,
                                                 DriverFamily family,
                                                 std::string_view workingDirectory,
                                                 PathCanon::Layout const& layout,
                                                 std::span<std::string const> includeSearch)
{
    std::vector<std::string> spelled;
    auto const introducers = IntroducersOf(family);
    std::size_t at = 0;
    while (at < args.size())
    {
        auto const match = MatchPathValueFlag(args[at], introducers, family);
        if (match.has_value() && match->flag.role == PathValueRole::IncludeDir)
        {
            if (!match->value.empty())
                spelled.emplace_back(match->value);
            else if (at + 1 < args.size())
            {
                ++at; // the separated spelling: the directory is the next argument
                spelled.push_back(args[at]);
            }
        }
        ++at;
    }
    for (auto const& variable: includeSearch)
    {
        auto const equals = variable.find('=');
        if (equals == std::string::npos)
            continue;
        for (auto const entry: PathListEntries(std::string_view { variable }.substr(equals + 1)))
            spelled.emplace_back(entry);
    }

    std::vector<std::string> resolved;
    for (auto const& directory: spelled)
    {
        auto absolute = ResolveAgainst(directory, workingDirectory, layout);
        // A directory this machine cannot place is searched by nothing the census can list.
        if (PathCanon::AnchorForLayout(absolute, layout) != PathCanon::Anchor::Absolute)
            continue;
        if (!std::ranges::contains(resolved, absolute))
            resolved.push_back(std::move(absolute));
    }
    return resolved;
}

std::vector<std::string> ShadowCensus(std::span<std::string const> headers,
                                      std::span<std::string const> searchDirectories,
                                      PathCanon::Layout const& layout,
                                      IDirectoryListing const& listing)
{
    bool const foldCase = PathCanon::IsWindowsLayout(layout);

    std::set<std::string> folded;
    for (auto const& header: headers)
        folded.insert(Folded(header, foldCase));

    // Every place a compiler might look: the stated search path, and every includer's own
    // directory. A compiler's own install is left out -- its contents move only with the
    // compiler, which the toolchain stamp already keys.
    std::set<std::string> directories;
    auto const consider = [&directories, foldCase](std::string_view directory) {
        if (!directory.empty() && !CoveredByToolchainStamp(directory))
            directories.insert(Folded(directory, foldCase));
    };
    for (auto const& directory: searchDirectories)
        consider(directory);
    for (auto const& header: folded)
        consider(ParentOf(header));

    // Each header's tail under each directory holding it: the spellings that could have found it.
    std::set<std::string> tails;
    for (auto const& header: folded)
        for (auto const& directory: directories)
            if (header.size() > directory.size() + 1 && header.starts_with(directory) && header[directory.size()] == '/')
                tails.insert(header.substr(directory.size() + 1));

    ListingCache cache { listing, foldCase };
    std::vector<std::string> census;
    for (auto const& directory: directories)
        for (auto const& tail: tails)
        {
            std::string candidate = directory;
            candidate += '/';
            candidate += tail;
            if (folded.contains(candidate) || !cache.Exists(directory, tail))
                continue;
            auto token = PathCanon::CanonicalToken(candidate, layout);
            census.push_back(token.has_value() ? *std::move(token) : std::move(candidate));
        }
    std::ranges::sort(census);
    auto const duplicates = std::ranges::unique(census);
    census.erase(duplicates.begin(), duplicates.end());
    return census;
}

std::vector<std::string> LocalizedEntries(DirectManifest const& manifest, PathCanon::Layout const& layout)
{
    std::vector<std::string> paths;
    paths.reserve(manifest.entries.size());
    for (auto const& entry: manifest.entries)
        paths.push_back(PathCanon::Localize(entry.canonicalPath, layout));
    return paths;
}

} // namespace Pace::Cc
