// SPDX-License-Identifier: Apache-2.0
#include "ShadowCensus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Pace;
using namespace Pace::Cc;

namespace
{

using Paths = std::vector<std::string>;

/// A disk made of the files a case names. A directory exists when some file lies under it, and
/// `Names` answers a directory's immediate children. Paths are compared exactly as given, so a
/// case writes them in the census's own fold (`/`-separated; lower case on a Windows layout).
class FakeDisk final: public IDirectoryListing
{
  public:
    explicit FakeDisk(std::set<std::string> files):
        _files { std::move(files) }
    {
    }

    void Add(std::string file)
    {
        _files.insert(std::move(file));
    }

    [[nodiscard]] std::optional<std::vector<std::string>> Names(std::string const& directory) const override
    {
        ++_listings;
        std::set<std::string> names;
        auto const prefix = directory + '/';
        for (auto const& file: _files)
        {
            if (!file.starts_with(prefix))
                continue;
            auto const rest = std::string_view { file }.substr(prefix.size());
            names.emplace(rest.substr(0, rest.find('/')));
        }
        if (names.empty())
            return std::nullopt;
        return std::vector<std::string> { names.begin(), names.end() };
    }

    [[nodiscard]] int Listings() const noexcept
    {
        return _listings;
    }

  private:
    std::set<std::string> _files;
    mutable int _listings = 0;
};

// The layout of #1754's reproduction: one checkout, two include directories, `inc0` in front.
PathCanon::Layout const Windows { .sourceRoot = "c:/work/src", .buildTree = "c:/work/build" };
Paths const SearchPath { "c:/work/src/inc0", "c:/work/src/inc" };
Paths const Recorded { "c:/work/src/main.cpp", "c:/work/src/inc/a.hpp" };

[[nodiscard]] FakeDisk ReproDisk()
{
    return FakeDisk { { "c:/work/src/main.cpp", "c:/work/src/inc/a.hpp", "c:/work/src/inc0/.keep" } };
}

} // namespace

TEST_CASE("A header created in an earlier include directory changes the census", "[shadow-census][1754]")
{
    auto disk = ReproDisk();
    auto const recorded = ShadowCensus(Recorded, SearchPath, Windows, disk);

    disk.Add("c:/work/src/inc0/a.hpp");
    auto const now = ShadowCensus(Recorded, SearchPath, Windows, disk);

    CHECK(now != recorded);
    CHECK(std::ranges::contains(now, std::string { "<SRCROOT>/inc0/a.hpp" }));
}

TEST_CASE("A header created beside the includer changes the census", "[shadow-census][1754]")
{
    // `#include "a.hpp"` searches the includer's own directory first, so `src/a.hpp` shadows
    // `src/inc/a.hpp` without being on any include path.
    auto disk = ReproDisk();
    auto const recorded = ShadowCensus(Recorded, SearchPath, Windows, disk);

    disk.Add("c:/work/src/a.hpp");
    CHECK(ShadowCensus(Recorded, SearchPath, Windows, disk) != recorded);
}

TEST_CASE("An unrelated new file leaves the census unchanged", "[shadow-census][1754]")
{
    // The precision half: a census over every file name would refuse every manifest whose
    // directory gained a `.cpp`, which is most of them during active development.
    auto disk = ReproDisk();
    auto const recorded = ShadowCensus(Recorded, SearchPath, Windows, disk);

    disk.Add("c:/work/src/inc0/b.hpp");
    disk.Add("c:/work/src/other.cpp");
    CHECK(ShadowCensus(Recorded, SearchPath, Windows, disk) == recorded);
}

TEST_CASE("A same-named file that already existed is recorded and does not refuse", "[shadow-census][1754]")
{
    // It existed when the compiler chose `inc/a.hpp`, so it is not in front of it -- or the
    // compiler would have read it. Recording it is what keeps the manifest valid.
    FakeDisk const disk {
        { "c:/work/src/main.cpp", "c:/work/src/inc/a.hpp", "c:/work/src/inc0/.keep", "c:/work/build/gen/a.hpp" }
    };
    Paths const searchPath { "c:/work/src/inc0", "c:/work/src/inc", "c:/work/build/gen" };
    auto const recorded = ShadowCensus(Recorded, searchPath, Windows, disk);
    CHECK(recorded == Paths { "<BUILDTREE>/gen/a.hpp" });
    CHECK(ShadowCensus(Recorded, searchPath, Windows, disk) == recorded);
}

