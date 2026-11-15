#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# A compile over its node's memory ceiling is killed by the KERNEL, refused to its client,
# and rebuilt locally -- on this machine, in a delegated cgroup, with a real compiler.
#
# ## What it proves that the unit tests cannot
#
# `JobCgroups_test`, `CompileMemoryMeter_test` and `KilledCompiles_test` drive the cgroup
# layout, the ceiling write and the kill classification over the fake `ICgroupHost`. Every
# one of them passes on a node whose `compiles/memory.max` never reaches the kernel, or whose
# kill the kernel never reports, because the fake answers whatever the case stages. What
# only a real host can say is the chain end to end:
#
#   1. The ceiling is IN FORCE    -- the node says `compile memory ceiling: <bytes> (typed)`
#                                    AND its `compiles/memory.max` reads those bytes. The log
#                                    line alone is a claim.
#   2. The kernel KILLED it       -- `compiles/memory.events` counts an `oom_kill`, read off
#                                    the cgroup rather than off the node, so the node's own
#                                    classification is checked against an instrument it
#                                    does not own.
#   3. It was classified CEILING  -- `pace_worker_compiles_killed_ceiling_total` is 1 and the
#                                    shed and signal rows are 0: WHICH kill, not whether one.
#                                    The node logs `was killed (ceiling)`.
#   4. The client still BUILT it  -- the launcher was refused `worker-compile-killed`,
#                                    compiled locally, exited 0, and its object is this
#                                    machine's reference object byte for byte.
#   5. The node SURVIVED          -- the ceiling sits on `compiles/`, never on the node.
#
# And the control, a second node with the ceiling at 4g: the same construction is
# DISPATCHED and succeeds there, the ceiling counter reads 0 (present, not absent) and the
# kernel counted no `oom_kill`. Without it, a node that killed every compile -- or a
# construction no compiler could finish -- would pass assertions 1 to 5.
#
# ## The construction, and its own positive control
#
# A binary fan-out of class templates, 2^17 instantiations deep: a few hundred bytes of
# text that a compiler needs hundreds of MiB to hold. Measured on Fedora 44 (clang 21,
# gcc 15): peak RSS 576 MiB under clang++ and 731 MiB under g++ at `-O1`, in about 3.5 s --
# over twice the 256 MiB ceiling and a sixth of the 4 GiB control. Those figures are not
# relied on: every reference compile below runs in a cgroup scope of its own and the
# fixture reads the KERNEL's `memory.peak` for it, refusing to judge the node unless the
# peak sits strictly between the two ceilings. A compiler that some day memoizes the
# fan-out away makes this fixture fail naming the construction, never the ceiling.
#
# ## Skip versus fail
#
# The ceiling needs a delegated cgroup with the memory controller (`Delegate=memory`), a
# kernel with `memory.peak` (5.19) and a glibc that spawns into a cgroup (2.39). Each one a
# host lacks is a SKIP (77) naming it -- PROBED in a scope of the same shape before any node
# starts, never inferred from the node's own complaint: a node that FAILED to apply a
# ceiling on a host that delegates is the regression this fixture exists for, and reading
# its warning as "this host cannot" would turn that into a skip. Once the probe passes,
# anything that misbehaves is a FAILURE.
#
# The memory-pressure governor (layer C) is turned off on both nodes
# (`--memory-pressure-yield=0`, `--memory-pressure-shed=0`): its subject is the machine,
# and a host busy with other builds could yield or shed this compile, which is a different
# kill than the one asserted here. Layer A (`oom_score_adj`) is in force and changes
# nothing: it orders victims for the GLOBAL killer, and this kill is the cgroup's.
#
# Usage:
#   compile-oom-e2e.sh --node <pace-node> --launcher <pace-cc> --compiler <cxx>
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a prerequisite was missing.

set -uo pipefail

node=""
launcher=""
compiler="c++"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        --launcher) launcher="$2"; shift 2 ;;
        --compiler) compiler="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

# The two ceilings, verbatim from the plan, and the bytes each node must report and write.
readonly KillLimit="256m"
readonly KillLimitBytes=$(( 256 * 1024 * 1024 ))
readonly ControlLimit="4g"
readonly ControlLimitBytes=$(( 4 * 1024 * 1024 * 1024 ))

# How deep the fan-out goes: 2^17 instantiations. See the header for what it costs.
readonly FanDepth=17

[[ -n "$node" && -x "$node" ]] || { echo "SKIP: pace-node not found: '$node'"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "SKIP: compiler not found: '$compiler'"; exit "$SKIP"; }

