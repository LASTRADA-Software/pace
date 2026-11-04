#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The MCP enrollment tools against a REAL leader and REAL joiners (POSIX). The unit tests drive
# `enrollment_approve` and `enrollment_reject` over a scripted upstream; only this proves that the
# leader's row, read back after the decision, is rendered as the `decision` field -- and that a
# machine the fleet admitted as a learner is refused the control verbs BY NAME, from its own MCP.
#
#   1. A joiner is listed       — two nodes started with `--fleet-seed` at the leader each ask to
#                                 enrol; the leader's `enrollment_status` lists both, `pending`,
#                                 with the key each printed at its start.
#   2. approve                  — `enrollment_approve` of the first under that key answers
#                                 `decision: approved`; the joiner applies the fleet's state and
#                                 counts itself a learner.
#   3. reject                   — `enrollment_reject` of the second answers `decision: rejected`,
#                                 and `enrollment_status` then reads one approved, one rejected.
#   4. A learner is refused     — `enrollment_approve`, `enrollment_reject` and
#                                 `enrollment_auto_approve` on the LEARNER's own MCP surface are
#                                 tool errors naming that it lacks operator standing, with the
#                                 remedy -- and its `enrollment_status` is refused by the leader as
#                                 `operator-standing-required`, a control verb's refusal.
#
# **It needs an address other machines could reach.** A fleet whose consensus address reaches
# only this machine offers itself to no other (`FleetSummaryResponder`), so a joiner on loopback
# never finds it. The nodes bind this host's primary IPv4 address -- the source address the kernel
# picks for a route off the machine, asked of a UDP socket that sends nothing -- and a host with
# none skips (77), saying so. MCP stays on loopback, as it must.
#
# Registered in `src/tests/CMakeLists.txt`, beside `node-mcp-e2e.sh`.
#
# Usage:
#   node-mcp-enrollment-e2e.sh --node <pace-node>

set -uo pipefail

node=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
[[ -n "$node" && -x "$node" ]] || { echo "pace-node not found: '$node'; skipping"; exit "$SKIP"; }
command -v curl >/dev/null 2>&1 || { echo "curl not found; skipping"; exit "$SKIP"; }
command -v python3 >/dev/null 2>&1 || { echo "python3 not found, so no JSON parser to judge a body with; skipping"; exit "$SKIP"; }

# 192.0.2.1 is TEST-NET-1: routed nowhere, and a UDP connect sends nothing.
host_ip="$(python3 -I -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.connect(('192.0.2.1', 9))
    print(s.getsockname()[0])
except OSError:
    pass
" 2>/dev/null)"
case "$host_ip" in
    "" | 127.* | 0.0.0.0) echo "this host has no IPv4 address off loopback ('${host_ip}'), and a fleet on loopback admits no joiner; skipping"; exit "$SKIP" ;;
esac

workdir="$(mktemp -d)"
leader_pid=""
approved_pid=""
rejected_pid=""

cleanup() {
    reap_background_jobs
    rm -rf "$workdir"
    e2e_exit_if_reap_left_survivors
}
trap cleanup EXIT

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "node MCP enrollment E2E" "$workdir"

dump_logs() {
    local log
    for log in leader approved rejected; do
        [[ -f "${workdir}/${log}.log" ]] && { echo "--- ${log} log ---" >&2; cat "${workdir}/${log}.log" >&2; }
    done
    return 0
}
e2e_on_fail dump_logs
e2e_wait_seconds 120
e2e_note "nodes bind ${host_ip}"

readonly MODERN="2026-07-28"
readonly META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}'

# Start one node on this host's address, MCP on loopback.
# @param 1 its name, for its state directory and log
# @param 2 its 0xFC port
# @param 3 its consensus port
# @param 4 its MCP port
# @param 5.. extra flags
start_node() {
    local name="$1" node_port="$2" raft_port="$3" mcp_port="$4"
    shift 4
    "$node" \
        --slots=0 \
        --advertise "${host_ip}:${node_port}" \
        --listen-node "${host_ip}:${node_port}" \
        --listen-raft "${host_ip}:${raft_port}" \
        --raft-self "$host_ip" \
        --discovery= \
        --cluster-dir "${workdir}/${name}" \
        --mcp-listen "$mcp_port" \
        "$@" \
        > "${workdir}/${name}.log" 2>&1 &
    started_pid=$!
}