TEST_CASE("A shadow of a nested spelling is seen", "[shadow-census][1754]")
{
    // `#include "detail/x.hpp"` found under `inc` has the tail `detail/x.hpp`, and its shadow is a
    // whole new subdirectory of `inc0`.
    Paths const recorded { "c:/work/src/main.cpp", "c:/work/src/inc/detail/x.hpp" };
    FakeDisk disk { { "c:/work/src/main.cpp", "c:/work/src/inc/detail/x.hpp", "c:/work/src/inc0/.keep" } };
    auto const before = ShadowCensus(recorded, SearchPath, Windows, disk);

    disk.Add("c:/work/src/inc0/detail/x.hpp");
    CHECK(ShadowCensus(recorded, SearchPath, Windows, disk) != before);
}

TEST_CASE("A Windows layout compares case-insensitively", "[shadow-census][1754]")
{
    // The compiler's spelling and the layout's differ in case on Windows; the census must not
    // read that as a different header, or every hit would be refused.
    Paths const mixedCase { "C:/Work/Src/Main.cpp", "C:/Work/Src/Inc/A.hpp" };
    auto disk = ReproDisk();
    CHECK(ShadowCensus(mixedCase, SearchPath, Windows, disk) == ShadowCensus(Recorded, SearchPath, Windows, disk));

    disk.Add("c:/work/src/inc0/a.hpp");
    CHECK_FALSE(ShadowCensus(mixedCase, SearchPath, Windows, disk).empty());
}

TEST_CASE("A compiler's own install is not searched for shadows", "[shadow-census][1754]")
{
    // Its contents move only with the compiler, which the toolchain stamp keys -- and listing an
    // SDK directory on every direct hit is a cost with nothing to find.
    FakeDisk disk {
        { "c:/work/src/main.cpp", "c:/work/src/inc/a.hpp", "c:/program files (x86)/windows kits/10/include/um/a.hpp" }
    };
    Paths const searchPath { "c:/program files (x86)/windows kits/10/include/um", "c:/work/src/inc" };
    CHECK(ShadowCensus(Recorded, searchPath, Windows, disk).empty());
}

TEST_CASE("Each directory is listed once however many tails it is asked about", "[shadow-census]")
{
    Paths const recorded {
        "c:/work/src/main.cpp", "c:/work/src/inc/a.hpp", "c:/work/src/inc/b.hpp", "c:/work/src/inc/c.hpp"
    };
    FakeDisk const disk {
        { "c:/work/src/main.cpp", "c:/work/src/inc/a.hpp", "c:/work/src/inc/b.hpp", "c:/work/src/inc/c.hpp" }
    };
    (void) ShadowCensus(recorded, SearchPath, Windows, disk);
    // src, src/inc, src/inc0 -- and nothing listed twice.
    CHECK(disk.Listings() <= 3);
}

// ---------------------------------------------------------------------------
// HeaderSearchDirectories

namespace
{
/// @p paths with every separator spelled `/`. A Windows layout resolves into backslashes on a
/// Windows host and keeps the forward slashes on a POSIX one (`ResolveAgainst` joins with the
/// host's arithmetic), and which DIRECTORIES were found is the property under test, never the
/// separator the host chose for them.
[[nodiscard]] Paths Slashed(Paths paths)
{
    for (auto& path: paths)
        std::ranges::replace(path, '\\', '/');
    return paths;
}
} // namespace

TEST_CASE("Include directories are read in every spelling, separated or fused", "[shadow-census]")
{
    Paths const args { "/nologo", "/Ic:/work/src/inc0", "/I", "c:/work/src/inc", "/external:Ic:/work/ext", "/c", "a.cpp" };
    CHECK(Slashed(HeaderSearchDirectories(args, DriverFamily::Msvc, "c:/work/build", Windows, {}))
          == Paths { "c:/work/src/inc0", "c:/work/src/inc", "c:/work/ext" });
}

TEST_CASE("The include-search environment adds its directories", "[shadow-census][1756]")
{
    Paths const environment { "INCLUDE=c:/sdk/one;c:/sdk/two" };
    CHECK(
        Slashed(HeaderSearchDirectories(Paths { "/c", "a.cpp" }, DriverFamily::Msvc, "c:/work/build", Windows, environment))
        == Paths { "c:/sdk/one", "c:/sdk/two" });
}

TEST_CASE("A GNU path list splits on colons", "[shadow-census]")
{
    PathCanon::Layout const posix { .sourceRoot = "/work/src", .buildTree = "/work/build" };
    Paths const environment { "CPATH=/opt/a/include:/opt/b/include" };
    CHECK(HeaderSearchDirectories(Paths { "-c", "a.cpp" }, DriverFamily::Gnu, "/work/build", posix, environment)
          == Paths { "/opt/a/include", "/opt/b/include" });
}
