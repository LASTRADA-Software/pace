# SPDX-License-Identifier: Apache-2.0
#
# `node-working-directory` must be SEEN to refuse, on each thing it claims and on
# nothing else.
#
# A check nobody has watched refuse is not a guard, and this one is easier than
# most to get wrong in the silent direction: it looks for the ABSENCE of a
# directive, so every mistake in it -- a pattern that also matches comments, a
# reader that takes the first assignment instead of the last, a resolution step
# that lets an unresolved specifier through -- makes it pass more often rather
# than less. All three of those are cases below, and each was written because the
# check could plausibly have shipped with it.
#
# Two of the cases exist for the opposite reason and are the ones that stop this
# becoming a check that refuses everything: a unit that spawns no compiler and
# names no working directory must PASS, and a later assignment overriding an
# earlier `/` must PASS. Without them a check that objected unconditionally would
# satisfy every other case here.
#
# Runs as `cmake -P`. See check-script-check-signals.cmake for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DPACE_SOURCE_DIR=<dir> -DPACE_SCRATCH_DIR=<dir> \
#         -P scripts/check-node-working-directory-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED PACE_SOURCE_DIR)
    message(FATAL_ERROR "PACE_SOURCE_DIR must be set")
endif()
if(NOT DEFINED PACE_SCRATCH_DIR)
    message(FATAL_ERROR "PACE_SCRATCH_DIR must be set")
endif()

set(check "${PACE_SOURCE_DIR}/scripts/check-node-working-directory.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${PACE_SCRATCH_DIR}/node-working-directory-selftest")
file(REMOVE_RECURSE "${root}")

set(failures "")
set(caseCount 0)

