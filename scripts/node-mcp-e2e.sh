#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the node's MCP surface (POSIX). Starts a real pace-node
# with `--mcp-listen` and speaks MCP to it the way a client does, over curl:
#
#   1. The surface is actually served   — the unit tests drive `McpEndpoint`
#                                         over a listener the test bound; only
#                                         this proves `--mcp-listen` reaches a
#                                         bound port through `main`.
#   2. A modern client is answered      — `server/discover` names `2026-07-28`
#                                         among the supported versions, and
#                                         `tools/list` names the fleet tools;
#                                         both bodies parse as JSON.
#   2b. A fleet tool reaches the loop   — `fleet_overview` on a node that runs
#                                         no consensus is a tool error naming
#                                         `no-leader-known`: the call got PAST
#                                         the loop gate (the loop turns), and
#                                         the answer is an error, not a fleet.
#   2c. A fleet tool answers through    — on a one-node consensus scheduler, which leads
#       main's wiring                     alone, `fleet_overview` and `running_compiles`
#                                         answer with real content; run twice, the
#                                         second WITH `--dashboard-token-file`, which
#                                         proves the leader's call to itself is not
#                                         refused for want of the token.
#   3. A legacy client is answered      — `initialize` answers `2025-11-25`.
#   4. It is not a scrape endpoint      — `GET /metrics` on the MCP port is a 404.
#   5. A network address refuses        — `--mcp-listen=0.0.0.0:<port>` ends the
#                                         start, naming the rule.
#
# Registered in `src/tests/CMakeLists.txt`, for `fleet-dashboard-e2e.sh`'s reason.

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
# POST bodies and a JSON parse are the subject, and `/dev/tcp` offers neither cleanly.
command -v curl >/dev/null 2>&1 || { echo "curl not found; skipping"; exit "$SKIP"; }
command -v python3 >/dev/null 2>&1 || { echo "python3 not found, so no JSON parser to judge a body with; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
node_pid=""

cleanup() {
    reap_background_jobs
    rm -rf "$workdir"
    e2e_exit_if_reap_left_survivors
}
trap cleanup EXIT

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "node MCP E2E" "$workdir"

dump_node_log() {
    [[ -f "${workdir}/node.log" ]] && { echo "--- node log ---" >&2; cat "${workdir}/node.log" >&2; }
    return 0
}
e2e_on_fail dump_node_log

# No toolchain survey stands between the start and the bind here (`--slots=0` runs no
# worker), so this is generous rather than measured.
e2e_wait_seconds 120

node_port="$(free_port)"
mcp_port="$(free_port)"

# The smallest node that serves: no worker, no consensus, no discovery -- its private
# cache tier is what it runs, and MCP beside it. Its own state directory, so it never
# meets a node already running on this machine.
"$node" \
    --slots=0 \
    --listen-node "127.0.0.1:${node_port}" \
    --listen-raft= \
    --discovery= \
    --cluster-dir "${workdir}/state" \
    --mcp-listen "$mcp_port" \
    > "${workdir}/node.log" 2>&1 &
node_pid=$!

# The MCP endpoint is started before the node announces readiness, so the marker
# means its loop is accepting -- not merely that its port is bound.
wait_for_node_ready 127.0.0.1 "$mcp_port" "$node_pid" "the node's MCP surface" "${workdir}/node.log"

# POST one MCP request; echoes the whole response, head included.
# @param 1 the JSON-RPC method, mirrored into `Mcp-Method` for a modern request
# @param 2 the body
# @param 3 the `MCP-Protocol-Version` header value, empty for none
# @param 4 the tool name to mirror into `Mcp-Name` for a modern `tools/call`, empty for none
mcp_post() {
    local method="$1" body="$2" version="$3" name="${4:-}"
    local args=(-s -i -m 20 -X POST -H "Content-Type: application/json"
                -H "Accept: application/json, text/event-stream")
    [[ -n "$version" ]] && args+=(-H "MCP-Protocol-Version: ${version}")
    [[ -n "$method" ]] && args+=(-H "Mcp-Method: ${method}")
    [[ -n "$name" ]] && args+=(-H "Mcp-Name: ${name}")
    curl "${args[@]}" --data "$body" "http://127.0.0.1:${mcp_port}/mcp" \
        || fail "curl could not complete ${method:-a request} against the MCP surface (status $?)"
}

# The body of a response, after the blank line ending its head.
body_of() {
    local response="$1"
    printf '%s' "${response#*$'\r\n\r\n'}"
}

# Run a Python predicate over a JSON body; fails the run when it does not parse or
# the predicate is false. `-I`, so nothing in the working directory is imported.
# @param 1 what is being checked, for the message
# @param 2 the body
# @param 3 a Python expression over `d`, the parsed document
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

readonly MODERN="2026-07-28"
readonly LEGACY="2025-11-25"
readonly META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}'

