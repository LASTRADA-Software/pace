#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Give each changed HEADER a compile command borrowed from a unit that includes it.

## Why this exists

The `clang-tidy` job's "Analyse the changed lines" step hands every changed header to
`clang-tidy-diff.py`, which runs `clang-tidy -p=<db> <header>`. A header has no entry in
`compile_commands.json`, so clang-tidy INTERPOLATES a command from the entry whose path
looks most like the header's. That guess knows nothing about includes: for a test-only
header the nearest-looking unit is often one built without the include directories the
header needs, and the analysis fails `[clang-diagnostic-error] ... file not found` on
a header that compiles fine in every unit that actually includes it. CI run 43735465655
failed that way on four headers, and `src/tests/CgroupFakes.hpp` is guessed wrong from
master's own files alone.

So the command is taken from a unit that INCLUDES the header, which by construction has
the include directories and definitions the header compiles under.

## What it writes

A NEW `compile_commands.json` in `--out`, never the build tree's own: the input database
is left byte-for-byte as configure wrote it, so nothing else that reads it -- ninja's
generated-header pass, a later sweep -- can see an entry this step invented. Every input
entry is copied unchanged, and one entry is appended per header that has an includer.
`directory` stays the build tree's, so every relative path in the borrowed command
resolves exactly as it did for the unit.

A header entry is the includer's argument vector with the unit's source, `-c` and the
`-o <object>` pair removed, and `-x c++-header <header>` appended. `c++-header` rather
than `c++` because a header analysed as a main file otherwise draws `#pragma once in main
file`, which is a finding about how it was invoked rather than about the header.

## How the includer is chosen

The include graph is built by RESOLVING each `#include` spelling the way the compiler
would -- against the including file's own directory, then against every `-I`, `-isystem`
and `-iquote` directory the database names -- and keeping only files under `<repo>/src/`.
No tree listing is taken: the files are the units the database names and whatever they
reach, so a header nothing in this database reaches is, correctly, not in the graph. A
spelling that resolves in more than one directory keeps every candidate; that can only
offer an extra includer, never hide one.

From each header a breadth-first walk over the reverse graph stops at the first depth
holding a unit the database compiles: a DIRECT includer wins over one that reaches the
header through another first-party header. Ties go to the unit whose stem is the
header's (`Foo.hpp` -> `Foo.cpp`, or `Foo_test.cpp`), then to the smallest path, so the
choice is stable from run to run.

## A header with no includer

Left to interpolation EXACTLY as before, and NAMED on stdout -- the step used to do that
silently. It is never dropped from the analysis: this script only adds entries.

GNU-style driver lines only (the Linux leg's clang): an MSVC-style `/c` `/Fo` line is not
rewritten correctly here, and `clang-tidy-windows` does not call this.

Usage:
  python3 scripts/tidy-header-commands.py --db <dir> --out <dir> [--repo <dir>] <header>...
  python3 scripts/tidy-header-commands.py --self-test

Exit 0 written, 2 usage or an unreadable database.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import sys
import tempfile

IncludeLine = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^">]+)[">]', re.MULTILINE)

# The flags whose value names an include directory, joined (`-Ifoo`) or separate
# (`-I foo`). One row per spelling; the search order between them does not matter here,
# since every resolving candidate is kept.
IncludeDirFlags = ("-I", "-isystem", "-iquote", "-idirafter")

# The tokens dropped from a borrowed command: the unit's own compile-only switch, and the
# object it writes (the flag and its value). The SOURCE is dropped separately, by path.
DroppedSwitches = ("-c",)
DroppedWithValue = ("-o",)


def ArgvOf(entry):
    """The entry's argument vector, from `arguments` or a POSIX-split `command`."""
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def AbsoluteIn(directory, path):
    """`path` made absolute against `directory` and normalised."""
    return os.path.normpath(os.path.join(directory, path))


