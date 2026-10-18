# SPDX-License-Identifier: Apache-2.0
# The configure refusal of a build tree configured before the rename to Pace
# (cmake/RetiredOptions.cmake, #1713), driven by its decision over names it is handed.
#
# Judged by its OUTPUT (`FAIL_REGULAR_EXPRESSION`): a case that goes wrong prints a
# `CMake Error` line naming the case, and every case runs whatever an earlier one said.
#
#   cmake -DPACE_SOURCE_DIR=<repo> -P scripts/check-retired-options.cmake

if(NOT PACE_SOURCE_DIR)
    message(FATAL_ERROR "PACE_SOURCE_DIR must name the repository")
endif()
include("${PACE_SOURCE_DIR}/cmake/RetiredOptions.cmake")

set(ran 0)

# Each case: a name, the cache names it hands the decision, and the retired entries it must name.
function(retired_options_case name expected)
    pace_retired_cache_entries(found ${ARGN})
    if(NOT "${found}" STREQUAL "${expected}")
        message(SEND_ERROR "case '${name}': named '${found}', expected '${expected}'")
    endif()
endfunction()

# A tree configured before the rename: its FASTCACHED_ entry is named, and nothing else is.
retired_options_case("a FASTCACHED_ entry is refused" "FASTCACHED_BUILD_NODE"
    CMAKE_BUILD_TYPE FASTCACHED_BUILD_NODE PACE_BUILD_NODE)
math(EXPR ran "${ran} + 1")

# A tree configured after it: PACE_ alone, and a name that merely CONTAINS the old spelling.
retired_options_case("PACE_ entries are not refused" ""
    CMAKE_BUILD_TYPE PACE_BUILD_NODE MY_FASTCACHED_THING)
math(EXPR ran "${ran} + 1")

message(STATUS "check-retired-options: ran ${ran} cases")
