#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# priv-ci.sh - host-side driver for the opt-in privileged Podman test route.
#
# It runs the CI test legs locally, as root in a podman container, the way
# .gitlab-ci.yml runs them, without qb.  It does NOT replace qb; privileged
# runs remain local evidence (see docs/architecture/user-mode-podman.md and
# builders/priv/README.md).
#
# Two sources of truth, no new list (Plan rev 4, section 2):
#   - .devcontainer/quantum/qb-ci.sh chooses the legs and their variables
#     (--verify, then --dry-run --no-builder).
#   - qb.yaml supplies the commands for each leg's task, extracted with awk.
# Neither file is modified.
#
# P1 provides list and dry-run, leg selection, command extraction, the
# destructive-target guards, and the single scoped-root podman wrapper
# (--root/--runroot/--tmpdir/--volumepath + TMPDIR, per R2).  The in-container
# root script is builders/priv/run-legs.sh.
#
# P3 adds the live subcommands image/test/compare/diff/clean.  They go through
# the one scoped wrapper (run_priv_podman / priv_podman_argv) for every
# privileged podman call -- the sudo line is never re-assembled -- and honour the
# two cross-phase contracts:
#   (a) legs.tsv = <leg>\t<build-key>\t<container-script-path>, each leg's script
#       at <run>/<leg>/leg.sh (mounted at /results), the build root at
#       /build-root; this is exactly what run-legs.sh reads.
#   (b) <run>/meta.txt carries image_id, repo_digest, sha, qb_yaml_digest, the
#       keys tools/privci/compare.py reads (compare's .Id gate).
#
# PRIV_DRY_RUN=1 makes image/test/compare/diff/clean preview: they print every
# sudo line and (for test) write the run dir, legs.tsv, per-leg leg.sh and
# meta.txt, but run nothing as root and start no container.  The real privileged
# run needs sudo and is operator-run (P4).

set -uo pipefail

PROG=${0##*/}

# Repo top level: builders/priv/priv-ci.sh -> <root>.  realpath -m normalises
# without requiring the path to exist.
SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) || exit 2
ROOT=$(realpath -m "$SELF_DIR/../..") || exit 2

QB_CI=$ROOT/.devcontainer/quantum/qb-ci.sh
QB_YAML=$ROOT/qb.yaml

# The only qb tasks a privileged leg is allowed to run (Plan 2.1 "Leg
# selection" step 4: refuse any task other than the five ctest/quint/kvm tasks).
ALLOWED_TASKS=(ctest_release ctest_debug ctest_quint_release ctest_quint_debug ctest_kvm)

# --------------------------------------------------------------------------
# Configuration (the facade passes these as environment; see mk/vp-facade.mk).

PRIV_IMAGE=${PRIV_IMAGE:-repository.mdh.quantum.com/chimera/chimera-dev:develop}
PRIV_DIR=${PRIV_DIR:-$HOME/.cache/chimera/podman-priv}
PRIV_LEGS=${PRIV_LEGS:-ctest_*_pynfs ctest_*_smbtorture ctest_*_wpts ctest_*_rest ctest_quint_*}
PRIV_MODE=${PRIV_MODE:-release}
PRIV_SUDO=${PRIV_SUDO-sudo}
PRIV_REF=${PRIV_REF:-}
PRIV_FRESH=${PRIV_FRESH:-}
PRIV_DRY_RUN=${PRIV_DRY_RUN:-}
PRIV_CLEAN_STORE=${PRIV_CLEAN_STORE:-}

# compare / diff inputs.  PRIV_EXPECT and PRIV_REQUIRE default to the fixed
# section-3 files (P2); PRIV_BASE/PRIV_HEAD are the two commits a compare runs.
PRIV_BASE=${PRIV_BASE:-}
PRIV_HEAD=${PRIV_HEAD:-}
PRIV_EXPECT=${PRIV_EXPECT:-$ROOT/builders/priv/section3/expect.txt}
PRIV_REQUIRE=${PRIV_REQUIRE:-$ROOT/builders/priv/section3/require.txt}

# Thresholds (GiB of free space) before image / test will run (Plan 2.1).
PRIV_IMAGE_MIN_GIB=25
PRIV_TEST_MIN_GIB=10

# For the PRIV_DIR guards: the unprivileged podman build dir and the cache that
# holds it must never be inside, or equal to, PRIV_DIR.
PODMAN_FLAVOR=${PODMAN_FLAVOR:-thin}
PODMAN_BUILD_DIR=${PODMAN_BUILD_DIR:-$HOME/.cache/chimera/podman-$PODMAN_FLAVOR}
CHIMERA_CACHE=$HOME/.cache/chimera

# The scoped store's runroot and libpod tmpdir live on the /run tmpfs, keyed by
# user so two people on one host do not collide (R2, c-L1).
PRIV_RUN=/run/chimera-priv-${USER:-$(id -un)}

MARKER=.chimera-podman-priv

# Host-side interrupt/recovery net (Plan 2.1 "Interrupt handling, host side").
# fs.aio-max-nr is read unprivileged before a run so the recovery line can name
# the value to restore even if the in-container cleanup is bypassed (SIGKILL,
# kill -9, a wedged container).  PRIV_TEST_FAKE_CONTAINER lets a dry run simulate
# a pre-existing container to exercise the refusal without sudo or a container.
AIO_PATH=/proc/sys/fs/aio-max-nr
PRIV_TEST_FAKE_CONTAINER=${PRIV_TEST_FAKE_CONTAINER:-}

# --------------------------------------------------------------------------
# Output helpers

say()  { printf '%s\n' "$*"; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
die()  { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }
# A subcommand whose live path belongs to a later phase (P3/P4).
defer() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 3; }

usage() {
    cat <<EOF
Usage: $PROG <subcommand>

Subcommands:
  list        Select the legs (PRIV_LEGS) and print them.  No sudo.
  dry-run     Select + extract, and print each leg's build key, its container
              script, the scoped-root podman wrapper, and the privileged run
              line.  Writes only under \$PRIV_DIR/dry/.  No sudo, any uid.
  image       Pull + stage + load the image into the scoped root store.
  test        Build and run the selected legs in one privileged container.
  compare     Run PRIV_BASE and PRIV_HEAD and compare (tools/privci/compare.py).
  diff A B    Compare two existing run dirs.
  clean       Remove \$PRIV_DIR's own subdirs; with PRIV_CLEAN_STORE=1 also the
              root store and the scoped /run dirs (sudo).

Environment: PRIV_IMAGE PRIV_DIR PRIV_LEGS PRIV_MODE PRIV_SUDO PRIV_REF
             PRIV_FRESH PRIV_DRY_RUN PRIV_CLEAN_STORE
             PRIV_BASE PRIV_HEAD PRIV_EXPECT PRIV_REQUIRE.
PRIV_DRY_RUN=1 makes image/test/compare/diff/clean preview: they print every
sudo line (and 'test' writes the run dir, legs.tsv, leg.sh and meta.txt) and run
nothing as root.  The real privileged run needs sudo and is operator-run (P4).
EOF
}

