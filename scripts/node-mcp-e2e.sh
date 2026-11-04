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
#   2d. A node tool answers about it    — `node_status` with no `node` answers about this node
#                                         with real content: its memory tier present and the
#                                         disk tier ABSENT, the listen spec it runs by, and the
#                                         `--requirepass` it holds redacted and nowhere in the body.
#   2e. The actions act                 — on a one-node scheduler that runs a worker:
#                                         `cordon` then `uncordon` of this node, each
#                                         confirmed through `node_status`;
#                                         `explain_admission` about the caller and about
#                                         this node; `enrollment_auto_approve` armed for a
#                                         minute and ended, each confirmed through
#                                         `enrollment_status`; and the audit line each
#                                         action leaves in the node's log; `set_display_name`
#                                         of this node, read back from the fleet document's
#                                         member row and by `node_status` resolving that name;
#                                         `set_cache_budget` of this node's memory tier, read back
#                                         from the fleet document's member row once it commits,
#                                         then `clear_cache_budget`, read back absent again; and
#                                         node_status shows the override APPLIED (source=cluster,
#                                         baseline recorded) and then configured again.
#   2f. toolchains answers              — on that same scheduler, once its worker has
#                                         registered what it identified: every toolchain
#                                         served by this one machine, counted once, each
#                                         family the banner's, unserved derived (EMPTY on
#                                         one machine), demand ABSENT (null), and an upgrade
#                                         report whose every row is consistent -- nothing
#                                         recommended where the ordering is unknown, a
#                                         fingerprint per DRIVER otherwise, nobody behind
#                                         or ahead on a one-machine fleet, a basis on each.
#   2g. toolchain_diff explains         — on that same scheduler, whose capture walk is HELD
#       a fingerprint                     until released: the worker has registered while it
#                                         is held (asserted -- registration does not wait on
#                                         the walk), then, released: one of its fingerprints
#                                         against itself is identical, with the recompute
#                                         PASSED -- the inputs the survey kept reproduce the
#                                         fingerprint it registered -- and a fingerprint it
#                                         does not serve is a clean tool error.
#   2h. history answers for a machine   — that same scheduler's own cpu-busy series, asked by its id: named by
#                                         id and display name, sixty one-minute buckets, every unsampled one a
#                                         gap and the first sampled one a gap too (no CPU interval yet), never 0;
#                                         asked of the fleet, refused naming `node`. Last, so
#                                         2f/2g pay for the node uptime it waits on.
#   2i. the keyed histories answer      — requesters and toolchain-demand on that same scheduler: each an object
#                                         (it leads) counted since a stated instant, observed minutes, nothing
#                                         granted on a fleet with no launcher (total 0, no key, no other); refused
#                                         with `node`. No wait of its own: 2h's sampled minutes are what it reads.
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
# A credential the node holds, which no MCP answer may carry: `node_status` must report it redacted.
readonly E2E_SECRET="e2e-not-a-real-credential-$$"

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
    --requirepass "$E2E_SECRET" \
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
readonly READ_TOOLS="['fleet_overview','running_compiles','conditions','history','enrollment_status','toolchains','toolchain_diff','node_status']"
json_check "tools/list" "$(body_of "$listed")" \
    "all(t in [x['name'] for x in d['result']['tools']] for t in ${READ_TOOLS}) and all(x['annotations']['readOnlyHint'] for x in d['result']['tools'] if x['name'] in ${READ_TOOLS})"
e2e_note "tools/list names the five fleet tools, toolchains, toolchain_diff and node_status, each read-only"

# ------------------------------------------------- 2b. a fleet tool reaches the loop
called="$(mcp_post tools/call "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"fleet_overview\",\"arguments\":{},${META}}}" "$MODERN" fleet_overview)"
[[ "$called" == HTTP/1.1\ 200* ]] || fail "tools/call fleet_overview did not answer 200: ${called%%$'\r'*}"
# `no-leader-known`, and not `loop-not-running`: the gate asks the loop FIRST, so this name is the evidence
# that the node's 0xFC loop turns; and an error, since a node running no consensus has no fleet to report.
json_check "tools/call fleet_overview" "$(body_of "$called")" \
    "d['result']['isError'] is True and 'no-leader-known' in d['result']['content'][0]['text'] and 'structuredContent' not in d['result']"
e2e_note "fleet_overview on a node with no consensus is a tool error naming no-leader-known"

# ----------------------------------------------- 2d. node_status answers about this node
# Answered in-process through the node's own 0xFC router: the reply `pacectl node` reads, with the detail
# only a running node can give -- its cache tier, the configuration it runs by, and its credential REDACTED.
status_called="$(mcp_post tools/call "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"node_status\",\"arguments\":{},${META}}}" "$MODERN" node_status)"
[[ "$status_called" == HTTP/1.1\ 200* ]] || fail "tools/call node_status did not answer 200: ${status_called%%$'\r'*}"
status_body="$(body_of "$status_called")"
readonly NS="d['result']['structuredContent']"
json_check "tools/call node_status" "$status_body" \
    "d['result']['isError'] is False and ${NS}['term_scope'] == 'this-node' and ${NS}['node'] is None and isinstance(${NS}['version'], str) and ${NS}['version'] != '' and isinstance(${NS}['uptime_s'], int) and 'cache-tier' in ${NS}['components'] and ${NS}['compile_slots'] is None and ${NS}['slot_ceilings'] is None"
