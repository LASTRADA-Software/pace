// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/FingerprintDiff.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <ranges>

#include <core/Ranges.hpp>

namespace Pace::Distributed
{

namespace
{
    /// Every path of a record, with its content digests -- several, in the one theoretical case of a path reported
    /// twice, so that case reads as a content difference rather than as one digest winning.
    using PathDigests = std::map<std::string, std::vector<std::string>>;

    /// @param inputs A record.
    /// @return Its files by path, digests sorted.
    [[nodiscard]] PathDigests ByPath(Cc::FingerprintInputs const& inputs)
    {
        auto byPath = PathDigests {};
        for (auto const& file: inputs.files)
            byPath[file.relativePath].push_back(file.contentHash);
        for (auto& digests: byPath | std::views::values)
            std::ranges::sort(digests);
        return byPath;
    }

    /// @param count How many.
    /// @param noun The singular.
    /// @return E.g. `3 files`, `1 file`.
    [[nodiscard]] std::string Counted(std::size_t count, std::string_view noun)
    {
        return std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
    }

    /// @param directory A common directory, or empty.
    /// @return The reading of it: the first `TreeInferences` row whose needle it holds.
    [[nodiscard]] TreeInference const& InferenceFor(std::string const& directory)
    {
        auto const anchored = std::format("/{}", directory);
        auto const* const row = core::findIfOrNull(TreeInferences, [&anchored](TreeInference const& candidate) {
            return candidate.needle.empty() || anchored.contains(candidate.needle);
        });
        // The last row's empty needle matches everything, so the walk always ends on a row.
        return *row;
    }
} // namespace

std::string CommonDirectory(std::span<std::string const> paths)
{
    if (paths.empty())
        return {};
    // A path's directory is everything up to and including its last slash; a file at the root has none.
    auto const directoryOf = [](std::string_view path) {
        auto const slash = path.rfind('/');
        return slash == std::string_view::npos ? std::string_view {} : path.substr(0, slash + 1);
    };
    auto common = directoryOf(paths.front());
    for (auto const& path: paths.subspan(1))
    {
        auto const directory = directoryOf(path);
        // A length rather than the iterator `mismatch` returns: that is a pointer on one standard library and a class
        // on another, so no spelling of a variable holding it is portable (`readability-qualified-auto`).
        auto const agreed =
            static_cast<std::size_t>(std::ranges::distance(common.begin(), std::ranges::mismatch(common, directory).in1));
        common = common.substr(0, agreed);
        // Cut back to a whole component: `c++/13/` and `c++/14/` share `c++/`, never `c++/1`.
        auto const slash = common.rfind('/');
        common = slash == std::string_view::npos ? std::string_view {} : common.substr(0, slash + 1);
    }
    return std::string { common };
}

FingerprintDiff DiffFingerprintInputs(Cc::FingerprintInputs const& a, Cc::FingerprintInputs const& b)
{
    auto diff = FingerprintDiff {};
    diff.schemaDiffers = a.schema != b.schema;
    if (a.banner != b.banner)
        diff.banner = std::pair { a.banner, b.banner };
    if (a.driverGrammar != b.driverGrammar)
        diff.grammar = std::pair { a.driverGrammar, b.driverGrammar };

    auto const left = ByPath(a);
    auto const right = ByPath(b);
    for (auto const& [path, digests]: left)
    {
        auto const other = right.find(path);
        if (other == right.end())
            diff.onlyA.push_back(path);
        else if (other->second != digests)
            diff.contentDiffers.push_back(path);
    }
    for (auto const& path: right | std::views::keys)
        if (!left.contains(path))
            diff.onlyB.push_back(path);

    if (diff.Identical())
    {
        diff.verdict = "the inputs are identical: same schema, banner, grammar and files";
        return diff;
    }

    // The facts first, each one a comparison's direct result.
    auto facts = std::vector<std::string> {};
    if (diff.schemaDiffers)
        facts.push_back(std::format("the fingerprint schemas differ ({} vs {}), so the two pace-node builds digest "
                                    "differently",
                                    a.schema,
                                    b.schema));
    if (diff.banner.has_value())
        facts.emplace_back("the compiler banners differ");
    if (diff.grammar.has_value())
        facts.push_back(std::format("the driver grammars differ ({} vs {})", a.driverGrammar, b.driverGrammar));

    auto const differing = diff.onlyA.size() + diff.onlyB.size() + diff.contentDiffers.size();
    auto directory = std::string {};
    if (differing != 0)
    {
        auto paths = std::vector<std::string> {};
        paths.reserve(differing);
        paths.insert_range(paths.end(), diff.onlyA);
        paths.insert_range(paths.end(), diff.onlyB);
        paths.insert_range(paths.end(), diff.contentDiffers);
        directory = CommonDirectory(paths);
        auto const where =
            directory.empty() ? std::string { "across the include tree" } : std::format("under `{}`", directory);
        facts.push_back(std::format("{} {} differ ({} only on A, {} only on B, {} with different content)",
                                    Counted(differing, "file"),
                                    where,
                                    diff.onlyA.size(),
                                    diff.onlyB.size(),
                                    diff.contentDiffers.size()));
    }

    auto const sameIdentity = !diff.schemaDiffers && !diff.banner.has_value() && !diff.grammar.has_value();
    diff.verdict = std::format("{}{}", sameIdentity ? "same banner and grammar; " : "", [&facts] {
        auto joined = std::string {};
        for (auto const& fact: facts)
            joined += joined.empty() ? fact : std::format("; {}", fact);
        return joined;
    }());

    // The one inference: where the compiler identity agrees and only its tree differs, WHERE the difference sits is
    // read for what installs that directory. Said only then, and labelled -- a different banner already names the
    // compiler as different, and nothing past that is what two lists of digests show.
    if (sameIdentity && differing != 0)
    {
        diff.verdict += std::format(" -- inferred: likely {}", InferenceFor(directory).likely);
        diff.inferred = true;
    }
    return diff;
}

} // namespace Pace::Distributed