# --------------------------------------------------------------------------
# Guards

# Refuse an empty PRIV_SUDO (Plan 2.1: "no override"); it would silently turn a
# privileged call into a rootless one.  Checked wherever the wrapper is built,
# including a dry-run preview.
require_sudo_set() {
    [[ -n $PRIV_SUDO ]] || die "PRIV_SUDO is empty; refusing (privileged calls need sudo, and there is no override)"
}

# PRIV_MODE picks the leg set's mode; it is handed to qb-ci.sh -m.
validate_mode() {
    [[ $PRIV_MODE == release || $PRIV_MODE == debug ]] ||
        die "PRIV_MODE must be release or debug (got '$PRIV_MODE')"
}

# True when $2 is $1 or a path under $1.  Both arguments are already normalised.
path_within() {
    local base=$1 p=$2
    [[ $p == "$base" || $p == "$base"/* ]]
}

# PRIV_DIR sanity (Plan 2.1 "Guards").  Applies to every subcommand that uses
# PRIV_DIR, so a dry-run on a dangerous PRIV_DIR is refused before it writes.
validate_priv_dir() {
    local d home
    [[ $PRIV_DIR == /* ]] || die "PRIV_DIR must be an absolute path (got '$PRIV_DIR')"
    d=$(realpath -m "$PRIV_DIR")
    home=$(realpath -m "$HOME")

    [[ $d != / ]]     || die "PRIV_DIR must not be /"
    [[ $d != "$home" ]] || die "PRIV_DIR must not be \$HOME ($home)"

    # PRIV_DIR must not be an ancestor of (or equal to) anything we could wipe
    # by deleting it, nor the repo itself.
    local other
    for other in "$(realpath -m "$PODMAN_BUILD_DIR")" \
                 "$(realpath -m "$CHIMERA_CACHE")" \
                 "$ROOT"; do
        path_within "$d" "$other" &&
            die "PRIV_DIR ($d) must not contain or equal $other"
    done

    # And it must not be inside the repo: with PRIV_REF unset the working-tree
    # export would try to copy the root-owned storage/ back into the context
    # (L-d).
    path_within "$ROOT" "$d" &&
        die "PRIV_DIR ($d) must not be inside the repo ($ROOT)"
}

# The marker proves this PRIV_DIR is ours, so a destructive op cannot be aimed
# at an unrelated directory.  image and the first test create it; test, compare
# and clean require it (Plan 2.1 "Marker file").
require_marker() {
    [[ -e $PRIV_DIR/$MARKER ]] ||
        die "no $MARKER in $PRIV_DIR; run 'image' (or 'test') there first"
}

# uid-0 is refused only for the sudo subcommands (Plan 2.1): a privileged run
# must go through sudo from an unprivileged user, never as an already-root
# process.  list, dry-run and extraction work under any uid (the CI rest leg
# runs the unit test as root).
refuse_root() {
    [[ $(id -u) -ne 0 ]] ||
        die "refusing to run '$1' as root; run it unprivileged and let PRIV_SUDO raise privilege"
}

# --------------------------------------------------------------------------
# The single scoped-root podman wrapper (Plan 2.1 "Fully scoped root store",
# H3 + R2).  Every privileged podman call goes through exactly this prefix.

# Prints the wrapper argv, one token per line (for previews and for building a
# command array).  Pass "-n" to make sudo non-interactive (the host-side checks
# and traps that must never re-prompt); omitted, it is the interactive form the
# operator authorises once per run.
priv_podman_argv() {
    require_sudo_set
    local nflag=${1:-}
    printf '%s\n' \
        "$PRIV_SUDO" ${nflag:+"$nflag"} env "TMPDIR=$PRIV_DIR/tmp" podman \
        --root "$PRIV_DIR/storage" \
        --runroot "$PRIV_RUN/storage" \
        --tmpdir "$PRIV_RUN/libpod" \
        --volumepath "$PRIV_DIR/storage/volumes"
}

# A single shell-quoted line, for a dry-run preview that pastes back.  Forwards
# an optional "-n" to priv_podman_argv.
priv_podman_line() {
    local -a a=()
    mapfile -t a < <(priv_podman_argv "$@")
    fmt_cmd "${a[@]}"
}

# Run a scoped podman call for real.  Used by the live image/test/clean bodies
# in P3/P4; defined here as the one wrapper so there is only ever one.
run_priv_podman() {
    local -a a=()
    mapfile -t a < <(priv_podman_argv)
    "${a[@]}" "$@"
}

# The same wrapper with sudo -n (never prompts): the pre-run and post-run
# container checks in the host-side net.
run_priv_podman_n() {
    local -a a=()
    mapfile -t a < <(priv_podman_argv -n)
    "${a[@]}" "$@"
}

# Print an argv as a line that pastes back into a shell (same contract as
# qb-ci.sh's fmt_cmd).
fmt_cmd() {
    local out="" a
    for a in "$@"; do
        if [[ ! $a =~ ^[A-Za-z0-9_./:=@%+,-]+$ ]]; then
            a=${a//\\/\\\\}
            a=${a//\"/\\\"}
            a=${a//\$/\\\$}
            a=${a//\`/\\\`}
            a="\"$a\""
        fi
        out+="${out:+ }$a"
    done
    printf '%s\n' "$out"
}

# --------------------------------------------------------------------------
# qb.yaml command extraction (Plan 2.1 "Command extraction").
#
# A small, shape-aware reader: two-space task indent, four-space task keys,
# six-space variable entries and list items, eight-space folded continuations.
# It is not a general YAML parser; a shape it cannot read is reported, never
# guessed past.

QB_AWK='
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function indent(s,   n) { n = 0; while (substr(s, n + 1, 1) == " ") n++; return n }

# Unwrap a scalar as YAML flow quoting: '\''..'\'' (\x27\x27 -> \x27) or
# "..\".." (\\\" -> \", \\\\ -> \\); a plain scalar is returned trimmed.
function unquote(s,   q, body) {
    s = trim(s)
    if (s == "") return ""
    q = substr(s, 1, 1)
    if (q == "\x27" && substr(s, length(s), 1) == "\x27") {
        body = substr(s, 2, length(s) - 2)
        gsub(/\x27\x27/, "\x27", body)
        return body
    }
    if (q == "\"" && substr(s, length(s), 1) == "\"") {
        body = substr(s, 2, length(s) - 2)
        gsub(/\\"/, "\"", body)
        gsub(/\\\\/, "\\", body)
        return body
    }
    return s
}

BEGIN { intask = 0; mode = ""; folding = 0; fold = ""; havefold = 0 }

# End a pending folded scalar and emit it.
function flush_fold() {
    if (havefold) { print "CMD\t" fold; havefold = 0; fold = ""; folding = 0 }
}

# Task boundary: a two-space "  name:" line.
/^  [A-Za-z0-9_.-]+:[ \t]*$/ {
    name = $0; sub(/^  /, "", name); sub(/:.*/, "", name)
    if (intask) { flush_fold(); exit }
    if (name == task) { intask = 1; mode = ""; next }
}

intask == 0 { next }

# Inside the task.  A four-space key ends any variables/commands section.
/^    [A-Za-z0-9_]+:/ {
    flush_fold()
    key = $0; sub(/^    /, "", key); sub(/:.*/, "", key)
    rest = $0; sub(/^    [A-Za-z0-9_]+:[ \t]*/, "", rest)
    if (key == "image")       { if (want == "image")      print "VAL\t" unquote(rest) }
    else if (key == "privileged") { if (want == "privileged") print "VAL\t" unquote(rest) }
    if (key == "variables") { mode = "variables"; next }
    if (key == "commands")  { mode = "commands";  next }
    mode = ""
    next
}

# A folded continuation line (deeper than the list item).
folding == 1 {
    if (indent($0) >= foldindent) {
        line = trim($0)
        fold = (fold == "" ? line : fold " " line)
        next
    }
    flush_fold()
}

mode == "variables" && want == "variables" {
    if ($0 ~ /^      #/ || $0 ~ /^[ \t]*$/) next
    if ($0 ~ /^      [A-Za-z0-9_]+:/) {
        k = $0; sub(/^      /, "", k); sub(/:.*/, "", k)
        v = $0; sub(/^      [A-Za-z0-9_]+:[ \t]*/, "", v)
        print "VAR\t" k "\t" unquote(v)
    }
    next
}

mode == "commands" && want == "commands" {
    if ($0 ~ /^      #/ || $0 ~ /^[ \t]*$/) next
    if ($0 ~ /^      - /) {
        item = $0; sub(/^      - /, "", item); item = trim(item)
        if (item == ">" || item == ">-" || item == "|" || item == "|-") {
            folding = 1; havefold = 1; fold = ""; foldindent = 8
            next
        }
        print "CMD\t" unquote(item)
    }
    next
}

END { flush_fold() }
'

# qb_field TASK image|privileged  -> prints the scalar
qb_field() {
    awk -v task="$1" -v want="$2" "$QB_AWK" "$QB_YAML" |
        sed -n 's/^VAL\t//p' | head -1
}

# --------------------------------------------------------------------------
# Build the container script for one leg.
#
# leg_script LEG TASK  with the leg's K=V overrides in the global array VARS_KV.
# Prints the bash the container runs for this leg; sets BUILD_KEY as a side
# effect (global).  Substitution is textual: task variable defaults first, then
# the -V overrides, exactly as qb does.

# leg_script sets two globals (not stdout, which would run in a subshell and
# lose BUILD_KEY and swallow a die): LEG_SCRIPT is the container script,
# BUILD_KEY is "<src>/<task>-<hash of the cmake line>".  BUILD_SRC, when set,
# overrides the <src> segment (a run keys its build dir by the resolved sha, or
# "wt" for the working tree).
BUILD_KEY=""
LEG_SCRIPT=""
BUILD_SRC=""

leg_script() {
    local leg=$1 task=$2
    local img priv
    local -A vars=()
    local -a cmds=() assigned=()

    img=$(qb_field "$task" image)
    priv=$(qb_field "$task" privileged)
    [[ $img == chimera-dev ]] ||
        die "leg $leg: task $task has image '$img', not chimera-dev; refusing"
    [[ $priv == true ]] ||
        die "leg $leg: task $task is not privileged: true; refusing"

    # Task variable defaults, then this leg's -V overrides (textual, as qb does).
    local _tag k v line kv
    while IFS=$'\t' read -r _tag k v; do
        [[ $_tag == VAR ]] || continue
        vars[$k]=$v
    done < <(awk -v task="$task" -v want=variables "$QB_AWK" "$QB_YAML")
    for kv in "${VARS_KV[@]}"; do
        vars[${kv%%=*}]=${kv#*=}
    done

    # The raw commands, in order.
    while IFS=$'\t' read -r _tag line; do
        [[ $_tag == CMD ]] || continue
        cmds+=("$line")
    done < <(awk -v task="$task" -v want=commands "$QB_AWK" "$QB_YAML")
    ((${#cmds[@]})) || die "leg $leg: task $task has no commands"

    # Substitute ${NAME}, flag a leftover one, record shell assignments, append
    # --output-junit to the one ctest line, and capture the cmake line.
    local out="" cmd cmake_line="" ctest_seen=0 rest name a found
    for cmd in "${cmds[@]}"; do
        for k in "${!vars[@]}"; do
            cmd=${cmd//\$\{$k\}/${vars[$k]}}
        done
        # A ${NAME} left over must have been assigned by an earlier command.
        rest=$cmd
        while [[ $rest =~ \$\{([A-Za-z_][A-Za-z0-9_]*)\} ]]; do
            name=${BASH_REMATCH[1]}
            rest=${rest#*"${BASH_REMATCH[0]}"}
            found=0
            for a in "${assigned[@]}"; do [[ $a == "$name" ]] && found=1 && break; done
            ((found)) || die "leg $leg: \${$name} is neither a task variable nor assigned earlier"
        done
        # Record this command's own assignment (checked against later commands).
        if [[ $cmd =~ ^[[:space:]]*(export[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*)= ]]; then
            assigned+=("${BASH_REMATCH[2]}")
        fi
        if [[ $cmd =~ ^[[:space:]]*cmake([[:space:]]|$) ]]; then
            cmake_line=$cmd
        fi
        if [[ $cmd =~ ^[[:space:]]*ctest([[:space:]]|$) ]]; then
            cmd="$cmd --output-junit /results/$leg/ctest.xml"
            ctest_seen=1
        fi
        out+="$cmd"$'\n'
    done

    ((ctest_seen)) || die "leg $leg: task $task has no ctest command to add --output-junit to"
    [[ -n $cmake_line ]] || die "leg $leg: task $task has no cmake command for the build key"

    local src=${BUILD_SRC:-${PRIV_REF:-wt}} hash
    hash=$(printf '%s' "$cmake_line" | sha256sum | cut -c1-12)
    BUILD_KEY="$src/$task-$hash"
    LEG_SCRIPT=$out
}

# --------------------------------------------------------------------------
# Leg selection (Plan 2.1 "Leg selection").

# Fills SEL_LEG, SEL_TASK and SEL_KV (one "K=V\x1f..." string per leg).
SEL_LEG=() SEL_TASK=() SEL_KV=()

verify_gate() {
    [[ -x $QB_CI ]] || die "$QB_CI not found or not executable"
    "$QB_CI" --verify >/dev/null 2>&1 ||
        die "precondition failed: '$QB_CI --verify' did not exit 0 (the leg table does not match .gitlab-ci.yml)"
}

# Parse one record's qb line into VARS_KV + TASK_OUT.  The line is our own
# tool's paste-safe output, so eval reconstructs its argv.
VARS_KV=() TASK_OUT=""
parse_qb_line() {
    local line=$1
    [[ $line == qb\ * ]] || die "unexpected command line (not a qb invocation): $line"
    local -a argv=()
    eval "argv=($line)" || die "could not parse qb line: $line"
    TASK_OUT="" ; VARS_KV=()
    local i
    for ((i = 0; i < ${#argv[@]}; i++)); do
        case ${argv[i]} in
            -v) TASK_OUT=${argv[i+1]:-}; ((i++)) ;;
            -V) VARS_KV+=("${argv[i+1]:-}"); ((i++)) ;;
        esac
    done
    [[ -n $TASK_OUT ]] || die "qb line has no -v <task>: $line"
}

task_allowed() {
    local t=$1 a
    for a in "${ALLOWED_TASKS[@]}"; do [[ $a == "$t" ]] && return 0; done
    return 1
}

leg_already_selected() {
    local leg=$1 x
    for x in "${SEL_LEG[@]}"; do [[ $x == "$leg" ]] && return 0; done
    return 1
}

select_legs() {
    validate_mode
    verify_gate

    # Reset, so a second call (e.g. compare's base then head side) does not
    # accumulate the previous side's legs.
    SEL_LEG=() SEL_TASK=() SEL_KV=()

    local word out rc leg line i kv joined
    # Word-split PRIV_LEGS without pathname expansion, so a glob like
    # ctest_*_pynfs reaches qb-ci.sh --only verbatim, never matched against cwd.
    set -f
    # shellcheck disable=SC2086 # deliberate word-split of the leg patterns
    set -- $PRIV_LEGS
    set +f
    for word in "$@"; do
        # qb-ci.sh dies (non-zero) when a --only glob matches nothing, which is
        # how an unmatched word is caught -- by name.
        out=$("$QB_CI" --dry-run --no-builder --all -a amd64 -m "$PRIV_MODE" --only "$word" 2>/dev/null)
        rc=$?
        ((rc != 0)) && die "PRIV_LEGS word '$word' matched no leg (qb-ci.sh selected nothing)"

        # Parse the records first (step 3): '# <leg>' then the next 'qb ' line,
        # with '#   ...' continuation lines skipped.  Refuse disallowed tasks
        # afterwards (step 4), so a continuation is always skipped cleanly.
        local -a c_leg=() c_qb=()
        leg=""
        while IFS= read -r line; do
            case $line in
                '#   '*) continue ;;                 # continuation: on failure / downloads
                '# '*)   leg=${line#\# } ;;          # start of a record
                qb\ *)
                    [[ -n $leg ]] || die "qb line with no preceding '# <leg>': $line"
                    c_leg+=("$leg"); c_qb+=("$line"); leg=""
                    ;;
                '#'*) ;;                              # other comment, ignore
                '') ;;
                *) die "unexpected line in qb-ci.sh dry-run output: $line" ;;
            esac
        done <<<"$out"
        ((${#c_leg[@]})) || die "PRIV_LEGS word '$word' produced no legs"

        for i in "${!c_leg[@]}"; do
            parse_qb_line "${c_qb[i]}"
            task_allowed "$TASK_OUT" ||
                die "leg '${c_leg[i]}' uses task '$TASK_OUT', which the privileged route does not run (allowed: ${ALLOWED_TASKS[*]})"
            leg_already_selected "${c_leg[i]}" && continue
            SEL_LEG+=("${c_leg[i]}")
            SEL_TASK+=("$TASK_OUT")
            joined=""
            for kv in "${VARS_KV[@]}"; do
                joined+=${joined:+$'\x1f'}$kv
            done
            SEL_KV+=("$joined")
        done
    done

    ((${#SEL_LEG[@]})) || die "no legs selected from PRIV_LEGS='$PRIV_LEGS'"
}

# Load leg $1's overrides into the global VARS_KV array.
load_leg_kv() {
    local joined=${SEL_KV[$1]}
    VARS_KV=()
    [[ -n $joined ]] || return 0
    local old=$IFS
    IFS=$'\x1f' read -r -a VARS_KV <<<"$joined"
    IFS=$old
}

# --------------------------------------------------------------------------
# Run identity and the run directory (Plan 2.1 "Source", "Build key", and the
# meta.txt contract read by tools/privci/compare.py).

# The source commit recorded for a side: the resolved ref, or the working-tree
# HEAD when PRIV_REF is unset.
resolve_sha() {
    local ref=$1
    if [[ -n $ref ]]; then
        git -C "$ROOT" rev-parse "$ref" 2>/dev/null || printf '%s' "$ref"
    else
        git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf 'wt'
    fi
}

# Digest of the qb.yaml the legs came from (recorded; compare.py compares it
# only across tooling versions in a diff A B).
qb_yaml_digest() { sha256sum "$QB_YAML" | cut -c1-16; }

# Read a key: value (or key = value) line from image.lock, if it exists.
image_lock_get() {
    [[ -f $PRIV_DIR/image.lock ]] || return 0
    sed -n "s/^$1[[:space:]]*[:=][[:space:]]*//p" "$PRIV_DIR/image.lock" | head -1
}

# Write <run>/meta.txt with the EXACT keys compare.py reads (image_id,
# repo_digest, sha, qb_yaml_digest); a wrong spelling silently no-ops the .Id
# gate, so these are fixed.
write_meta() {
    local run=$1 sha=$2 iid rd
    iid=$(image_lock_get image_id)
    rd=$(image_lock_get repo_digest)
    {
        printf 'image_id: %s\n'       "${iid:-unknown}"
        printf 'repo_digest: %s\n'    "${rd:-unknown}"
        printf 'sha: %s\n'            "$sha"
        printf 'qb_yaml_digest: %s\n' "$(qb_yaml_digest)"
    } >"$run/meta.txt"
}

# Create the marker on first use in a new or empty PRIV_DIR; refuse a non-empty
# PRIV_DIR that is not ours (Plan 2.1 "Marker file").  Used by image and test.
ensure_marker() {
    [[ -e $PRIV_DIR/$MARKER ]] && return 0
    if [[ ! -d $PRIV_DIR || -z $(ls -A "$PRIV_DIR" 2>/dev/null) ]]; then
        mkdir -p "$PRIV_DIR" && : >"$PRIV_DIR/$MARKER"
        return 0
    fi
    die "no $MARKER in non-empty $PRIV_DIR; refusing (not a priv-ci directory)"
}

# Free GiB on the filesystem holding $1 (its nearest existing ancestor).
free_gib() {
    local p=$1
    while [[ ! -e $p && $p == */* ]]; do p=${p%/*}; [[ -z $p ]] && p=/; done
    df -PBG "${p:-/}" 2>/dev/null | awk 'NR==2 { gsub(/G/, "", $4); print $4+0 }'
}

# --network for the run: bridge only when a ctest_kvm leg is selected (Plan 2.1).
run_network() {
    local i
    for i in "${!SEL_TASK[@]}"; do
        [[ ${SEL_TASK[i]} == ctest_kvm ]] && { printf 'bridge'; return; }
    done
    printf 'none'
}

# Build <run>: the per-leg scripts, legs.tsv (the P1 run-legs.sh reader's
# contract), and meta.txt.  SEL_* must already be populated.
#   build_src  the <src> segment of the build key (sha or "wt")
#   sha        the source commit recorded in meta.txt
prepare_run() {
    local run=$1 build_src=$2 sha=$3
    mkdir -p "$run" || die "cannot create $run"
    local tsv=$run/legs.tsv
    : >"$tsv"
    local i leg task
    BUILD_SRC=$build_src
    for i in "${!SEL_LEG[@]}"; do
        leg=${SEL_LEG[i]} task=${SEL_TASK[i]}
        load_leg_kv "$i"
        leg_script "$leg" "$task"                   # sets LEG_SCRIPT, BUILD_KEY
        mkdir -p "$run/$leg"
        printf '%s' "$LEG_SCRIPT" >"$run/$leg/leg.sh"
        # legs.tsv: <leg>\t<build-key>\t<container-script-path>  (contract a).
        printf '%s\t%s\t%s\n' "$leg" "$BUILD_KEY" "/results/$leg/leg.sh" >>"$tsv"
    done
    BUILD_SRC=""
    write_meta "$run" "$sha"                        # contract b
}

# The podman args (after `podman`) for the privileged run of one side.  The
# wrapper is prepended by run_priv_podman / priv_podman_line, never re-assembled.
PRIV_BUILDROOT=""
container_run_argv() {
    local run=$1 runid=$2 image_id=$3 net
    net="--network=$(run_network)"
    PRIV_BUILDROOT=$PRIV_DIR/build
    printf '%s\n' \
        run --rm --init --privileged --pids-limit=0 --stop-timeout=300 \
        --name "chimera-priv-${USER:-user}-$runid" "$net" \
        -v "$run/context:/context:Z" \
        -v "$PRIV_BUILDROOT:/build-root:Z" \
        -v "$run:/results:Z" \
        -v "$ROOT/builders/priv/run-legs.sh:/priv/run-legs.sh:ro,Z" \
        -e "PRIV_UID=$(id -u)" -e "PRIV_GID=$(id -g)" -e "PRIV_FRESH=$PRIV_FRESH" \
        "$image_id" bash /priv/run-legs.sh /results/legs.tsv
}

# --------------------------------------------------------------------------
# Host-side interrupt/recovery net (Plan 2.1 "Interrupt handling, host side",
# H1).  The in-container run-legs.sh cleanup covers the sig-proxied path; this
# net covers what bypasses it (a --stop-timeout SIGKILL, an operator kill -9, a
# wedged container): it records fs.aio-max-nr unprivileged before the run, so the
# restore value is known, and it prints -- never runs -- the exact recovery lines
# when a container survives.  Every check uses sudo -n, so nothing re-prompts.

# fs.aio-max-nr, read unprivileged (empty if unreadable).
read_aio_before() { cat "$AIO_PATH" 2>/dev/null; }

# Any surviving chimera-priv-$USER-* container, by name (one per line).  A dry
# run never invokes sudo (PRIV_TEST_FAKE_CONTAINER simulates one for testing);
# a real check uses sudo -n and yields nothing if it cannot confirm.
existing_priv_container() {
    if [[ -n $PRIV_TEST_FAKE_CONTAINER ]]; then
        printf '%s\n' "$PRIV_TEST_FAKE_CONTAINER"
        return 0
    fi
    [[ -n $PRIV_DRY_RUN ]] && return 1
    run_priv_podman_n ps -a --filter "name=chimera-priv-$USER" \
        --format '{{.Names}}' 2>/dev/null
}

# Print the EXACT recovery lines (Plan 2.1), with real values substituted.  The
# rm goes through the scoped wrapper; the sysctl restores <before>; the chown
# hands <run-dir> and <build-root> back.  Guidance only -- nothing is run here,
# and no interactive sudo is invoked.
print_recovery() {
    local name=$1 before=$2 run=$3 pfx=${4:-}
    say "${pfx}$(priv_podman_line) rm -f $name"
    say "${pfx}$PRIV_SUDO sysctl -w fs.aio-max-nr=${before:-<before>}"
    say "${pfx}$PRIV_SUDO chown -R $(id -u):$(id -g) $run ${PRIV_BUILDROOT:-$PRIV_DIR/build}"
}

# Run the privileged container in the foreground under an INT/TERM trap that
# never re-prompts for sudo.  After the run returns, or on interrupt, it detects
# a surviving container and prints the recovery lines.
PRIV_INT=0
run_privileged_container() {
    local before=$1 run=$2 runid=$3; shift 3
    local name="chimera-priv-${USER:-user}-$runid" rc surviving
    PRIV_INT=0
    trap 'PRIV_INT=1; say "interrupted: starting nothing new; checking for a surviving container"' INT TERM
    run_priv_podman "$@"
    rc=$?
    trap - INT TERM

    # Post-run / post-interrupt check (sudo -n; never prompts).
    surviving=$(existing_priv_container) || true
    if [[ -n $surviving ]]; then
        say "a privileged container is still present after the run; recover with:"
        print_recovery "$surviving" "$before" "$run"
    elif ((PRIV_INT)); then
        say "interrupted; if a container or a raised aio-max-nr remains, recover with:"
        print_recovery "$name" "$before" "$run"
    fi
    return "$rc"
}

# Run one side end to end: select, prepare the run dir, export the context, and
# run the privileged container (or preview it under PRIV_DRY_RUN).  Prints the
# run dir path on the last line of stdout via RUN_DIR_OUT.
RUN_DIR_OUT=""
do_test_run() {
    local ref=$1 label=${2:-test}
    local sha build_src runid run image_id
    sha=$(resolve_sha "$ref")
    build_src=${ref:+$sha}; build_src=${build_src:-wt}
    runid=$(date -u +%Y%m%dT%H%M%SZ)-$label${ref:+-$sha}
    run=$PRIV_DIR/runs/$runid
    RUN_DIR_OUT=$run

    ensure_marker
    select_legs
    prepare_run "$run" "$build_src" "$sha"

    image_id=$(image_lock_get image_id)
    image_id=${image_id:-<image .Id>}

    PRIV_BUILDROOT=$PRIV_DIR/build        # shared build root (D4), mounted /build-root
    local -a argv=()
    mapfile -t argv < <(container_run_argv "$run" "$runid" "$image_id")

    # Host-side net: record aio-max-nr before the run, and refuse to start a
    # second container when one is already present (Plan 2.1).
    local before existing
    before=$(read_aio_before)
    existing=$(existing_priv_container) || true
    if [[ -n $existing ]]; then
        say "refusing to start: a privileged container is already present ($existing)."
        say "stop it and restore the host before retrying:"
        print_recovery "$existing" "$before" "$run"
        exit 2
    fi

    if [[ -n $PRIV_DRY_RUN ]]; then
        say "# $label (dry) -- nothing runs as root"
        say "# run dir:    $run"
        say "# source:     ${ref:-working tree} (sha $sha)"
        say "# build root: $PRIV_BUILDROOT (shared; D4)"
        say "# legs.tsv, per-leg leg.sh and meta.txt written under $run"
        say "# context export (live): cp -a <source> $run/context"
        say "# privileged run (live):"
        say "#   $(priv_podman_line) $(fmt_cmd "${argv[@]}")"
        say "# host-side interrupt/recovery net (Plan 2.1):"
        say "#   fs.aio-max-nr before = ${before:-unknown} (read unprivileged from $AIO_PATH)"
        say "#   pre/post-run check (never prompts): $(priv_podman_line -n) ps -a --filter name=chimera-priv-$USER --format '{{.Names}}'"
        say "#   INT/TERM trap wraps the run; on a surviving container it prints (runs no interactive sudo):"
        print_recovery "chimera-priv-${USER:-user}-$runid" "$before" "$run" "#     "
        return 0
    fi

    # Live (operator-run, P4): export the context fresh, then run the container
    # through the scoped wrapper under the host-side interrupt/recovery net.  Not
    # exercised in P3 (needs sudo + podman).
    export_context "$run/context" "$ref"

    # The bind SOURCES must exist on the host before the podman run: podman
    # statfs's every -v source at mount time, so run-legs.sh's in-container
    # `mkdir -p /build-root/<key>` is too late for the /build-root mount source.
    # Create the shared build root (D4, mounted /build-root) and the scoped
    # wrapper's TMPDIR here, in the live path -- for both `test` and `compare`,
    # which drives this twice.  ($run/context and $run are already created by
    # export_context and prepare_run.)
    mkdir -p "$PRIV_BUILDROOT" "$PRIV_DIR/tmp" ||
        die "cannot create host build root $PRIV_BUILDROOT or $PRIV_DIR/tmp"

    say "$label: running ${#SEL_LEG[@]} leg(s) in $run (source $sha); fs.aio-max-nr before=${before:-unknown}"
    run_privileged_container "$before" "$run" "$runid" "${argv[@]}"
}

# Export the source into <dest> (Plan 2.1 "Source"): a recursive git archive of
# a ref, or the tracked + untracked-not-ignored working tree.  Live path only.
export_context() {
    local dest=$1 ref=$2
    rm -rf "${dest:?}"
    mkdir -p "$dest"
    if [[ -n $ref ]]; then
        # Recursive archive including submodules, from local objects.
        git -C "$ROOT" archive --format=tar "$ref" | tar -x -C "$dest"
        git -C "$ROOT" submodule --quiet foreach --recursive \
            'git archive --format=tar --prefix="$displaypath/" HEAD | tar -x -C "'"$dest"'"' 2>/dev/null || true
    else
        # Working tree: tracked plus untracked-not-ignored.
        (cd "$ROOT" && git ls-files -z --cached --others --exclude-standard) |
            tar -C "$ROOT" --null -T - -cf - | tar -x -C "$dest"
    fi
}

# --------------------------------------------------------------------------
# Subcommands

cmd_list() {
    validate_priv_dir
    select_legs
    printf '%-28s %-22s %s\n' LEG TASK VARIABLES
    local i vars
    for i in "${!SEL_LEG[@]}"; do
        load_leg_kv "$i"
        vars=$(IFS=' '; echo "${VARS_KV[*]:-}")
        printf '%-28s %-22s %s\n' "${SEL_LEG[i]}" "${SEL_TASK[i]}" "$vars"
    done
    say ""
    say "${#SEL_LEG[@]} leg(s), mode=$PRIV_MODE, image=$PRIV_IMAGE"
}

cmd_dry_run() {
    validate_priv_dir
    require_sudo_set
    select_legs

    local drydir=$PRIV_DIR/dry
    mkdir -p "$drydir" || die "cannot create $drydir"

    say "# priv-ci dry run (nothing runs as root; scripts under $drydir)"
    say "# image:  $PRIV_IMAGE"
    say "# source: ${PRIV_REF:-working tree}"
    say "# scoped podman wrapper:"
    say "#   $(priv_podman_line)"
    say ""

    local tsv=$drydir/legs.tsv
    : >"$tsv"
    local i leg task script
    for i in "${!SEL_LEG[@]}"; do
        leg=${SEL_LEG[i]} task=${SEL_TASK[i]}
        load_leg_kv "$i"
        leg_script "$leg" "$task"             # sets LEG_SCRIPT and BUILD_KEY
        script=$LEG_SCRIPT
        printf '%s' "$script" >"$drydir/$leg.sh"
        printf '%s\t%s\t%s\n' "$leg" "$BUILD_KEY" "/results/$leg/leg.sh" >>"$tsv"
        say "# === $leg  (task $task, build $BUILD_KEY) ==="
        printf '%s' "$script"
        say ""
    done
    say "# legs.tsv -> $tsv"

    # The privileged run line that P4 would execute (preview only).
    say "# privileged run (preview; operator-run in P4):"
    local netns="--network=none"
    local i2
    for i2 in "${!SEL_TASK[@]}"; do
        [[ ${SEL_TASK[i2]} == ctest_kvm ]] && netns="--network=bridge  # a ctest_kvm leg is selected"
    done
    say "#   $(priv_podman_line) run --rm --init --privileged --pids-limit=0 --stop-timeout=300 \\"
    say "#     --name chimera-priv-${USER:-user}-<runid> $netns \\"
    say "#     -v <run>/context:/context:Z -v <buildroot>:/build-root:Z -v <run>:/results:Z \\"
    say "#     -v $ROOT/builders/priv/run-legs.sh:/priv/run-legs.sh:ro,Z \\"
    say "#     -e PRIV_UID=\$(id -u) -e PRIV_GID=\$(id -g) -e PRIV_FRESH \\"
    say "#     <image .Id> bash /priv/run-legs.sh /results/legs.tsv"
}

# clean (Plan 2.1 "clean", c-L1).  Always removes our own user-owned subdirs;
# with PRIV_CLEAN_STORE=1 it also removes the root-owned store ($PRIV_DIR/storage)
# and the R2-scoped /run dirs ($PRIV_RUN/{storage,libpod}) via sudo.  Both bases
# are guarded (${...:?}: never an empty/unset base) and validate_priv_dir has
# already refused /, $HOME and the repo.
cmd_clean() {
    validate_priv_dir
    require_marker
    local sub
    if [[ -n $PRIV_CLEAN_STORE ]]; then
        refuse_root clean
        require_sudo_set
        if [[ -n $PRIV_DRY_RUN ]]; then
            say "# clean (store) preview -- nothing runs:"
            say "  $(priv_podman_line) rmi -a"
            say "  $PRIV_SUDO rm -rf ${PRIV_DIR:?}/storage"
            say "  $PRIV_SUDO rm -rf ${PRIV_RUN:?}/storage ${PRIV_RUN:?}/libpod"
            say "  rm -rf ${PRIV_DIR:?}/{ctx,build,runs,tmp,dry}"
            return 0
        fi
        # Live (operator-run, P4): remove the images, then the root-owned store
        # and the scoped /run dirs.  rmi goes through the scoped wrapper; the
        # rm's are plain sudo (there is no podman wrapper for a filesystem rm).
        run_priv_podman rmi -a 2>/dev/null || true
        "$PRIV_SUDO" rm -rf "${PRIV_DIR:?}/storage"
        "$PRIV_SUDO" rm -rf "${PRIV_RUN:?}/storage" "${PRIV_RUN:?}/libpod"
        say "removed scoped store ${PRIV_DIR}/storage and ${PRIV_RUN}/{storage,libpod}"
    fi
    # Our own user-owned subdirs (always; no sudo).
    for sub in ctx build runs tmp dry; do
        rm -rf "${PRIV_DIR:?}/$sub"
    done
    say "cleaned ${PRIV_DIR}/{ctx,build,runs,tmp,dry}"
}

# The rootless podman args (after `podman`) that run compare.py in the locked
# image (Plan 2.3).  compare/diff are rootless, so there is no sudo wrapper here.
compare_argv() {
    local base=$1 head=$2 image_id=$3
    printf '%s\n' \
        podman run --rm --network=none --cap-drop=ALL --security-opt no-new-privileges \
        --userns=keep-id --user "$(id -u):$(id -g)" \
        -v "$base:/base:Z" -v "$head:/head:Z" \
        -v "$ROOT/tools/privci:/privci:ro,Z" \
        -v "$(dirname "$PRIV_EXPECT"):/expect:ro,Z" \
        -v "$(dirname "$PRIV_REQUIRE"):/require:ro,Z" \
        "$image_id" \
        python3 /privci/compare.py /base /head \
        --expect "/expect/$(basename "$PRIV_EXPECT")" \
        --require "/require/$(basename "$PRIV_REQUIRE")"
}

# image: pull rootless, stage, load into the scoped root store, verify identity,
# self-check the store, and write image.lock (Plan 2.1 "image").  The load, the
# scoped inspect and the self-check go through run_priv_podman.
cmd_image() {
    validate_priv_dir
    refuse_root image
    require_sudo_set
    ensure_marker
    mkdir -p "$PRIV_DIR/tmp"
    local tar=$PRIV_DIR/tmp/image.tar

    if [[ -n $PRIV_DRY_RUN ]]; then
        say "# image (dry) -- nothing runs:"
        say "#   df -h $PRIV_DIR /var/lib/containers /var/tmp   # refuse < ${PRIV_IMAGE_MIN_GIB} GiB free on \$PRIV_DIR"
        say "#   podman pull $PRIV_IMAGE"
        say "#   podman save -o $tar $PRIV_IMAGE"
        say "#   $(priv_podman_line) load -i $tar"
        say "#   rm -f $tar"
        say "#   # assert rootless .Id == scoped .Id (not .Digest); self-check:"
        say "#   $(priv_podman_line) info --format '{{.Store.GraphRoot}} {{.Store.RunRoot}}'"
        say "#   #   graphRoot == $PRIV_DIR/storage, runRoot == $PRIV_RUN/storage, tmpdir $PRIV_RUN/libpod"
        say "#   # write $PRIV_DIR/image.lock (ref, image_id, repo_digest, time)."
        return 0
    fi

    # Live (operator-run, P4): rootless podman + the scoped wrapper.
    local free; free=$(free_gib "$PRIV_DIR")
    [[ -z $free ]] || ((free >= PRIV_IMAGE_MIN_GIB)) ||
        die "image: only ${free} GiB free on $PRIV_DIR's filesystem (need >= ${PRIV_IMAGE_MIN_GIB})"
    df -h "$PRIV_DIR" /var/lib/containers /var/tmp 2>/dev/null || true

    podman pull "$PRIV_IMAGE" || die "image: rootless pull of $PRIV_IMAGE failed"
    podman save -o "$tar" "$PRIV_IMAGE" || die "image: podman save failed"
    run_priv_podman load -i "$tar" || die "image: scoped load failed"
    rm -f "$tar"

    local rootless_id scoped_id
    rootless_id=$(podman image inspect --format '{{.Id}}' "$PRIV_IMAGE")
    scoped_id=$(run_priv_podman image inspect --format '{{.Id}}' "$PRIV_IMAGE")
    [[ -n $scoped_id && $rootless_id == "$scoped_id" ]] ||
        die "image: .Id mismatch (rootless '$rootless_id' vs scoped '$scoped_id')"

    # Self-check the scoped store (R2): graphRoot, runRoot, and the libpod tmpdir.
    local gr rr
    read -r gr rr < <(run_priv_podman info --format '{{.Store.GraphRoot}} {{.Store.RunRoot}}')
    [[ $gr == "$PRIV_DIR/storage" ]] || die "image self-check: graphRoot '$gr' != $PRIV_DIR/storage"
    [[ $rr == "$PRIV_RUN/storage" ]] || die "image self-check: runRoot '$rr' != $PRIV_RUN/storage"
    # L1: podman info does not authoritatively expose the libpod tmp dir, so this
    # [[ -d ]] presence check is the plan's documented fallback (Plan 2.1, R2).
    [[ -d $PRIV_RUN/libpod ]] || die "image self-check: libpod tmpdir $PRIV_RUN/libpod missing"

    local rd
    rd=$(podman image inspect --format '{{range .RepoDigests}}{{.}} {{end}}' "$PRIV_IMAGE")
    {
        printf 'ref: %s\n'         "$PRIV_IMAGE"
        printf 'image_id: %s\n'    "$scoped_id"
        printf 'repo_digest: %s\n' "${rd% }"
        printf 'time: %s\n'        "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$PRIV_DIR/image.lock"
    say "image: loaded $PRIV_IMAGE (.Id $scoped_id) into the scoped store; wrote $PRIV_DIR/image.lock"
}

# test: build and run the selected legs in one privileged container (Plan 2.1).
cmd_test() {
    validate_priv_dir
    refuse_root test
    require_sudo_set
    if [[ -z $PRIV_DRY_RUN ]]; then
        local free; free=$(free_gib "$PRIV_DIR")
        [[ -z $free ]] || ((free >= PRIV_TEST_MIN_GIB)) ||
            die "test: only ${free} GiB free on $PRIV_DIR's filesystem (need >= ${PRIV_TEST_MIN_GIB})"
    fi
    do_test_run "$PRIV_REF" test
}

# compare: run PRIV_BASE and PRIV_HEAD each in its own container (D7), then run
# compare.py rootless in the locked image (Plan 2.3).  Passes its exit status
# through; prints the operator-review banner on exit 2.
cmd_compare() {
    validate_priv_dir
    refuse_root compare
    require_marker
    require_sudo_set
    [[ -n $PRIV_BASE && -n $PRIV_HEAD ]] ||
        die "compare needs PRIV_BASE and PRIV_HEAD (the two commits to compare)"
    [[ -f $PRIV_EXPECT ]]  || die "PRIV_EXPECT not found: $PRIV_EXPECT"
    [[ -f $PRIV_REQUIRE ]] || die "PRIV_REQUIRE not found: $PRIV_REQUIRE"

    do_test_run "$PRIV_BASE" base; local base_run=$RUN_DIR_OUT
    do_test_run "$PRIV_HEAD" head; local head_run=$RUN_DIR_OUT

    local image_id; image_id=$(image_lock_get image_id); image_id=${image_id:-<image .Id>}
    local -a argv=(); mapfile -t argv < <(compare_argv "$base_run" "$head_run" "$image_id")

    if [[ -n $PRIV_DRY_RUN ]]; then
        say ""
        say "# compare (dry): base=$base_run"
        say "#                head=$head_run"
        say "#                expect=$PRIV_EXPECT require=$PRIV_REQUIRE"
        say "# compare run (live; rootless in the locked image, writes head/compare.md):"
        say "#   $(fmt_cmd "${argv[@]}")"
        return 0
    fi

    "${argv[@]}"
    local rc=$?
    ((rc == 2)) &&
        say "OPERATOR REVIEW REQUIRED: expected-transition mismatch(es); see $head_run/compare.md"
    exit "$rc"
}

# diff A B: compare two existing run dirs (Plan 2.1 subcommands), via the same
# rootless compare.py in the locked image.
cmd_diff() {
    [[ $# -eq 2 ]] || die "usage: $PROG diff A B  (two existing run dirs)"
    local a=$1 b=$2
    [[ -d $a ]] || die "diff: '$a' is not a directory"
    [[ -d $b ]] || die "diff: '$b' is not a directory"
    [[ -f $PRIV_EXPECT ]]  || die "PRIV_EXPECT not found: $PRIV_EXPECT"
    [[ -f $PRIV_REQUIRE ]] || die "PRIV_REQUIRE not found: $PRIV_REQUIRE"

    local image_id; image_id=$(image_lock_get image_id); image_id=${image_id:-<image .Id>}
    local -a argv=(); mapfile -t argv < <(compare_argv "$a" "$b" "$image_id")

    if [[ -n $PRIV_DRY_RUN ]]; then
        say "# diff (dry): $a vs $b"
        say "#   $(fmt_cmd "${argv[@]}")"
        return 0
    fi
    "${argv[@]}"
    local rc=$?
    ((rc == 2)) && say "OPERATOR REVIEW REQUIRED: expected-transition mismatch(es); see $b/compare.md"
    exit "$rc"
}

# --------------------------------------------------------------------------

main() {
    [[ $# -ge 1 ]] || { usage >&2; exit 2; }
    local sub=$1; shift
    case $sub in
        list)     cmd_list "$@" ;;
        dry-run)  cmd_dry_run "$@" ;;
        image)    cmd_image "$@" ;;
        test)     cmd_test "$@" ;;
        compare)  cmd_compare "$@" ;;
        diff)     cmd_diff "$@" ;;
        clean)    cmd_clean "$@" ;;
        -h | --help | help) usage ;;
        *) die "unknown subcommand '$sub' (see --help)" ;;
    esac
}

main "$@"
