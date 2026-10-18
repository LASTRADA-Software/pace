# SPDX-License-Identifier: Apache-2.0
#
# Drives scripts/check-target-pragmas.cmake over synthetic trees in both directions. Each case is named by its
# first argument. The accept cases include the tree's legitimate spelling -- per-function target attributes,
# copied from the hardware SHA-256 path (#1420) -- beside a near-miss line that gets past the prefilter, so the
# scanner itself is seen accepting them; and one case asserts a file holding only `#pragma once` is not walked,
# which is what keeps the check off the quadratic walk for every header.
#
# Usage:
#   cmake -DPACE_SOURCE_DIR=<repo> -DPACE_SCRATCH_DIR=<dir> -P scripts/check-target-pragmas-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS PACE_SOURCE_DIR PACE_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${PACE_SOURCE_DIR}/scripts/check-target-pragmas.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(ran 0)
set(mismatches "")

function(pace_tree name out)
    set(root "${PACE_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(WRITE "${root}/src/Pace/Core/Unit.cpp" "int Unit() { return 1; }\n")
    set(${out} "${root}" PARENT_SCOPE)
endfunction()

# Run the check; @p refuses says which side it must land on, @p expected is text the output must hold.
function(pace_judge name root refuses expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACE_SOURCE_DIR=${root}" -P "${check}"
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    string(APPEND output "${errors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
    # Both words, as ctest's own `PACE_SCRIPT_CHECK_FAILED` reads a check: a sub-run that merely WARNS
    # must not be scored a clean pass here while ctest would refuse it.
    set(refused OFF)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused ON)
    endif()
    string(FIND "${flat}" "${expected}" said)
    set(problem "")
    if(refuses AND NOT refused)
        set(problem "accepted, and must refuse")
    elseif(NOT refuses AND refused)
        set(problem "refused, and must accept")
    elseif(said EQUAL -1)
        set(problem "reached the right side for the wrong reason: no `${expected}`")
    endif()
    math(EXPR ran "${ran} + 1")
    set(ran "${ran}" PARENT_SCOPE)
    if(NOT problem STREQUAL "")
        set(mismatches "${mismatches}\n  ${name}: ${problem}\n${output}" PARENT_SCOPE)
    endif()
endfunction()

pace_tree(pragmaOnceOnly root)
file(WRITE "${root}/src/Pace/Core/Unit.hpp" "#pragma once\nint Unit();\n")
pace_judge(pragmaOnceOnly "${root}" OFF "2 source(s) under src/ carry no target pragma (0 got past the prefilter)")

pace_tree(pragmaInComment root)
file(WRITE "${root}/src/Pace/Core/Notes.hpp" [==[
#pragma once
// Never write #pragma GCC target("sha") here: it reaches every function after it.
/* #pragma clang attribute push (__attribute__((target("avx2"))), apply_to = function) */
]==])
pace_judge(pragmaInComment "${root}" OFF "2 source(s) under src/ carry no target pragma (1 got past the prefilter)")

# The tree's one legitimate spelling, copied from the hardware SHA-256 path (#1420), with a diagnostic pragma
# whose comment names a target attribute: that line gets past the prefilter, so the scanner judges the file.
pace_tree(perFunctionAttributes root)
file(WRITE "${root}/src/Pace/Core/Sha256.cpp" [==[
// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Sha256.hpp>

#pragma GCC diagnostic ignored "-Wattributes" // a target attribute an older GCC does not know is not an error

#if defined(_M_X64) || defined(__x86_64__)
    #include <immintrin.h>
#elif defined(__APPLE__) && defined(__aarch64__)
    #include <arm_neon.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
    #if defined(__GNUC__) || defined(__clang__)
        #define PACE_SHA_NI_TARGET __attribute__((target("sha,ssse3,sse4.1")))
    #else
        #define PACE_SHA_NI_TARGET
    #endif

    [[nodiscard]] PACE_SHA_NI_TARGET __m128i LoadWordsX86(std::span<std::byte const> block, std::size_t index) noexcept
    {
    }

    PACE_SHA_NI_TARGET void CompressBlocksX86ShaNi(State& state, std::span<std::byte const> blocks) noexcept
    {
    }

    #undef PACE_SHA_NI_TARGET
#endif

#if defined(__APPLE__) && defined(__aarch64__)
    #define PACE_ARM_SHA2_TARGET __attribute__((target("sha2")))

    PACE_ARM_SHA2_TARGET void CompressBlocksArmSha2(State& state, std::span<std::byte const> blocks) noexcept
    {
    }

    #undef PACE_ARM_SHA2_TARGET
#endif
]==])
pace_judge(perFunctionAttributes "${root}" OFF "2 source(s) under src/ carry no target pragma (1 got past the prefilter)")

pace_tree(pragmaGcc root)
file(WRITE "${root}/src/Pace/Core/Fast.cpp" "#include <x.hpp>\n#pragma GCC target(\"sha\")\nint Fast() { return 3; }\n")
pace_judge(pragmaGcc "${root}" ON "src/Pace/Core/Fast.cpp:2: a target pragma enables an instruction set")

pace_tree(pragmaClangIndented root)
file(WRITE "${root}/src/Pace/Core/Fast.hpp" [==[
#pragma once
  #  pragma clang attribute push (__attribute__((target("avx2"))), apply_to = function)
]==])
pace_judge(pragmaClangIndented "${root}" ON "src/Pace/Core/Fast.hpp:2: a target pragma")

# A block comment between the words: stripping turns it into a space, and the prefilter still admits the line.
pace_tree(pragmaBehindBlockComment root)
file(WRITE "${root}/src/Pace/Core/Fast.cpp" "#pragma/**/GCC target(\"avx2\")\n")
pace_judge(pragmaBehindBlockComment "${root}" ON "src/Pace/Core/Fast.cpp:1: a target pragma")

pace_tree(pragmaInConfiguredTemplate root)
file(WRITE "${root}/src/Pace/Core/Version.hpp.in" "#pragma once\n#pragma GCC target(\"avx2\")\n")
pace_judge(pragmaInConfiguredTemplate "${root}" ON "src/Pace/Core/Version.hpp.in:2: a target pragma")

pace_tree(noSources root)
file(REMOVE "${root}/src/Pace/Core/Unit.cpp")
file(WRITE "${root}/src/README.md" "#pragma GCC target(\"sha\") in prose\n")
pace_judge(noSources "${root}" ON "no C or C++ source under")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "target-pragmas-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "target-pragmas-selftest: ${ran} case(s) ran, every verdict as it must be")