# The memory tier is present with real figures; a cache with no disk tier says so rather than reporting zeroes.
json_check "node_status tiers" "$status_body" \
    "[t['tier'] for t in ${NS}['tiers']] == ['memory', 'disk'] and ${NS}['tiers'][0]['present'] is True and isinstance(${NS}['tiers'][0]['entries'], int) and ${NS}['tiers'][1]['present'] is False and ${NS}['tiers'][1]['entries'] is None"
# The configuration in force: the listen spec this node was started with, and the credential redacted.
json_check "node_status effective_config" "$status_body" \
    "{s['name']: s['values'] for s in ${NS}['effective_config']}['--listen-node'] == ['127.0.0.1:${node_port}'] and {s['name']: s['values'] for s in ${NS}['effective_config']}['--requirepass'] == ['<redacted>'] and [(b['tier'], b['source'], b['override_bytes']) for b in ${NS}['budget_overrides']] == [('memory', 'configured', None)] and ${NS}['budget_overrides'][0]['bytes'] == ${NS}['budget_overrides'][0]['configured_bytes']"
[[ "$status_body" != *"${E2E_SECRET}"* ]] || fail "node_status carried the --requirepass value: ${status_body}"
e2e_note "node_status answered about this node: its memory tier, a disk tier absent, its listen spec, its credential redacted, its memory budget configured"

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
    #
    # Fetched into a variable FIRST, never as `"$(body_of "$(curl ... --data "{\"...\",...}")")"`: bash 3.2 --
    # macOS's /bin/bash, which runs this fixture there -- brace-expands a command ARGUMENT whose `$(...)` holds
    # `\"` and a `{...,...}`, and sent `"jsonrpc":"2.0"` as the body, answered `Parse error`. An assignment's
    # right-hand side is not brace-expanded, so a body with a comma in it is fetched in one.
    response="$(curl -s -i -m 20 -X POST -H "Content-Type: application/json" \
            -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
            -H "Mcp-Method: tools/call" -H "Mcp-Name: fleet_overview" \
            --data "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"fleet_overview\",\"arguments\":{},${META}}}" \
            "http://127.0.0.1:${fleet_mcp_port}/mcp")" \
        || fail "curl could not complete fleet_overview content (${label})"
    json_check "fleet_overview content (${label})" "$(body_of "$response")" \
        "d['result']['structuredContent']['role'] == 'leader' and d['result']['structuredContent']['leader'] is None and isinstance(d['result']['structuredContent']['nodes'], list) and all('heartbeat_age_s' in n for n in d['result']['structuredContent']['nodes']) and 'version_skew' in d['result']['structuredContent'] and 'fleet led by ${expected}, which answered' in d['result']['content'][0]['text']"
    e2e_note "fleet_overview and running_compiles answered by the leading scheduler itself (${label})"
    stop_and_require_clean_exit "$fleet_pid" "the scheduler (${label})" 30 "${workdir}/node.log"
    node_pid=""
}

fleet_round no-token
printf 's3cret-dashboard-token\n' > "${workdir}/token"
chmod 600 "${workdir}/token"
fleet_round with-token --dashboard-token-file "${workdir}/token"

