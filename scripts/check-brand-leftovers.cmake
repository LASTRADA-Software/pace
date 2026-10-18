# SPDX-License-Identifier: Apache-2.0
# Holds the tree to the rename to Pace (0.6.0): no first-party tracked file and no tracked
# PATH spells the old product's name, except where a row of
# `scripts/brand-leftovers-exemptions.tsv` says why it must.
#
#   cmake -DPACE_SOURCE_DIR=<repo> [-DPACE_BRAND_EXEMPTIONS=<tsv>] -P scripts/check-brand-leftovers.cmake
#
# WHAT IT ASKS
# Every tracked file the tracked-files seam answers with (`pace_tracked_files`), minus the
# third-party roots (`pace_decline_third_party`, which it NAMES), is searched for the old
# name case-insensitively. A match is reported as `path:line: text` unless an exemption row
# covers it: `path-glob <TAB> regex <TAB> reason`, where the glob is matched against the
# path (`*` crosses `/`, as Python's fnmatch reads it) and every match of the regex on the
# line is removed before the line is judged again. The rename tool read the same file to
# know what it must not rewrite, so the tool and this check cannot disagree about it.
#
# A row that covers NOTHING is refused as stale: an exemption outliving its reason is a
# hole nobody can see. A tracked PATH carrying the old name is never exempt.
#
# The regex column is read by CMake's regex engine here and by Python's in the tool, so a
# row is spelled in what both read alike: classes (`[.]`, `[?]`), `|`, `( )`, `*`, `+`.
# No `\b`, no `\.`, no `(?i)`.
#
# BLIND SPOT, AND THE DIRECTION IT FAILS IN
# Which files are READ is narrowed first by a literal search for the casings the name was
# ever written in (all lower, all upper, Capitalised, CamelCase and their mixtures below).
# A file spelling it in some other casing only is never read, so this fails OPEN for an
# exotic casing -- a missed leftover, never a false refusal. Every file that IS read is
# judged case-insensitively.
#
# A SECOND BLIND SPOT, failing open the same way: a name that is never written as a string. The
# search reads text, so a name assembled character by character is invisible to it. Two are, on
# purpose: the CoW store's reserved marker keys in src/Pace/Cache/CowTreeStorage.hpp
# (`FormatMarkerKeyBytes` and `MigrationMarkerKeyBytes`, the old daemon name followed by
# `.fmt` and `.mig`) are spelled as char arrays, and they stay UNCHANGED: they are records
# of the on-disk format, as the file's magic is, and renaming one would make every existing
# store unreadable. They need no row because nothing here can see them.
#
# The verdict is printed with the number of files scanned, the matches exempted and the
# rows used, so a run that judged nothing cannot look like one that judged everything.

cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED PACE_SOURCE_DIR)
    message(FATAL_ERROR "PACE_SOURCE_DIR must be set")
endif()
if(NOT DEFINED PACE_BRAND_EXEMPTIONS)
    set(PACE_BRAND_EXEMPTIONS "${PACE_SOURCE_DIR}/scripts/brand-leftovers-exemptions.tsv")
endif()

# The old name, assembled rather than spelled, so this file is not one of its own findings.
set(oldName "fast")
string(APPEND oldName "cache")
set(oldNamePattern "[Ff][Aa][Ss][Tt][Cc][Aa][Cc][Hh][Ee]")
set(casings "")
foreach(head "fast" "Fast" "FAST")
    foreach(tail "cache" "Cache" "CACHE")
        list(APPEND casings "${head}${tail}")
    endforeach()
endforeach()

# Placeholders for the three characters a CMake list would read as structure, so a line
# survives the split verbatim and is restored before it is matched. Control characters no
# text file here carries.
string(ASCII 1 leftBracket)
string(ASCII 2 rightBracket)
string(ASCII 3 semicolon)

