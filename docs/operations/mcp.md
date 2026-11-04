# Asking the fleet through MCP

**`pace-node` can serve [MCP](https://modelcontextprotocol.io) to a Claude Code session on
its own machine, so you can ask the fleet questions — what is running, who is behind on a
compiler, why two toolchains with the same name do not share a cache — and act on the
answer: approve a machine waiting to join, name a member, override a cache budget. The
surface is off unless you turn it on, it answers this machine only, and it introduces no
credential of its own.**

Every answer comes from the same function the dashboard, `/fleet.json` or `pacectl`
answers from. Where `pacectl node` and the `node_status` tool both report a quantity, they
read one reply through one derivation, so the two cannot disagree. A field that was not
reported is `null`, never `0`, as on the fleet page.

## Turning it on

```sh
pace-node --mcp-listen=6690
```

or, in `pace-node.yaml`:

```yaml
mcp_listen: 6690
```

The surface serves one route, `POST /mcp`, on a thread of its own: never the dashboard's
and never one of the reactors the compile surface runs on. Slow or hostile MCP input can
stall only MCP.

**Loopback only, checked twice.**

- A bare port binds `127.0.0.1`. An explicit loopback address (`127.0.0.1:6690`, any
  other `127.x.x.x`, `[::1]:6690`) is accepted.
- Any other address is a **startup refusal**, an install included. That covers a LAN
  address, the wildcard (`0.0.0.0`, `[::]`) and a host name — `localhost` too, because a
  resolver decides what a name reaches. MCP has no authentication scheme here, so an
  endpoint other machines could reach would hand this node's fleet controls to anyone who
  dials it.
- Every request is checked again before its body is parsed. The connection's peer must be
  on this machine, or the request is answered `403`. That moves
  `pace_mcp_requests_refused_not_loopback_total`, so a future bind that widened the
  address could not quietly widen who is answered.
- A browser `Origin` that is not a loopback page is refused `403` as well.

`--mcp-listen` takes effect only at a restart. A reload that changes it is declined whole,
because the option is not reloadable.

Another machine is reached *through* your own node, never by dialling its MCP port (see
[Who answers](#who-answers)). So you never need a firewall rule for MCP
([What to open on a firewall](cluster-communication.md#what-to-open-on-a-firewall)).
`pacectl node` reports the surface as `mcp-port` while it is served, and
`--print-surfaces` lists it.

## Connecting Claude Code

```sh
claude mcp add --transport http pace http://127.0.0.1:6690/mcp
```

That writes the server into Claude Code's configuration, at the `local` scope by default.
To try the surface without touching any configuration, give one session a file of its
own:

```sh
cat > pace-mcp.json <<'EOF'
{"mcpServers":{"pace":{"type":"http","url":"http://127.0.0.1:6690/mcp"}}}
EOF
claude -p --mcp-config pace-mcp.json --strict-mcp-config "Which compiles are running on the fleet?"
```

`--strict-mcp-config` makes that session use only the servers in the file.

The node speaks MCP revision `2026-07-28` and also the earlier `2025-11-25`. The earlier
revision starts with `initialize` and mints no session. Every answer is
`application/json`, with no server-sent events. The tool set is fixed per build, so
`listChanged` is false.

## The tools

Each tool returns structured content that matches its declared output schema, plus a short
text rendered from that same value. Durations are in seconds (fields ending `_s`). Every
answer carries `answered_by`, the `0xFC` endpoint that answered (per side, as `a.answered_by`
and `b.answered_by`, for `toolchain_diff`, whose two sides may be answered by two nodes), and
`term`. **`term` is the
term THIS node holds**, not a field of the leader's reply (`term_scope: this-node`). The two
agree once leadership has settled, and they may differ during an election.

Wherever a tool takes a node, it accepts a member's id or its
[display name](#display-names), matched exactly and never by prefix.

### Reading

| Tool | Answers | Arguments |
|---|---|---|
| `node_status` | One node's full status: version, uptime, components, consensus standing, worker slots with every ceiling and the binding one named, cordon, each cache tier (entries, bytes, budget, hit ratio), the surfaces it opened, the budget in force per tier with any override (`budget_overrides`), and the configuration in force after reloads (`effective_config`). The reply `pacectl node` reads. | `node` (optional; this node when omitted) |
| `fleet_overview` | The leader's view: who leads, the cluster members, every machine with its version, load and heartbeat age, and whether the machines run mixed builds. Read from the document `/fleet.json` serves. | none |
| `running_compiles` | Every compile on the fleet's queue (running, queued, waiting on an identical compile) with requester, source, toolchain, worker, age and lease expiry, and totals over every ticket. `compiles: null` means the answering node leads no queue, which is not the same as an empty queue. | `requester`, `worker` (optional, exact) |
| `conditions` | Every condition each machine reports to the leader, raised or not, latched or live, with detail and remedy in the machine's own words. A machine whose report carried no list is under `not_reported`: absent, not clear. | none |
| `history` | One series over a range, as bucket values with explicit gaps (`null`): the series `/fleet/series.json` serves, or the keyed `requesters` and `toolchain-demand` series (compiles granted per requesting machine and per toolchain fingerprint), returned exactly as `/fleet.json`'s `requesters` and `toolchain_demand` sections. An unknown series, range or node is refused, with the valid ones listed. | `series`, `range` (required); `node` (one machine's own series; `cpu-busy` requires it, and the dispatch and keyed series refuse it); `key` (one row of a keyed series) |
| `enrollment_status` | The leader's enrollment window: `manual`, or `auto-approve` with the seconds left, and every machine that asked to join, with its id, role, claimed endpoint, source host, key, first-seen age and `claims_changed`. The report `pace-node --enroll-list` prints. Needs [operator standing](#who-may-act). | none |
| `toolchains` | Which toolchains the fleet serves, from the leader's `/fleet.json`. Per fingerprint: label, compiler family (read off the banner, so Apple clang installed as `gcc` is `apple-clang`), version, and the machines that serve it. Per machine: what it serves and what in its families it does not. Each family also gets a conservative upgrade report, with every row carrying its basis (see below). | none |
| `toolchain_diff` | Why two toolchain fingerprints differ, even under the same name and version. | `a`, `b`: each a `node` (optional) and exactly one of `fingerprint` or `toolchain` (a served label such as `gcc 13.2.0`, or its compiler path) |
| `explain_admission` | Why this node admits or refuses a machine, or this MCP caller's own connection when `machine` is omitted. Gives the verdict, the machine's standing in the roster, and **every** route that decided. The answer `pacectl explain-admission` prints. | `machine` (optional: an id or a key's text, exact) |

**The upgrade report in `toolchains` names a version only when it can be checked.** It
names a version to converge on only when every version in the family parses unambiguously.
It places each machine by its highest version: `behind`, `ahead`, or
`same_version_other_fingerprint` (the same version built differently for the same driver:
ask `toolchain_diff` why). `lacks_recommended` lists the machines that do not serve the
recommended toolchain. The fingerprint is named per driver (`recommended_fingerprints`),
because `gcc` and `cc` are one install under two names. When builds of one version cannot
be told apart by demand, no fingerprint is named for that driver. Demand decides only when
the leader has counted at least a day of the seven-day window and the lead is larger than
everything folded into `other`. Otherwise the report falls back to versions and says so.

**`toolchain_diff` recomputes before it compares.** Each node sends what its survey
digested: the schema, the compiler's banner, the driver grammar, and every include-tree file
with its content digest. Asking never starts a walk. The record is the one the survey
captured, so it describes what the fingerprint was computed from, not the tree as it is
now. This node recomputes both fingerprints from those records (`recompute: passed`) and
refuses a side whose record no longer reproduces its fingerprint as stale. The answer names
what differs: schema, both banners, both grammars, files only on `a`, files only on `b`,
and files on both with different content. The `verdict` states only what the comparison
shows, and anything more is marked `inferred:`. A side past 200 000 files, or past its byte
or page bound, is compared on schema, banner and grammar only (`complete: false`). Shortly
after a start, a node may still be capturing its inputs, and it says so rather than
answering.

### Acting

Every acting tool carries the MCP annotation `readOnlyHint: false` (`enrollment_reject` also
`destructiveHint: true`); read tools carry `readOnlyHint: true`. Claude Code asks before any
MCP tool you have not allowlisted, read tools included. The node that runs the tool logs it at Info, with
the arguments, the MCP client's name and who answered.

| Tool | Does | Arguments | The same change elsewhere |
|---|---|---|---|
| `enrollment_approve` | Admits one waiting machine under the key named with it. Compare that key with the one the machine printed first; that comparison is what makes this safe. An approval naming any other key is refused and admits nothing. | `node_id`, `public_key` | `pace-node --enroll-approve` |
| `enrollment_reject` | Refuses one waiting machine. Final for that request. It does **not** remove a machine that was already approved. | `node_id` | `pace-node --enroll-reject` |
| `enrollment_auto_approve` | Arms the leader's auto-approve window for `minutes` from now (arming again restarts it), or ends it with `off: true`. While it is armed, any machine that asks is admitted under the key it asks with, and nobody compares that key. The window lives in the leader's memory, so a leader restart or a change of leader ends it. | `minutes` or `off` | `pace-node --enroll-auto-approve` |
| `set_display_name` | Names a member, or clears its name with an empty `name`. | `node`, `name` | `pacectl cluster-name`, `pace-node --cluster-name` |
| `set_cache_budget` | Overrides a member's memory budget, disk budget, or both. | `node`, `memory` and/or `disk` | `pacectl cluster-budget`, `pace-node --cluster-budget` |
| `clear_cache_budget` | Clears one tier's override. | `node`, `which` (`memory` or `disk`) | `pacectl cluster-budget-clear`, `pace-node --cluster-budget-clear` |
| `cordon` / `uncordon` | Takes **this** node's worker out of the fleet (it refuses new compiles and lets the running ones finish) or puts it back. Not persisted, so a restart lifts it. A worker is cordoned only from its own machine, so naming another member is refused, along with where to run it instead. | `node` (this node's own id; omit it) | `pacectl cordon`, `pacectl uncordon` |

The replicated changes (`set_display_name`, `set_cache_budget`, `clear_cache_budget`)
answer `state: replicating`. The leader appended the change, and it has not committed yet.
A change that stops holding before it commits is dropped there: for example, the member is
forgotten, or another rename takes the name first. Read `node_status` or `fleet_overview` to
see what is in force.

**`cluster_forget` is deliberately not offered.** Removing a member revokes its key
permanently, and an MCP session is not where that should be one tool call away. Use
`pacectl cluster-forget <id>` or `pace-node --cluster-forget=<id>`. That is also the remedy
for a machine that was approved and should not have been, since `enrollment_reject` cannot
undo an approval.

## Who may act

**No new credential.** MCP authenticates nobody. The surface is safe only because only this
machine can reach it. The node then acts **as itself**. It forwards each request over the
same proven, sealed `0xFC` connection a node uses for any other verb, proving its own
identity key to the node that answers. When the answer is its own, it calls its own router
in process and states the proof it holds, without a handshake to itself. The leader judges
the forwarding **node**, never the MCP client.

- **Read tools other than `enrollment_status` work on any member**, voter or learner. A node that does not lead reads the
  leader's fleet model by proving its roster key; it needs no dashboard credential. A
  machine ticket, `--fleet-open` or a loopback caller without a proof earns no such read.
- **Acting on the fleet needs operator standing at the leader**: the node must be the leader
  itself or hold a **voter** seat. This applies to `enrollment_status` and to every
  enrollment, naming and budget tool. A learner is refused by name: *"\<id\> lacks operator
  standing (it is not a voter); run it from a voter's machine, or promote this node"*. There
  is no delegation: a learner's session cannot borrow a voter's standing.
- A node the leader cannot identify at all is told so separately, and is never told it is
  "not a voter". That happens when no key it proved is one the leader's roster records.
- `cordon` and `uncordon` act on this machine's own worker and ask the gate `pacectl cordon`
  meets on this machine.
- `node_status` redacts every credential. `--requirepass` reads as redacted. A path to a
  secret file (`--tls-key`, `--dashboard-token-file`, `--cluster-dir`) is shown, because
  the path is not the secret and the file is never read for this.

## Who answers

You always talk to your own node. Each tool decides who holds the answer:

| Asked of | Tools | How |
|---|---|---|
| the current leader | `fleet_overview`, `running_compiles`, `conditions`, `history`, `enrollment_status`, `toolchains`, and the enrollment, naming and budget tools | forwarded to the leader, following `NotLeader` to whoever leads now |
| one named member | `node_status`, each side of `toolchain_diff` | forwarded to that member's `0xFC` endpoint from the applied cluster state, never redirected; this node when no node is named |
| this node | `cordon`, `uncordon`, `explain_admission` | answered here |

A forwarded call is bounded at 10 seconds, redirects included. When the node that holds
the answer cannot be reached, the tool returns an error that says who could not be reached
and why. It never presents a stale or partial answer as complete.

**What is the leader's alone, and what survives a change of leader:**

- **The compile queue** (`running_compiles`) is the leader's. A node that does not lead
  answers `compiles: null`.
- **The requester and toolchain-demand history** is counted by the leader and **not handed
  over at an election**. A new leader starts empty, and `coverage_since_ms` says when it
  started counting. A demotion is a gap, never a run of zeros. The `--history-requesters`
  most recently active keys (default **256**, at most 1024, restart-only) are kept by name,
  and the rest are summed into an `other` row whose `keys_folded` says how many machines or
  toolchains it holds, with totals that stay exact. What names no machine or toolchain is
  counted under `unattributed`.
- **The auto-approve window** is held in the leader's memory and ends with it.
- **Display names and budget overrides are replicated.** Every node agrees on them, and they
  survive a change of leader and a restart of any node.
- **One machine's own series** (`history` with `node`) is what that machine handed the
  leader, found by the identity key its member record holds.

## Display names

A display name is a label that **decides nothing**: no admission, route, key or redirect
reads it. Every surface shows it: the fleet page, `pacectl`, and every MCP answer. Every
tool that takes a node accepts it. A name is at most 64 bytes of text with no control
character. It is unique across the members, compared with **ASCII letters folded to one
case** and every other byte exact, so `Office` and `office` are one name, while `Ä` and `ä`
are two. A name spelled like another member's id is taken as well, because ids are matched
first. A re-admission keeps the name, and a forget removes it with the record. More in
[`--cluster-name`](../tools/pace-node.md#changing-it-while-it-runs).

## Cache budget overrides

`set_cache_budget` records an override in the member's replicated record. The leader records
it even while the member is down. The member applies it **live** when it next sees the
state: its memory tier evicts to the new budget, and its disk tier evicts toward it in
bounded slices while it keeps serving. The capacity the member reports is then re-derived
from what its tiers hold.

- **Sizes are those `--cache-memory` and `--cache-disk` take** (`k`, `m`, `g`). `0` is
  refused, because an override removes no tier. A share of RAM (`25%`) is refused, because
  it would be the RAM of whichever machine read it.
- **The later change wins, and no clock decides which is later.** The override starts with
  no baseline. When the member first applies it, the member reports its own configured value
  as the **baseline**, because only the member knows its configuration. From then on, at
  every start, every accepted reload and every applied state, the member compares its
  configuration in force with that baseline. If they are equal, the override stands. If
  they differ, the member's own edit is the later change: it logs both values at Warn,
  raises the latched condition **`cache-budget-override-dropped`**, asks the leader to drop
  exactly that override, and runs with its own value.
- **It survives restarts.** The override and its baseline live in the replicated state, so a
  restarted member applies the override again unless its configuration moved since the
  baseline.
- `node_status` (`budget_overrides`) and `pacectl node` show each tier's budget in force and
  where it comes from (`cluster` or `configured`), together with the override, its baseline,
  when it was set (by the leader's clock, for display only) and from which member, the bytes
  still being evicted, and why an override is not in force.

The member's side of this is described in
[cache budget overrides, applied by the member](../tools/pace-node.md#cache-budget-overrides-applied-by-the-member).

## When it does not answer

| What you see | Why |
|---|---|
| The node refuses to start, naming `--mcp-listen` | The address is not loopback. Use a bare port. |
| `403` | The request did not come from this machine, or a browser page sent it from an origin that is not a loopback page. |
| `503` with `mcp: too many concurrent reqs` | The surface serves four requests at once, each on a thread of its own, and refuses a fifth rather than queueing it. Each refusal lingers on the accept thread for up to 400 ms before the next connection is accepted. Retry; a slot frees when a request ahead is answered. Each forwarded call has a 10-second deadline, and most tools make one, so they answer within it; `toolchain_diff` makes up to 66, starts none after 30 seconds and answers within 40, or says its budget ran out. A connection that sends nothing gives its slot back after two seconds. A local process that keeps holding connections open, or keeps reconnecting, keeps the surface refusing for as long as it keeps doing so. |
| A tool error saying the MCP surface is stopping | The node is shutting down. A call already forwarded is still awaited, up to its 10-second deadline, and nothing further is asked, so a stop takes at most about 16.5 seconds of the MCP surface's time whatever a client does: once the 15-second drain runs out, every MCP connection still open -- a client still reading a large answer slowly, or one that stopped reading -- is cancelled, and the node logs a warning naming how many (`mcp: stopping, cancelled N connection(s) still open when its 15000 ms drain ended`). Such a client sees its connection end mid-answer. Ask again once the node is back. |
| A tool error naming a node that could not be reached | The leader, or the named member, did not answer within the deadline. The error says which. |
| *"lacks operator standing (it is not a voter)"* | You asked a learner to act. Run the tool from a voter's machine, or promote this node. |