# --- the host prerequisites, each a SKIP naming itself -----------------------------------
[[ "$(uname -s)" == "Linux" ]] \
    || { echo "SKIP: the compile memory ceiling is a Linux cgroup, and this host is $(uname -s)"; exit "$SKIP"; }
systemd_run="$(command -v systemd-run 2>/dev/null || true)"
[[ -n "$systemd_run" ]] \
    || { echo "SKIP: systemd-run not found, so no delegated cgroup can be had"; exit "$SKIP"; }

# glibc 2.39 is the first that spawns a process straight into a cgroup (`clone3` with
# `CLONE_INTO_CGROUP` through `posix_spawn`); the node runs every compile uncapped without it.
libc="$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
libc_version="${libc#glibc }"
if [[ "$libc_version" =~ ^([0-9]+)\.([0-9]+) ]]; then
    if (( BASH_REMATCH[1] < 2 || (BASH_REMATCH[1] == 2 && BASH_REMATCH[2] < 39) )); then
        echo "SKIP: glibc ${libc_version} cannot start a process inside a cgroup (2.39 can)"
        exit "$SKIP"
    fi
else
    echo "SKIP: not a glibc host ('${libc}'), so a compile cannot be started inside a cgroup"
    exit "$SKIP"
fi

# The delegation probe: a scope of the SAME shape the node runs in, asked what the node will
# need of it. It prints `delegated` or the first thing it found missing.
delegation_probe='
cg="$(sed -n "s/^0:://p" /proc/self/cgroup)"
dir="/sys/fs/cgroup${cg}"
[ -n "$cg" ] && [ -f "${dir}/cgroup.controllers" ] || { echo "no cgroup v2 hierarchy at /sys/fs/cgroup"; exit 0; }
grep -qw memory "${dir}/cgroup.controllers" || { echo "the scope was handed no memory controller"; exit 0; }
[ -w "${dir}/cgroup.procs" ] && [ -w "${dir}/cgroup.subtree_control" ] || { echo "the scope is not writable by this user"; exit 0; }
[ -e "${dir}/memory.peak" ] || { echo "this kernel has no memory.peak (5.19 has)"; exit 0; }
echo delegated
'
#
# On STDIN, never as an argument: `systemd-run` substitutes `$NAME` in the command line it is
# handed, so a script passed as `-c` text arrives with every variable already expanded to
# nothing. No argument this fixture hands it carries a `$`.
probe_said="$("$systemd_run" --user --scope -p Delegate=memory -q "$BASH" -s 2>&1 <<< "$delegation_probe")" || true
[[ "$probe_said" == "delegated" ]] \
    || { echo "SKIP: no delegated memory cgroup for this user: ${probe_said}"; exit "$SKIP"; }

workdir="$(mktemp -d)"

cleanup() {
    # A node is a job of this shell: `systemd-run --scope` execs it in place, so `jobs -pr`
    # names the node itself, and its scope goes when it does.
    reap_background_jobs 5
    rm -rf "$workdir"
    e2e_exit_if_reap_left_survivors
}
trap cleanup EXIT

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "compile-oom E2E" "$workdir"

# A node walks the compiler's include tree to fingerprint it before it registers, and its
# consensus has to record it first: seconds on a warm host, and nobody has measured a cold
# one here, so the bound is generous rather than argued.
e2e_wait_seconds 120

[[ -n "$launcher" && -x "$launcher" ]] || { echo "SKIP: pace-cc not found: '$launcher'"; exit "$SKIP"; }
e2e_launcher_state_enter launcher
# And the node keeps its state -- the fingerprint cache its worker walks into -- in a root of the run's
# own, never the caller's (#1774), as every fixture starting a worker does (`node-fixture-starts`).
e2e_node_state_enter node
export PACE_VERBOSE=1

e2e_note "mode: REAL delegated cgroup via ${systemd_run} --user --scope -p Delegate=memory"
e2e_note "compiler: ${compiler}; ${libc}; kernel $(uname -r)"

dump_logs() {
    local log=""
    for log in "$workdir"/*.log; do
        [[ -f "$log" ]] || continue
        echo "--- ${log##*/} ---" >&2
        cat "$log" >&2
    done
    return 0
}
e2e_on_fail dump_logs

proj="${workdir}/proj"
mkdir -p "${proj}/build"
export PACE_SOURCE_DIR="$proj"
export PACE_BINARY_DIR="${proj}/build"