# ------------------------------------------------------------ 2e. the actions act
# A one-node scheduler that also runs a WORKER (`--slots=1`): a cordon governs a worker, and a node with none
# answers the compile family's not-served sentence. It leads alone, so every leader-gated action is answered
# in-process by this node as the loopback caller it is -- which holds an operator's standing there.
readonly ACTION_CLIENT="node-mcp-e2e"
readonly ACTION_META="\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"${MODERN}\",\"io.modelcontextprotocol/clientInfo\":{\"name\":\"${ACTION_CLIENT}\",\"version\":\"1\"},\"io.modelcontextprotocol/clientCapabilities\":{}}"
action_node_port="$(free_port)"
action_raft_port="$(free_port)"
action_mcp_port="$(free_port)"
# Whether this host has a compiler the worker can discover: without one it registers no toolchain, and 2f and 2g
# have nothing to read.
host_has_compiler() {
    command -v cc >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 \
        || command -v clang >/dev/null 2>&1
}
# The capture walk is HELD until this file exists (the node's test seam, read only when the variable is set), so 2g
# can assert that the worker registers while the walk has not run -- by state, never by a timing threshold.
capture_release="${workdir}/capture-release"
# The toolchain-fingerprint cache the acting node reads, this run's own and WARM before it starts. 2g's held diff is a
# property of a WARM start only: on a cache miss the registering survey walks the tree itself and keeps what it
# walked, so the toolchain is recorded before the capture walk runs and the diff answers -- correctly. That is what
# arm64 CI met with the caller's cache cold for the fingerprint 2g picked, while x86-64 and a developer machine had it
# warm from earlier runs. So the cache is not the caller's (`$XDG_STATE_HOME`, else `~/.local/state`), whose
# temperature this fixture can neither know nor change honestly, but a directory this run fills: a first node's
# registering survey walks every discovered toolchain into it and stops, its own capture held and never released.
toolchain_state="${workdir}/toolchain-state"
mkdir -p "$toolchain_state"
if host_has_compiler; then
    warm_node_port="$(free_port)"
    warm_raft_port="$(free_port)"
    XDG_STATE_HOME="$toolchain_state" PACE_NODE_TEST_CAPTURE_HOLD="${workdir}/warm-never-released" "$node" \
        --slots=1 \
        --advertise "127.0.0.1:${warm_node_port}" \
        --listen-node "127.0.0.1:${warm_node_port}" \
        --listen-raft "127.0.0.1:${warm_raft_port}" \
        --raft-self 127.0.0.1 \
        --discovery= \
        --cluster-dir "${workdir}/warm" \
        > "${workdir}/warm.log" 2>&1 &
    warm_pid=$!
    node_pid="$warm_pid"
    # Logged once the registering survey has served its map, so every fingerprint it computed is in the cache.
    wait_for_log "the toolchain capture walk is held until" "$warm_pid" "the cache-warming node" "${workdir}/warm.log" 300
    stop_and_require_clean_exit "$warm_pid" "the cache-warming node" 30 "${workdir}/warm.log"
    node_pid=""
    # The positive control: the warming node wrote HERE, so the variable reaches the cache the acting node reads.
    [[ -n "$(ls -A "${toolchain_state}/pace-cc" 2>/dev/null)" ]] \
        || fail "the cache-warming node left no toolchain-fingerprint cache under ${toolchain_state}/pace-cc"
    e2e_note "warmed this run's toolchain-fingerprint cache: $(grep -c 'computed the toolchain fingerprint for' "${workdir}/warm.log") fingerprint(s)"
fi
XDG_STATE_HOME="$toolchain_state" PACE_NODE_TEST_CAPTURE_HOLD="$capture_release" "$node" \
    --slots=1 \
    --advertise "127.0.0.1:${action_node_port}" \
    --listen-node "127.0.0.1:${action_node_port}" \
    --listen-raft "127.0.0.1:${action_raft_port}" \
    --raft-self 127.0.0.1 \
    --discovery= \
    --cluster-dir "${workdir}/actions" \
    --mcp-listen "$action_mcp_port" \
    > "${workdir}/node.log" 2>&1 &
action_pid=$!
node_pid="$action_pid"
wait_for_node_ready 127.0.0.1 "$action_mcp_port" "$action_pid" "the acting scheduler's MCP surface" "${workdir}/node.log"
wait_for_log "consensus: this node is now the leader" "$action_pid" "the node" "${workdir}/node.log"
wait_for_log "this node's own cluster has recorded it" "$action_pid" "the node" "${workdir}/node.log"

# Call one tool on the acting node and echo the BODY; fails the run on anything but a 200.
# @param 1 the tool
# @param 2 its arguments, as JSON
call_action() {
    local tool="$1" arguments="$2" response
    response="$(curl -s -i -m 20 -X POST -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: ${MODERN}" \
        -H "Mcp-Method: tools/call" -H "Mcp-Name: ${tool}" \
        --data "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"${tool}\",\"arguments\":${arguments},${ACTION_META}}}" \
        "http://127.0.0.1:${action_mcp_port}/mcp")" \
        || fail "curl could not complete tools/call ${tool} ${arguments}"
    [[ "$response" == HTTP/1.1\ 200* ]] || fail "tools/call ${tool} ${arguments} did not answer 200: ${response%%$'\r'*}"
    body_of "$response"
}
readonly SC="d['result']['structuredContent']"
readonly OK="d['result']['isError'] is False"

# tools/list names every action, each NOT read-only, the reject destructive, and explain_admission read-only.
listed="$(curl -s -i -m 20 -X POST -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
    -H "MCP-Protocol-Version: ${MODERN}" -H "Mcp-Method: tools/list" \
    --data "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\",\"params\":{${ACTION_META}}}" \
    "http://127.0.0.1:${action_mcp_port}/mcp")" || fail "curl could not complete tools/list on the acting node"
json_check "tools/list actions" "$(body_of "$listed")" \
    "(lambda a: all(a[t]['readOnlyHint'] is False for t in ['enrollment_approve','enrollment_reject','enrollment_auto_approve','set_display_name','set_cache_budget','clear_cache_budget','cordon','uncordon']) and a['enrollment_reject']['destructiveHint'] is True and a['cordon']['destructiveHint'] is False and a['explain_admission']['readOnlyHint'] is True)({x['name']: x['annotations'] for x in d['result']['tools']})"
e2e_note "tools/list names the actions, none read-only, the reject destructive"

# The worker answers node_status with a cordon state only once it runs; wait on that STATE rather than a delay.
cordon_reads_serving() {
    local body
    body="$(call_action node_status '{}')"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False and d['result']['structuredContent']['cordon'] == 'serving' else 1)" <<< "$body"
}
wait_until cordon_reads_serving "the worker to report cordon serving through node_status" "$action_pid" "${workdir}/node.log" 60

