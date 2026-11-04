#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Hostile input against the node's MCP surface (POSIX), and what it must NOT disturb. Starts a
# real pace-node with `--mcp-listen` beside `--admin-listen` and its cache tier, and sends the MCP
# port what a broken or malicious local client would:
#
#   1. A body past the ceiling   — 1 MiB, against `RequestParseLimits.maxBytes` (64 KiB): `413`,
#                                  refused before the body is buffered.
#   2. A nest past the depth     — a document 33 containers deep, against `maxDepth` (32): `400`
#                                  with JSON-RPC `-32700`; and the control, 32 deep, is answered
#                                  `200` -- so the refusal is the DEPTH, not the shape.
#   3. Bytes that are not UTF-8  — a string holding `0xFF 0xFE`: `400` with `-32700`.
#   4. A body that stops         — `Content-Length: 100`, ten bytes sent, then silence: answered
#                                  `408` and closed once `BodyTimeout` (2 s) passes, measured from
#                                  the client's own monotonic clock -- not sooner, and not never.
#   5. Twenty idle connects      — twenty sockets that send nothing, held open by the client: an MCP
#                                  request made at once is ANSWERED -- `503` past the four slots, or
#                                  `200` on a freed one -- inside `ProbeBound`, MCP answers `200`
#                                  inside `RecoveryBound` while the twenty are STILL held, the server
#                                  has ended all twenty itself (a `503` past the cap, or a silent
#                                  close after MCP's two-second first-byte bound), and the other
#                                  surfaces answer throughout.
#
# Throughout -- after every case, and DURING the two that hold a connection -- `/healthz` on
# `--admin-listen` answers `200` inside `HealthzBound`, and a `pace-cc` compile through the node's
# cache tier is a HIT: the FETCH reached the node and was answered, since a starved tier would have
# timed out into a local compile. The node is alive at the end and stops cleanly.
#
# Case 5 is the one that was red in fact before MCP served its connections on threads of its own:
# one blocking thread, thirty seconds per silent peer, and 614 s measured for twenty (Task 21).
#
# Registered in `src/tests/CMakeLists.txt`: it names two executables (`pace-node`, `pace-cc`).
#
# Usage:
#   node-mcp-hostile-e2e.sh --node <pace-node> --launcher <pace-cc> [--compiler <cxx>]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime prerequisite was missing.

set -uo pipefail

node=""
launcher=""
compiler=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        --launcher) launcher="$2"; shift 2 ;;
        --compiler) compiler="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
[[ -n "$node" && -x "$node" ]] || { echo "pace-node not found: '$node'; skipping"; exit "$SKIP"; }
command -v curl >/dev/null 2>&1 || { echo "curl not found; skipping"; exit "$SKIP"; }
# The hostile clients need raw sockets and a monotonic clock, and the bodies need a JSON judge.
command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping"; exit "$SKIP"; }
if [[ -z "$compiler" ]]; then
    compiler="$(command -v c++ || command -v g++ || command -v clang++ || true)"