fingerprint="$("$launcher" --print-toolchain-fingerprint "$compiler")" \
    || fail "the launcher could not compute a toolchain fingerprint for ${compiler}"
[[ -n "$fingerprint" ]] || fail "the launcher reported an empty toolchain fingerprint"

# One slot above the core count, for `dist-compile-e2e.sh`'s `worker_slots` reason: a host
# running other builds beside this one must not withdraw the only worker.
host_cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
[[ "$host_cores" =~ ^[1-9][0-9]*$ ]] || fail "could not read this host's online core count ('${host_cores}')"
worker_slots=$(( host_cores + 1 ))

# --- helpers --------------------------------------------------------------------------

# Write the fan-out translation unit, unique to the caller so no case is served what
# another stored.
# @param 1 path
# @param 2 token
write_fan_source() {
    cat > "$1" <<EOF
// unique: $2
template <int Depth, long Index>
struct Fan_$2
{
    static constexpr long value = Fan_$2<Depth - 1, 2 * Index>::value + Fan_$2<Depth - 1, 2 * Index + 1>::value;
};

template <long Index>
struct Fan_$2<0, Index>
{
    static constexpr long value = Index;
};

long Entry_$2() { return Fan_$2<${FanDepth}, 0>::value; }
EOF
}

# What runs inside the reference compile's scope: the compile its arguments spell, then the
# scope's own `memory.peak`. On stdin, for the delegation probe's reason.
readonly ReferenceScopeScript='"$@" >&2 || exit $?
cat "/sys/fs/cgroup$(sed -n "s/^0:://p" /proc/self/cgroup)/memory.peak"'

# Compile @p 1 to @p 2 with this machine's compiler, no launcher, in a cgroup scope of its
# own, and set `reference_peak` to the kernel's `memory.peak` for it: the positive control
# that the construction exceeds the kill ceiling and fits under the control's.
# @param 1 source
# @param 2 object
reference_compile() {
    local said=""
    said="$("$systemd_run" --user --scope -q "$BASH" -s -- "$compiler" -std=c++17 -O1 -c "$1" -o "$2" \
        2>"${workdir}/reference.err" <<< "$ReferenceScopeScript")" \
        || { cat "${workdir}/reference.err" >&2; fail "the reference compile of $1 failed"; }
    [[ "$said" =~ ^[0-9]+$ ]] || fail "the reference compile's scope reported no memory.peak: '${said}'"
    reference_peak="$said"
    (( reference_peak > KillLimitBytes )) \
        || fail "the construction peaked at ${reference_peak} bytes, not above the ${KillLimit} ceiling, so no kill can be asserted: deepen FanDepth"
    (( reference_peak < ControlLimitBytes )) \
        || fail "the construction peaked at ${reference_peak} bytes, not below the ${ControlLimit} control, so the control cannot succeed: shallow FanDepth"
    e2e_note "reference compile of ${1##*/} peaked at $(( reference_peak / 1024 / 1024 )) MiB (kernel memory.peak)"
}

# Whether the node in @p 1's log serves the launcher's fingerprint AND a heartbeat round was
# accepted since it began to. A first survey may file the compiler under another digest and
# re-survey (`the toolchain capture walk filed a served compiler under another fingerprint`);
# a registration of THAT digest would match no lease this launcher asks for, so the wait is
# for the round after the last fingerprint change, not for any accepted round.
node_registered_launcher_fingerprint() {
    awk -v fp="$fingerprint" -v mark="$E2eRegisteredMarker" '
        / serving .* as [0-9a-f]+$/ { if ($NF == fp) { if (!ok) { ok = 1; reg = 0 } } else { ok = 0; reg = 0 } next }
        ok && index($0, mark) { reg = 1 }
        END { exit !(ok && reg) }
    ' "$node_log"
}

# Whether the node in `node_log` has said what it did about the ceiling, either way.
node_said_ceiling() {
    grep -q "compile memory ceiling: \|compiles run with no memory ceiling" "$node_log" 2>/dev/null
}

# The node's `compiles/` cgroup: a sibling of the leaf `node/` it moved itself into.
# @param 1 the node's pid
compiles_cgroup_of() {
    local cg=""
    cg="$(sed -n 's/^0:://p' "/proc/$1/cgroup" 2>/dev/null)"
    [[ "$cg" == */node ]] || fail "the node did not move itself into a leaf node/ cgroup: it is in '${cg}'"
    printf '/sys/fs/cgroup%s/compiles' "${cg%/node}"
}

