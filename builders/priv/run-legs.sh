#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# run-legs.sh - runs the selected CI test legs inside the privileged container,
# as root.  priv-ci.sh (the host driver) mounts this read-only at
# /priv/run-legs.sh and invokes it as:
#
#   bash /priv/run-legs.sh /results/legs.tsv
#
# legs.tsv has one tab-separated row per leg:  <leg>\t<build-key>\t<script>
# where <script> is the container path of the leg's bash script (the extracted,
# variable-substituted task commands) and <build-key> names its build tree under
# /build-root (shared across legs by default; D4).
#
# The design (Plan 2.2) keeps the host side out of a second sudo on interrupt:
# this script owns cleanup.  It restores fs.aio-max-nr, kills the running leg's
# process group, and chowns the results and build trees back to the invoking
# user, on normal exit and on a signal, and a repeated Ctrl-C cannot cut it
# short.

set -uo pipefail

LEGS_TSV=${1:?usage: run-legs.sh <legs.tsv>}

AIO_PATH=/proc/sys/fs/aio-max-nr
PRIV_UID=${PRIV_UID:?PRIV_UID not set}
PRIV_GID=${PRIV_GID:?PRIV_GID not set}
PRIV_FRESH=${PRIV_FRESH:-}

# The per-case JUnit dirs the converters write under the build tree; cleared
# before each leg and collected after it.
JUNIT_DIRS=(smbtorture-junit wpts-junit pynfs-junit pike-junit)

# Read and print the pre-run aio-max-nr; it is restored by cleanup (D5).
AIO_BEFORE=$(cat "$AIO_PATH" 2>/dev/null || true)
echo "aio-max-nr: ${AIO_BEFORE:-unknown} (at start)"

leg_pgid=""
RUN_RC=0
SIGNAME=""
cleanup_done=0

cleanup() {
    ((cleanup_done)) && return
    cleanup_done=1
    # L-c: a second, impatient Ctrl-C (sig-proxied again) must not interrupt the
    # grace period or the chown half-way.
    trap '' INT TERM

    # Stop the running leg's whole process group: TERM, up to 60 s, then KILL.
    if [[ -n $leg_pgid ]] && kill -0 "-$leg_pgid" 2>/dev/null; then
        kill -TERM "-$leg_pgid" 2>/dev/null || true
        local i
        for ((i = 0; i < 60; i++)); do
            kill -0 "-$leg_pgid" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "-$leg_pgid" 2>/dev/null || true
    fi

    # Restore aio-max-nr and say what was done (D5).
    if [[ -n $AIO_BEFORE ]]; then
        local after
        after=$(cat "$AIO_PATH" 2>/dev/null || echo "?")
        echo "$AIO_BEFORE" >"$AIO_PATH" 2>/dev/null || true
        echo "aio-max-nr: restored $after -> $AIO_BEFORE"
    fi

    # Hand the results and build trees back to the invoking user, so nothing
    # root-owned is left on the host.
    chown -R "$PRIV_UID:$PRIV_GID" /build-root /results 2>/dev/null || true

    case $SIGNAME in
        INT)  exit 130 ;;
        TERM) exit 143 ;;
        *)    exit "$RUN_RC" ;;
    esac
}
trap cleanup EXIT
trap 'SIGNAME=INT; cleanup' INT
trap 'SIGNAME=TERM; cleanup' TERM

mkdir -p /results
: >/results/summary.txt

leg=""; key=""; script=""; rc=0; result=""
while IFS=$'\t' read -r leg key script; do
    [[ -n $leg ]] || continue
    mkdir -p "/results/$leg" "/build-root/$key"

    # Point /build at this leg's (possibly shared) build tree, as the extracted
    # commands expect (cmake -B /build).
    ln -sfn "/build-root/$key" /build

    # PRIV_FRESH gives an empty build dir per leg, like a fresh qb pod.
    if [[ -n $PRIV_FRESH ]]; then
        find "/build-root/$key" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    fi
    # Clear stale per-case JUnit so one leg's cases cannot be read as another's.
    for d in "${JUNIT_DIRS[@]}"; do
        rm -rf "/build/$d" 2>/dev/null || true
    done

    echo "=== leg $leg (build $key) ==="
    # setsid so the leg is its own process group: the background child of this
    # non-interactive script is not a group leader, so setsid runs in place and
    # $! is the new pgid, which cleanup can signal as a group.  The leg runs
    # under bash -e -o pipefail (an assumption about qb's executor, recorded).
    setsid bash -e -o pipefail "$script" >"/results/$leg/leg.log" 2>&1 &
    leg_pgid=$!
    # Stream the log until the leg exits; wait then collects its status.  A
    # trapped signal interrupts the wait and cleanup takes over.
    tail -n +1 --pid="$leg_pgid" -f "/results/$leg/leg.log" 2>/dev/null || true
    wait "$leg_pgid"
    rc=$?
    leg_pgid=""

    # Collect artifacts.
    for d in "${JUNIT_DIRS[@]}"; do
        [[ -d /build/$d ]] && cp -a "/build/$d" "/results/$leg/" 2>/dev/null || true
    done
    if [[ -f /build/Testing/Temporary/LastTest.log ]]; then
        cp -a /build/Testing/Temporary/LastTest.log "/results/$leg/" 2>/dev/null || true
    fi

    # A ctest.xml with zero <testcase> means the leg selected no tests: a FAIL,
    # not a pass (Plan 2.2).
    if ((rc == 0)) && [[ -f /results/$leg/ctest.xml ]] &&
        ! grep -q '<testcase' "/results/$leg/ctest.xml"; then
        echo "leg $leg: ctest.xml has zero testcases (selected no tests)" >>"/results/$leg/leg.log"
        rc=1
    fi

    if ((rc == 0)); then result=PASS; else result=FAIL; RUN_RC=1; fi
    printf '%s\t%s\t%s\n' "$leg" "$result" "$rc" >>/results/summary.txt
    echo "--- leg $leg: $result (rc=$rc)"
done <"$LEGS_TSV"

exit "$RUN_RC"