# Call one tool and echo the BODY; fails the run on anything but a 200.
# @param 1 the MCP port
# @param 2 the tool
# @param 3 its arguments, as JSON
call_tool() {
    local port="$1" tool="$2" arguments="$3" response
    response="$(curl -s -i -m 20 -X POST -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
        -H "Mcp-Method: tools/call" -H "Mcp-Name: ${tool}" \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"${tool}\",\"arguments\":${arguments},${META}}}" \
        "http://127.0.0.1:${port}/mcp")" \
        || fail "curl could not complete tools/call ${tool} ${arguments} on port ${port}"
    [[ "$response" == HTTP/1.1\ 200* ]] || fail "tools/call ${tool} ${arguments} did not answer 200: ${response%%$'\r'*}"
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

# The identity key a node printed at its start, which a person compares before approving.
# @param 1 its log
printed_key() {
    local line
    while IFS= read -r line; do
        case "$line" in
            *"identity key "*) line="${line#*identity key }"; printf '%s' "${line%% *}"; return 0 ;;
        esac
    done < "$1"
    return 1
}
printed_id() {
    local line
    while IFS= read -r line; do
        case "$line" in
            *"node identity "*) line="${line#*node identity }"; printf '%s' "${line%% *}"; return 0 ;;
        esac
    done < "$1"
    return 1
}

readonly SC="d['result']['structuredContent']"
readonly OK="d['result']['isError'] is False"

# ------------------------------------------------------------------- the leader
leader_node_port="$(free_port)"
leader_raft_port="$(free_port)"
leader_mcp_port="$(free_port)"
start_node leader "$leader_node_port" "$leader_raft_port" "$leader_mcp_port"
leader_pid="$started_pid"
wait_for_node_ready 127.0.0.1 "$leader_mcp_port" "$leader_pid" "the leader's MCP surface" "${workdir}/leader.log"
wait_for_log "consensus: this node is now the leader" "$leader_pid" "the leader" "${workdir}/leader.log"
wait_for_log "this node's own cluster has recorded it" "$leader_pid" "the leader" "${workdir}/leader.log"

# ------------------------------------------------------------------- 1. two joiners are listed
approved_mcp_port="$(free_port)"
start_node approved "$(free_port)" "$(free_port)" "$approved_mcp_port" --fleet-seed "${host_ip}:${leader_node_port}"
approved_pid="$started_pid"
rejected_mcp_port="$(free_port)"
start_node rejected "$(free_port)" "$(free_port)" "$rejected_mcp_port" --fleet-seed "${host_ip}:${leader_node_port}"
rejected_pid="$started_pid"
wait_for_node_ready 127.0.0.1 "$approved_mcp_port" "$approved_pid" "the first joiner's MCP surface" "${workdir}/approved.log"
wait_for_node_ready 127.0.0.1 "$rejected_mcp_port" "$rejected_pid" "the second joiner's MCP surface" "${workdir}/rejected.log"

approved_id="$(printed_id "${workdir}/approved.log")" || fail "the first joiner printed no node identity"
approved_key="$(printed_key "${workdir}/approved.log")" || fail "the first joiner printed no identity key"
rejected_id="$(printed_id "${workdir}/rejected.log")" || fail "the second joiner printed no node identity"

# Formation asks the seed on its own cadence; wait on the leader's LIST, which is the state.
both_listed() {
    local body
    body="$(call_tool "$leader_mcp_port" enrollment_status '{}')"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); ids=[p['node_id'] for p in d['result']['structuredContent']['pending']]; sys.exit(0 if d['result']['isError'] is False and '${approved_id}' in ids and '${rejected_id}' in ids else 1)" <<< "$body"
}
wait_until both_listed "the leader's enrollment_status to list both joiners" "$leader_pid" "${workdir}/leader.log" 120
json_check "enrollment_status with two joiners" "$(call_tool "$leader_mcp_port" enrollment_status '{}')" \
    "${OK} and ${SC}['enrollment'] == 'manual' and (lambda p: p['${approved_id}']['decision'] == 'pending' and p['${rejected_id}']['decision'] == 'pending' and p['${approved_id}']['public_key'] == '${approved_key}' and p['${approved_id}']['role'] == 'learner' and p['${approved_id}']['peer'] == '${host_ip}')({r['node_id']: r for r in ${SC}['pending']})"