def IncludeDirsOf(entry):
    """Every include directory one database entry names, absolute."""
    argv = ArgvOf(entry)
    dirs = []
    pending = None
    for token in argv:
        if pending is not None:
            dirs.append(AbsoluteIn(entry["directory"], token))
            pending = None
            continue
        for flag in IncludeDirFlags:
            if token == flag:
                pending = flag
                break
            if token.startswith(flag) and len(token) > len(flag):
                dirs.append(AbsoluteIn(entry["directory"], token[len(flag):]))
                break
    return dirs


def IsUnder(path, root):
    """Whether `path` lies inside `root` (both absolute, normalised)."""
    return path == root or path.startswith(root + os.sep)


def ReadIncludes(path):
    """The `(bracket, spelling)` pairs one file includes; empty if it cannot be read."""
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return IncludeLine.findall(handle.read())
    except OSError:
        return []


def BuildReverseGraph(db, srcRoot):
    """Map each first-party file to the set of first-party files that `#include` it.

    @param db The parsed compile database.
    @param srcRoot The absolute directory a file must lie under to be followed.
    @return (includers, units): the reverse graph, and the set of unit paths in `db`.
    """
    units = {AbsoluteIn(e["directory"], e["file"]) for e in db}
    searchDirs = []
    for entry in db:
        for directory in IncludeDirsOf(entry):
            if directory not in searchDirs:
                searchDirs.append(directory)

    resolved = {}

    def Resolve(includer, bracket, spelling):
        key = (os.path.dirname(includer) if bracket == '"' else None, spelling)
        if key in resolved:
            return resolved[key]
        bases = ([key[0]] if key[0] else []) + searchDirs
        hits = []
        for base in bases:
            candidate = os.path.normpath(os.path.join(base, spelling))
            if candidate not in hits and IsUnder(candidate, srcRoot) and os.path.isfile(candidate):
                hits.append(candidate)
        resolved[key] = hits
        return hits

    includers = {}
    seen = set()
    queue = sorted(u for u in units if IsUnder(u, srcRoot))
    while queue:
        current = queue.pop()
        if current in seen:
            continue
        seen.add(current)
        for bracket, spelling in ReadIncludes(current):
            for target in Resolve(current, bracket, spelling):
                if target == current:
                    continue
                includers.setdefault(target, set()).add(current)
                if target not in seen:
                    queue.append(target)
    return includers, units


def StemOf(path):
    """A file's stem with a trailing `_test` removed, for the same-name tie-break."""
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem[: -len("_test")] if stem.endswith("_test") else stem


def ChooseIncluder(header, includers, units):
    """The unit whose command `header` borrows, or None when no unit reaches it.

    Breadth-first over the reverse graph: the first depth holding a database unit wins.
    """
    frontier = [header]
    visited = {header}
    while frontier:
        nextFrontier = []
        for node in frontier:
            for includer in includers.get(node, ()):
                if includer not in visited:
                    visited.add(includer)
                    nextFrontier.append(includer)
        found = [n for n in nextFrontier if n in units]
        if found:
            stem = StemOf(header)
            return sorted(found, key=lambda unit: (StemOf(unit) != stem, unit))[0]
        frontier = nextFrontier
    return None


def HeaderEntry(unitEntry, header):
    """`unitEntry`'s command, re-aimed at `header` and parsed as a C++ header.

    @param unitEntry The includer's database entry.
    @param header The absolute header path.
    @return A new database entry for the header.
    """
    directory = unitEntry["directory"]
    unitFile = AbsoluteIn(directory, unitEntry["file"])
    argv = ArgvOf(unitEntry)
    kept = [argv[0]]
    skipNext = False
    for token in argv[1:]:
        if skipNext:
            skipNext = False
            continue
        if token in DroppedWithValue:
            skipNext = True
            continue
        if token in DroppedSwitches:
            continue
        if AbsoluteIn(directory, token) == unitFile:
            continue
        kept.append(token)
    return {
        "directory": directory,
        "arguments": kept + ["-x", "c++-header", header],
        "file": header,
    }