# ------------------------------------------------------------- 2. modern era
discover="$(mcp_post server/discover "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{${META}}}" "$MODERN")"
[[ "$discover" == HTTP/1.1\ 200* ]] || fail "server/discover did not answer 200: ${discover%%$'\r'*}"
json_check "server/discover" "$(body_of "$discover")" \
    "'${MODERN}' in d['result']['supportedVersions'] and '${LEGACY}' in d['result']['supportedVersions']"
e2e_note "server/discover answered, naming ${MODERN} and ${LEGACY}"

listed="$(mcp_post tools/list "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{${META}}}" "$MODERN")"
[[ "$listed" == HTTP/1.1\ 200* ]] || fail "tools/list did not answer 200: ${listed%%$'\r'*}"
readonly FLEET_TOOLS="['fleet_overview','running_compiles','conditions','history','enrollment_status']"
json_check "tools/list" "$(body_of "$listed")" \
    "all(t in [x['name'] for x in d['result']['tools']] for t in ${FLEET_TOOLS}) and all(x['annotations']['readOnlyHint'] for x in d['result']['tools'] if x['name'] in ${FLEET_TOOLS})"
e2e_note "tools/list names the five fleet tools, each read-only"

# ------------------------------------------------- 2b. a fleet tool reaches the loop
called="$(mcp_post tools/call "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"fleet_overview\",\"arguments\":{},${META}}}" "$MODERN" fleet_overview)"
[[ "$called" == HTTP/1.1\ 200* ]] || fail "tools/call fleet_overview did not answer 200: ${called%%$'\r'*}"
# `no-leader-known`, and not `loop-not-running`: the gate asks the loop FIRST, so this name is the evidence
# that the node's 0xFC loop turns; and an error, since a node running no consensus has no fleet to report.
json_check "tools/call fleet_overview" "$(body_of "$called")" \
    "d['result']['isError'] is True and 'no-leader-known' in d['result']['content'][0]['text'] and 'structuredContent' not in d['result']"
e2e_note "fleet_overview on a node with no consensus is a tool error naming no-leader-known"

# ------------------------------------------------------------- 3. legacy era
initialized="$(mcp_post "" '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"e2e","version":"1"}}}' "")"
[[ "$initialized" == HTTP/1.1\ 200* ]] || fail "initialize did not answer 200: ${initialized%%$'\r'*}"
json_check "initialize" "$(body_of "$initialized")" "d['result']['protocolVersion'] == '${LEGACY}'"
e2e_note "initialize answered ${LEGACY}"

# --------------------------------------------------------- 4. not a scrape endpoint
metrics="$(curl -s -i -m 10 "http://127.0.0.1:${mcp_port}/metrics")" || fail "curl could not complete GET /metrics on the MCP port"
[[ "$metrics" == HTTP/1.1\ 404* ]] || fail "GET /metrics on the MCP port did not answer 404: ${metrics%%$'\r'*}"
e2e_note "the MCP port serves no /metrics"

stop_and_require_clean_exit "$node_pid" "the node" 30 "${workdir}/node.log"
node_pid=""

