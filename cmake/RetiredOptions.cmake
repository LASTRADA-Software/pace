# SPDX-License-Identifier: Apache-2.0
# The rename to Pace retired every FASTCACHED_* option (#1713 removes this check in 0.8.0).
#
# Nothing reads a FASTCACHED_* cache entry any more, so a build tree configured before the
# rename would go on configuring with every option it was given silently ignored -- the
# defaults in force, a reference build quietly not one. That is refused here, naming the
# entries and the remedy, rather than bridged: no value is read.
#
# Included first thing after project() by the top-level CMakeLists.txt, and NEVER from
# cmake/portable/CompileCache.cmake, which may not fail a configure.

# The cache entries among @p ARGN that carry a retired spelling.
#   pace_retired_cache_entries(<out> <name>...)
# The decision alone, so `scripts/check-retired-options.cmake` can feed it names.
function(pace_retired_cache_entries out)
    set(names ${ARGN})
    list(FILTER names INCLUDE REGEX "^FASTCACHED_")
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# The refusal over this configure's own cache.
function(pace_refuse_retired_cache_entries)
    get_cmake_property(cached CACHE_VARIABLES)
    pace_retired_cache_entries(retired ${cached})
    if(retired)
        list(JOIN retired ", " listed)
        message(FATAL_ERROR
            "This build tree was configured before the rename to Pace (${listed}). FASTCACHED_* options "
            "are renamed PACE_* and are no longer read: delete this build tree and configure again "
            "(docs/upgrading-to-pace.md).")
    endif()
endfunction()