# The three units the check's table names, in their good form. A case then
# overwrites exactly one of them, so every failure is attributable to the thing
# the case changed rather than to the tree being incomplete.
set(GoodWorker
"[Service]
Type=simple
ExecStart=/usr/bin/pace-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-node
CacheDirectory=fastcache-node
")
set(GoodNodeDefaults
"inline constexpr std::string_view NodeStateDirectoryName = \"fastcache-node\";
")
set(GoodDaemon
"[Service]
Type=simple
ExecStart=/usr/bin/pace-kv
StateDirectory=pace-kv
")
# The reference configuration and a page, each citing the directory the good worker unit
# creates -- the reference in prose ending a sentence, the page as a flag -- so a case can
# overwrite either one and every other citation still agrees.
set(GoodReference
"# back the local cache tier with disk at this path.
#
# Under the packaged Linux service, cache_dir: /var/cache/fastcache-node. The unit creates it.
#cache_dir:
")
set(GoodPage
"| `--cache-dir=/var/cache/fastcache-node` | Both halves. |
")

# Build a good tree, then replace one file's text.
#
# @param name The case name; also the scratch subdirectory.
# @param file The file to overwrite, relative to the tree root, or "" for none.
# @param body The replacement text.
# @param outVar Set to the tree root.
function(pace_make_tree name file body outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/packaging/linux")
    file(MAKE_DIRECTORY "${tree}/packaging/config")
    file(MAKE_DIRECTORY "${tree}/docs/tools")
    file(MAKE_DIRECTORY "${tree}/src/apps/pace-node")
    file(WRITE "${tree}/packaging/linux/pace-node.service" "${GoodWorker}")
    file(WRITE "${tree}/packaging/linux/pace-kv.service" "${GoodDaemon}")
    file(WRITE "${tree}/packaging/linux/pace-kv-user.service" "${GoodDaemon}")
    file(WRITE "${tree}/packaging/config/pace-node.yaml" "${GoodReference}")
    file(WRITE "${tree}/docs/tools/pace-node.md" "${GoodPage}")
    file(WRITE "${tree}/src/apps/pace-node/NodeDefaults.hpp" "${GoodNodeDefaults}")
    if(NOT file STREQUAL "")
        file(WRITE "${tree}/${file}" "${body}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# @param tree The tree to run the check against.
# @param outObjected Set to TRUE when the check reported a violation.
# @param outOutput Set to the check's combined stdout and stderr.
function(pace_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPACE_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely WARNS
    # changes meaning silently and, read for the error word alone, is scored a clean pass
    # (#672). Stated in full -- and enforced -- in `scripts/check-script-check-signals.cmake`.
    set(sawSignal FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    set(${outObjected} ${sawSignal} PARENT_SCOPE)
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# One case. `expected` is REFUSE or PASS; `mustSay` is a substring the refusal has
# to contain, so a case cannot be satisfied by the check objecting for some other
# reason -- which is how a selftest comes to certify a check that is right by
# accident. Empty means the wording is not part of the claim.
#
# @param name The case name.
# @param file The file to overwrite, relative to the tree root, or "" to leave the good tree alone.
# @param body The replacement text.
# @param expected REFUSE or PASS.
# @param mustSay A substring the refusal must contain, or "".
# @param because What this case is claiming, printed when it does not hold.
function(pace_file_case name file body expected mustSay because)
    pace_make_tree("${name}" "${file}" "${body}" tree)
    pace_run_check("${tree}" objected output)
    math(EXPR next "${caseCount} + 1")
    set(caseCount "${next}" PARENT_SCOPE)
    if(expected STREQUAL "REFUSE")
        if(NOT objected)
            list(APPEND failures "${name}: not refused -- ${because}")
            set(failures "${failures}" PARENT_SCOPE)
            return()
        endif()
        if(NOT mustSay STREQUAL "")
            string(FIND "${output}" "${mustSay}" said)
            if(said EQUAL -1)
                list(APPEND failures "${name}: refused, but not for its own reason -- the report never says '${mustSay}', so this case would pass against a check that objects to everything")
                set(failures "${failures}" PARENT_SCOPE)
            endif()
        endif()
    elseif(objected)
        list(APPEND failures "${name}: refused a good tree -- ${because}")
        set(failures "${failures}" PARENT_SCOPE)
    endif()
endfunction()

# One case that overwrites a UNIT, named relative to packaging/linux/.
#
# A function rather than a macro, so its parameters are variables `if()` can read; the
# count and the failures the case above records one scope down are handed back up here,
# or every case run through this one would go uncounted.
#
# @param name The case name.
# @param unit The unit to overwrite, relative to packaging/linux/, or "" for none.
# @param body The replacement text.
# @param expected REFUSE or PASS.
# @param mustSay A substring the refusal must contain, or "".
# @param because What this case is claiming, printed when it does not hold.
function(pace_case name unit body expected mustSay because)
    set(file "")
    if(NOT unit STREQUAL "")
        set(file "packaging/linux/${unit}")
    endif()
    pace_file_case("${name}" "${file}" "${body}" "${expected}" "${mustSay}" "${because}")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# --- the shape that must PASS, or every refusal below means nothing ----------
pace_case("clean" "" "" "PASS" ""
    "the worker names a WorkingDirectory it creates through RuntimeDirectory, which is exactly what #674 asks for")

# --- #674 itself -------------------------------------------------------------
pace_case("missing" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
" "REFUSE" "names no WorkingDirectory"
    "a worker unit with no WorkingDirectory= starts in / -- this IS #674, and a check that misses it has no reason to exist")

pace_case("root" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/
" "REFUSE" "the defect spelled out"
    "/ is the value the defect produces, and writing it explicitly must not be a way to satisfy the check")

# --- the three silent ways a checker for an ABSENCE gets it wrong ------------
# A comment is not a directive. The fix for #674 puts a long paragraph directly
# above the directive, so a pattern matching anywhere on the line would report the
# defective file as fixed the moment somebody documented it (#723).
pace_case("comment-only" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
# WorkingDirectory=/run/fastcache-node
" "REFUSE" "names no WorkingDirectory"
    "a commented-out directive configures nothing, and a check that counted it would pass the very file that caused #674")

# systemd overrides rather than accumulates, and an empty assignment resets to the
# default. A reader that answers 'is one present' passes this.
pace_case("reset" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
WorkingDirectory=
" "REFUSE" "RESETS"
    "a later empty assignment undoes the earlier one and puts the service back in /")

# The mirror of the case above, and the one that stops 'take the last' being read
# as 'take any': an override that FIXES an earlier bad value must pass.
pace_case("last-wins" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
StateDirectory=fastcache-node
WorkingDirectory=/
WorkingDirectory=/run/fastcache-node
" "PASS" ""
    "the last assignment is the one in force, so an earlier / is overridden and harmless")

# --- values that look like a fix and are not ---------------------------------
pace_case("dash-prefix" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=-/run/fastcache-node
" "REFUSE" "starts the service anyway"
    "the '-' prefix means start even if the chdir fails, and where it fails the service starts in / with a directive present to hide it")

pace_case("home" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=~
" "REFUSE" "not an absolute path"
    "~ is the service account's home, which this project's sysusers gives as / -- the bug by another spelling")

pace_case("unresolved-specifier" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=%h/fastcache-node
" "REFUSE" "cannot resolve"
    "a specifier this check does not resolve is a value it cannot judge, and %h is the account home again")

pace_case("resolvable-specifier" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
StateDirectory=fastcache-node
WorkingDirectory=%t/fastcache-node
" "PASS" ""
    "%t is the runtime root for a system unit, so %t/fastcache-node is the RuntimeDirectory above and must be accepted rather than refused as unresolvable")

# --- a directory nothing creates ---------------------------------------------
pace_case("uncreated" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
WorkingDirectory=/opt/fastcache-node
" "REFUSE" "does not create"
    "under ProtectSystem=strict a directory the unit does not declare is read-only or absent, and a service whose chdir fails does not start at all -- a worse outcome than the one being fixed")

pace_case("state-directory" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
StateDirectory=fastcache-node
WorkingDirectory=/var/lib/fastcache-node
" "PASS" ""
    "StateDirectory= creates a directory just as RuntimeDirectory= does, so a unit using it is not a violation")

# --- rows that spawn no compiler ---------------------------------------------
# The exemption, and the reason the middle column of the table exists. Without
# this case a check that demanded a directive everywhere would pass every case
# above.
pace_case("daemon-needs-none" "pace-kv.service"
"[Service]
ExecStart=/usr/bin/pace-kv
" "PASS" ""
    "the cache daemon executes nothing, so no prefix-map rule is ever derived from its directory and it is exempt by table")

# And the half of the rule that is NOT about compilers: a broken directive breaks
# the service whatever the program does.
pace_case("daemon-uncreated" "pace-kv.service"
"[Service]
ExecStart=/usr/bin/pace-kv
WorkingDirectory=/opt/nowhere
" "REFUSE" "does not create"
    "a WorkingDirectory nothing creates stops the service starting regardless of whether it spawns a compiler, so the exemption is from NEEDING one, not from being checked")

# --- the failure mode a consistency check must not have ----------------------
pace_case("state-directory-missing" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
" "REFUSE" "declares no StateDirectory="
    "a worker unit that hands over no state directory leaves the unprivileged service keeping the machine's identity in its account's home")

pace_case("state-directory-other-name" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-worker
" "REFUSE" "the node declines it"
    "systemd hands over a directory the node does not take, since it takes only one named NodeStateDirectoryName")

pace_case("state-directory-reset" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-node
StateDirectory=
" "REFUSE" "in force"
    "a later empty assignment resets the list, so nothing is handed over")

pace_case("state-directory-list" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
CacheDirectory=fastcache-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-cache fastcache-node
" "PASS" ""
    "one assignment may name several directories, and the node's is among them")

# --- the directory a node's disk tier lives in -------------------------------
# The shipped defect: a unit with no CacheDirectory= while the docs cite
# /var/cache/fastcache-node, so the one path an operator is told to write is read-only
# to the service.
pace_case("cache-directory-missing" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-node
" "REFUSE" "declares no CacheDirectory="
    "without CacheDirectory= nothing persistent is writable under ProtectSystem=strict but the state directory, which the node refuses as a cache_dir")

pace_case("cache-directory-comment-only" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-node
# CacheDirectory=fastcache-node
" "REFUSE" "declares no CacheDirectory="
    "a commented-out directive creates nothing, and the line that fixes this sits under a paragraph explaining it")

pace_case("cache-directory-reset" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-node
CacheDirectory=fastcache-node
CacheDirectory=
" "REFUSE" "RESETS"
    "a later empty assignment resets the list, so nothing is created")

pace_case("cache-directory-list" "pace-node.service"
"[Service]
ExecStart=/usr/bin/pace-node
RuntimeDirectory=fastcache-node
WorkingDirectory=/run/fastcache-node
StateDirectory=fastcache-node
CacheDirectory=fastcache-other fastcache-node
" "PASS" ""
    "one assignment may name several directories, and the cited one is among them")

# The workaround the defect invited, and what it cost: a cache_dir inside the state
# directory, which the node refuses to start beside.
pace_file_case("cited-state-directory" "docs/tools/pace-node.md"
"Under the service, `cache_dir: /var/lib/fastcache-node/cache` keeps objects.
" "REFUSE" "cites cache_dir /var/lib/fastcache-node/cache"
    "a cache_dir inside the state directory is writable and stops the node starting, so a page citing one must be refused")

pace_file_case("cited-other-name" "docs/tools/pace-node.md"
"| `--cache-dir=/var/cache/fastcache-worker` | Both halves. |
" "REFUSE" "cites cache_dir /var/cache/fastcache-worker"
    "a /var/cache directory the unit does not declare is read-only to it, whatever its name says")

pace_file_case("cited-space-spelling" "docs/tools/pace-node.md"
"    pace-node --migrate-cache --cache-dir /var/lib/elsewhere
" "REFUSE" "cites cache_dir /var/lib/elsewhere"
    "`--cache-dir <path>` is a citation as much as `--cache-dir=<path>`, and the docs spell both")

pace_file_case("cited-below" "docs/tools/pace-node.md"
"    pace-node --cache-dir /var/cache/fastcache-node/objects
" "PASS" ""
    "a path inside the directory the unit creates is just as writable as the directory itself")

pace_file_case("cited-not-a-citation" "docs/tools/pace-node.md"
"    --cache-dir cannot open /var/lib/elsewhere/objects.cow: refusing to start
" "PASS" ""
    "a quoted diagnostic NAMES the flag and a path; it tells nobody what to write, and refusing it would make every quoted error a violation")

# The stated blind spot, pinned in its stated direction: only /var/ is judged.
pace_file_case("cited-outside-var" "docs/tools/pace-node.md"
"    pace-node --cache-dir=/srv/fastcache
" "PASS" ""
    "the check reads only /var/, since the same text is right for a node started by hand -- the blind spot fails OPEN and says so")

pace_file_case("reference-cites-nothing" "packaging/config/pace-node.yaml"
"# back the local cache tier with disk at this path.
#cache_dir:
" "REFUSE" "cites no cache_dir"
    "the reference is where an operator learns the path, and a scan finding nothing there is no verdict")

pace_file_case("reference-cites-state-directory" "packaging/config/pace-node.yaml"
"# Under the packaged Linux service, cache_dir: /var/lib/fastcache-node/cache. Do it.
#cache_dir:
" "REFUSE" "cites cache_dir /var/lib/fastcache-node/cache, which"
    "a wrong path in the reference is refused like one in a page, and reported without the full stop that ended its sentence -- the trim the clean tree's reference also needs to pass")

# Which way the citing files were found is part of the verdict: this synthetic tree has no
# git index of its own, so the walk answered here while git answers in CI, and a pass that
# does not say which is a pass about an unnamed path.
pace_make_tree("citation-mode-stated" "" "" tree)
pace_run_check("${tree}" objected output)
math(EXPR caseCount "${caseCount} + 1")
if(objected)
    list(APPEND failures "citation-mode-stated: the good tree was refused, so its verdict line was never printed")
else()
    string(FIND "${output}" "cache_dir citation(s) across 2 file(s) name (directory walk" said)
    if(said EQUAL -1)
        list(APPEND failures "citation-mode-stated: the verdict does not say it read both citing files by directory walk -- output was: ${output}")
    endif()
endif()

pace_make_tree("state-name-unreadable" "" "" tree)
file(WRITE "${tree}/src/apps/pace-node/NodeDefaults.hpp" "// renamed\n")
pace_run_check("${tree}" objected output)
math(EXPR caseCount "${caseCount} + 1")
if(NOT objected)
    list(APPEND failures "state-name-unreadable: a header spelling no NodeStateDirectoryName was not refused -- the unit would be judged against nothing")
else()
    string(FIND "${output}" "spells no NodeStateDirectoryName" said)
    if(said EQUAL -1)
        list(APPEND failures "state-name-unreadable: refused, but not for its own reason")
    endif()
endif()

pace_make_tree("absent" "" "" tree)
file(REMOVE "${tree}/packaging/linux/pace-node.service")
pace_run_check("${tree}" objected output)
math(EXPR caseCount "${caseCount} + 1")
if(NOT objected)
    list(APPEND failures "absent: a unit named by the table and missing from the tree was not refused -- a scan that finds nothing would report every remaining row as agreeing")
else()
    string(FIND "${output}" "nothing was scanned" said)
    if(said EQUAL -1)
        list(APPEND failures "absent: refused, but not for its own reason -- the report never says 'nothing was scanned'")
    endif()
endif()

# ---------------------------------------------------------------------------
# The count is printed whether or not anything failed. A selftest that stops early
# and a selftest that judged every case look identical otherwise, which is the
# defect #723 records for the shell ones.
if(NOT failures STREQUAL "")
    list(JOIN failures "\n  " report)
    message(FATAL_ERROR
        "check-node-working-directory.cmake does not refuse what it claims to "
        "(${caseCount} case(s) run):\n  ${report}\n\n"
        "Each case above is a way the shipped compile-node unit could go back to starting in /, "
        "which produces a wrong object under a right key. The check lives in ${check}.")
endif()

message(STATUS "node working directory selftest: ${caseCount} case(s), every verdict as claimed")