e2e_note "the leader lists both joiners, pending, the first under the key it printed"

# ------------------------------------------------------------------- 2. approve
approved_body="$(call_tool "$leader_mcp_port" enrollment_approve "{\"node_id\":\"${approved_id}\",\"public_key\":\"${approved_key}\"}")"
json_check "enrollment_approve" "$approved_body" \
    "${OK} and ${SC}['node_id'] == '${approved_id}' and ${SC}['decision'] == 'approved' and ${SC}['enrollment'] == 'manual' and 'the leader\\'s row now reads approved' in d['result']['content'][0]['text']"
wait_for_log "it is a learner" "$approved_pid" "the approved joiner" "${workdir}/approved.log"
e2e_note "enrollment_approve answered decision approved, and the joiner counts itself a learner"

# ------------------------------------------------------------------- 3. reject
rejected_body="$(call_tool "$leader_mcp_port" enrollment_reject "{\"node_id\":\"${rejected_id}\"}")"
json_check "enrollment_reject" "$rejected_body" \
    "${OK} and ${SC}['node_id'] == '${rejected_id}' and ${SC}['decision'] == 'rejected' and 'the leader\\'s row now reads rejected' in d['result']['content'][0]['text']"
json_check "enrollment_status after the decisions" "$(call_tool "$leader_mcp_port" enrollment_status '{}')" \
    "${OK} and (lambda p: p['${approved_id}']['decision'] == 'approved' and p['${rejected_id}']['decision'] == 'rejected')({r['node_id']: r for r in ${SC}['pending']})"
e2e_note "enrollment_reject answered decision rejected; the list reads one approved, one rejected"

# ------------------------------------------------------------------- 4. a learner is refused
# The learner's own MCP surface, asking the leader as itself: refused for standing, BY NAME, with
# the remedy -- never "not a member" and never a silent success. Asked of a third machine's id, so
# a slip that let it through would decide nobody real.
# Exported, and read by the judge from its environment: the sentence carries an apostrophe.
export STANDING="${approved_id} lacks operator standing (it is not a voter); run it from a voter's machine, or promote this node"
for request in "enrollment_approve|{\"node_id\":\"${rejected_id}\",\"public_key\":\"${approved_key}\"}" \
               "enrollment_reject|{\"node_id\":\"${rejected_id}\"}" \
               "enrollment_auto_approve|{\"minutes\":1}"; do
    tool="${request%%|*}"
    json_check "${tool} from the learner" "$(call_tool "$approved_mcp_port" "$tool" "${request#*|}")" \
        "d['result']['isError'] is True and d['result']['content'][0]['text'] == __import__('os').environ['STANDING']"
done
json_check "enrollment_status from the learner" "$(call_tool "$approved_mcp_port" enrollment_status '{}')" \
    "d['result']['isError'] is True and 'operator-standing-required' in d['result']['content'][0]['text']"
# And nothing it asked moved the leader's list.
json_check "enrollment_status after the learner's attempts" "$(call_tool "$leader_mcp_port" enrollment_status '{}')" \
    "${OK} and ${SC}['enrollment'] == 'manual' and (lambda p: p['${approved_id}']['decision'] == 'approved' and p['${rejected_id}']['decision'] == 'rejected')({r['node_id']: r for r in ${SC}['pending']})"
e2e_note "the learner was refused approve, reject and auto-approve by name, and its enrollment_status as a control verb"

stop_and_require_clean_exit "$rejected_pid" "the rejected joiner" 30 "${workdir}/rejected.log"
rejected_pid=""
stop_and_require_clean_exit "$approved_pid" "the learner" 30 "${workdir}/approved.log"
approved_pid=""
stop_and_require_clean_exit "$leader_pid" "the leader" 30 "${workdir}/leader.log"
leader_pid=""

echo "node MCP enrollment E2E passed"