# ------------------------------------------ 2c. a fleet tool answers through main's wiring
# A node that runs consensus and leads alone: the leader endpoint is this node's own, so the call is
# answered in-process through its own 0xFC router (`FrameRouterAnswerer`), presenting this node's key.
# @param 1 what this round is, for the notes
# @param 2.. extra flags for the node
fleet_round() {
    local label="$1"; shift
    local fleet_node_port fleet_raft_port fleet_mcp_port fleet_pid
    fleet_node_port="$(free_port)"
    fleet_raft_port="$(free_port)"
    fleet_mcp_port="$(free_port)"
    "$node" \
        --slots=0 \
        --advertise "127.0.0.1:${fleet_node_port}" \
        --listen-node "127.0.0.1:${fleet_node_port}" \
        --listen-raft "127.0.0.1:${fleet_raft_port}" \
        --raft-self 127.0.0.1 \
        --discovery= \
        --cluster-dir "${workdir}/fleet-${label}" \
        --mcp-listen "$fleet_mcp_port" \
        "$@" \
        > "${workdir}/node.log" 2>&1 &
    fleet_pid=$!
    node_pid="$fleet_pid"
    wait_for_node_ready 127.0.0.1 "$fleet_mcp_port" "$fleet_pid" "the scheduler's MCP surface" "${workdir}/node.log"
    # The fleet model is the leader's: wait for the one-member election as the fact, never as a delay.
    wait_for_log "consensus: this node is now the leader" "$fleet_pid" "the node" "${workdir}/node.log"
    # And RECORDED: the call to itself presents this node's key, which reads the fleet only once its own
    # cluster holds that key -- until then a token file refuses it, which is a refusal and not a wrong answer.
    wait_for_log "this node's own cluster has recorded it" "$fleet_pid" "the node" "${workdir}/node.log"

    local expected="127.0.0.1:${fleet_node_port}" tool response
    for tool in fleet_overview running_compiles; do
        response="$(curl -s -i -m 20 -X POST -H "Content-Type: application/json" \
            -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
            -H "Mcp-Method: tools/call" -H "Mcp-Name: ${tool}" \
            --data "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"${tool}\",\"arguments\":{},${META}}}" \
            "http://127.0.0.1:${fleet_mcp_port}/mcp")" \
            || fail "curl could not complete tools/call ${tool} (${label})"
        [[ "$response" == HTTP/1.1\ 200* ]] || fail "tools/call ${tool} (${label}) did not answer 200: ${response%%$'\r'*}"
        json_check "tools/call ${tool} (${label})" "$(body_of "$response")" \
            "d['result']['isError'] is False and d['result']['structuredContent']['answered_by'] == '${expected}' and isinstance(d['result']['structuredContent']['term'], int) and d['result']['structuredContent']['term_scope'] == 'this-node'"
    done
    # And fields only the fleet document gives: it was rendered by a LEADER -- whose document names no
    # leader, `null` rather than itself -- every machine row carries its heartbeat age in seconds, and the
    # text names the node that answered as the leader.
    json_check "fleet_overview content (${label})" "$(body_of "$(curl -s -i -m 20 -X POST -H "Content-Type: application/json" \
            -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
            -H "Mcp-Method: tools/call" -H "Mcp-Name: fleet_overview" \
            --data "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"fleet_overview\",\"arguments\":{},${META}}}" \
            "http://127.0.0.1:${fleet_mcp_port}/mcp")")" \
        "d['result']['structuredContent']['role'] == 'leader' and d['result']['structuredContent']['leader'] is None and isinstance(d['result']['structuredContent']['nodes'], list) and all('heartbeat_age_s' in n for n in d['result']['structuredContent']['nodes']) and 'version_skew' in d['result']['structuredContent'] and 'fleet led by ${expected}, which answered' in d['result']['content'][0]['text']"
    e2e_note "fleet_overview and running_compiles answered by the leading scheduler itself (${label})"
    stop_and_require_clean_exit "$fleet_pid" "the scheduler (${label})" 30 "${workdir}/node.log"
    node_pid=""
}

fleet_round no-token
printf 's3cret-dashboard-token\n' > "${workdir}/token"
chmod 600 "${workdir}/token"
fleet_round with-token --dashboard-token-file "${workdir}/token"

# ----------------------------------------------------- 5. a network address refuses
refusing_port="$(free_port)"
refusal_status=0
refusal="$(run_bounded 60 "$node" \
    --slots=0 \
    --listen-node "127.0.0.1:${node_port}" \
    --listen-raft= \
    --discovery= \
    --cluster-dir "${workdir}/state" \
    --mcp-listen "0.0.0.0:${refusing_port}" 2>&1)" || refusal_status=$?
[[ "$refusal_status" -ne 0 ]] || fail "a node with --mcp-listen on 0.0.0.0 started: ${refusal}"
[[ "$refusal" == *"--mcp-listen must be a loopback address"* ]] \
    || fail "a node with --mcp-listen on 0.0.0.0 ended (status ${refusal_status}) without naming the loopback rule: ${refusal}"
e2e_note "--mcp-listen on 0.0.0.0 refused at startup (status ${refusal_status})"

echo "node MCP E2E passed"