cordoned="$(call_action cordon '{}')"
json_check "cordon" "$cordoned" \
    "${OK} and ${SC}['cordon'] in ('draining','drained') and isinstance(${SC}['compiles_in_flight'], int) and 'a restart of this node lifts it' in d['result']['content'][0]['text']"
json_check "node_status after cordon" "$(call_action node_status '{}')" "${OK} and ${SC}['cordon'] in ('draining','drained')"
uncordoned="$(call_action uncordon '{}')"
json_check "uncordon" "$uncordoned" "${OK} and ${SC}['cordon'] == 'serving'"
json_check "node_status after uncordon" "$(call_action node_status '{}')" "${OK} and ${SC}['cordon'] == 'serving'"
e2e_note "cordon then uncordon of this node, each confirmed through node_status"

self_id="$(python3 -I -c "import json,sys; print(json.loads(sys.stdin.read())['result']['structuredContent']['node_id'])" \
    <<< "$(call_action node_status '{}')")" || fail "node_status named no node_id"
json_check "explain_admission (caller)" "$(call_action explain_admission '{}')" \
    "${OK} and ${SC}['machine'] is None and ${SC}['verdict'] == 'admitted' and 'loopback' in ${SC}['decided_by'] and ${SC}['standing'] is None"
json_check "explain_admission (this node)" "$(call_action explain_admission "{\"machine\":\"${self_id}\"}")" \
    "${OK} and ${SC}['machine'] == '${self_id}' and ${SC}['verdict'] == 'admitted' and ${SC}['standing'] == 'voter' and 'proven-key' in ${SC}['decided_by']"
e2e_note "explain_admission answered about the caller (loopback) and about this node (a voter, by its key)"

armed="$(call_action enrollment_auto_approve '{"minutes":1}')"
json_check "enrollment_auto_approve minutes 1" "$armed" \
    "${OK} and ${SC}['enrollment'] == 'auto-approve' and 0 < ${SC}['auto_approve_left_s'] <= 60"
json_check "enrollment_status while armed" "$(call_action enrollment_status '{}')" \
    "${OK} and ${SC}['enrollment'] == 'auto-approve' and 0 < ${SC}['auto_approve_left_s'] <= 60"
ended="$(call_action enrollment_auto_approve '{"off":true}')"
json_check "enrollment_auto_approve off" "$ended" "${OK} and ${SC}['enrollment'] == 'manual' and ${SC}['auto_approve_left_s'] is None"
json_check "enrollment_status once ended" "$(call_action enrollment_status '{}')" \
    "${OK} and ${SC}['enrollment'] == 'manual' and ${SC}['auto_approve_left_s'] is None"
e2e_note "enrollment_auto_approve armed for a minute and ended, each confirmed through enrollment_status"

# set_display_name, then the name read back where every surface reads it: the fleet document's member row (once the
# entry COMMITS -- the answer says only that it was appended, so the wait is on the STATE), and node_status resolving
# the node BY that name, exactly.
named="$(call_action set_display_name "{\"node\":\"${self_id}\",\"name\":\"e2e-box\"}")"
json_check "set_display_name" "$named" \
    "${OK} and ${SC}['node_id'] == '${self_id}' and ${SC}['display_name'] == 'e2e-box' and ${SC}['state'] == 'replicating'"
member_reads_named() {
    local body
    body="$(call_action fleet_overview '{}')"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False and any(m['id'] == '${self_id}' and m['name'] == 'e2e-box' and m['name_source'] == 'replicated' for m in d['result']['structuredContent']['members']) else 1)" <<< "$body"
}
wait_until member_reads_named "the fleet document to show this member named e2e-box" "$action_pid" "${workdir}/node.log" 30
# This one node's own MACHINE row is named too: by the member holding the key its connection PROVED (the leader's
# `node-id`), over loopback, where no member records an endpoint an address join could have matched.
machine_row_named() {
    local body
    body="$(call_action fleet_overview '{}')"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False and [(n['node_id'], n['display_name']) for n in d['result']['structuredContent']['nodes']] == [('${self_id}', 'e2e-box')] else 1)" <<< "$body"
}
wait_until machine_row_named "the fleet document's one machine row to carry node_id ${self_id}" "$action_pid" "${workdir}/node.log" 60
json_check "node_status by display name" "$(call_action node_status '{"node":"e2e-box"}')" \
    "${OK} and ${SC}['node_id'] == '${self_id}' and ${SC}['node'] == 'e2e-box' and ${SC}['display_name'] == 'e2e-box' and ${SC}['name'] == 'e2e-box' and ${SC}['name_source'] == 'replicated'"
json_check "node_status by another case of the name" "$(call_action node_status '{"node":"E2E-BOX"}')" \
    "d['result']['isError'] is True"
# Each JSON argument with a comma goes through an assignment, never straight into an argument: see
# "fleet_overview content" above for what bash 3.2 makes of the other form.
refused_name="$(call_action set_display_name "{\"node\":\"${self_id}\",\"name\":\"a\\tb\"}")"
json_check "set_display_name refuses a control byte" "$refused_name" \
    "d['result']['isError'] is True and 'control character' in d['result']['content'][0]['text']"