fi
# The FETCH is the subject of every "the cache tier still answers" check, and a compile is what
# asks it; without a compiler there is nothing to ask with.
[[ -n "$compiler" && -x "$compiler" ]] || { echo "no C++ compiler found; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
node_pid=""

cleanup() {
    reap_background_jobs
    rm -rf "$workdir"
    e2e_exit_if_reap_left_survivors
}
trap cleanup EXIT

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "node MCP hostile E2E" "$workdir"
# After `e2e_begin`, whose snapshot of the caller's launcher statistics must precede any line naming
# the launcher.
[[ -n "$launcher" && -x "$launcher" ]] || { echo "pace-cc not found: '$launcher'; skipping"; exit "$SKIP"; }

dump_node_log() {
    [[ -f "${workdir}/node.log" ]] && { echo "--- node log ---" >&2; cat "${workdir}/node.log" >&2; }
    return 0
}
e2e_on_fail dump_node_log
e2e_wait_seconds 120

export PACE_VERBOSE=1
e2e_launcher_state_enter launcher

# ------------------------------------------------------------------- the bounds, each derived
# `/healthz` is answered from memory on the admin surface's OWN thread. A reply behind a held MCP
# connection would take at least one `AdminHttpServer::RequestTimeout` (2 s), so a bound under
# that tells "answered" from "answered once something else let go". Measured at about a
# millisecond; one second is that with three orders of magnitude for a loaded machine.
readonly HealthzBound=1.0
# `AdminHttpServer::BodyTimeout` (2 s): a body that stops is refused when it passes. The lower
# bound is that less half a second (the client starts its clock after the server armed the read,
# so it can only read LONGER); the upper adds the linger (`Linger.total`, 2 s) and six seconds of
# scheduling -- far below the 30 s a never-armed deadline would show as.
readonly TruncatedAtLeast=1.5
readonly TruncatedAtMost=10.0
# Twenty idle sockets against `McpEndpoint::MaxConcurrentRequests` (4) slots. Four take a slot each
# and hold it for MCP's first-byte bound (`McpEndpoint::Deadlines.firstByte`, 2 s); the other sixteen
# are answered `503` on the accept thread, each lingering at most a quarter of the 400 ms linger in
# silence -- 1.6 s for all of them. So a request made at once is answered by about 2 s, `503` or `200`
# by which came first: `ProbeBound` is that with three seconds of scheduling. A `200` follows once the
# four held slots are freed by their own bound, at about 2 s: `RecoveryBound` adds eight. Served one at
# a time instead, twenty silent peers cost twenty first-byte bounds -- forty seconds at 2 s each, and
# the 614 s measured at the dashboard's 30 s -- so either bound tells the two apart.
readonly IdleConnects=20
readonly ProbeBound=5
readonly RecoveryBound=10

node_port="$(free_port)"
mcp_port="$(free_port)"
admin_port="$(free_port)"

# A node that serves a private cache tier, MCP and the admin surface: no worker, no consensus, no
# discovery. Its own state directory.
"$node" \
    --slots=0 \
    --cache-memory=64m \
    --listen-node "127.0.0.1:${node_port}" \
    --listen-raft= \
    --discovery= \
    --cluster-dir "${workdir}/state" \
    --mcp-listen "$mcp_port" \
    --admin-listen "$admin_port" \
    > "${workdir}/node.log" 2>&1 &
node_pid=$!
wait_for_node_ready 127.0.0.1 "$mcp_port" "$node_pid" "the node's MCP surface" "${workdir}/node.log"
# Every compile below asks the node's own tier, and nothing dispatches.
export PACE_ADDR="127.0.0.1:${node_port}"
unset PACE_SCHEDULER

readonly MODERN="2026-07-28"
readonly META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}'

# POST a body file to the MCP surface as a modern `tools/list`; echoes the response, head included.
# @param 1 the file holding the body
mcp_post_file() {
    curl -s -i -m 20 -X POST -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
        -H "Mcp-Method: tools/list" --data-binary @"$1" "http://127.0.0.1:${mcp_port}/mcp" \
        || fail "curl could not complete a POST of ${1##*/} against the MCP surface (status $?)"
}

body_of() {
    local response="$1"
    printf '%s' "${response#*$'\r\n\r\n'}"
}

# @param 1 what is being checked
# @param 2 the body
# @param 3 a Python expression over `d`
json_check() {
    local what="$1" body="$2" predicate="$3" status=0
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if (${predicate}) else 3)" \
        <<< "$body" || status=$?
    case "$status" in
        0) ;;
        3) fail "${what}: the body parsed and did not satisfy ${predicate}: ${body}" ;;
        *) fail "${what}: the body is not JSON (python3 exited ${status}): ${body}" ;;
    esac
}

# A strict "less than" over two decimals, in Python rather than `bc`, which is not everywhere.
# @param 1 the measured value
# @param 2 the bound
below() {
    python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) < float(sys.argv[2]) else 1)" "$1" "$2"
}