# One field of a cgroup's `memory.events`.
# @param 1 the cgroup directory
# @param 2 the field
memory_event() {
    awk -v field="$2" '$1 == field { print $2 }' "$1/memory.events"
}

# Start one node under a delegated scope with the given ceiling, and wait until it is
# serving, capped, and registered with the launcher's fingerprint. Sets `node_pid`,
# `node_log`, `node_port`, `admin_port` and `compiles_cg`.
# @param 1 tag: the log's name and every message's
# @param 2 the `--compile-memory-limit` value
# @param 3 the bytes that value is
start_capped_node() {
    local tag="$1" limit="$2" bytes="$3" max=""
    node_port="$(free_port)"
    admin_port="$(free_port)"
    local raft_port
    raft_port="$(free_port)"
    node_log="${workdir}/${tag}.log"
    : > "$node_log"
    # A cache tier of its own, so the client's exchange is the production one: fetch, the
    # fleet queue's admission, the dispatch, and the store of what was built.
    "$systemd_run" --user --scope -p Delegate=memory -q \
        "$node" \
            --drain-timeout=5s \
            --listen-node="127.0.0.1:${node_port}" --advertise="127.0.0.1:${node_port}" \
            --listen-raft="127.0.0.1:${raft_port}" --raft-self=127.0.0.1 --discovery= \
            --cluster-dir="${workdir}/${tag}.state" \
            --cache-memory=64m \
            --fleet-open \
            --admin-listen="$admin_port" \
            --toolchain="$compiler" --slots="$worker_slots" \
            --compile-memory-limit="$limit" \
            --memory-pressure-yield=0 --memory-pressure-shed=0 \
            --log-level=debug \
        >> "$node_log" 2>&1 &
    node_pid=$!
    wait_for_node_ready 127.0.0.1 "$node_port" "$node_pid" "$tag" "$node_log"

    # 1. The ceiling is in force, said AND written. The probe above proved this host
    #    delegates, so a node that runs uncapped here is the defect, never a skip.
    wait_until node_said_ceiling "${tag} to say whether its ceiling is in force" "$node_pid" "$node_log" 30
    grep -qF "compile memory ceiling: ${bytes} (typed)" "$node_log" \
        || fail "the ${tag} node did not put its ${limit} ceiling in force on a host that delegates memory: $(grep -m1 'memory ceiling' "$node_log")"
    compiles_cg="$(compiles_cgroup_of "$node_pid")"
    max="$(cat "${compiles_cg}/memory.max" 2>/dev/null || true)"
    [[ "$max" == "$bytes" ]] \
        || fail "the ${tag} node said its ceiling is ${bytes} but ${compiles_cg}/memory.max reads '${max}'"
    e2e_note "${tag}: ${compiles_cg##*/user@*.service/}/memory.max = ${max}"

    wait_until node_registered_launcher_fingerprint \
        "${tag} to register ${compiler} under the launcher's fingerprint ${fingerprint}" \
        "$node_pid" "$node_log" "$_e2e_wait_seconds"
}

# Read one counter off the current node's /metrics, failing when it is absent.
# @param 1 the series
read_counter() {
    local body="" status=0 value=""
    body="$(http_get 127.0.0.1 "$admin_port" /metrics)" || status=$?
    [ "$status" -eq "$E2eHttpAnswered" ] || fail "$(e2e_http_outcome "$status" "the node's /metrics on ${admin_port}")"
    value="$(metric_value "$body" "$1")"
    [[ -n "$value" ]] || fail "the node exports no $1 series"
    printf '%s' "$value"
}

# --- the kill: a compile over 256m is killed by the kernel and rebuilt locally ---------
echo "== case 1: a compile over a ${KillLimit} ceiling is killed and rebuilt locally"
write_fan_source "${proj}/killed.cpp" "killed"
reference_compile "${proj}/killed.cpp" "${proj}/build/killed-reference.o"

start_capped_node "kill-node" "$KillLimit" "$KillLimitBytes"
kill_cg="$compiles_cg"
ooms_before="$(memory_event "$kill_cg" oom_kill)"
[[ "$ooms_before" =~ ^[0-9]+$ ]] || fail "${kill_cg}/memory.events carries no oom_kill count"

export PACE_ADDR="127.0.0.1:${node_port}"
export PACE_SCHEDULER="127.0.0.1:${node_port}"
"$launcher" "$compiler" -std=c++17 -O1 -c "${proj}/killed.cpp" -o "${proj}/build/killed.o" \
    > "${workdir}/kill-client.log" 2>&1 \
    || fail "the client's build FAILED, where a killed dispatch must fall back to a local compile"