e2e_note "set_display_name named this node e2e-box, shown replicated in the fleet document, and node_status resolved it by that name"

# set_cache_budget, then the RECORDED override read back where every node reads it -- the fleet document's member row,
# once the entry commits -- and cleared again. Recorded, never applied here: applying is the member's own act.
budgeted="$(call_action set_cache_budget "{\"node\":\"e2e-box\",\"memory\":\"64m\"}")"
json_check "set_cache_budget" "$budgeted" \
    "${OK} and ${SC}['node_id'] == '${self_id}' and ${SC}['display_name'] == 'e2e-box' and ${SC}['memory'] == 67108864 and ${SC}['disk'] is None and ${SC}['state'] == 'replicating'"
member_override_reads() {
    local expected="$1" body
    body="$(call_action fleet_overview '{}')"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False and any(m['id'] == '${self_id}' and m['memory_override'] == ${expected} and m['disk_override'] is None for m in d['result']['structuredContent']['members']) else 1)" <<< "$body"
}
member_override_recorded() { member_override_reads 67108864; }
member_override_cleared() { member_override_reads None; }
wait_until member_override_recorded "the fleet document to record this member's 64 MiB memory override" "$action_pid" "${workdir}/node.log" 30
# And APPLIED by the member itself: it reported its own configured value as the baseline (MEMBER-REPORT, under its own
# proof, answered in-process on this leader), then put the override in force -- read back through node_status.
node_budget_reads() {
    local expected="$1" body
    body="$(call_action node_status '{}')"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); r=[b for b in d['result']['structuredContent']['budget_overrides'] or [] if b['tier'] == 'memory']; sys.exit(0 if d['result']['isError'] is False and len(r) == 1 and (${expected}) else 1)" <<< "$body"
}
node_budget_applied() {
    node_budget_reads "r[0]['source'] == 'cluster' and r[0]['bytes'] == 67108864 and r[0]['override_bytes'] == 67108864 and r[0]['baseline_bytes'] == r[0]['configured_bytes'] and r[0]['set_at'] is not None and r[0]['note'] is None"
}
node_budget_configured() {
    node_budget_reads "r[0]['source'] == 'configured' and r[0]['bytes'] == r[0]['configured_bytes'] and r[0]['override_bytes'] is None and r[0]['baseline_bytes'] is None"
}
wait_until node_budget_applied "node_status to show the 64 MiB memory override in force from the cluster, its baseline recorded" "$action_pid" "${workdir}/node.log" 60
refused_share="$(call_action set_cache_budget "{\"node\":\"${self_id}\",\"memory\":\"25%\"}")"
json_check "set_cache_budget refuses a share of RAM" "$refused_share" \
    "d['result']['isError'] is True and 'absolute size' in d['result']['content'][0]['text']"
cleared="$(call_action clear_cache_budget "{\"node\":\"${self_id}\",\"which\":\"memory\"}")"
json_check "clear_cache_budget" "$cleared" "${OK} and ${SC}['node_id'] == '${self_id}' and ${SC}['which'] == 'memory'"
wait_until member_override_cleared "the fleet document to show this member's memory override cleared" "$action_pid" "${workdir}/node.log" 30
wait_until node_budget_configured "node_status to show the memory budget configured again once the override is cleared" "$action_pid" "${workdir}/node.log" 60
e2e_note "set_cache_budget recorded this node's 64 MiB memory override, the node recorded its baseline and applied it (node_status source=cluster), and clear_cache_budget gave the tier its configured budget back"

# Every action left its audit line, naming the client; the read did not.
for line in "mcp action set_display_name {\"node\":\"${self_id}\",\"name\":\"e2e-box\"} by ${ACTION_CLIENT} answered by" \
            "mcp action set_cache_budget {\"node\":\"${self_id}\",\"memory\":\"64m\"} by ${ACTION_CLIENT} answered by" \
            "mcp action clear_cache_budget {\"node\":\"${self_id}\",\"which\":\"memory\"} by ${ACTION_CLIENT} answered by" \
            "mcp action cordon {\"node\":\"${self_id}\"} by ${ACTION_CLIENT} answered by" \
            "mcp action uncordon {\"node\":\"${self_id}\"} by ${ACTION_CLIENT} answered by" \
            "mcp action enrollment_auto_approve {\"minutes\":1} by ${ACTION_CLIENT} answered by" \
            "mcp action enrollment_auto_approve {\"off\":true} by ${ACTION_CLIENT} answered by"; do
    grep -qF -- "$line" "${workdir}/node.log" || fail "the node's log carries no audit line '${line}'"
done
if grep -qF -- "mcp action explain_admission" "${workdir}/node.log"; then
    fail "explain_admission, a read, left an action audit line"
fi
e2e_note "each action left its audit line naming ${ACTION_CLIENT}; the read left none"