# The MCP surface answers a modern `server/discover`, which is what "MCP answers again" means.
mcp_answers() {
    local response
    response="$(curl -s -i -m 5 -X POST -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
        -H "Mcp-Method: server/discover" \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{${META}}}" \
        "http://127.0.0.1:${mcp_port}/mcp")" || return 1
    [[ "$response" == HTTP/1.1\ 200* ]]
}

# `/healthz` on the admin surface answers 200 inside `HealthzBound`, timed by curl's own clock.
# @param 1 when, for the messages
healthz_answers() {
    local when="$1" out status took
    out="$(curl -s -o /dev/null -m 10 -w '%{http_code} %{time_total}' "http://127.0.0.1:${admin_port}/healthz")" \
        || fail "/healthz did not answer ${when} (curl status $?)"
    status="${out%% *}"
    took="${out#* }"
    [[ "$status" == 200 ]] || fail "/healthz answered ${status} ${when}"
    below "$took" "$HealthzBound" || fail "/healthz took ${took}s ${when}, against a ${HealthzBound}s bound"
    e2e_note "/healthz answered 200 in ${took}s ${when}"
}

# A compile through `pace-cc` is a HIT from the node's tier: the FETCH reached the node and was
# answered. Bounded, so a wedged launcher ends the run with a reason rather than at ctest's timeout.
# @param 1 when, for the messages
proj="${workdir}/proj"
mkdir -p "${proj}/build"
printf 'int hostile_e2e(int x) { return x * 3 + 1; }\n' > "${proj}/tu.cpp"
export PACE_SOURCE_DIR="$proj"
export PACE_BINARY_DIR="${proj}/build"
compile_once() {
    local log="$1"
    rm -f "${proj}/build/tu.o"
    run_bounded 60 "$launcher" "$compiler" -c "${proj}/tu.cpp" -o "${proj}/build/tu.o" \
        > "$log" 2>&1
}
fetch_hits() {
    local when="$1" log="${workdir}/fetch.log"
    compile_once "$log" || { cat "$log" >&2; fail "the compile through pace-cc failed ${when}"; }
    # The launcher's outcome line; a manifest or object exchange alone is not one.
    grep -q "^pace-cc: HIT " "$log" \
        || { cat "$log" >&2; fail "the compile through the node's tier was not a HIT ${when}"; }
    e2e_note "pace-cc's FETCH was answered from the node's tier (HIT) ${when}"
}

surfaces_answer() {
    healthz_answers "$1"
    fetch_hits "$1"
}

# The object the HITs below are served: stored by a first compile.
compile_once "${workdir}/store.log" || { cat "${workdir}/store.log" >&2; fail "the first compile through pace-cc failed"; }
grep -q "^pace-cc: MISS" "${workdir}/store.log" || { cat "${workdir}/store.log" >&2; fail "the first compile through the node's tier was not a MISS"; }
surfaces_answer "before any hostile input"
mcp_answers || fail "the MCP surface did not answer server/discover before any hostile input"

# ------------------------------------------------------------- 1. a body past the ceiling
python3 -I -c "import sys; sys.stdout.write('{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"pad\":\"' + 'a' * (1024 * 1024) + '\"}}')" \
    > "${workdir}/big.json" || fail "could not write the 1 MiB body"
big="$(mcp_post_file "${workdir}/big.json")"
[[ "$big" == HTTP/1.1\ 413* ]] || fail "a 1 MiB body was not answered 413: ${big%%$'\r'*}"
e2e_note "a 1 MiB body was answered 413"
surfaces_answer "after the 1 MiB body"

