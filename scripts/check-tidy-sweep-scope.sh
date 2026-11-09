#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The `clang-tidy` sweep still sweeps EVERYTHING somewhere, that somewhere is off
# every change's path, and it still reaches a person.
#
# ## The property, and why nothing else holds it
#
# Every change -- pull request, merge-queue entry, push -- is analysed on the
# lines it touched (`clang-tidy-diff.py`, in `build.yml`'s `clang-tidy` job). What
# makes that sound is the whole-tree sweep in `tidy-nightly.yml`: a finding outside
# a change's lines (an analyser upgrade, a `.clang-tidy` change, a header change
# whose finding lands in an untouched file) is seen there and nowhere else.
#
# Take that sweep away, or let it drift back onto a change's path, and every run in
# this repository stays green while the analysis is permanently narrower -- or
# permanently slower -- than the rulebook says. There is no failing check and no log
# line. So this asserts:
#
#   * a step runs `scripts/tidy-sweep.sh --all`, in a workflow triggered by
#     `schedule:` and by no change event (`push`, `pull_request`, `merge_group`);
#   * no step runs the old per-change `--ci` sweep, which put the include-closure
#     sweep back on the change path;
#   * the sweep has a reader keyed on its STEP and scoped to master, in a job that
#     can write issues: a scheduled run blocks nothing and nobody is obliged to look
#     at it, so a failure that files nothing reaches no one;
#   * the line-scoped step receives the queue entry's own base commit, under the
#     name it reads, so a queue entry does not diff against master instead.
#
# The `--only=` leg (`clang-tidy-windows`) keeps the rules it always had.
#
# ## What is asserted rather than demonstrated
#
# This proves the SHAPE of the workflow, not the behaviour of a run: neither a
# master push nor a queue entry can be staged here. The behaviours it rests on
# are measurements, recorded in the rulebook rather than re-derived. What it can
# do is make the wiring impossible to remove silently, which is the half with no
# other guard -- `tidy-sweep.sh --self-test` covers the scope COMPUTATION, and
# nothing at all covered which events reach it with which scope.
#
# ## bash 3.2, and no `sed` in the self-test
#
# It is in the default ctest set and macOS ships a 2007 `/bin/bash`. No
# `mapfile`, no `declare -A`, no `${var^^}`.
#
# The self-test's broken workflows are GENERATED rather than produced by editing
# a correct one. Editing was the first shape and it was quietly wrong twice over:
# `sed` ranges like `/pat/,+2d` are a GNU extension that BSD sed rejects outright,
# and nothing checked sed's status -- so on macOS the edit failed, the fragment
# came out EMPTY, the check failed because there was no workflow at all, and the
# case printed `ok` for a break it had never staged. A generated fragment cannot
# fail that way, and every case additionally asserts its fragment DIFFERS from the
# correct one, so a knob that does nothing is a failure rather than a pass.
#
# Usage:  scripts/check-tidy-sweep-scope.sh [--self-test]
#
#   --self-test  drive every rule against generated workflows, in both
#                directions, and exit. Needs nothing but bash and awk.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

WorkflowDir=".github/workflows"
DiffWorkflow=".github/workflows/build.yml"
SweepScript="scripts/tidy-sweep.sh"