# ------------------------------------------------------------- 2f. toolchains answers
# The worker registers the toolchains it identified only once each fingerprint is real (the survey walks
# include trees on the heartbeat thread), so wait on that STATE. A host with no compiler on PATH registers
# none, and the content check below has nothing to read: said, never passed as if it had been checked.
action_endpoint="127.0.0.1:${action_node_port}"
if host_has_compiler; then
    toolchains_registered() {
        local body
        body="$(call_action toolchains '{}')"
        python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False and d['result']['structuredContent']['toolchain_count'] > 0 else 1)" <<< "$body"
    }
    wait_until toolchains_registered "the worker to register a toolchain the toolchains tool reports" "$action_pid" "${workdir}/node.log" 120
    toolchains_body="$(call_action toolchains '{}')"
    # One machine, however many toolchains it serves; each toolchain served by exactly it.
    json_check "toolchains machines" "$toolchains_body" \
        "${OK} and ${SC}['answered_by'] == '${action_endpoint}' and ${SC}['term_scope'] == 'this-node' and ${SC}['machine_count'] == 1 and ${SC}['toolchain_count'] == len(${SC}['toolchains']) and all(t['nodes'] == ['${action_endpoint}'] for t in ${SC}['toolchains']) and [n['endpoint'] for n in ${SC}['nodes']] == ['${action_endpoint}'] and sorted(${SC}['nodes'][0]['served']) == sorted(t['fingerprint'] for t in ${SC}['toolchains'])"
    # Unserved is derived: on a one-machine fleet this machine serves every toolchain of its own families, so the
    # list is EMPTY -- and null only if none of its toolchains has a known family. No demand is recorded yet: absent.
    # Named by the member its registrations' PROVEN key joins to, over loopback -- which no recorded 0xFC endpoint can
    # place, since a member records none on loopback.
    json_check "toolchains names the machine by its node id" "$toolchains_body" \
        "${SC}['nodes'][0]['node_id'] == '${self_id}' and ${SC}['nodes'][0]['display_name'] == 'e2e-box'"
    json_check "toolchains unserved and demand" "$toolchains_body" \
        "${SC}['nodes'][0]['unserved'] == ([] if any(t['family'] is not None for t in ${SC}['toolchains']) else None) and ${SC}['demand'] is None and isinstance(${SC}['unserved_note'], str) and isinstance(${SC}['demand_note'], str)"
    # A version is parsed off the label or is the word `unknown`; a family is the banner's, one of the known keys
    # -- never the label's tool name, so a `cc` that is GNU is in `gcc`.
    json_check "toolchains versions and families" "$toolchains_body" \
        "all((t['label'] is None and t['version'] == 'unknown') or t['version'] == 'unknown' or t['label'] == t['label'].split(' ')[0] + ' ' + t['version'] for t in ${SC}['toolchains']) and all(t['family'] in (None, 'gcc', 'g++', 'clang', 'clang++', 'apple-clang', 'apple-clang++', 'cl', 'clang-cl') for t in ${SC}['toolchains']) and any(t['family'] is not None for t in ${SC}['toolchains'])"
    # The upgrade report, row by row: an unknown ordering recommends nothing; a known one names a version and,
    # per DRIVER, a fingerprint of its own family served under that driver -- `gcc` and `cc` of one install are both
    # named, so on a host that serves either some row names one. On a one-machine fleet nobody is behind, ahead or on
    # another build; every row carries its basis.
    json_check "toolchains upgrade report" "$toolchains_body" \
        "len(${SC}['upgrade_report']) > 0 and all(r['basis'] != '' and r['behind'] == [] and r['ahead'] == [] and r['same_version_other_fingerprint'] == [] and ((r['recommended_version'] is None and r['recommended_fingerprints'] is None) if r['ordering_unknown'] else (r['recommended_version'] is not None and isinstance(r['recommended_fingerprints'], dict) and all(any(t['fingerprint'] == fp and t['family'] == r['family'] and t['driver'] == driver for t in ${SC}['toolchains']) for driver, fp in r['recommended_fingerprints'].items()) and 'no demand data; highest labelled version served' in r['basis'])) for r in ${SC}['upgrade_report']) and any(r['recommended_fingerprints'] for r in ${SC}['upgrade_report'])"
    e2e_note "toolchains reported $(python3 -I -c "import json,sys; print(json.loads(sys.stdin.read())['result']['structuredContent']['toolchain_count'])" <<< "$toolchains_body") toolchain(s) on this one machine, unserved derived and empty, demand absent, and a consistent upgrade report"

    # ------------------------------------------------------------- 2g. toolchain_diff explains a fingerprint
    # One served fingerprint against itself, through main's wiring: this node pages its own survey's inputs over
    # FINGERPRINT-INPUTS, recomputes the fingerprint from them, and finds nothing to tell apart. `recompute: passed`
    # is the wrong-answer guard having RUN -- the inputs the survey kept reproduce the fingerprint it registered.
    diff_fingerprint="$(python3 -I -c "import json,sys; print(json.loads(sys.stdin.read())['result']['structuredContent']['toolchains'][0]['fingerprint'])" <<< "$toolchains_body")"
    # REGISTRATION DOES NOT WAIT ON THE CAPTURE WALK, asserted: the walk is still held (the release file does not
    # exist), the worker registered above anyway, the node answers the diff with the transient "not yet captured"
    # refusal, and its log has no capture line yet. A worker that walked before registering could not have got here.
    [[ ! -e "$capture_release" ]] || fail "the capture release file exists before the test released it"
    # And the start was WARM: the registering survey took every fingerprint from the cache and walked no tree, so
    # nothing but the held capture can record inputs. A walk here would make the refusal below unreachable.
    if grep -q "hashing [0-9]* toolchain file(s)" "${workdir}/node.log"; then
        fail "the acting node's registering survey walked a toolchain tree although this run warmed its fingerprint cache, so its held diff cannot be told from a recorded one"
    fi
    held_body="$(call_action toolchain_diff "{\"a\":{\"fingerprint\":\"${diff_fingerprint}\"},\"b\":{\"fingerprint\":\"${diff_fingerprint}\"}}")"
    json_check "toolchain_diff while the capture walk is held" "$held_body" \
        "d['result']['isError'] is True and 'has not yet finished the one walk per start' in d['result']['content'][0]['text']"
    if grep -q "toolchain inputs captured" "${workdir}/node.log"; then
        fail "the node logged its capture while the capture walk was held"
    fi
    e2e_note "the worker registered while its capture walk was held, and answered the diff not-yet-captured"
    : > "$capture_release"
    # Released: the walk runs on its own thread and the node answers once it is adopted. Wait on that STATE.
    diff_captured() {
        local body
        body="$(call_action toolchain_diff "{\"a\":{\"fingerprint\":\"${diff_fingerprint}\"},\"b\":{\"fingerprint\":\"${diff_fingerprint}\"}}")"
        python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False else 1)" <<< "$body"
    }
    wait_until diff_captured "the node's capture walk to record the inputs of ${diff_fingerprint}" "$action_pid" "${workdir}/node.log" 300
    grep -q "toolchain inputs captured" "${workdir}/node.log" \
        || fail "the node recorded inputs without logging its capture stage"
    diff_body="$(call_action toolchain_diff "{\"a\":{\"fingerprint\":\"${diff_fingerprint}\"},\"b\":{\"fingerprint\":\"${diff_fingerprint}\"}}")"
    json_check "toolchain_diff of a fingerprint against itself" "$diff_body" \
        "${OK} and ${SC}['identical'] is True and ${SC}['same_fingerprint'] is True and ${SC}['recompute'] == 'passed' and ${SC}['complete'] is True and ${SC}['skew'] is None and ${SC}['inferred'] is False and ${SC}['only_a']['count'] == 0 and ${SC}['only_b']['count'] == 0 and ${SC}['content_differs']['count'] == 0 and ${SC}['a']['fingerprint'] == '${diff_fingerprint}' and ${SC}['a']['total_files'] == ${SC}['b']['total_files'] and ${SC}['verdict'].startswith('the inputs are identical')"
    # A fingerprint this node does not serve: the node refuses by name, and the tool answers a clean tool error.
    missing_body="$(call_action toolchain_diff "{\"a\":{\"fingerprint\":\"${diff_fingerprint}\"},\"b\":{\"fingerprint\":\"00000000000000000000000000000000\"}}")"
    json_check "toolchain_diff of a fingerprint nobody serves" "$missing_body" \
        "d['result']['isError'] is True and 'serves no toolchain with fingerprint 00000000000000000000000000000000' in d['result']['content'][0]['text']"
    e2e_note "toolchain_diff found ${diff_fingerprint} identical to itself over $(python3 -I -c "import json,sys; print(json.loads(sys.stdin.read())['result']['structuredContent']['a']['total_files'])" <<< "$diff_body") file(s) with the recompute passed, and refused a fingerprint nobody serves"