# ------------------------------------------------------------- 2. a nest past the depth
# The envelope and `params` are two containers, so 31 arrays make 33 and 30 make 32 -- the deepest
# the parser takes. The control is a tools/list that carries an argument it ignores.
# @param 1 the file
# @param 2 how many arrays
nested_body() {
    python3 -I -c "import sys; n=int(sys.argv[1]); sys.stdout.write('{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{${META},\"x\":' + '[' * n + ']' * n + '}}')" "$2" \
        > "$1" || fail "could not write a body nested ${2} arrays deep"
}
nested_body "${workdir}/deep33.json" 31
nested_body "${workdir}/deep32.json" 30
deep="$(mcp_post_file "${workdir}/deep33.json")"
[[ "$deep" == HTTP/1.1\ 400* ]] || fail "a body 33 containers deep was not answered 400: ${deep%%$'\r'*}"
json_check "a body 33 containers deep" "$(body_of "$deep")" "d['error']['code'] == -32700"
control="$(mcp_post_file "${workdir}/deep32.json")"
[[ "$control" == HTTP/1.1\ 200* ]] || fail "the control, 32 containers deep, was not answered 200: ${control%%$'\r'*}"
json_check "a body 32 containers deep" "$(body_of "$control")" "isinstance(d['result']['tools'], list)"
e2e_note "a body 33 deep was answered 400/-32700, and one 32 deep was answered"
surfaces_answer "after the deep nest"

# ------------------------------------------------------------- 3. bytes that are not UTF-8
printf '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{%s,"x":"\xff\xfe"}}' "$META" > "${workdir}/utf8.json" \
    || fail "could not write the non-UTF-8 body"
utf8="$(mcp_post_file "${workdir}/utf8.json")"
[[ "$utf8" == HTTP/1.1\ 400* ]] || fail "a body that is not UTF-8 was not answered 400: ${utf8%%$'\r'*}"
json_check "a body that is not UTF-8" "$(body_of "$utf8")" "d['error']['code'] == -32700"
e2e_note "a body that is not UTF-8 was answered 400/-32700"
surfaces_answer "after the non-UTF-8 body"

# ------------------------------------------------------------- 4. a body that stops
# A command fork, not a subshell: it inherits no trap. It writes one line when the server ends
# the connection: the seconds it waited by `time.monotonic`, and the status line it was sent.
truncated_result="${workdir}/truncated.result"
python3 -I -c "
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
s.sendall(b'POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n0123456789')
start = time.monotonic()
s.settimeout(60)
got = b''
try:
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        got += chunk
    ended = 'closed'
except socket.timeout:
    ended = 'still-open'
line = got.split(b'\r\n', 1)[0].decode('latin-1') or '-'
open(sys.argv[2] + '.tmp', 'w').write('%s %.3f %s\n' % (ended, time.monotonic() - start, line))
import os; os.rename(sys.argv[2] + '.tmp', sys.argv[2])
" "$mcp_port" "$truncated_result" &
# While it is held: the admin surface is on its own thread.
healthz_answers "while a truncated body is held"
truncated_reported() { [[ -s "$truncated_result" ]]; }
wait_until truncated_reported "the truncated-body client to see the server end its connection" "$node_pid" "${workdir}/node.log" 70
read -r ended waited status_line < "$truncated_result"
[[ "$ended" == closed ]] || fail "the server held a truncated body's connection past the client's 60 s (${ended})"
below "$TruncatedAtLeast" "$waited" || fail "a truncated body was closed after ${waited}s -- sooner than BodyTimeout allows (at least ${TruncatedAtLeast}s)"
below "$waited" "$TruncatedAtMost" || fail "a truncated body was closed after ${waited}s, against a ${TruncatedAtMost}s bound"
[[ "$status_line" == "HTTP/1.1 408"* ]] || fail "a truncated body was answered '${status_line}', not 408"
e2e_note "a body that stopped at 10 of 100 bytes was answered 408 and closed after ${waited}s"
surfaces_answer "after the truncated body"
mcp_answers || fail "the MCP surface did not answer once the truncated body was closed"

