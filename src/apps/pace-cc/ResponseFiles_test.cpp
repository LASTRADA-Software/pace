// SPDX-License-Identifier: Apache-2.0
#include "ResponseFiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Pace::Cc;

namespace
{

using Args = std::vector<std::string>;

/// A fake filesystem: exactly the files a case names.
[[nodiscard]] ResponseFileReader FakeFiles(std::map<std::string, std::string, std::less<>> files)
{
    return [files = std::move(files)](std::string_view path) -> std::optional<std::string> {
        if (auto const found = files.find(path); found != files.end())
            return found->second;
        return std::nullopt;
    };
}

} // namespace

// ---------------------------------------------------------------------------
// The Windows grammar

TEST_CASE("Windows grammar: whitespace and newlines separate, quotes group", "[response-files]")
{
    CHECK(SplitResponseFile("/nologo /c\r\n/DPROBE=rsp1\t\"a b.cpp\"\n", DriverFamily::Msvc)
          == Args { "/nologo", "/c", "/DPROBE=rsp1", "a b.cpp" });
}

TEST_CASE("Windows grammar: backslashes are literal unless a quote follows them", "[response-files]")
{
    // The inputs are named outside the assertion macros: MSVC's release build read a raw
    // literal carrying `\"` inside a macro argument as an escape sequence (C4129).
    // A path is the ordinary case, and its backslashes must survive untouched.
    std::string_view const paths = "/IC:\\src\\inc C:\\src\\a.cpp";
    CHECK(SplitResponseFile(paths, DriverFamily::Msvc) == Args { "/IC:\\src\\inc", "C:\\src\\a.cpp" });
    // 2n backslashes before a quote: n backslashes, and the quote toggles.
    std::string_view const evenRun = "\"C:\\dir\\\\\" next";
    CHECK(SplitResponseFile(evenRun, DriverFamily::Msvc) == Args { "C:\\dir\\", "next" });
    // 2n+1 backslashes before a quote: n backslashes and a literal quote.
    std::string_view const oddRun = "/DMSG=\\\"hi\\\"";
    CHECK(SplitResponseFile(oddRun, DriverFamily::Msvc) == Args { "/DMSG=\"hi\"" });
    // `""` inside quotes is one literal quote.
    std::string_view const doubled = "\"a\"\"b\"";
    CHECK(SplitResponseFile(doubled, DriverFamily::Msvc) == Args { "a\"b" });
}

TEST_CASE("Windows grammar: an empty quoted argument is an argument", "[response-files]")
{
    CHECK(SplitResponseFile(R"(a "" b)", DriverFamily::Msvc) == Args { "a", "", "b" });
}

// ---------------------------------------------------------------------------
// The GNU grammar

TEST_CASE("GNU grammar: single quotes are literal, a backslash escapes", "[response-files]")
{
    std::string_view const escapes = "-c 'a b.c' -DX=\\\"y\\\" a\\ b";
    CHECK(SplitResponseFile(escapes, DriverFamily::Gnu) == Args { "-c", "a b.c", "-DX=\"y\"", "a b" });
    CHECK(SplitResponseFile("-I\"/opt/my inc\"\n-O2", DriverFamily::Gnu) == Args { "-I/opt/my inc", "-O2" });
}

TEST_CASE("The two grammars disagree about a backslash, which is why the family chooses", "[response-files]")
{
    // The distinguishing case: a Windows path read by the GNU grammar loses its separators.
    CHECK(SplitResponseFile(R"(C:\src\a.cpp)", DriverFamily::Msvc) == Args { R"(C:\src\a.cpp)" });
    CHECK(SplitResponseFile(R"(C:\src\a.cpp)", DriverFamily::Gnu) == Args { "C:srca.cpp" });
}

// ---------------------------------------------------------------------------
// Expansion

TEST_CASE("A response file is replaced by its arguments, in place", "[response-files][1756]")
{
    auto const read = FakeFiles({ { "args.rsp", "/DPROBE=rsp1 /O2" } });
    auto const expanded = ExpandResponseFiles(Args { "cl", "/c", "@args.rsp", "a.cpp" }, DriverFamily::Msvc, read);
    REQUIRE(expanded.has_value());
    CHECK(*expanded == Args { "cl", "/c", "/DPROBE=rsp1", "/O2", "a.cpp" });
}

TEST_CASE("Changing a response file's CONTENTS changes the expanded command line", "[response-files][1756]")
{
    // #1756's reproduction: same file name, different contents. Everything the key hashes comes
    // from the expanded list, so this inequality is what makes the second build a different key.
    Args const command { "cl", "/c", "@args.rsp", "a.cpp" };
    auto const first = ExpandResponseFiles(command, DriverFamily::Msvc, FakeFiles({ { "args.rsp", "/DPROBE=rsp1" } }));
    auto const second = ExpandResponseFiles(command, DriverFamily::Msvc, FakeFiles({ { "args.rsp", "/DPROBE=rsp2" } }));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);
}