else
    e2e_note "no compiler on PATH, so the worker registers no toolchain: the toolchains and toolchain_diff checks were NOT run"
fi

# ------------------------------------------------- 2h. history answers for one machine
# This node's own cpu-busy series, asked BY ITS ID: the leader (itself) answers from what its presence loop handed
# over, filed under the key the announcement proved. LAST, after 2f and 2g, because what it waits for is the node's
# UPTIME, not anything this section does: a minute bucket travels only once its minute has closed, and the sampler's
# first CPU reading has no earlier counters to difference against -- so "two sampled minutes, the second with a
# reading" needs about three sample intervals (`FleetSampleInterval`, 60 s: the open minute at start, the absent
# first, the read second) and one announce cadence (`NodeAnnounceInterval`, 20 s) from the node's start. Whatever
# 2f and 2g already spent pays for that.
#
# The bound is DERIVED from the node's own `uptime_s` (a steady clock, not this shell's wall clock): what is left of
# those 200 s, plus one announce cadence of slack, and never less than that slack. Counted from the node's start, the
# whole case then stays inside the ctest TIMEOUT, so a stall is reported by `wait_until` -- what it waited for and the
# measured elapsed -- rather than by ctest killing the run.
readonly HISTORY_NEEDS_S=$((3 * 60 + 20))
readonly HISTORY_SLACK_S=20
uptime_s="$(python3 -I -c "import json,sys; print(json.loads(sys.stdin.read())['result']['structuredContent']['uptime_s'])" \
    <<< "$(call_action node_status '{}')")" || fail "node_status reported no uptime_s"