# ------------------------------------------------------------- 5. twenty idle connects
# The holder opens the sockets, says so, and holds them until the release file appears (60 s at
# most, so a failed run cannot leave it holding). On release it reports what the SERVER did with each
# -- answered `503`, closed silently, or still holding it -- and then closes every one.
idle_opened="${workdir}/idle.opened"
idle_release="${workdir}/idle.release"
idle_result="${workdir}/idle.result"
python3 -I -c "
import os, socket, sys, time
port, n, opened, release, result = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
socks = [socket.create_connection(('127.0.0.1', port)) for _ in range(n)]
open(opened, 'w').write('%d\n' % n)
deadline = time.monotonic() + 60
while not os.path.exists(release) and time.monotonic() < deadline:
    time.sleep(0.05)
refused = silent = held = 0
for s in socks:
    s.setblocking(False)
    try:
        said = s.recv(64)
        if said.startswith(b'HTTP/1.1 503'):
            refused += 1
        elif said == b'':
            silent += 1
        else:
            held += 1
    except BlockingIOError:
        held += 1
    except OSError:
        silent += 1
for s in socks:
    s.close()
open(result + '.tmp', 'w').write('%d %d %d %s\n' % (refused, silent, held, 'released' if os.path.exists(release) else 'expired'))
os.rename(result + '.tmp', result)
" "$mcp_port" "$IdleConnects" "$idle_opened" "$idle_release" "$idle_result" &
idle_holding() { [[ -s "$idle_opened" ]]; }
wait_until idle_holding "the holder to open ${IdleConnects} idle connections" "$node_pid" "${workdir}/node.log" 30
# At once, while every slot can still be held: answered, by curl's own clock, and which answer is said.
probe="$(curl -s -o /dev/null -m 20 -w '%{http_code} %{time_total}' -X POST -H "Content-Type: application/json" \
    -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
    -H "Mcp-Method: server/discover" \
    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{${META}}}" \
    "http://127.0.0.1:${mcp_port}/mcp")" || fail "an MCP request made while ${IdleConnects} idle connections were held was not answered (curl status $?)"
probe_status="${probe%% *}"
probe_took="${probe#* }"
case "$probe_status" in
    200 | 503) ;;
    *) fail "an MCP request made while ${IdleConnects} idle connections were held was answered ${probe_status}, neither 200 nor 503" ;;
esac
below "$probe_took" "$ProbeBound" \
    || fail "an MCP request made while ${IdleConnects} idle connections were held took ${probe_took}s to be answered (${probe_status}), against a ${ProbeBound}s bound"
e2e_note "an MCP request made while ${IdleConnects} idle connections were held was answered ${probe_status} in ${probe_took}s"
surfaces_answer "while ${IdleConnects} idle connections are held"
# And 200, with every one of the twenty still open on the client's side: their slots are freed by the server.
wait_until mcp_answers "the MCP surface to answer 200 while ${IdleConnects} idle connections are still held" "$node_pid" "${workdir}/node.log" "$RecoveryBound"
: > "$idle_release"
idle_reported() { [[ -s "$idle_result" ]]; }
wait_until idle_reported "the holder to release its ${IdleConnects} connections" "$node_pid" "${workdir}/node.log" 30
read -r idle_refused idle_silent idle_held idle_how < "$idle_result"
[[ "$idle_how" == released ]] || fail "the idle holder expired before the checks released it"
[[ "$idle_held" -eq 0 ]] \
    || fail "the server still held ${idle_held} of ${IdleConnects} idle connections once MCP answered again (${idle_refused} answered 503, ${idle_silent} closed silently)"
[[ "$idle_refused" -ge 1 ]] \
    || fail "none of ${IdleConnects} idle connections was answered 503, so the cap of four refused nothing (${idle_silent} closed silently)"
e2e_note "of ${IdleConnects} idle connections the server answered ${idle_refused} with 503 and closed ${idle_silent} silently; it held none"
surfaces_answer "after the idle connections"

kill -0 "$node_pid" 2>/dev/null || fail "the node is not running after the hostile input"
stop_and_require_clean_exit "$node_pid" "the node" 30 "${workdir}/node.log"
node_pid=""

echo "node MCP hostile E2E passed"
