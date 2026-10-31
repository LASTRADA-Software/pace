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
#                                         `tools/list` answers; both bodies parse
#                                         as JSON.
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
mcp_post() {
    local method="$1" body="$2" version="$3"
    local args=(-s -i -m 10 -X POST -H "Content-Type: application/json"
                -H "Accept: application/json, text/event-stream")
    [[ -n "$version" ]] && args+=(-H "MCP-Protocol-Version: ${version}")
    [[ -n "$method" ]] && args+=(-H "Mcp-Method: ${method}")
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
json_check "tools/list" "$(body_of "$listed")" "isinstance(d['result']['tools'], list)"
e2e_note "tools/list answered"

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
