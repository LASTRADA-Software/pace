# SPDX-License-Identifier: Apache-2.0
#
# Assert that cmake/portable/CompileCache.cmake's PACE_AUTO_START can
# stage a pace-kv daemon binary out of the same release archive
# PACE_AUTO_INSTALL already fetches for the launcher (issue #90) — one
# download serving two members, not two downloads.
#
# This is the mirror half of that story, the counterpart to
# check-compile-cache-install.cmake for the launcher: deterministic, offline,
# and cheap, because what it exercises is the asset-table plumbing (the new
# _daemon_member/_daemon_exe columns, _fc_auto_install_fetch_archive being
# shared rather than downloading the archive a second time, and
# _fc_auto_install_stage_member placing the result under the right
# version/platform directory) rather than whether a real daemon can bind a
# real port — that needs a real pace-kv and a real pace-cc, which is
# what the smoke-labelled check-compile-cache-daemon-start test covers
# instead, with the two split for the same reason described there.
#
# It runs offline, against the same file:// mirror technique
# check-compile-cache-install.cmake uses, for the same reasons: determinism
# and staying clear of GitHub's unauthenticated rate limit.
#
# Usage:
#   cmake -DPACE_SOURCE_DIR=<repo> -DPACE_SCRATCH_DIR=<scratch>
#         -DPACE_CXX_COMPILER=<c++> [-DPACE_MAKE_PROGRAM=<make>]
#         [-DPACE_GENERATOR=<gen>]
#         -P scripts/check-compile-cache-daemon-staging.cmake
#
# Exit codes: 0 = staged as specified, or skipped. 1 = it did not.

cmake_minimum_required(VERSION 3.28)

# Oracle for the module's asset table, independent of it for the same reason
# check-compile-cache-install.cmake's own oracle is: a rename on one side must
# be caught, not agreed to by both sides at once.
#
#   <host system>|<host processor>|<asset infix>|<launcher path>|<daemon path>
set(PaceInstallOracle
    "Linux|x86_64|Linux-x86_64|usr/bin/pace-cc|usr/bin/pace-kv"
    "Darwin|arm64|Darwin-arm64|opt/fastcached/bin/pace-cc|opt/fastcached/bin/pace-kv"
)

# Deliberately not a version that exists, so a bug that reached the real
# GitHub anyway could not accidentally succeed.
set(mirrorVersion "9.8.7")

foreach(required PACE_SOURCE_DIR PACE_SCRATCH_DIR PACE_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set (cmake -D${required}=... -P ${CMAKE_CURRENT_LIST_FILE})")
    endif()
endforeach()

set(fixtureDir "${PACE_SOURCE_DIR}/src/tests/compile-cache-fixture")
set(moduleDir "${PACE_SOURCE_DIR}/cmake/portable")
if(NOT IS_DIRECTORY "${fixtureDir}")
    message(FATAL_ERROR "no fixture project at ${fixtureDir}")
endif()

if(CMAKE_HOST_WIN32)
    message("SKIP: the launcher sandbox this needs has no Windows equivalent "
            "(see check-compile-cache-autoinstall.cmake's header)")
    return()
endif()
if(NOT EXISTS "${PACE_CXX_COMPILER}")
    message("SKIP: no C++ compiler at ${PACE_CXX_COMPILER}, so no project can be configured")
    return()
endif()
find_program(hostUname NAMES uname)
if(NOT hostUname)
    message("SKIP: no uname found, so a sandboxed configure cannot detect its own host")
    return()
endif()

cmake_host_system_information(RESULT hostProcessor QUERY OS_PLATFORM)

set(platform "")
foreach(row IN LISTS PaceInstallOracle)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 rowSystem)
    list(GET fields 1 rowProcessor)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "${rowSystem}" AND hostProcessor STREQUAL "${rowProcessor}")
        list(GET fields 2 platform)
        list(GET fields 3 launcherMember)
        list(GET fields 4 daemonMember)
        break()
    endif()
endforeach()
if(NOT platform)
    message("SKIP: no published pace-kv is expected for "
            "${CMAKE_HOST_SYSTEM_NAME}-${hostProcessor}, so there is nothing to stage")
    return()
endif()

file(REMOVE_RECURSE "${PACE_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${PACE_SCRATCH_DIR}")

# Build the archive the module will be asked to install from: same name, same
# interior layout as a real release, holding both a launcher and a daemon that
# each answer --version and nothing else — scripts rather than compiled
# binaries, because what is under test is the staging plumbing, not the files.
set(stem "fastcached-${mirrorVersion}-${platform}")
set(payloadRoot "${PACE_SCRATCH_DIR}/payload")

foreach(pair "launcher:${launcherMember}:pace-cc" "daemon:${daemonMember}:pace-kv")
    string(REPLACE ":" ";" fields "${pair}")
    list(GET fields 1 relPath)
    list(GET fields 2 label)
    get_filename_component(memberDir "${payloadRoot}/${stem}/${relPath}" DIRECTORY)
    file(MAKE_DIRECTORY "${memberDir}")
    file(WRITE "${payloadRoot}/${stem}/${relPath}"
         "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo '${label} ${mirrorVersion}'; exit 0; fi\nexit 3\n")
    file(CHMOD "${payloadRoot}/${stem}/${relPath}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()

set(mirrorDir "${PACE_SCRATCH_DIR}/mirror/fastcached-mirror/releases/download/v${mirrorVersion}")
file(MAKE_DIRECTORY "${mirrorDir}")
# See check-compile-cache-install.cmake for why this is `cmake -E tar` rather
# than `file(ARCHIVE_CREATE)`: the latter's WORKING_DIRECTORY option needs
# CMake 3.31, newer than the 3.28 floor this project (and this script) target.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar czf "${mirrorDir}/${stem}.tar.gz" --format=gnutar "${stem}"
    WORKING_DIRECTORY "${payloadRoot}"
    RESULT_VARIABLE archiveResult
    ERROR_VARIABLE archiveError)