# 4. Refused as a kill BY THE CEILING, and rebuilt locally.
grep -qF "refused the job: rejected (worker-compile-killed)" "${workdir}/kill-client.log" \
    || fail "the client was not refused worker-compile-killed: the compile was not dispatched, or not killed"
grep -qF "was killed (ceiling)" "${workdir}/kill-client.log" \
    || fail "the client's refusal does not name the ceiling as the cause"
grep -qF "compiling locally" "${workdir}/kill-client.log" \
    || fail "the client did not say it compiled locally after the kill"
cmp -s "${proj}/build/killed-reference.o" "${proj}/build/killed.o" \
    || fail "the locally rebuilt object differs from this machine's reference object"

# 5. The node survived the kill of its compile.
kill -0 "$node_pid" 2>/dev/null || fail "the node died with its compile: the ceiling reached the node"

# 3. Classified a ceiling kill, and only that.
wait_for_counter 127.0.0.1 "$admin_port" pace_worker_compiles_killed_ceiling_total 1 "$node_pid" "kill-node" "$node_log"
killed_ceiling="$(read_counter pace_worker_compiles_killed_ceiling_total)"
killed_shed="$(read_counter pace_worker_compiles_killed_shed_total)"
killed_signal="$(read_counter pace_worker_compiles_killed_signal_total)"
[[ "$killed_ceiling" == "1" ]] || fail "pace_worker_compiles_killed_ceiling_total is ${killed_ceiling}, expected 1"
[[ "$killed_shed" == "0" && "$killed_signal" == "0" ]] \
    || fail "the kill was counted under another cause too: shed=${killed_shed} signal=${killed_signal}"
grep -qF "was killed (ceiling)" "$node_log" || fail "the node log does not name the ceiling kill"

# 2. The kernel's own account: an oom_kill inside compiles/.
ooms_after="$(memory_event "$kill_cg" oom_kill)"
(( ooms_after > ooms_before )) \
    || fail "${kill_cg}/memory.events counted no oom_kill (${ooms_before} -> ${ooms_after}): the kernel did not kill this compile"
e2e_note "killed by the kernel (oom_kill ${ooms_before} -> ${ooms_after}), counted ceiling=1 shed=0 signal=0, rebuilt locally, node alive"

stop_and_require_exit "$node_pid" "the kill node" 10

# --- the control: the same construction under 4g is dispatched and succeeds ------------
echo "== case 2: the same construction under a ${ControlLimit} ceiling is dispatched and succeeds"
write_fan_source "${proj}/control.cpp" "control"
reference_compile "${proj}/control.cpp" "${proj}/build/control-reference.o"

start_capped_node "control-node" "$ControlLimit" "$ControlLimitBytes"
control_cg="$compiles_cg"
control_ooms_before="$(memory_event "$control_cg" oom_kill)"
[[ "$control_ooms_before" =~ ^[0-9]+$ ]] || fail "${control_cg}/memory.events carries no oom_kill count"

export PACE_ADDR="127.0.0.1:${node_port}"
export PACE_SCHEDULER="127.0.0.1:${node_port}"
"$launcher" "$compiler" -std=c++17 -O1 -c "${proj}/control.cpp" -o "${proj}/build/control.o" \
    > "${workdir}/control-client.log" 2>&1 \
    || fail "the client's build failed under the ${ControlLimit} control"
grep -qF "DISPATCHED to " "${workdir}/control-client.log" \
    || fail "the control compile was not dispatched, so it says nothing about the ceiling"
cmp -s "${proj}/build/control-reference.o" "${proj}/build/control.o" \
    || fail "the dispatched object differs from this machine's reference object"
kill -0 "$node_pid" 2>/dev/null || fail "the control node died"

control_ceiling="$(read_counter pace_worker_compiles_killed_ceiling_total)"
[[ "$control_ceiling" == "0" ]] || fail "pace_worker_compiles_killed_ceiling_total is ${control_ceiling} under the ${ControlLimit} control, expected 0"
control_ooms_after="$(memory_event "$control_cg" oom_kill)"
[[ "$control_ooms_after" == "$control_ooms_before" ]] \
    || fail "the kernel killed a compile under the ${ControlLimit} control (oom_kill ${control_ooms_before} -> ${control_ooms_after})"
e2e_note "dispatched and built under ${ControlLimit}, ceiling counter 0, no oom_kill"

stop_and_require_exit "$node_pid" "the control node" 10

e2e_launcher_state_assert_used

echo "compile-oom E2E PASSED"