def Augment(db, repo, headers, report):
    """The augmented database: `db` unchanged, plus one entry per header with an includer.

    @param db The parsed compile database.
    @param repo The repository root; relative header paths are taken against it.
    @param headers The changed headers, relative or absolute.
    @param report Called with one line of text per decision.
    @return The new list of entries.
    """
    srcRoot = os.path.join(os.path.normpath(os.path.abspath(repo)), "src")
    includers, units = BuildReverseGraph(db, srcRoot)
    byFile = {}
    for entry in db:
        byFile.setdefault(AbsoluteIn(entry["directory"], entry["file"]), entry)
    out = list(db)
    for spelled in headers:
        header = os.path.normpath(os.path.join(os.path.abspath(repo), spelled))
        if header in byFile:
            report(f"already in the database: {spelled}")
            continue
        unit = ChooseIncluder(header, includers, units)
        if unit is None:
            report(f"no unit in the database includes {spelled}; "
                   f"clang-tidy will interpolate its command from the nearest-named unit")
            continue
        out.append(HeaderEntry(byFile[unit], header))
        report(f"{spelled}: command of {os.path.relpath(unit, repo)}")
    return out


def Main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--db", help="the directory holding compile_commands.json")
    parser.add_argument("--out", help="the directory to write the augmented database to")
    parser.add_argument("--repo", default=".", help="the repository root (default: .)")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("headers", nargs="*")
    args = parser.parse_args(argv)
    if args.self_test:
        return SelfTest()
    if not args.db or not args.out:
        parser.error("--db and --out are required")
    try:
        with open(os.path.join(args.db, "compile_commands.json"), encoding="utf-8") as handle:
            db = json.load(handle)
    except (OSError, ValueError) as error:
        print(f"tidy-header-commands: cannot read {args.db}/compile_commands.json: {error}",
              file=sys.stderr)
        return 2
    augmented = Augment(db, args.repo, args.headers, lambda line: print(f"  {line}"))
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "compile_commands.json"), "w", encoding="utf-8") as handle:
        json.dump(augmented, handle, indent=1)
    print(f"tidy-header-commands: {len(augmented) - len(db)} of {len(args.headers)} header(s) "
          f"given an includer's command, in {args.out}")
    return 0


