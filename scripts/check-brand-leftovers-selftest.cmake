# SPDX-License-Identifier: Apache-2.0
# Drives scripts/check-brand-leftovers.cmake over synthetic git trees, both directions:
#
#   planted     a leftover in a source file                       -> OBJECTED, naming src/a.cpp:1
#   exempted    the same text where a row covers it               -> ACCEPTED
#   clean       nothing but the table, which exempts itself       -> ACCEPTED
#   stale       a row that covers nothing                         -> OBJECTED, as a stale exemption
#   path        a tracked PATH carrying the old name, never exempt -> OBJECTED, naming the PATH
#
# An objection is asserted by WHICH refusal it is, not only that there was one: every OBJECTED
# case names the output that only its own refusal prints.
#
# Git trees, through `pace_scratch_git`, because git mode is the path CI takes; the check
# states its mode and this asserts it, so a walk standing in for git is a failure.
#
#   cmake -DPACE_SOURCE_DIR=<repo> -DPACE_SCRATCH_DIR=<dir> -P scripts/check-brand-leftovers-selftest.cmake

cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED PACE_SOURCE_DIR)
    message(FATAL_ERROR "PACE_SOURCE_DIR must be set")
endif()
if(NOT DEFINED PACE_SCRATCH_DIR)
    message(FATAL_ERROR "PACE_SCRATCH_DIR must be set")
endif()
set(check "${PACE_SOURCE_DIR}/scripts/check-brand-leftovers.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
find_program(gitExecutable git)
if(NOT gitExecutable)
    message(FATAL_ERROR "brand-leftovers selftest: no git, and the git mode is the one under test")
endif()
pace_scratch_git("${gitExecutable}" git)

# The old name, assembled, so this file is not one of the check's findings.
set(oldName "fast")
string(APPEND oldName "cache")

set(root "${PACE_SCRATCH_DIR}/brand-leftovers-selftest")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(ran 0)

# A tree holding @p table as its exemption table and every `path|content` pair in ARGN.
function(brand_make_tree name table outVar)
    set(tree "${root}/${name}")
    file(MAKE_DIRECTORY "${tree}/scripts/lib")
    file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "vendor/upstream\n")
    file(WRITE "${tree}/scripts/brand-leftovers-exemptions.tsv" "${table}")
    foreach(pair IN LISTS ARGN)
        string(FIND "${pair}" "|" bar)
        string(SUBSTRING "${pair}" 0 ${bar} relative)
        math(EXPR afterBar "${bar} + 1")
        string(SUBSTRING "${pair}" ${afterBar} -1 content)
        get_filename_component(directory "${tree}/${relative}" DIRECTORY)
        file(MAKE_DIRECTORY "${directory}")
        file(WRITE "${tree}/${relative}" "${content}")
    endforeach()
    execute_process(COMMAND ${git} init -q WORKING_DIRECTORY "${tree}" RESULT_VARIABLE initResult)
    execute_process(COMMAND ${git} add -A WORKING_DIRECTORY "${tree}" RESULT_VARIABLE addResult)
    if(NOT initResult EQUAL 0 OR NOT addResult EQUAL 0)
        message(FATAL_ERROR "brand-leftovers selftest: could not make the git tree ${tree}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# @p reason is a regex the output must match when @p want is OBJECTED, and empty otherwise.
function(brand_case name want reason table)
    brand_make_tree("${name}" "${table}" tree ${ARGN})
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACE_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    string(REGEX REPLACE "[ \t\n]+" " " flattened "${captured}${capturedErrors}")
    set(verdict "ACCEPTED")
    if(flattened MATCHES "CMake Error|CMake Warning")
        set(verdict "OBJECTED")
        if(NOT reason STREQUAL "" AND NOT flattened MATCHES "${reason}")
            set(verdict "OBJECTED, but not with '${reason}'")
        endif()
    endif()
    # The mode is part of the output: a walk standing in for git judged a different path.
    if(NOT flattened MATCHES "brand leftovers [(]git ls-files[)]")
        set(verdict "${verdict}, NOT in git mode")
    endif()
    math(EXPR nextRan "${ran} + 1")
    set(ran "${nextRan}" PARENT_SCOPE)
    if(NOT verdict STREQUAL want)
        set(failures "${failures}\n  FAIL  ${name}: wanted ${want}, got ${verdict}" PARENT_SCOPE)
        message(STATUS "  FAIL  ${name}: wanted ${want}, got ${verdict}")
    else()
        message(STATUS "  ok    ${name}: ${verdict}")
    endif()
endfunction()

# The table names the old spelling in its own header, as the real one does, so its own row is
# used in every tree -- and a clean tree is clean rather than holding a stale row.
set(selfRow "# where ${oldName} stays\nscripts/brand-leftovers-exemptions.tsv\t[Ff][Aa][Ss][Tt][Cc][Aa][Cc][Hh][Ee]\tthe table spells what it exempts\n")
set(labelRow "src/label.cpp\t${oldName}-lease-v[0-9]+\ta signing label\n")
set(planted "src/a.cpp|// the ${oldName} daemon\n")
set(label "src/label.cpp|auto const label = \"${oldName}-lease-v4\"\n")

brand_case("planted" "OBJECTED" " src/a[.]cpp:1: // the ${oldName} daemon" "${selfRow}" "${planted}")
brand_case("exempted" "ACCEPTED" "" "${selfRow}${labelRow}" "${label}")
brand_case("clean" "ACCEPTED" "" "${selfRow}" "src/a.cpp|// the Pace daemon\n")
brand_case("stale" "OBJECTED" "stale exemption: 'src/label[.]cpp'" "${selfRow}${labelRow}" "src/a.cpp|// the Pace daemon\n")
brand_case("path" "OBJECTED" " src/${oldName}-tool[.]cpp: the PATH carries the old name" "${selfRow}" "src/${oldName}-tool.cpp|// nothing inside\n")

if(failures)
    message(FATAL_ERROR "brand-leftovers selftest: ran ${ran} cases, and:${failures}")
endif()
message(STATUS "brand-leftovers selftest: ran ${ran} cases, all as expected")