TEST_CASE("A command line with no response file is returned unchanged", "[response-files]")
{
    Args const command { "g++", "-c", "a.cpp", "-o", "a.o" };
    auto const expanded = ExpandResponseFiles(command, DriverFamily::Gnu, FakeFiles({}));
    REQUIRE(expanded.has_value());
    CHECK(*expanded == command);
}

TEST_CASE("The compiler and a lone @ are never response files", "[response-files]")
{
    auto const expanded = ExpandResponseFiles(Args { "@cl", "@", "a.cpp" }, DriverFamily::Msvc, FakeFiles({}));
    REQUIRE(expanded.has_value());
    CHECK(*expanded == Args { "@cl", "@", "a.cpp" });
}

TEST_CASE("A response file that cannot be read is refused by name", "[response-files]")
{
    auto const expanded = ExpandResponseFiles(Args { "cl", "@missing.rsp" }, DriverFamily::Msvc, FakeFiles({}));
    REQUIRE_FALSE(expanded.has_value());
    CHECK(expanded.error() == ResponseFileFailure { .reason = ResponseFileRefusal::Unreadable, .argument = "@missing.rsp" });
    CHECK(DescribeResponseFileFailure(expanded.error()) == "the response file could not be read: @missing.rsp");
}

TEST_CASE("A nested response file is refused rather than guessed at", "[response-files]")
{
    auto const read = FakeFiles({ { "outer.rsp", "/O2 @inner.rsp" }, { "inner.rsp", "/DX" } });
    auto const expanded = ExpandResponseFiles(Args { "cl", "@outer.rsp" }, DriverFamily::Msvc, read);
    REQUIRE_FALSE(expanded.has_value());
    CHECK(expanded.error().reason == ResponseFileRefusal::Nested);
}

TEST_CASE("A UTF-16LE response file is decoded, and a UTF-8 BOM is dropped", "[response-files]")
{
    // "/O2 a.cpp" in UTF-16LE behind its BOM.
    std::string utf16 { "\xFF\xFE" };
    for (auto const c: std::string_view { "/O2 a.cpp" })
    {
        utf16.push_back(c);
        utf16.push_back('\0');
    }
    auto const wide = ExpandResponseFiles(Args { "cl", "@w.rsp" }, DriverFamily::Msvc, FakeFiles({ { "w.rsp", utf16 } }));
    REQUIRE(wide.has_value());
    CHECK(*wide == Args { "cl", "/O2", "a.cpp" });

    auto const bom =
        ExpandResponseFiles(Args { "cl", "@u.rsp" }, DriverFamily::Msvc, FakeFiles({ { "u.rsp", "\xEF\xBB\xBF/O2" } }));
    REQUIRE(bom.has_value());
    CHECK(*bom == Args { "cl", "/O2" });
}

TEST_CASE("A BOM-less non-ASCII response file is refused for cl and read as bytes by gcc", "[response-files]")
{
    // cl reads it in the ANSI code page, which this process cannot name; gcc reads bytes.
    auto const files = FakeFiles({ { "a.rsp", "-I/home/j\xC3\xBCrgen/inc" } });
    auto const msvc = ExpandResponseFiles(Args { "cl", "@a.rsp" }, DriverFamily::Msvc, files);
    REQUIRE_FALSE(msvc.has_value());
    CHECK(msvc.error().reason == ResponseFileRefusal::NotText);

    auto const gnu = ExpandResponseFiles(Args { "gcc", "@a.rsp" }, DriverFamily::Gnu, files);
    REQUIRE(gnu.has_value());
    CHECK(gnu->back() == "-I/home/j\xC3\xBCrgen/inc");
}

TEST_CASE("An expansion too long for CreateProcess is refused for a Windows driver only", "[response-files]")
{
    std::string const huge(WindowsCommandLineBudget, 'x');
    auto const files = FakeFiles({ { "big.rsp", huge } });
    auto const msvc = ExpandResponseFiles(Args { "cl", "@big.rsp" }, DriverFamily::Msvc, files);
    REQUIRE_FALSE(msvc.has_value());
    CHECK(msvc.error() == ResponseFileFailure { .reason = ResponseFileRefusal::TooLong, .argument = "@big.rsp" });

    CHECK(ExpandResponseFiles(Args { "gcc", "@big.rsp" }, DriverFamily::Gnu, files).has_value());
}