def SelfTest():
    """Drive `Augment` over a synthetic tree and database; exit 0 pass, 1 fail."""
    scratch = tempfile.mkdtemp(prefix="tidy-header-commands-")
    try:
        return RunSelfTest(scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def RunSelfTest(repo):
    files = {
        "src/Lib/Base.hpp": "#pragma once\n",
        "src/Lib/Middle.hpp": '#pragma once\n#include "Base.hpp"\n',
        "src/Lib/Lib.cpp": "#include <Lib/Middle.hpp>\n",
        "src/tests/Fake.hpp": "#pragma once\n#include <Lib/Base.hpp>\n",
        # `A_test.cpp` sorts BEFORE `Fake_test.cpp`, so the same-stem rule is what
        # makes `Fake_test.cpp` win -- smallest path alone would pick `A_test.cpp`.
        "src/tests/A_test.cpp": '#include "Fake.hpp"\n',
        "src/tests/Fake_test.cpp": '#include "Fake.hpp"\n',
        "src/tests/Orphan.hpp": "#pragma once\n",
        # `Deep.hpp` has a DIRECT includer, `Z_user.cpp`, which loses every tie-break
        # (no shared stem, the largest path), and a deeper unit, `Deep_test.cpp`,
        # which would win both. Depth must decide before the tie-break is asked.
        "src/tests/Deep.hpp": "#pragma once\n",
        "src/tests/DeepWrap.hpp": '#pragma once\n#include "Deep.hpp"\n',
        "src/tests/Deep_test.cpp": '#include "DeepWrap.hpp"\n',
        "src/tests/Z_user.cpp": '#include "Deep.hpp"\n',
    }
    for relative, text in files.items():
        path = os.path.join(repo, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
    build = os.path.join(repo, "build")
    src = os.path.join(repo, "src")

    # `arguments`, never a `command` string: the scratch path is the host's, and a
    # POSIX split of a Windows-spelled path would eat its backslashes.
    def Unit(relative, extra):
        source = os.path.join(repo, relative)
        obj = relative.replace("/", "_") + ".o"
        return {
            "directory": build,
            "arguments": ["/usr/bin/clang++", f"-I{src}", *extra, "-std=c++23",
                          "-o", obj, "-c", source],
            "file": source,
            "output": os.path.join(build, obj),
        }

    db = [
        Unit("src/Lib/Lib.cpp", ["-DLIB"]),
        Unit("src/tests/A_test.cpp", ["-isystem", "/catch2", "-DOTHER"]),
        Unit("src/tests/Fake_test.cpp", ["-isystem", "/catch2", "-DFAKE"]),
        Unit("src/tests/Deep_test.cpp", ["-DDEEPTEST"]),
        Unit("src/tests/Z_user.cpp", ["-DZUSER"]),
    ]
    original = json.loads(json.dumps(db))
    lines = []
    out = Augment(db, repo, ["src/tests/Fake.hpp", "src/Lib/Base.hpp", "src/tests/Orphan.hpp",
                             "src/tests/Deep.hpp"],
                  lines.append)
    added = {entry["file"]: entry for entry in out[len(db):]}

    cases = 0
    failures = 0

    def Expect(what, condition):
        nonlocal cases, failures
        cases += 1
        print(f"  {'ok  ' if condition else 'FAIL'} {what}")
        if not condition:
            failures += 1

    print("TIDY HEADER COMMANDS SELF-TEST")
    fake = os.path.join(repo, "src/tests/Fake.hpp")
    base = os.path.join(repo, "src/Lib/Base.hpp")
    orphan = os.path.join(repo, "src/tests/Orphan.hpp")
    fakeArgs = added.get(fake, {}).get("arguments", [])
    Expect("a direct includer's command is borrowed, the same-stem unit preferred",
           "-DFAKE" in fakeArgs and "-DOTHER" not in fakeArgs)
    Expect("the unit's source, -c and -o <object> are swapped for the header as c++-header",
           fakeArgs[-3:] == ["-x", "c++-header", fake]
           and not any(t.endswith("Fake_test.cpp") for t in fakeArgs)
           and "-c" not in fakeArgs and "-o" not in fakeArgs
           and "-isystem" in fakeArgs and "-std=c++23" in fakeArgs)
    Expect("the borrowed entry keeps the unit's directory",
           added.get(fake, {}).get("directory") == build)
    baseArgs = added.get(base, {}).get("arguments", [])
    Expect("a header reached only through other headers borrows the nearest unit's command",
           "-DFAKE" in baseArgs or "-DLIB" in baseArgs or "-DOTHER" in baseArgs)
    Expect("the transitive walk stops at the first depth holding a unit (Fake.hpp -> *_test.cpp, "
           "Middle.hpp -> Lib.cpp: both depth 2, tie to the smallest path)",
           "-DLIB" in baseArgs)
    deepArgs = added.get(os.path.join(repo, "src/tests/Deep.hpp"), {}).get("arguments", [])
    Expect("a DIRECT includer beats a deeper unit that would win the tie-break",
           "-DZUSER" in deepArgs and "-DDEEPTEST" not in deepArgs)
    Expect("a header no unit includes gets no entry", orphan not in added)
    Expect("a header no unit includes is NAMED in the output",
           any("src/tests/Orphan.hpp" in line and "no unit" in line for line in lines))
    Expect("every unit already in the database is unchanged and in order",
           out[: len(db)] == original)
    Expect("exactly the three reachable headers were added", len(out) - len(db) == 3)
    print(f"tidy-header-commands self-test: {cases} case(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(Main(sys.argv[1:]))
