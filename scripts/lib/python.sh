# SPDX-License-Identifier: Apache-2.0
#
# The one way a script finds a Python interpreter that RUNS (#1734).
#
# `python3` on PATH is not evidence of a Python. On Windows it is, by default, the Microsoft
# Store's app-execution alias (`%LOCALAPPDATA%\Microsoft\WindowsApps\python3.exe`): a stub that
# prints "Python was not found" and exits 49 -- even on a host with a real interpreter installed,
# which then reads as a missing prerequisite or, worse, as the script's own check failing.
# `command -v python3` answers yes to the stub.
#
# So a candidate is ASKED, never trusted for its name: it must print a token the probe chose.
# Order: `PACE_PYTHON` when set (the operator's answer wins), then `py -3` (the Windows launcher,
# which no stub shadows), then `python3`, then `python`.
#
# bash 3.2: an array and a function, nothing newer.
#
# Usage:
#   . "<scripts>/lib/python.sh"
#   if pace_python_resolve; then pace_python -c '...'; else echo "SKIP: $PACE_PYTHON_REFUSAL"; fi

PACE_PYTHON_CMD=()
PACE_PYTHON_REFUSAL=""

# Whether the command in "$@" is a Python that runs: it must print the probe's own token.
pace_python_answers() {
    local answer
    answer="$("$@" -c 'print("pace-python-probe-7c1e")' 2> /dev/null)" || return 1
    [ "${answer%$'\r'}" = "pace-python-probe-7c1e" ]
}

# Fill PACE_PYTHON_CMD with the first candidate that runs. Returns 1, with PACE_PYTHON_REFUSAL
# saying what was tried, when none does.
pace_python_resolve() {
    PACE_PYTHON_CMD=()
    if [ -n "${PACE_PYTHON:-}" ]; then
        if pace_python_answers "$PACE_PYTHON"; then
            PACE_PYTHON_CMD=("$PACE_PYTHON")
            return 0
        fi
        PACE_PYTHON_REFUSAL="PACE_PYTHON=$PACE_PYTHON does not run a Python"
        return 1
    fi
    if command -v py > /dev/null 2>&1 && pace_python_answers py -3; then
        PACE_PYTHON_CMD=(py -3)
        return 0
    fi
    local candidate
    for candidate in python3 python; do
        if command -v "$candidate" > /dev/null 2>&1 && pace_python_answers "$candidate"; then
            PACE_PYTHON_CMD=("$candidate")
            return 0
        fi
    done
    PACE_PYTHON_REFUSAL="no usable Python (tried py -3, python3, python; a Windows Store alias is not one)"
    return 1
}

# Run the resolved interpreter. Call pace_python_resolve first.
pace_python() {
    "${PACE_PYTHON_CMD[@]}" "$@"
}