# Split @p content into lines, every one restorable by `brand_restore`.
function(brand_split content outVar)
    string(REPLACE "[" "${leftBracket}" content "${content}")
    string(REPLACE "]" "${rightBracket}" content "${content}")
    string(REPLACE ";" "${semicolon}" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(${outVar} "${lines}" PARENT_SCOPE)
endfunction()

# A line as the file spelled it.
function(brand_restore line outVar)
    string(REPLACE "${leftBracket}" "[" line "${line}")
    string(REPLACE "${rightBracket}" "]" line "${line}")
    string(REPLACE "${semicolon}" ";" line "${line}")
    set(${outVar} "${line}" PARENT_SCOPE)
endfunction()

# A glob, as fnmatch reads it, as an anchored CMake regex.
function(brand_glob_regex glob outVar)
    string(REGEX REPLACE "([.+^$()|{}\\\\])" "\\\\\\1" escaped "${glob}")
    string(REPLACE "*" ".*" escaped "${escaped}")
    string(REPLACE "?" "." escaped "${escaped}")
    set(${outVar} "^${escaped}$" PARENT_SCOPE)
endfunction()

# --- The exemption table ----------------------------------------------------------------

if(NOT EXISTS "${PACE_BRAND_EXEMPTIONS}")
    message(FATAL_ERROR "brand leftovers: the exemption table ${PACE_BRAND_EXEMPTIONS} does not exist, so nothing could be judged")
endif()
file(READ "${PACE_BRAND_EXEMPTIONS}" tableText)
brand_split("${tableText}" tableLines)
set(rowCount 0)
set(problems "")
foreach(rawRow IN LISTS tableLines)
    brand_restore("${rawRow}" row)
    if(row STREQUAL "" OR row MATCHES "^#")
        continue()
    endif()
    string(FIND "${row}" "\t" firstTab)
    if(firstTab EQUAL -1)
        list(APPEND problems "  exemption row '${row}' has no tab: wanted path-glob, regex and reason")
        continue()
    endif()
    string(SUBSTRING "${row}" 0 ${firstTab} glob)
    math(EXPR afterGlob "${firstTab} + 1")
    string(SUBSTRING "${row}" ${afterGlob} -1 rest)
    string(FIND "${rest}" "\t" secondTab)
    if(secondTab EQUAL -1)
        list(APPEND problems "  exemption row '${row}' has one tab: wanted path-glob, regex and reason")
        continue()
    endif()
    string(SUBSTRING "${rest}" 0 ${secondTab} regex)
    math(EXPR afterRegex "${secondTab} + 1")
    string(SUBSTRING "${rest}" ${afterRegex} -1 reason)
    if(glob STREQUAL "" OR regex STREQUAL "" OR reason STREQUAL "")
        list(APPEND problems "  exemption row '${row}' leaves a column empty; a reason is what makes it an exemption")
        continue()
    endif()
    brand_glob_regex("${glob}" globRegex)
    set(rowGlob_${rowCount} "${glob}")
    set(rowGlobRegex_${rowCount} "${globRegex}")
    set(rowRegex_${rowCount} "${regex}")
    set(rowUsed_${rowCount} 0)
    math(EXPR rowCount "${rowCount} + 1")
endforeach()
if(rowCount EQUAL 0)
    list(APPEND problems "  ${PACE_BRAND_EXEMPTIONS} holds no row, which is a table that stopped being read rather than a tree with no exception")
endif()

# --- The tree ---------------------------------------------------------------------------

pace_tracked_files("${PACE_SOURCE_DIR}"
    GLOBS "*"
    FILES_OUT files
    MODE_OUT mode
    CONTAINING ${casings}
    CONTAINING_OUT holding)
pace_decline_third_party("${PACE_SOURCE_DIR}" files declined)
pace_decline_third_party("${PACE_SOURCE_DIR}" holding declinedHolding)
if(declined)
    list(LENGTH declined declinedCount)
    list(GET declined 0 firstDeclined)
    message(STATUS "brand leftovers: declined ${declinedCount} third-party file(s) under scripts/lib/third-party-roots.txt, first ${firstDeclined}")
endif()
list(LENGTH files fileCount)
if(fileCount EQUAL 0)
    message(FATAL_ERROR "brand leftovers: the ${mode} found no tracked file at all, which is a scan that stopped working, not a clean tree")
endif()

set(findings "")
set(exempted 0)
foreach(path IN LISTS files)
    if(path MATCHES "${oldNamePattern}")
        list(APPEND findings "  ${path}: the PATH carries the old name")
    endif()
endforeach()

foreach(path IN LISTS holding)
    file(READ "${PACE_SOURCE_DIR}/${path}" content)
    brand_split("${content}" lines)
    # The rows whose glob takes this path, decided once per file.
    set(rows "")
    if(rowCount GREATER 0)
        math(EXPR lastRow "${rowCount} - 1")
        foreach(index RANGE 0 ${lastRow})
            if(path MATCHES "${rowGlobRegex_${index}}")
                list(APPEND rows ${index})
            endif()
        endforeach()
    endif()
    set(lineNumber 0)
    foreach(rawLine IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")
        if(NOT rawLine MATCHES "${oldNamePattern}")
            continue()
        endif()
        brand_restore("${rawLine}" line)
        set(judged "${line}")
        foreach(index IN LISTS rows)
            if(judged MATCHES "${rowRegex_${index}}")
                string(REGEX REPLACE "${rowRegex_${index}}" "" judged "${judged}")
                math(EXPR rowUsed_${index} "${rowUsed_${index}} + 1")
            endif()
        endforeach()
        if(judged MATCHES "${oldNamePattern}")
            # Shown, not matched: the three list-structure characters are drawn as others,
            # so one finding stays one entry of the list it is reported from.
            string(STRIP "${line}" shown)
            string(REPLACE ";" "," shown "${shown}")
            string(REPLACE "[" "(" shown "${shown}")
            string(REPLACE "]" ")" shown "${shown}")
            string(LENGTH "${shown}" shownLength)
            if(shownLength GREATER 160)
                string(SUBSTRING "${shown}" 0 160 shown)
                string(APPEND shown "...")
            endif()
            list(APPEND findings "  ${path}:${lineNumber}: ${shown}")
        else()
            math(EXPR exempted "${exempted} + 1")
        endif()
    endforeach()
endforeach()

set(usedRows 0)
if(rowCount GREATER 0)
    math(EXPR lastRow "${rowCount} - 1")
    foreach(index RANGE 0 ${lastRow})
        if(rowUsed_${index} EQUAL 0)
            list(APPEND problems "  stale exemption: '${rowGlob_${index}}' / '${rowRegex_${index}}' covers nothing in this tree; delete the row")
        else()
            math(EXPR usedRows "${usedRows} + 1")
        endif()
    endforeach()
endif()

list(LENGTH holding holdingCount)
message(STATUS "brand leftovers (${mode}): scanned ${fileCount} file(s), read ${holdingCount}, exempted ${exempted} match(es), ${usedRows} of ${rowCount} row(s) used")

if(findings OR problems)
    set(text "")
    foreach(entry IN LISTS findings problems)
        string(APPEND text "\n${entry}")
    endforeach()
    message(FATAL_ERROR
        "brand leftovers: the rename to Pace is incomplete or its exemption table is stale:${text}\n"
        "Rename the leftover (docs/upgrading-to-pace.md has the map), or, where the old name must stay, "
        "add a row to scripts/brand-leftovers-exemptions.tsv with the reason it stays.")
endif()