# The env NAME `scripts/tidy-sweep.sh --ci` actually READS for the queue's base
# commit, taken from that script's own call rather than restated here.
#
# Rule B is about a value REACHING the table, and a value reaches it under a
# name. Matching only the `github.event.merge_group.base_sha` expression cannot
# see the one edit that breaks the wiring while leaving the expression in place
# -- renaming the key -- and that edit fails in the silent direction: the script
# reads an unset variable, `CiScopeFor` takes the `origin/master` fallback, and
# every queue entry diffs against master while this check prints `ok`. A second
# copy of the name here would be a second thing to be wrong, so it is read.
#
# @param 1 The sweep script.
FindQueueBaseEnvName() {
    local line
    line="$(grep -m1 'CiScopeFor "\${GITHUB_EVENT_NAME' "$1")" || return 1
    printf '%s\n' "$line" \
        | grep -o '\${[A-Za-z_][A-Za-z0-9_]*:-}' \
        | sed 's/^\${//; s/:-}$//' \
        | sed -n '3p'
}

# ---------------------------------------------------------------------------
# Find the sweep in a workflow: the step that invokes `scripts/tidy-sweep.sh` as
# the sweep rather than as its own self-test. Emits one
# `<job key>\t<step id>\t<run: block, joined>\t<env: block, joined>` row per such
# step.
#
# The whole `env:` block rather than one key: since #570 the step passes FACTS and
# the decision lives in `CiScopeTable`, so what matters is which facts reach the
# script, not what any one of them is called. Keyed on a single name, renaming the
# variable would empty the field and rule B would report the queue's base as
# deleted -- a red build accusing an author of something they did not do, which
# this file already carries two controls against.
#
# By what it RUNS, never by its name -- a step found by name is a step a rename
# turns into "no step matched", which every rule below would then pass over
# silently. Comment lines are dropped first: build.yml explains itself at length
# and much of what it says names the script, so a scan that reads comments finds
# three sweeps and can vouch for none of them.
#
# The whole `run:` block is joined and only THEN tested for the two markers,
# rather than testing line by line as it is read. Line by line, a step whose
# command sits on the `run:` key line itself was invisible -- reflowing
# `run: >-` plus a continuation into the identical one-liner made this report
# that the sweep had been deleted, which is a red build accusing an author of
# something they did not do.
#
# @param 1 The workflow file.
# ---------------------------------------------------------------------------
# Every fact this check reads out of a workflow, from the SHARED walk.
#
# Three awk programs used to live here, each spelling `build.yml`'s indentation
# as literal columns -- `/^        id:/`, `/^        env:/`, `/^        if:/` at
# eight spaces, `/^          /` at ten, `/^      - /` at six. Every one was right
# for the file as written and said nothing about a file whose jobs sit at another
# depth, which is legal YAML. `scripts/check-tidy-sweep-scope.awk` holds this
# check's half over `scripts/lib/workflow-walk.awk` and documents the records
# (#1456).
#
# A missing awk program is refused BY NAME: a reader that ran over no program is
# not a clean tree, and awk's own complaint names a file rather than the check
# whose verdict just became meaningless.
PaceWalkAwk="$(dirname "${BASH_SOURCE[0]}")/lib/workflow-walk.awk"
PaceScopeAwk="$(dirname "${BASH_SOURCE[0]}")/check-tidy-sweep-scope.awk"
for awkProgram in "$PaceWalkAwk" "$PaceScopeAwk"; do
    if [ ! -f "$awkProgram" ]; then
        echo "check-tidy-sweep-scope: missing awk program ${awkProgram}; no workflow was read, so" >&2
        echo "  this is a refusal and not a clean run. It is expected beside this script." >&2
        exit 2
    fi
done

# @param 1 the workflow  @param 2 which records  @param 3 a step id  @param 4 a job key
WorkflowRead() {
    awk -v want="$2" -v sweepId="${3:-}" -v sweepJob="${4:-}" \
        -f "$PaceWalkAwk" -f "$PaceScopeAwk" "$1" "$1" | cut -f2-
}

FindSweep() {
    WorkflowRead "$1" sweep
}

# The `if:` of every step that reads a named step's conclusion AND names master.
#
# The `if:` is joined across continuation lines for the same reason the `run:` is:
# the shipped expression is over a hundred characters, wrapping it as a folded
# scalar is the natural response to that, and read one line at a time a wrapped
# expression looks exactly like a deleted reader.
#
# @param 1 The workflow file.
# @param 2 The sweep step's id.
# @param 1 workflow  @param 2 the sweep step's `id:`  @param 3 the JOB it sits in
#
# Scoped to the job, and that is not tidiness. `steps.<id>.conclusion` only ever
# resolves within its own job, so a reader in job A says nothing about a sweep in
# job B. Before this took a job, two sweep steps both using `id: sweep` made the
# check report the SECOND one as having a reader -- it had found the FIRST one's.
# The generalisation to N steps is what exposed it: with one sweep the bug was
# unreachable, and it would have shipped a leg whose failures reach nobody while
# the guard said otherwise.
FindReader() {
    WorkflowRead "$1" reader "$2" "$3"
}

# The `permissions:` block of one job, comments dropped.
# @param 1 The workflow file.
# @param 2 The job key.
FindDiff() {
    WorkflowRead "$1" diff
}

# The events a workflow's top-level `on:` names, one per line. Both the block form
# (`on:` then indented keys) and a one-line list or scalar are read.
#
# @param 1 The workflow file.
WorkflowEvents() {
    awk '
        /^on:[ \t]*$/ { inOn = 1; next }
        /^on:[ \t]*[^ \t]/ {
            v = $0; sub(/^on:[ \t]*/, "", v); gsub(/[\[\]," ]/, " ", v)
            n = split(v, ev, " "); for (i = 1; i <= n; i++) if (ev[i] != "") print ev[i]
            next
        }
        inOn && /^[^ \t#]/ { inOn = 0 }
        inOn && /^  [A-Za-z_]+:/ { k = $1; sub(/:.*/, "", k); print k }
    ' "$1"
}

FindJobPermissions() {
    WorkflowRead "$1" perms "" "$2"
}

# ---------------------------------------------------------------------------
# Every rule, over one workflow. Prints its findings; returns non-zero on any.
# @param 1 The workflow file.
CheckWorkflow() {
    local workflow="$1"
    local problems=0
    Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

    if [[ ! -f "$workflow" ]]; then
        echo "  FAIL: $workflow does not exist" >&2
        return 1
    fi

    local sweep sweepCount sweepJob sweepId sweepRun sweepEnv
    sweep="$(FindSweep "$workflow")"
    if [[ -z "$sweep" ]]; then
        Fail "no step in $workflow runs scripts/tidy-sweep.sh as a sweep; this project's only clang-tidy coverage would be gone and every run would stay green"
        return 1
    fi

    sweepCount="$(printf '%s\n' "$sweep" | wc -l | tr -d ' ')"
    echo "  ok: $sweepCount step(s) run the sweep; every rule below is applied to each"

    # EVERY sweep step, not the first. This used to assert there was exactly one,
    # saying it "reasons about one and cannot vouch for the rest" -- which was
    # honest, and became a wall the moment a second analyser leg existed (#858).
    #
    # The rules were always per-STEP, so the generalisation is a loop rather than a
    # rewrite. What must NOT happen is the count assertion being replaced by a
    # count: rules A-D each close a way a sweep reports nothing, so a check that
    # merely tallied steps would pass while the reporting hole it exists to guard
    # reopened -- the fix wearing the defect's clothes.
    while IFS=$'\t' read -r sweepJob sweepId sweepRun sweepEnv; do
        [[ -z "${sweepJob:-}" ]] && continue
        echo "  -- sweep in job \`${sweepJob}\`"

    [[ -n "$sweepId" ]] \
        || Fail "the sweep step has no \`id:\`, so nothing downstream can read its conclusion apart from the whole job's -- which fires on every unrelated infrastructure failure in the job, and is the alarm people mute"

    # -----------------------------------------------------------------------
    # Rule A: the workflow CONSULTS the mapping and restates none of it.
    #
    # Until #570 the scope decision was two GitHub expressions on this step,
    # composed with the shell inside the script, and this rule asserted their
    # SPELLING: `--all` had to be guarded by EXCLUDING the two
    # diff-scoped events rather than by naming `push`, because the inclusive
    # spelling diff-scopes any trigger added to `on:` later, and on master that
    # diff is EMPTY, so the sweep exits 0 having analysed nothing.
    #
    # That property did not go away; it moved somewhere it cannot be spelled
    # backwards. `CiScopeTable` in `scripts/tidy-sweep.sh` lists the diff-scoped
    # events and everything else falls to a default row that sweeps everything,
    # and `scripts/tidy-sweep.sh --self-test` -- which the step above this one
    # already runs -- drives every row, `push`, a tag push and an unforeseen
    # trigger included. So what is left for THIS check is the wiring: that the
    # workflow reaches the table at all, and that it has not started deciding
    # again alongside it. Two decisions that can disagree are worse than one that
    # is coarse.
    # `--only=` is the second legitimate scope source: an explicit set named by a
    # file IN THE REPOSITORY, for a leg that covers a fixed subset rather than a
    # diff (#858's Windows leg reads the units `tidy-blind-spots.txt` says it
    # reaches). It satisfies the same property `--ci` does -- the workflow does not
    # DECIDE the scope, it passes a fact -- so it is accepted, and the branch-on-
    # event rules below still apply to it.
    local fullSweep=""
    if [[ "$sweepRun" == *"--only="* ]]; then
        if [[ "$sweepRun" != *"scripts/"* ]]; then
            Fail "the sweep passes \`--only=\` but the set does not come from a file under \`scripts/\`, so what this leg covers is decided in the workflow rather than somewhere reviewable -- run: $sweepRun"
        else
            echo "  ok: the sweep is explicitly scoped from a file under \`scripts/\`"
        fi
    elif [[ "$sweepRun" == *"--ci"* ]]; then
        Fail "the sweep passes \`--ci\`, which sweeps every unit a change's headers reach and, on a push, the whole tree. Changes are analysed on their changed lines by \`clang-tidy-diff.py\`; the whole tree belongs to the scheduled \`--all\` sweep -- run: $sweepRun"
    elif [[ "$sweepRun" == *"--all"* ]]; then
        fullSweep=yes
        local events
        events="$(WorkflowEvents "$workflow")"
        if ! grep -qx schedule <<< "$events"; then
            Fail "the \`--all\` sweep is in a workflow with no \`schedule:\`, so nothing guarantees it runs at all -- on: $(echo $events)"
        elif grep -qxE 'push|pull_request|pull_request_target|merge_group' <<< "$events"; then
            Fail "the \`--all\` sweep runs on a change event, so the whole-tree sweep is back on a change's path, which is the cost moving it to a schedule removed -- on: $(echo $events)"
        else
            echo "  ok: the whole-tree sweep runs on a schedule and on no change event"
        fi
    else
        Fail "the sweep passes neither \`--all\` nor \`--only=\`, so what it covers is whatever the script defaults to -- run: $sweepRun"
    fi

    local queueBaseEnv
    queueBaseEnv="$(FindQueueBaseEnvName "$SweepScript")"
    if [[ -n "$fullSweep" ]]; then
        echo "  ok: a whole-tree sweep diffs against nothing, so it needs no queue base"
    elif [[ -z "$queueBaseEnv" ]]; then
        # Every way of not knowing escalates, as everywhere else here: a name this
        # check could not read is a rule it cannot apply, not a rule that passed.
        Fail "cannot read from $SweepScript which environment variable its \`--ci\` path takes the queue's base commit from, so this rule cannot be applied at all"
    elif [[ "$sweepEnv" != *"${queueBaseEnv}:"* ]]; then
        Fail "the sweep's \`env:\` sets no \`${queueBaseEnv}\`, which is the name $SweepScript reads the queue's base commit from, so a queue entry falls back to \`origin/master\` and diffs against something other than the entry it is stacked on -- env: ${sweepEnv:-<none>}"
    elif [[ "$sweepEnv" != *"merge_group.base_sha"* ]]; then
        Fail "the sweep's \`env:\` does not pass \`github.event.merge_group.base_sha\`, so a queue entry falls back to \`origin/master\` and diffs against something other than the entry it is stacked on -- env: ${sweepEnv:-<none>}"
    else
        echo "  ok: the queue's own base_sha reaches the table as \`${queueBaseEnv}\`"
    fi

    # -----------------------------------------------------------------------
    # Rule C: the only full sweep left has a reader, keyed on the sweep STEP.
    #
    # Never on the job: over the 18 failed master-push runs in the rulebook's
    # sample the job failed 18 times and the sweep step itself failed none, so a
    # job-keyed alarm is a thing people mute by the second week.
    local reader
    reader="$(FindReader "$workflow" "$sweepId" "$sweepJob")"
    if [[ -z "$reader" ]]; then
        Fail "no step reads \`steps.${sweepId}.conclusion\` on \`refs/heads/master\`. A run off a change's path blocks nothing and notifies nobody -- 18 of 18 failed master-push runs were never once re-run when the full sweep lived there -- so with no reader the check runs, fails, and reaches no one."
    else
        echo "  ok: the sweep has a reader keyed on the sweep step, scoped to master"
    fi

    # -----------------------------------------------------------------------
    # Rule D: and that reader can actually write.
    #
    # A job-level `permissions:` block is exhaustive -- every scope not named
    # drops to none -- so this fails in the direction where the block exists and
    # does not name the scope, which is what happens when somebody adds a scope
    # for a new step and rewrites the list.
    #
    # The job key comes from where the SWEEP was found, never a literal: every
    # other rule follows the step wherever it moves, and a hard-coded
    # `clang-tidy` would leave this one reading the permissions of a job that no
    # longer contains the reporter.
    if [[ -n "$reader" ]]; then
        local perms
        perms="$(FindJobPermissions "$workflow" "$sweepJob")"
        if [[ "$perms" != *"issues:"*"write"* ]]; then
            Fail "job \`${sweepJob}\` does not grant \`issues: write\`, so its report step would be refused by the API and its reader would be decorative -- permissions: ${perms:-<none>}"
        else
            echo "  ok: job \`${sweepJob}\` grants the reader \`issues: write\`"
        fi
    fi
    done < <(printf '%s\n' "$sweep")

    return "$problems"
}

# ---------------------------------------------------------------------------
# The line-scoped step: it exists, and the queue entry's own base reaches it.
#
# The same silent failure rule B guards for a sweep: a queue entry whose base
# does not arrive falls back to another base and analyses lines this entry did not
# write -- or misses ones it did -- while every run stays green. The name is the
# one `tidy-sweep.sh` reads, so both consumers of the fact spell it the same way,
# and the step must both be HANDED it and READ it.
#
# @param 1 The workflow file.
CheckDiff() {
    local workflow="$1"
    local problems=0
    Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

    local diff queueBaseEnv diffJob diffId diffRun diffEnv
    diff="$(FindDiff "$workflow")"
    if [[ -z "$diff" ]]; then
        Fail "no step in $workflow drives \`clang-tidy-diff.py\`, so no change is analysed by clang-tidy on Linux at all and every run stays green"
        return 1
    fi
    queueBaseEnv="$(FindQueueBaseEnvName "$SweepScript")"
    [[ -n "$queueBaseEnv" ]] \
        || { Fail "cannot read from $SweepScript which name carries the queue's base commit"; return 1; }

    while IFS=$'\t' read -r diffJob diffId diffRun diffEnv; do
        [[ -z "${diffJob:-}" ]] && continue
        echo "  -- line-scoped step in job \`${diffJob}\`"
        if [[ "$diffEnv" != *"${queueBaseEnv}:"* ]]; then
            Fail "the step's \`env:\` sets no \`${queueBaseEnv}\`, so a queue entry analyses against some other base -- env: ${diffEnv:-<none>}"
        elif [[ "$diffEnv" != *"merge_group.base_sha"* ]]; then
            Fail "the step's \`env:\` does not pass \`github.event.merge_group.base_sha\` as \`${queueBaseEnv}\` -- env: ${diffEnv:-<none>}"
        elif [[ "$diffRun" != *"\$${queueBaseEnv}"* && "$diffRun" != *"\${${queueBaseEnv}"* ]]; then
            Fail "the step is handed \`${queueBaseEnv}\` and never reads it -- run: $diffRun"
        else
            echo "  ok: the queue's own base_sha reaches the line-scoped step as \`${queueBaseEnv}\`"
        fi
    done < <(printf '%s\n' "$diff")

    return "$problems"
}

# ---------------------------------------------------------------------------
# Self-test
#
# Each rule is driven in BOTH directions over a GENERATED workflow: a check that
# has never been seen to fail is a check that has told you nothing, and one that
# only ever fails is a check that would fail on a correct tree too.
#
# Every knob below changes the generated text, and `Case` asserts that it did --
# a knob that silently does nothing would otherwise stage the correct workflow,
# watch the check pass it, and report `ok` for a break that was never made.

# Knobs. `Reset` restores the shipped shape; a case changes one thing.
gPerms=""; gSweepId=""; gArgs=""; gEnvBase=""; gReaderIf=""; gReader=""; gSweepStep=""
gSchedule=""; gChangeEvent=""; gDiffStep=""; gDiffEnvBase=""; gDiffRead=""
Reset() {
    gPerms="yes"
    gSweepId="sweep"
    gArgs="--all"
    gEnvBase=""
    gReaderIf="\${{ failure() && steps.sweep.conclusion == 'failure' && github.ref == 'refs/heads/master' }}"
    gReader="yes"
    gSweepStep="yes"
    gSchedule="yes"
    gChangeEvent=""
    gDiffStep="yes"
    gDiffEnvBase="MERGE_GROUP_BASE_SHA: \${{ github.event.merge_group.base_sha }}"
    gDiffRead='"$MERGE_GROUP_BASE_SHA"'
}

# The synthetic workflow. Indentation matches build.yml's, because that is what
# the extractors key on.
Fragment() {
    echo "on:"
    if [[ -n "$gSchedule" ]]; then
        echo "  schedule:"
        echo "    - cron: \"17 3 * * *\""
    fi
    [[ -n "$gChangeEvent" ]] && echo "  ${gChangeEvent}:"
    echo "  workflow_dispatch:"
    echo ""
    echo "jobs:"
    echo "  clang-tidy:"
    echo "    name: \"clang-tidy\""
    if [[ -n "$gPerms" ]]; then
        echo "    permissions:"
        echo "      contents: read"
        echo "      issues: write"
    fi
    echo "    steps:"
    echo "      # A comment naming scripts/tidy-sweep.sh --all, which must not be read."
    echo "      - name: \"Self-test the sweep's scope computation\""
    echo "        run: scripts/tidy-sweep.sh --self-test"
    if [[ -n "$gSweepStep" ]]; then
        echo "      - name: \"Sweep\""
        [[ -n "$gSweepId" ]] && echo "        id: ${gSweepId}"
        echo "        env:"
        echo "          TIDY: clang-tidy-22"
        [[ -n "$gEnvBase" ]] && echo "          ${gEnvBase}"
        echo "        run: >-"
        echo "          scripts/tidy-sweep.sh"
        echo "          ${gArgs}"
    fi
    if [[ -n "$gReader" ]]; then
        echo "      - name: \"Report a full sweep that is failing on master\""
        echo "        if: ${gReaderIf}"
        echo "        run: gh issue create"
    fi
    echo "  other:"
    echo "    steps:"
    echo "      - run: true"
}

# The line-scoped step, as build.yml carries it.
DiffFragment() {
    echo "jobs:"
    echo "  clang-tidy:"
    echo "    steps:"
    if [[ -n "$gDiffStep" ]]; then
        echo "      - name: \"Analyse the changed lines\""
        echo "        id: tidy-diff"
        echo "        env:"
        echo "          TIDY_DIFF: clang-tidy-diff.py"
        [[ -n "$gDiffEnvBase" ]] && echo "          ${gDiffEnvBase}"
        echo "        run: |"
        echo "          base=${gDiffRead}"
        echo "          git diff -U0 \"\${base}...HEAD\" | python3 \"\$TIDY_DIFF\" -p1"
    fi
    echo "  other:"
    echo "    steps:"
    echo "      - run: true"
}

SelfTest() {
    local status=0 scratch correct
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    Reset
    correct="${scratch}/correct.yml"
    Fragment > "$correct"

    # @param 1 What is being staged. @param 2 want-pass|want-fail.
    # Reads the knobs as they stand; resets them afterwards.
    Case() {
        local what="$1" want="$2" file="${scratch}/wf.yml" got
        Fragment > "$file"
        if [[ "$want" == "want-fail" ]] && cmp -s "$file" "$correct"; then
            echo "  FAIL ${what}: the case generated the CORRECT workflow, so it staged no break at all"
            status=1
            Reset
            return
        fi
        if [[ "$want" == "want-pass" ]] && ! cmp -s "$file" "$correct"; then
            echo "  FAIL ${what}: expected the shipped shape and generated something else"
            status=1
            Reset
            return
        fi
        if CheckWorkflow "$file" >/dev/null 2>&1; then got=want-pass; else got=want-fail; fi
        if [[ "$got" == "$want" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: wanted ${want#want-}, got ${got#want-}"
            status=1
        fi
        Reset
    }

    echo "TIDY SWEEP SCOPE SELF-TEST"
    Case "the shipped shape passes" want-pass

    gArgs=""
    Case "a sweep with no mode at all" want-fail

    gArgs="--ci"
    Case "the per-change --ci sweep back on a change's path" want-fail

    gArgs="--only=x"
    Case "an --only= set not read from a file under scripts/" want-fail

    gSchedule=""
    Case "the whole-tree sweep with no schedule" want-fail

    gChangeEvent="push"
    Case "the whole-tree sweep on a push again" want-fail

    gChangeEvent="pull_request"
    Case "the whole-tree sweep on every pull request" want-fail

    gChangeEvent="merge_group"
    Case "the whole-tree sweep in the merge queue" want-fail

    gReader=""
    Case "the reader deleted" want-fail

    gReaderIf="\${{ failure() && steps.sweep.conclusion == 'failure' }}"
    Case "the reader no longer scoped to master" want-fail

    gReaderIf="\${{ failure() && github.ref == 'refs/heads/master' }}"
    Case "the reader keyed on the job, not the step" want-fail

    gPerms=""
    Case "issues: write withdrawn" want-fail

    gSweepId=""
    Case "the sweep step's id removed" want-fail

    gSweepStep=""
    Case "the sweep step gone entirely" want-fail

    # The line-scoped step, over its own generated workflow.
    local diffCorrect="${scratch}/diff-correct.yml"
    Reset
    DiffFragment > "$diffCorrect"
    DiffCase() {
        local what="$1" want="$2" file="${scratch}/diff.yml" got
        DiffFragment > "$file"
        if [[ "$want" == "want-fail" ]] && cmp -s "$file" "$diffCorrect"; then
            echo "  FAIL ${what}: the case generated the CORRECT workflow, so it staged no break at all"
            status=1; Reset; return
        fi
        if CheckDiff "$file" >/dev/null 2>&1; then got=want-pass; else got=want-fail; fi
        if [[ "$got" == "$want" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: wanted ${want#want-}, got ${got#want-}"
            status=1
        fi
        Reset
    }
    DiffCase "the shipped line-scoped step passes" want-pass

    gDiffStep=""
    DiffCase "the line-scoped step deleted" want-fail

    gDiffEnvBase=""
    DiffCase "the queue's own base_sha never reaching the line-scoped step" want-fail

    gDiffEnvBase="QUEUE_BASE_SHA: \${{ github.event.merge_group.base_sha }}"
    DiffCase "the queue's base carried under a name the step does not read" want-fail

    gDiffEnvBase="MERGE_GROUP_BASE_SHA: \${{ github.sha }}"
    DiffCase "the queue base name carrying some other commit" want-fail

    gDiffRead='"origin/master"'
    DiffCase "the queue base handed over and never read" want-fail

    # And the two shapes that must NOT be mistaken for a break. Both are pure
    # reflows of the shipped expressions, and both used to fail: the run: block
    # collapsed to one line, and the reader's if: wrapped as a folded scalar.
    # A red build accusing an author of deleting the sweep they only reformatted
    # is worse than no check, because it is acted on.
    ReflowedRun() {
        Fragment | awk '
            /^        run: >-$/ { inRun = 1; line = "        run: scripts/tidy-sweep.sh"; next }
            inRun && /^          scripts\/tidy-sweep\.sh$/ { next }
            inRun && /^          / { sub(/^ +/, "", $0); print line " " $0; inRun = 0; next }
            { print }
        '
    }
    local file="${scratch}/wf.yml"
    ReflowedRun > "$file"
    if cmp -s "$file" "$correct"; then
        echo "  FAIL a one-line run: is not a deleted sweep: the reflow changed nothing"
        status=1
    elif CheckWorkflow "$file" >/dev/null 2>&1; then
        echo "  ok   a one-line run: is not a deleted sweep"
    else
        echo "  FAIL a one-line run: is not a deleted sweep: the check rejected a pure reflow"
        status=1
    fi

    # The reader's `if:` written as a FOLDED scalar. Its own case below asserts the
    # check accepts one; the walk neuter reuses it, because `WfBlockKey` is reached
    # only by a CONTINUATION line and the shipped fragment writes that `if:` on one
    # line -- a neuter is evidence only over input that reaches the line it disabled.
    FoldedReaderIf() {
        gReaderIf=""
        Fragment | awk '
        /^        if: $/ { print "        if: >-"
                           print "          ${{ failure() && steps.sweep.conclusion == '\''failure'\''"
                           print "          && github.ref == '\''refs/heads/master'\'' }}"
                           next }
        { print }
      '
    }

    Reset
    FoldedReaderIf > "$file"
    if CheckWorkflow "$file" >/dev/null 2>&1; then
        echo "  ok   a folded reader if: is not a deleted reader"
    else
        echo "  FAIL a folded reader if: is not a deleted reader: the check rejected a pure reflow"
        status=1
    fi
    Reset

    # The verdict must come from the SHARED walk, which is a different claim from
    # "the check refuses". Every case above would pass just as well if this script
    # had kept a private reader beside the shared one. So the library is copied,
    # one line disabled, and `PaceWalkAwk` pointed at the copy for one case.
    #
    # @param 1 what the line holds up  @param 2 a sed expression disabling it
    # @param 3 want-pass|want-fail  @param 4 which generator stages the workflow
    WalkNeuter() {
        local what="$1" expr="$2" want="$3" stage="${4:-Fragment}"
        local file="${scratch}/wf.yml" neutered got realWalk
        Reset
        "$stage" > "$file"
        neutered="${scratch}/neutered-walk.awk"
        sed "$expr" "$PaceWalkAwk" > "$neutered"
        if cmp -s "$neutered" "$PaceWalkAwk"; then
            echo "  FAIL ${what}: the sed expression neutered no line, so the case stages nothing"
            status=1
            return
        fi
        realWalk="$PaceWalkAwk"
        PaceWalkAwk="$neutered"
        if CheckWorkflow "$file" >/dev/null 2>&1; then got=want-pass; else got=want-fail; fi
        PaceWalkAwk="$realWalk"
        if [[ "$got" == "$want" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: wanted ${want#want-}, got ${got#want-}"
            status=1
        fi
    }

    WalkNeuter "neutering the shared walk's step boundary makes this check REFUSE, so its verdict comes from that walk" \
        '/WorkflowFlushStep(); WorkflowResetStep(); stepItem = 1/s/^/#/' want-fail
    WalkNeuter "neutering the shared walk's WfBlockKey makes this check REFUSE over a FOLDED reader if:, so that if: is joined through that field" \
        's/^        WfBlockOwner = ind; WfBlockKey = key$/        WfBlockOwner = ind/' want-fail FoldedReaderIf
    # The same folded shape with the library untouched must PASS, or the case above
    # says only that a folded `if:` is refused for some other reason entirely.
    WalkNeuter "a FOLDED reader if: passes over an untouched copy of the shared walk, so the case above names its cause" \
        '1s|^# SPDX|# neutered-nothing\n# SPDX|' want-pass FoldedReaderIf
    # And the control, without which the two above prove only that a damaged
    # library breaks something: an UNTOUCHED copy at a different path must pass.
    WalkNeuter "a copy of the shared walk with only a comment changed still PASSES, so the two cases above name their cause" \
        '1s|^# SPDX|# neutered-nothing\n# SPDX|' want-pass
    Reset

    [[ "$status" -eq 0 ]] && echo "TIDY SWEEP SCOPE SELF-TEST PASSED"
    return "$status"
}

# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--self-test" ]]; then
    SelfTest
    exit $?
elif [[ $# -gt 0 ]]; then
    echo "usage: $0 [--self-test]" >&2
    exit 2
fi

problems=0
fullSweeps=0
for workflow in "$WorkflowDir"/*.yml; do
    sweeps="$(FindSweep "$workflow")"
    [[ -n "$sweeps" ]] || continue
    echo "check-tidy-sweep-scope: ${workflow}"
    CheckWorkflow "$workflow" || problems=1
    if [[ "$(cut -f3 <<< "$sweeps")" == *"--all"* ]]; then
        fullSweeps=$((fullSweeps + 1))
    fi
done
if [[ "$fullSweeps" -eq 0 ]]; then
    echo "  FAIL: no workflow under ${WorkflowDir} runs \`scripts/tidy-sweep.sh --all\`, so nothing reads the whole tree and a finding outside a change's lines is never seen" >&2
    problems=1
fi
echo "check-tidy-sweep-scope: ${DiffWorkflow} (line-scoped)"
CheckDiff "$DiffWorkflow" || problems=1

if [[ "$problems" -ne 0 ]]; then
    echo "check-tidy-sweep-scope: the tidy sweep's coverage would narrow silently" >&2
    exit 1
fi
echo "check-tidy-sweep-scope: the whole tree is swept on a schedule with a reader, and every change on its lines"