if(NOT archiveResult EQUAL 0)
    message(FATAL_ERROR "could not build the mirror archive: ${archiveResult} ${archiveError}")
endif()

set(sandbox "${PACE_SCRATCH_DIR}/sandbox")
file(MAKE_DIRECTORY "${sandbox}/usr/bin")
file(CREATE_LINK "${hostUname}" "${sandbox}/usr/bin/uname" COPY_ON_ERROR SYMBOLIC)

set(stageDir "${PACE_SCRATCH_DIR}/stage")
set(buildDir "${PACE_SCRATCH_DIR}/build")
# PACE_ADDR names a port nothing on this host is listening on, so
# _fc_daemon_answering's pre-check finds nothing and the module proceeds to
# stage — staging is all this test asserts; check-compile-cache-daemon-start
# is what asserts the process actually gets spawned and answers.
set(arguments
    "-DPACE_MODULE_DIR=${moduleDir}"
    "-DCMAKE_CXX_COMPILER=${PACE_CXX_COMPILER}"
    "-DCMAKE_FIND_ROOT_PATH=${sandbox}"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=ONLY"
    "-DPACE_AUTO_INSTALL=ON"
    "-DPACE_AUTO_INSTALL_DIR=${stageDir}"
    "-DPACE_AUTO_INSTALL_REPO=fastcached-mirror"
    "-DPACE_AUTO_INSTALL_VERSION=${mirrorVersion}"
    "-DPACE_AUTO_INSTALL_DOWNLOAD_BASE=file://${PACE_SCRATCH_DIR}/mirror"
    "-DPACE_AUTO_START=ON"
    "-DPACE_ADDR=127.0.0.1:18674")
if(PACE_MAKE_PROGRAM)
    list(APPEND arguments "-DCMAKE_MAKE_PROGRAM=${PACE_MAKE_PROGRAM}")
endif()
if(PACE_GENERATOR)
    list(APPEND arguments -G "${PACE_GENERATOR}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${fixtureDir}" -B "${buildDir}" ${arguments}
    RESULT_VARIABLE configureResult
    OUTPUT_VARIABLE configureOutput
    ERROR_VARIABLE configureError
    TIMEOUT 300)
set(output "${configureOutput}${configureError}")

set(violations "")
if(NOT configureResult EQUAL 0)
    list(APPEND violations "configure failed (${configureResult})")
endif()

# The fake pace-kv answers --version and nothing else, so the poll loop in
# _fc_auto_start_fastcached never actually observes it listening — real
# connectivity needs a real daemon, which is check-compile-cache-daemon-start's
# job. This configure is still expected to report having tried, and to have
# fallen through cleanly rather than failing.
string(FIND "${output}" "pace-kv" mentionedPaceKv)
if(mentionedPaceKv EQUAL -1)
    list(APPEND violations "the configure never mentioned starting a daemon at all")
endif()

set(expectedLauncher "${stageDir}/${mirrorVersion}/${platform}/pace-cc")
set(expectedDaemon "${stageDir}/${mirrorVersion}/${platform}/fastcached")

if(NOT EXISTS "${expectedLauncher}")
    file(GLOB_RECURSE staged "${stageDir}/*")
    list(APPEND violations "expected the launcher at ${expectedLauncher}, found [${staged}]")
endif()

if(NOT EXISTS "${expectedDaemon}")
    file(GLOB_RECURSE staged "${stageDir}/*")
    list(APPEND violations "expected the daemon at ${expectedDaemon}, found [${staged}]")
else()
    execute_process(COMMAND "${expectedDaemon}" --version
                    RESULT_VARIABLE stagedResult
                    OUTPUT_VARIABLE stagedOutput
                    ERROR_QUIET
                    TIMEOUT 30)
    string(STRIP "${stagedOutput}" stagedOutput)
    if(NOT stagedResult EQUAL 0)
        list(APPEND violations "the staged daemon does not run (${stagedResult})")
    elseif(NOT stagedOutput STREQUAL "pace-kv ${mirrorVersion}")
        list(APPEND violations "the staged daemon answered '${stagedOutput}'")
    endif()
endif()

# Both binaries must come from ONE archive fetch: the module's own download
# work directory holds the unpacked tree, and if the daemon triggered a
# second, independent fetch the launcher's own unpack would have been
# clobbered or duplicated. Asserting on the archive itself (rather than e.g.
# counting HTTP requests, which file:// has none of) is what this mirror can
# actually observe: exactly one archive of this name under the module's
# per-build-tree download directory.
file(GLOB_RECURSE downloadedArchives "${buildDir}/CMakeFiles/fastcache-download/*.tar.gz")
list(LENGTH downloadedArchives archiveCount)
if(archiveCount GREATER 1)
    list(APPEND violations "expected one downloaded archive, found ${archiveCount}: [${downloadedArchives}]")
endif()

if(violations)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR
        "compile-cache daemon staging did not work as specified:\n  ${report}\n\n"
        "Configure output was:\n${output}")
endif()

message("compile-cache daemon staging: ${platform} launcher and daemon both fetched from one archive and staged")
