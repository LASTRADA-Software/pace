# SPDX-License-Identifier: Apache-2.0
#
# Drives `check-error-popups.cmake` against staged build trees, in both directions (#1389).
#
# Each tree holds the files the check reads, in the shapes CMake generates: a manifest, a
# `CTestTestfile.cmake` (one nested, as a real build nests them) with bracket-named
# registrations, a `catch_discover_tests` include file and the `_tests.cmake` it names, and
# the `build.ninja` link edges. A refusal asserts WHICH refusal, by a phrase only that
# refusal carries: every refusal prints `CMake Error`, so asserting that alone would pass
# with the arms exchanged.
#
# Usage: cmake -DPACE_SOURCE_DIR=<repo> -DPACE_SCRATCH_DIR=<dir> -P <this>

cmake_minimum_required(VERSION 3.28)

foreach(required PACE_SOURCE_DIR PACE_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${PACE_SOURCE_DIR}/scripts/check-error-popups.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

file(REMOVE_RECURSE "${PACE_SCRATCH_DIR}")
set(failures "")
set(cases 0)
set(variable "PACE_SELFTEST_SUPPRESS")

# One CTestTestfile registration, spelled as CMake writes it.
# @param outVar receives the two lines
# @param name the test name
# @param command the rest of the add_test line
# @param marked TRUE when the test sets the variable
function(pace_registration outVar name command marked)
    set(text "add_test([=[${name}]=] ${command})\n")
    if(marked)
        string(APPEND text "set_tests_properties([=[${name}]=] PROPERTIES ENVIRONMENT_MODIFICATION \"A=unset:;${variable}=set:1\" _BACKTRACE_TRIPLES \"x;1;y\")\n")
    else()
        string(APPEND text "set_tests_properties([=[${name}]=] PROPERTIES TIMEOUT \"60\")\n")
    endif()
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

# Stage one build tree.
# @param name the tree's directory name under the scratch root
# @param manifest the manifest body, one `name|file|policy|reason` row per line
# @param registrations the top-level CTestTestfile.cmake body; `@TREE@` becomes the tree path
# @param nested a nested CTestTestfile.cmake body
# @param catchTests the body of the `_tests.cmake` a catch include names, or `<none>` for no file
# @param edges the build.ninja body, or `<none>` for no file
# @param outVar receives the tree's path
function(pace_stage name manifest registrations nested catchTests edges outVar)
    set(tree "${PACE_SCRATCH_DIR}/${name}")
    file(MAKE_DIRECTORY "${tree}/src/apps/x")
    file(WRITE "${tree}/error-popups-Debug.manifest" "${manifest}")
    string(REPLACE "@TREE@" "${tree}" registrations "${registrations}")
    string(REPLACE "@TREE@" "${tree}" nested "${nested}")
    # Reached the way ctest reaches it, through `subdirs()`; a testfile nothing lists is not read.
    file(WRITE "${tree}/CTestTestfile.cmake" "${registrations}subdirs(\"src/apps/x\")\n")
    file(WRITE "${tree}/src/apps/x/CTestTestfile.cmake" "${nested}")
    file(WRITE "${tree}/src/apps/x/cli-tests-b12d07c_include.cmake"
        "if(EXISTS \"${tree}/src/apps/x/cli-tests-b12d07c_tests.cmake\")\n  include(\"${tree}/src/apps/x/cli-tests-b12d07c_tests.cmake\")\nelse()\n  add_test(cli-tests_NOT_BUILT-b12d07c cli-tests_NOT_BUILT-b12d07c)\nendif()\n")
    if(NOT catchTests STREQUAL "<none>")
        file(WRITE "${tree}/src/apps/x/cli-tests-b12d07c_tests.cmake" "${catchTests}")
    endif()
    if(NOT edges STREQUAL "<none>")
        file(WRITE "${tree}/build.ninja" "${edges}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and record whether it matched expectations.
# @param label what the case establishes
# @param tree the staged tree
# @param want `pass`, `refuse` or `skip`
# @param phrase text the output must carry
function(pace_case label tree want phrase)
    math(EXPR count "${cases} + 1")
    set(cases ${count} PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${tree}" -DCONFIG=Debug -DANCHOR=PaceTest
                "-DVARIABLE=${variable}" -P "${check}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    set(output "${out}${err}")
    string(REPLACE "\n" " " flat "${output}")
    set(got "pass")
    if(flat MATCHES "CMake Error|CMake Warning")
        set(got "refuse")
    elseif(flat MATCHES "SKIP: ")
        set(got "skip")
    endif()
    string(FIND "${flat}" "${phrase}" at)
    if(NOT got STREQUAL want)
        set(failures "${failures}\n  FAIL ${label}: expected ${want}, got ${got}:\n${output}" PARENT_SCOPE)
    elseif(at EQUAL -1)
        set(failures "${failures}\n  FAIL ${label}: ${want} as expected but without '${phrase}':\n${output}" PARENT_SCOPE)
    else()
        message("  ok   ${label}")
    endif()
endfunction()

set(t "D:/b/target")
set(anchorRow "PaceTest|${t}/PaceTest.exe|always|\n")
pace_registration(anchorTest "a case" "${t}/PaceTest.exe [=[a case]=]" TRUE)
set(anchorEdge "build target\\PaceTest.exe: CXX_EXECUTABLE_LINKER__PaceTest_Debug src\\tests\\CMakeFiles\\PaceTest.dir\\a.cpp.obj src\\tests\\CMakeFiles\\PaceTest.dir\\ErrorPopupsAtStartup.cpp.obj\n")
set(catchInclude "include(\"@TREE@/src/apps/x/cli-tests-b12d07c_include.cmake\")\n")
set(catchTestsMarked "add_test( [==[the table is well formed]==] ${t}/cli-tests.exe [==[the table is well formed]==]  )\nset_tests_properties( [==[the table is well formed]==] PROPERTIES WORKING_DIRECTORY D:/b SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION ${variable}=set:1)\n")
set(catchEdge "build target\\cli-tests.exe: CXX_EXECUTABLE_LINKER__cli-tests_Debug x.obj ErrorPopupsAtStartup.cpp.obj\n")

# ---- the passing direction --------------------------------------------------------------

pace_registration(ccE2e "cc-e2e" "\"bash\" \"e2e.sh\" \"--launcher\" \"${t}/pace-cc\"" TRUE)
pace_registration(canary "canary" "\"cmake\" \"-DCANARY=${t}/canary.exe\" \"-P\" \"gate.cmake\"" TRUE)
pace_stage(clean
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\npace-cc|${t}/pace-cc|on-request|\ncanary|${t}/canary.exe|always|\nunlaunched|${t}/unlaunched.exe|always|\n"
    "${anchorTest}${ccE2e}${canary}"
    "${catchInclude}"
    "${catchTestsMarked}"
    "${anchorEdge}${catchEdge}build target/pace-cc: CXX_EXECUTABLE_LINKER__pace-cc_Debug y.o ErrorPopupsAtStartup.cpp.o\nbuild target\\canary.exe: CXX_EXECUTABLE_LINKER__canary_Debug ErrorPopupsAtStartup.cpp.obj\nbuild target\\unlaunched.exe: CXX_EXECUTABLE_LINKER__unlaunched_Debug z.obj\n"
    tree)
pace_case("a command, a catch_discover_tests include and a script argument all count as launched and covered" "${tree}" pass "4 of 5 executable(s) are launched by a test; 4 carry the suppression")
pace_case("every registered test, catch-discovered ones included, is read and sets the variable" "${tree}" pass "4 registered test(s) read, 0 without ${variable}")

pace_registration(odd "odd" "${t}/odd.exe" TRUE)
pace_stage(exempt
    "${anchorRow}odd|${t}/odd.exe|exempt|it is a probe that is linked and never run\n"
    "${anchorTest}${odd}" "" "<none>"
    "${anchorEdge}build target\\odd.exe: CXX_EXECUTABLE_LINKER__odd_Debug odd.obj\n"
    tree)
pace_case("a launched executable stating a reason passes and prints the reason" "${tree}" pass "exempt: odd -- it is a probe that is linked and never run")

# POSIX spells no `.exe`, so the launcher's path is a PREFIX of its test binary's. The
# uncovered launcher is not launched; its test binary is.
pace_registration(prefixTest "t" "/b/target/pace-cc-tests" TRUE)
pace_stage(prefix
    "${anchorRow}pace-cc|/b/target/pace-cc|on-request|\npace-cc-tests|/b/target/pace-cc-tests|always|\n"
    "${anchorTest}${prefixTest}" "" "<none>"
    "${anchorEdge}build target/pace-cc: CXX_EXECUTABLE_LINKER__pace-cc_Debug a.o\nbuild target/pace-cc-tests: CXX_EXECUTABLE_LINKER__pace-cc-tests_Debug ErrorPopupsAtStartup.cpp.o\n"
    tree)
pace_case("a path that is only a PREFIX of a launched one is not launched" "${tree}" pass "2 of 3 executable(s) are launched")

# ---- the refusing direction: coverage ---------------------------------------------------

pace_registration(bare "bare" "${t}/bare.exe" TRUE)
pace_stage(command
    "${anchorRow}bare|${t}/bare.exe|always|\n"
    "${anchorTest}${bare}" "" "<none>"
    "${anchorEdge}build target\\bare.exe: CXX_EXECUTABLE_LINKER__bare_Debug bare.obj\n"
    tree)
pace_case("an uncovered test COMMAND is refused by name" "${tree}" refuse "bare is launched by a test and its 'Debug' link line carries no")

pace_registration(e2e "e2e" "\"bash\" \"e2e.sh\" \"--pace-kv\" \"${t}/pace-kv.exe\"" TRUE)
pace_stage(argument
    "${anchorRow}pace-kv|${t}/pace-kv.exe|on-request|\n"
    "${anchorTest}${e2e}" "" "<none>"
    "${anchorEdge}build target\\pace-kv.exe: CXX_EXECUTABLE_LINKER__pace-kv_Debug main.obj\n"
    tree)
pace_case("an uncovered binary handed to a script as an ARGUMENT is refused" "${tree}" refuse "pace-kv is launched by a test")

pace_stage(catch
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" "" "${catchTestsMarked}"
    "${anchorEdge}build target\\cli-tests.exe: CXX_EXECUTABLE_LINKER__cli-tests_Debug t.obj\n"
    tree)
pace_case("an uncovered catch_discover_tests binary is refused, found through its include file" "${tree}" refuse "cli-tests is launched by a test")

pace_registration(rel "rel" "${t}/rel.exe" TRUE)
pace_stage(otherconfig
    "${anchorRow}rel|${t}/rel.exe|always|\n"
    "${anchorTest}${rel}" "" "<none>"
    "${anchorEdge}build target\\rel.exe: CXX_EXECUTABLE_LINKER__rel_Release ErrorPopupsAtStartup.cpp.obj\n"
    tree)
pace_case("an object on ANOTHER configuration's link line does not cover this one" "${tree}" refuse "rel is launched by a test and its 'Debug' link line")

pace_registration(blank "blank" "${t}/blank.exe" TRUE)
pace_stage(exemptnoreason
    "${anchorRow}blank|${t}/blank.exe|exempt|\n"
    "${anchorTest}${blank}" "" "<none>"
    "${anchorEdge}build target\\blank.exe: CXX_EXECUTABLE_LINKER__blank_Debug b.obj\n"
    tree)
pace_case("an exemption with an EMPTY reason is not an exemption" "${tree}" refuse "blank is launched by a test")

pace_stage(pipe
    "${anchorRow}piped|${t}/piped.exe|exempt|a reason | with a pipe\n"
    "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
pace_case("a reason carrying `|` is refused rather than misread" "${tree}" refuse "does not have four `|` fields")

pace_stage(empty "" "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
pace_case("an empty manifest is refused as vacuous" "${tree}" refuse "names no executable")

pace_registration(other "other" "D:/b/target/other.exe" TRUE)
pace_stage(noanchor "${anchorRow}" "${other}" "" "<none>" "${anchorEdge}" tree)
pace_case("an anchor that is not launched is refused" "${tree}" refuse "the anchor 'PaceTest' was not found launched AND covered")

pace_stage(nomanifest "" "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
file(REMOVE "${tree}/error-popups-Debug.manifest")
pace_case("no manifest at all is refused, naming the walk" "${tree}" refuse "configured without that walk")

# ---- the refusing direction: the variable -----------------------------------------------

pace_registration(unmarked "an unmarked script test" "\"bash\" \"e2e.sh\"" FALSE)
pace_stage(unmarkedaddtest "${anchorRow}" "${anchorTest}" "${unmarked}" "<none>" "${anchorEdge}" tree)
pace_case("an add_test that does not set the variable is refused by name" "${tree}" refuse "1 of 2 registered test(s) do not set ${variable}")
pace_case("... and the refusal names the test" "${tree}" refuse "an unmarked script test")

pace_stage(unmarkedcatch
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" ""
    "add_test( [==[a discovered case]==] ${t}/cli-tests.exe [==[a discovered case]==]  )\nset_tests_properties( [==[a discovered case]==] PROPERTIES WORKING_DIRECTORY D:/b SKIP_RETURN_CODE 4)\n"
    "${anchorEdge}${catchEdge}" tree)
pace_case("a catch_discover_tests case that does not set the variable is refused" "${tree}" refuse "a discovered case")

pace_stage(notbuilt
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" "" "<none>"
    "${anchorEdge}${catchEdge}" tree)
pace_case("a catch binary whose tests file was never generated is refused, not skipped" "${tree}" refuse "was never generated")

pace_stage(unnamed "${anchorRow}" "${anchorTest}add_test([==[a bracket never closed D:/b/x.exe)\n" "" "<none>" "${anchorEdge}" tree)
pace_case("a registration the reader cannot name is refused rather than counted as set" "${tree}" refuse "a test registration this reader cannot name")

# The two name spellings catch_discover_tests really writes, measured in this build: a
# bracket argument whose body holds brackets, and a bare token. Both set the variable.
pace_stage(spellings
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" ""
    "add_test( [==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==] ${t}/cli-tests.exe [==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==]  )\nset_tests_properties( [==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==] PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION ${variable}=set:1)\nadd_test( AvailableCodecsCoverEveryCodec ${t}/cli-tests.exe AvailableCodecsCoverEveryCodec  )\nset_tests_properties( AvailableCodecsCoverEveryCodec PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION ${variable}=set:1)\n"
    "${anchorEdge}${catchEdge}" tree)
pace_case("a name holding brackets, and a bare name, are both read and found set" "${tree}" pass "3 registered test(s) read, 0 without ${variable}")

pace_stage(bareunset
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" ""
    "add_test( BareUnsetCase ${t}/cli-tests.exe BareUnsetCase  )\nset_tests_properties( BareUnsetCase PROPERTIES SKIP_RETURN_CODE 4)\n"
    "${anchorEdge}${catchEdge}" tree)
pace_case("a bare-named case that does not set the variable is refused" "${tree}" refuse "1 of 2 registered test(s) do not set ${variable}")
pace_case("... and that refusal names the bare-named case" "${tree}" refuse "The first of them:     BareUnsetCase")

# Two names sharing everything up to a `]` inside them, one set and one not. A reader that
# ends a bracket name at the first `]` reads both as the same name, and the set one hides
# the unset one.
pace_stage(sharedprefix
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|
"
    "${anchorTest}${catchInclude}" ""
    "add_test( [==[codec [zstd] round trips]==] ${t}/cli-tests.exe [==[codec [zstd] round trips]==]  )
set_tests_properties( [==[codec [zstd] round trips]==] PROPERTIES ENVIRONMENT_MODIFICATION ${variable}=set:1)
add_test( [==[codec [zstd] refuses garbage]==] ${t}/cli-tests.exe [==[codec [zstd] refuses garbage]==]  )
set_tests_properties( [==[codec [zstd] refuses garbage]==] PROPERTIES SKIP_RETURN_CODE 4)
"
    "${anchorEdge}${catchEdge}" tree)
pace_case("a name is read to its CLOSING bracket, so a set case cannot hide an unset one sharing its prefix" "${tree}" refuse "codec [zstd] refuses garbage")

# A testfile ctest never reads -- here, one no `subdirs()` lists, the shape this check's own
# staged trees have inside a real build directory -- is not a registration.
pace_registration(stray "a stray unmarked test" "D:/b/x.exe" FALSE)
pace_stage(unreached "${anchorRow}" "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
file(MAKE_DIRECTORY "${tree}/staged/elsewhere")
file(WRITE "${tree}/staged/elsewhere/CTestTestfile.cmake" "${stray}")
pace_case("a testfile no subdirs() reaches is not read" "${tree}" pass "1 registered test(s) read, 0 without ${variable}")

pace_stage(notests "${anchorRow}" "# no registrations
" "" "<none>" "${anchorEdge}" tree)
pace_case("a build registering no test at all is refused as vacuous" "${tree}" refuse "no registered test was found")

# ---- neither ----------------------------------------------------------------------------

pace_stage(noninja "${anchorRow}" "${anchorTest}" "" "<none>" "<none>" tree)
pace_case("a build with no build.ninja is a SKIP naming why, not a pass" "${tree}" skip "has no build.ninja")

message("check-error-popups self-test: ${cases} case(s) ran")
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "check-error-popups self-test failed:${failures}")
endif()
message("check-error-popups self-test: all ${cases} passed")