history_bound=$((HISTORY_NEEDS_S - uptime_s + HISTORY_SLACK_S))
[ "$history_bound" -ge "$HISTORY_SLACK_S" ] || history_bound="$HISTORY_SLACK_S"
cpu_history_sampled() {
    local body
    body="$(call_action history "{\"series\":\"cpu-busy\",\"range\":\"1h\",\"node\":\"${self_id}\"}")"
    python3 -I -c "import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['result']['isError'] is False and sum(1 for b in d['result']['structuredContent']['buckets'] if b['coverage'] is not None) >= 2 else 1)" <<< "$body"
}
wait_until cpu_history_sampled "the leader to hold two sampled minutes of this node's cpu-busy history (node up ${uptime_s} s, bound ${history_bound} s)" "$action_pid" "${workdir}/node.log" "$history_bound"
cpu_body="$(call_action history "{\"series\":\"cpu-busy\",\"range\":\"1h\",\"node\":\"${self_id}\"}")"
# The shape: one machine, named by id and display name, a level in permille, sixty one-minute buckets. Every
# unsampled bucket is a gap; the first sampled one is a gap too (no interval yet); a value is a permille.
json_check "history cpu-busy for this node" "$cpu_body" \
    "${OK} and ${SC}['scope'] == 'node' and ${SC}['node_id'] == '${self_id}' and ${SC}['display_name'] == 'e2e-box' and ${SC}['series'] == 'cpu-busy' and ${SC}['unit'] == 'permille' and ${SC}['bucket_seconds'] == 60 and len(${SC}['buckets']) == 60 and all(b['value'] is None for b in ${SC}['buckets'] if b['coverage'] is None) and any(b['coverage'] is None for b in ${SC}['buckets']) and [b for b in ${SC}['buckets'] if b['coverage'] is not None][0]['value'] is None and all(0 <= b['value'] <= 1000 for b in ${SC}['buckets'] if b['value'] is not None)"
# Asked of the fleet, cpu-busy is refused naming the argument -- never a fleet aggregate.
json_check "history cpu-busy without node" "$(call_action history '{"series":"cpu-busy","range":"1h"}')" \
    "d['result']['isError'] is True and '\`node\`' in d['result']['content'][0]['text']"
e2e_note "history answered cpu-busy for ${self_id} (e2e-box): $(python3 -I -c "import json,sys; b=json.loads(sys.stdin.read())['result']['structuredContent']['buckets']; print(sum(1 for x in b if x['coverage'] is not None), 'sampled,', sum(1 for x in b if x['value'] is not None), 'with a reading')" <<< "$cpu_body"), gaps kept; refused without node"

# ------------------------------------------- 2i. the keyed histories answer, as the leader counts them
# Who asked and which toolchain: `requesters` and `toolchain-demand`, the leader's own `/fleet.json` sections. This
# fleet runs no launcher, so nothing was GRANTED and both are empty -- asserted as a SHAPE: the section is an object
# (this node leads), `coverage_since_ms` is a number inside this hour (the instant its sampler first folded while
# leading -- never the day ring's midnight), at least one minute is observed (the sampler folds once a minute, and the
# uptime 2h waited for spans at least one fold while leading), no key is named and nothing is folded, and the
# total is 0 -- a zero of a counted window, distinct from the `null` of a node that keeps none. No wait of its own:
# 2h already waited for the sampled minutes this reads. Asked with `node`, it is refused naming `key`.
for keyed in requesters:requesters toolchain-demand:toolchain_demand; do
    series="${keyed%%:*}"
    section="${keyed#*:}"
    history_body="$(call_action history "{\"series\":\"${series}\",\"range\":\"1h\"}")"
    json_check "history ${series}" "$history_body" \
        "${OK} and ${SC}['series'] == '${series}' and ${SC}['key'] is None and isinstance(${SC}['${section}'], dict) and isinstance(${SC}['${section}']['coverage_since_ms'], int) and ${SC}['${section}']['observed_minutes'] >= 1 and sum(${SC}['${section}']['coverage']) == ${SC}['${section}']['observed_minutes'] and ${SC}['${section}']['full_coverage'] == 1 and ${SC}['${section}']['window_minutes'] == 60 and ${SC}['${section}']['unattributed'] is None and isinstance(${SC}['${section}']['counts'], str) and ${SC}['${section}']['coverage_since_ms'] >= ${SC}['${section}']['bucket_starts_ms'][0] and ${SC}['${section}']['keys'] == [] and ${SC}['${section}']['other'] is None and ${SC}['${section}']['total'] == 0 and len(${SC}['${section}']['bucket_starts_ms']) == 60"
done
history_body="$(call_action history "{\"series\":\"requesters\",\"range\":\"1h\",\"node\":\"${self_id}\"}")"
json_check "history requesters with node" "$history_body" \
    "d['result']['isError'] is True and '\`key\`' in d['result']['content'][0]['text']"
e2e_note "history answered requesters and toolchain-demand: counted since the leader's first fold, nothing granted, refused with node"

stop_and_require_clean_exit "$action_pid" "the acting scheduler" 30 "${workdir}/node.log"
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
