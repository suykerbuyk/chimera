#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# qb-ci.sh - run the qb test suite split into legs the way .gitlab-ci.yml runs
# it, instead of `qb -o <tag> check` in a single pass.
#
# `qb check` runs ctest_release and ctest_debug as one pod each over the whole
# extended suite, and ctest_kvm as one pod over a whole guest image, so it
# takes as long as the slowest of those.  The pipeline shards them: four ctest
# groups per mode (pynfs, smbtorture, wpts, the rest) and two KVM shards per
# mode, each its own qb invocation.  This script does the same from the
# devcontainer: it builds chimera-dev once under the output tag, starts every
# leg as a separate `qb -i chimera/<tag>` process so they all test that image,
# and summarizes the results.
#
# The leg table mirrors the pipeline's jobs and matrix cells.  Every run (and
# --verify) checks it against .gitlab-ci.yml, so a pipeline change that is not
# carried over here stops the run instead of silently testing something else.
#
# Run with --help for the options.

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd) || exit 2
SELF=.devcontainer/quantum/$(basename "${BASH_SOURCE[0]}")
CI_FILE=.gitlab-ci.yml
QB_PROJECT=chimera
KNOWN_ARCHS=(amd64 arm64)

usage() {
    cat <<EOF
Usage: $SELF [options]

Build chimera-dev once, then run the qb test legs in parallel, split the way
.gitlab-ci.yml splits them.  By default that is the legs the merge train runs.

  -o, --tag TAG      qb output tag (default: \$USER-<branch>)
  -a, --arch ARCH    amd64 or arm64; repeat for both (default: both, as CI).
                     Only static_analysis has arm64 legs.
  -m, --mode MODE    release or debug; repeat for both (default: both).
                     Legs without a mode (syntax, coverage) are unaffected.
      --all          add the nightly-only legs and the coverage legs
      --no-kvm       skip the ctest_kvm legs
      --no-static    skip static_analysis
      --no-syntax    skip syntax
      --no-quint     skip ctest_quint (and quint_coverage)
      --no-ctest     skip the sharded ctest legs (and pynfs_diskfs, coverage,
                     smbtorture_explore)
      --only GLOB    keep only the legs whose name matches GLOB; repeatable,
                     applied after the options above (so a nightly leg still
                     needs --all)
      --no-builder   reuse chimera-dev:<tag> from an earlier run instead of
                     rebuilding it
  -j, --jobs N       run at most N legs at once (default: 0 = all at once,
                     like a pipeline)
      --retry N      retry a failed leg up to N times (default: 0; CI uses 1)
      --log-dir DIR  where the logs go (default:
                     \${CHIMERA_BUILD_DIR:-/tmp}/qb-ci/<tag>/<UTC timestamp>)
  -l, --list         print the selected legs and exit
  -n, --dry-run      print the qb command lines and exit
      --verify       check the leg table against $CI_FILE and exit
  -h, --help         show this help

Examples:
  $SELF -a amd64 --no-kvm
  $SELF --no-builder --only ctest_debug_pynfs
  $SELF --all --only 'kvm_*' -m release
EOF
}

say() {
    printf '[%(%H:%M:%S)T] %s\n' -1 "$*"
}

warn() {
    printf '%s: warning: %s\n' "$SELF" "$*" >&2
}

die() {
    printf '%s: %s\n' "$SELF" "$*" >&2
    exit 2
}

dur() {
    printf '%dm%02ds' $(($1 / 60)) $(($1 % 60))
}

# ---------------------------------------------------------------------------
# The leg table

declare -A JOB_RULES=() JOB_SET=() JOB_FAMILY=()

# The pipeline jobs mirrored here, with the rules anchor each carries in
# .gitlab-ci.yml, the leg set it belongs to, and the family its --no-* option
# names.  "default" is what the merge train runs; --all adds "all".
#
# coverage and quint_coverage do run in the train, but they are informational
# (their qb tasks swallow test failures) and ctest_coverage is the whole suite
# in one unsharded pod -- the wait this script exists to avoid -- so here they
# come with --all, alongside the nightly-only jobs.
load_jobs() {
    local job rules set family

    while read -r job rules set family; do
        [[ -z $job || $job == \#* ]] && continue
        JOB_RULES[$job]=$rules
        JOB_SET[$job]=$set
        JOB_FAMILY[$job]=$family
    done <<'EOF'
# CI job            rules          set      family
syntax              rules_fast     default  syntax
static_analysis     rules_fast     default  static
ctest               rules_full     default  ctest
ctest_quint         rules_fast     default  quint
ctest_kvm           rules_full     default  kvm
coverage            rules_full     all      ctest
quint_coverage      rules_fast     all      quint
ctest_explore       rules_explore  all      ctest
ctest_kvm_images    rules_explore  all      kvm
smbtorture_explore  rules_explore  all      ctest
EOF
}

# The rest of .gitlab-ci.yml, which the drift check skips: chimera-dev is the
# build every run starts with, and the other jobs build packages, publish, or
# talk to JIRA.  The last four are top-level keys that are not jobs.
CI_IGNORED=(chimera-dev 'builder_*' 'build_*' jira_validation jira_update
            release dev_branch default variables workflow stages)

L_NAME=() L_JOB=() L_TASK=() L_ARCH=() L_MODE=() L_SET=() L_FAMILY=()
L_VARS=() L_ASSETS=() L_REPORT=() L_REPORT_ASSETS=() L_CELL=()

# One row per matrix cell of the jobs above (one per job for the jobs without
# a matrix).  Columns: CI job, MODE, ARCH, GROUP or KVM_NAME, PLATFORM, and the
# ctest selector (CTEST_FILTER or KVM_FILTER), which runs to the end of the
# line.  "-" marks a column the job does not use.  The selector reaches qb
# verbatim, so its regexes stay single-quoted for the reason .gitlab-ci.yml
# gives: qb pastes it into the ctest command line textually.  The comments
# there explain each group and exclusion; they are not repeated here.
load_legs() {
    local job mode arch name platform filter

    while read -r job mode arch name platform filter; do
        [[ -z $job || $job == \#* ]] && continue
        add_leg "$job" "$mode" "$arch" "$name" "$platform" "$filter"
    done <<'EOF'
# CI job          MODE     ARCH   GROUP/KVM_NAME      PLATFORM        selector
syntax            -        -      -                   -               -
static_analysis   release  amd64  -                   -               -
static_analysis   release  arm64  -                   -               -
static_analysis   debug    amd64  -                   -               -
static_analysis   debug    arm64  -                   -               -
ctest             release  amd64  pynfs               -               -L '^pynfs$' -E diskfs_
ctest             debug    amd64  pynfs               -               -L '^pynfs$' -E diskfs_
ctest             release  amd64  smbtorture          -               -L '^smbtorture$' -E 'smbtorture_smb2_(compound|replay_channel_sequence|replay_replay_commands)_(linux|io_uring)$'
ctest             debug    amd64  smbtorture          -               -L '^smbtorture$' -E 'smbtorture_smb2_(compound|replay_channel_sequence|replay_replay_commands)_(linux|io_uring)$'
ctest             release  amd64  wpts                -               -L '^wpts$'
ctest             debug    amd64  wpts                -               -L '^wpts$'
ctest             release  amd64  rest                -               -LE '^pynfs$|^smbtorture$|^wpts$|^kvm$' -E 'libevpl/io_uring/(hello_world_stream_tcp|hello_world_connected_msg_tcp|ping_pong_stream_tcp|ping_pong_msg_tcp|bulk_msg_tcp|bulk_stream_tcp)$'
ctest             debug    amd64  rest                -               -LE '^pynfs$|^smbtorture$|^wpts$|^kvm$' -E 'libevpl/io_uring/(hello_world_stream_tcp|hello_world_connected_msg_tcp|ping_pong_stream_tcp|ping_pong_msg_tcp|bulk_msg_tcp|bulk_stream_tcp)$'
ctest_quint       release  amd64  -                   -               -
ctest_quint       debug    amd64  -                   -               -
ctest_kvm         Release  amd64  ubuntu2604          ubuntu2604      -E '/(nfstest|ltp)/'
ctest_kvm         Debug    amd64  ubuntu2604          ubuntu2604      -E '/(nfstest|ltp)/'
ctest_kvm         Release  amd64  ubuntu2604-nfstest  ubuntu2604      -R '/(nfstest|ltp)/' -E 'nfstest_alloc_diskfs'
ctest_kvm         Debug    amd64  ubuntu2604-nfstest  ubuntu2604      -R '/(nfstest|ltp)/' -E 'nfstest_alloc_diskfs'
coverage          -        amd64  -                   -               -
quint_coverage    -        amd64  -                   -               -
ctest_explore     release  amd64  pynfs_diskfs        -               -L '^pynfs$' -R diskfs_
ctest_explore     debug    amd64  pynfs_diskfs        -               -L '^pynfs$' -R diskfs_
ctest_kvm_images  Release  amd64  ubuntu2404          ubuntu2404      -E '/(nfstest|ltp)/'
ctest_kvm_images  Debug    amd64  ubuntu2404          ubuntu2404      -E '/(nfstest|ltp)/'
ctest_kvm_images  Release  amd64  ubuntu2404-nfstest  ubuntu2404      -R '/(nfstest|ltp)/' -E 'nfstest_alloc_diskfs'
ctest_kvm_images  Debug    amd64  ubuntu2404-nfstest  ubuntu2404      -R '/(nfstest|ltp)/' -E 'nfstest_alloc_diskfs'
ctest_kvm_images  Release  amd64  ubuntu2404_hwe      ubuntu2404_hwe  -E '/(nfstest|ltp)/'
ctest_kvm_images  Debug    amd64  ubuntu2404_hwe      ubuntu2404_hwe  -E '/(nfstest|ltp)/'
ctest_kvm_images  Release  amd64  ubuntu2204_hwe      ubuntu2204_hwe  -E '/(nfstest|ltp)/'
ctest_kvm_images  Debug    amd64  ubuntu2204_hwe      ubuntu2204_hwe  -E '/(nfstest|ltp)/'
smbtorture_explore -       amd64  -                   -               -
EOF
}

# A drift-check record: the CI job and its matrix variables, keys in sorted
# order, tab separated (the selectors contain spaces and |).
cell() {
    local out="CELL"$'\t'"$1"

    shift
    while (($#)); do
        out+=$'\t'"$1=$2"
        shift 2
    done
    printf '%s' "$out"
}

# Turn one table row into a leg: its name, the qb task and variables it runs
# with (as that job's script line in .gitlab-ci.yml builds them), the assets
# the job downloads, and the record the drift check compares.
add_leg() {
    local job=$1 mode=$2 arch=$3 name=$4 platform=$5 filter=$6
    local leg task vars="" assets="" report="" report_assets="" rec

    [[ -n ${JOB_SET[$job]:-} ]] || die "leg table: '$job' is not in the job table"
    case $job in
        syntax)
            # The pipeline's syntax job passes no -a, so it runs on every arch
            # the task declares; here that is every requested arch.
            leg=syntax task=syntax arch='*'
            rec=$(cell "$job")
            ;;
        static_analysis)
            leg=static_analysis_${mode}_$arch task=static_analysis_$mode
            report=${task}_report
            report_assets="$mode/scan-report/$arch/report.tar.gz $mode/scan-report/$arch/scan-build-output.txt"
            rec=$(cell "$job" ARCH "$arch" MODE "$mode")
            ;;
        ctest | ctest_explore)
            leg=ctest_${mode}_$name task=ctest_$mode
            vars="CTEST_FILTER=$filter"
            rec=$(cell "$job" ARCH "$arch" CTEST_FILTER "$filter" GROUP "$name" MODE "$mode")
            ;;
        ctest_quint)
            leg=ctest_quint_$mode task=ctest_quint_$mode
            rec=$(cell "$job" ARCH "$arch" MODE "$mode")
            ;;
        ctest_kvm | ctest_kvm_images)
            leg=kvm_${mode,,}_$name task=ctest_kvm
            vars="MODE=$mode"$'\x1f'"PLATFORM=$platform"$'\x1f'"KVM_FILTER=$filter"
            rec=$(cell "$job" ARCH "$arch" KVM_FILTER "$filter" KVM_NAME "$name" MODE "$mode" PLATFORM "$platform")
            ;;
        coverage)
            leg=coverage task=ctest_coverage
            assets="coverage_report/$arch/report.tar.gz coverage_report/$arch/coverage-report.txt"
            rec=$(cell "$job")
            ;;
        quint_coverage)
            # No pr.diff: the pipeline writes one into the checkout, where it
            # would ride into every other leg's context as well.  Without it
            # the report just leaves out its patch-coverage section.
            leg=quint_coverage task=ctest_quint_coverage
            assets="quint_coverage_report/$arch/coverage-report.md quint_coverage_report/$arch/report.tar.gz"
            rec=$(cell "$job")
            ;;
        smbtorture_explore)
            leg=smbtorture_explore task=smbtorture_explore
            assets="smbtorture_explore/$arch/results.tar.gz smbtorture_explore/$arch/smbtorture-report.txt"
            rec=$(cell "$job")
            ;;
        *)
            die "leg table: no qb command known for CI job '$job'"
            ;;
    esac

    L_NAME+=("$leg")
    L_JOB+=("$job")
    L_TASK+=("$task")
    L_ARCH+=("$arch")
    L_MODE+=("${mode,,}")
    L_SET+=("${JOB_SET[$job]}")
    L_FAMILY+=("${JOB_FAMILY[$job]}")
    L_VARS+=("$vars")
    L_ASSETS+=("$assets")
    L_REPORT+=("$report")
    L_REPORT_ASSETS+=("$report_assets")
    L_CELL+=("$rec")
}

# ---------------------------------------------------------------------------
# The drift check

# Reads .gitlab-ci.yml and prints the records the table produces: a JOB line
# per top-level key with its rules anchor, and a CELL line per matrix cell (the
# cross product of each matrix entry, keys sorted), or one bare CELL line for a
# job without a matrix.  This is not a YAML parser.  It knows the shapes the
# file uses (two-space job indent, `- KEY: [ a, "b" ]` matrix entries) and no
# more; a shape it cannot read shows up as drift rather than as a pass.
CI_AWK='
function trim(s) {
    sub(/^[ \t]+/, "", s)
    sub(/[ \t]+$/, "", s)
    return s
}

# Split a flow sequence ([ a, "b c" ]) or a bare scalar into out[1..n].
function split_list(s, out,    n, i, c, inq, cur) {
    s = trim(s)
    if (s ~ /^\[.*\]$/)
        s = substr(s, 2, length(s) - 2)
    n = 0
    cur = ""
    inq = 0
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "\"")
            inq = !inq
        else if (c == "," && !inq) {
            out[++n] = trim(cur)
            cur = ""
        } else
            cur = cur c
    }
    out[++n] = trim(cur)
    return n
}

function add_key(s,    k, v, tmp, n, j) {
    k = s
    sub(/:.*/, "", k)
    v = s
    sub(/^[^:]*:/, "", v)
    split("", tmp)
    n = split_list(v, tmp)
    nk++
    key[nk] = trim(k)
    cnt[nk] = n
    for (j = 1; j <= n; j++)
        val[nk, j] = tmp[j]
}

function flush_entry(    i, j, t, ord, idx, line) {
    if (nk == 0)
        return
    for (i = 1; i <= nk; i++)
        ord[i] = i
    for (i = 2; i <= nk; i++) {
        t = ord[i]
        for (j = i - 1; j >= 1 && key[ord[j]] > key[t]; j--)
            ord[j + 1] = ord[j]
        ord[j + 1] = t
    }
    for (i = 1; i <= nk; i++)
        idx[i] = 1
    for (;;) {
        line = "CELL\t" job
        for (i = 1; i <= nk; i++)
            line = line "\t" key[ord[i]] "=" val[ord[i], idx[i]]
        print line
        for (i = nk; i >= 1; i--) {
            if (idx[i] < cnt[ord[i]]) {
                idx[i]++
                break
            }
            idx[i] = 1
        }
        if (i < 1)
            break
    }
    nk = 0
    ncells++
}

function end_job() {
    flush_entry()
    if (job == "")
        return
    print "JOB\t" job "\t" (rules == "" ? "none" : rules)
    if (ncells == 0)
        print "CELL\t" job
}

/^[A-Za-z0-9_][A-Za-z0-9_.-]*:/ {
    end_job()
    job = $0
    sub(/:.*/, "", job)
    rules = ""
    inmatrix = 0
    ncells = 0
    nk = 0
    next
}
/^[ \t]*#/ || /^[ \t]*$/ { next }
job != "" && /^  rules:/ {
    r = $0
    sub(/^  rules:[ \t]*/, "", r)
    rules = (r ~ /^\*/) ? substr(r, 2) : "inline"
    next
}
job != "" && /^[ \t]+matrix:[ \t]*$/ { inmatrix = 1; next }
inmatrix && /^[ \t]*- [A-Z_][A-Z0-9_]*:/ {
    flush_entry()
    s = $0
    sub(/^[ \t]*- /, "", s)
    add_key(s)
    next
}
inmatrix && /^[ \t]+[A-Z_][A-Z0-9_]*:/ { add_key($0); next }
inmatrix { flush_entry(); inmatrix = 0 }
END { end_job() }
'

ci_ignored() {
    local pat

    for pat in "${CI_IGNORED[@]}"; do
        # shellcheck disable=SC2053 # CI_IGNORED holds glob patterns
        [[ $1 == $pat ]] && return 0
    done
    return 1
}

ci_records() {
    local kind job rest

    awk "$CI_AWK" "$CI_FILE" | while IFS=$'\t' read -r kind job rest; do
        ci_ignored "$job" && continue
        printf '%s\t%s%s\n' "$kind" "$job" "${rest:+$'\t'$rest}"
    done
}

table_records() {
    local job i

    for job in "${!JOB_RULES[@]}"; do
        printf 'JOB\t%s\t%s\n' "$job" "${JOB_RULES[$job]}"
    done
    for i in "${!L_NAME[@]}"; do
        printf '%s\n' "${L_CELL[i]}"
    done
}

# Compare the leg table with .gitlab-ci.yml.  Prints the difference and
# returns 1 when they disagree.
verify_ci() {
    local d

    [[ -f $CI_FILE ]] || die "$CI_FILE not found in $ROOT"
    d=$(diff -u --label "$SELF (leg table)" --label "$CI_FILE" \
        <(table_records | LC_ALL=C sort) <(ci_records | LC_ALL=C sort)) && return 0
    printf '%s\n' "$d" | sed $'s/\t/  /g' >&2
    return 1
}

# ---------------------------------------------------------------------------
# Selecting legs and building their qb command lines

OPT_TAG="" OPT_ALL=0 OPT_NO_BUILDER=0 OPT_JOBS=0 OPT_RETRY=0 OPT_LOG_DIR=""
OPT_LIST=0 OPT_DRY=0 OPT_VERIFY=0
OPT_ARCHS=() OPT_MODES=() OPT_ONLY=()
declare -A SKIP_FAMILY=()

need_arg() {
    [[ $# -ge 2 && -n $2 ]] || die "$1 needs an argument (see --help)"
}

parse_args() {
    local opt val

    while (($#)); do
        opt=$1
        val=""
        # Split --opt=value into --opt value.
        if [[ $opt == --*=* ]]; then
            val=${opt#*=}
            opt=${opt%%=*}
            set -- "$opt" "$val" "${@:2}"
        fi
        case $opt in
            -o | --tag)
                need_arg "$@"
                OPT_TAG=$2
                shift 2
                ;;
            -a | --arch)
                need_arg "$@"
                [[ " ${KNOWN_ARCHS[*]} " == *" $2 "* ]] || die "unknown arch '$2' (amd64 or arm64)"
                OPT_ARCHS+=("$2")
                shift 2
                ;;
            -m | --mode)
                need_arg "$@"
                [[ $2 == release || $2 == debug ]] || die "unknown mode '$2' (release or debug)"
                OPT_MODES+=("$2")
                shift 2
                ;;
            --all)
                OPT_ALL=1
                shift
                ;;
            --no-kvm | --no-static | --no-syntax | --no-quint | --no-ctest)
                SKIP_FAMILY[${opt#--no-}]=1
                shift
                ;;
            --only)
                need_arg "$@"
                OPT_ONLY+=("$2")
                shift 2
                ;;
            --no-builder)
                OPT_NO_BUILDER=1
                shift
                ;;
            -j | --jobs)
                need_arg "$@"
                [[ $2 =~ ^[0-9]+$ ]] || die "$opt takes a number"
                OPT_JOBS=$2
                shift 2
                ;;
            --retry)
                need_arg "$@"
                [[ $2 =~ ^[0-9]+$ ]] || die "$opt takes a number"
                OPT_RETRY=$2
                shift 2
                ;;
            --log-dir)
                need_arg "$@"
                OPT_LOG_DIR=$2
                shift 2
                ;;
            -l | --list)
                OPT_LIST=1
                shift
                ;;
            -n | --dry-run)
                OPT_DRY=1
                shift
                ;;
            --verify)
                OPT_VERIFY=1
                shift
                ;;
            -h | --help)
                usage
                exit 0
                ;;
            *)
                die "unknown option '$opt' (see --help)"
                ;;
        esac
    done
}

default_tag() {
    local branch tag

    branch=$(git -C "$ROOT" symbolic-ref --short -q HEAD 2>/dev/null) ||
        branch=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null) ||
        branch=local
    tag="${USER:-$(id -un)}-$branch"
    tag=${tag//[^A-Za-z0-9_.-]/-}
    printf '%s' "${tag:0:128}"
}

# The archs a leg runs on: its own, or every requested arch for syntax.
leg_archs() {
    if [[ ${L_ARCH[$1]} == '*' ]]; then
        printf '%s\n' "${ARCHS[@]}"
    else
        printf '%s\n' "${L_ARCH[$1]}"
    fi
}

SEL=()

select_legs() {
    local i glob hit

    for i in "${!L_NAME[@]}"; do
        [[ ${L_SET[i]} == all ]] && ((!OPT_ALL)) && continue
        [[ -n ${SKIP_FAMILY[${L_FAMILY[i]}]:-} ]] && continue
        if [[ ${L_MODE[i]} != - && ${#OPT_MODES[@]} -gt 0 ]]; then
            [[ " ${OPT_MODES[*]} " == *" ${L_MODE[i]} "* ]] || continue
        fi
        if [[ ${L_ARCH[i]} != '*' ]]; then
            [[ " ${ARCHS[*]} " == *" ${L_ARCH[i]} "* ]] || continue
        fi
        if ((${#OPT_ONLY[@]})); then
            hit=0
            for glob in "${OPT_ONLY[@]}"; do
                # shellcheck disable=SC2053 # --only takes a glob
                [[ ${L_NAME[i]} == $glob ]] && hit=1
            done
            ((hit)) || continue
        fi
        SEL+=("$i")
    done
}

# The qb options every invocation shares, as the pipeline's RUN_QB has them:
# -i makes the legs test the chimera-dev built under this tag rather than the
# develop one, and the longer pod timeout covers a burst of legs all pulling
# that image at once.
qb_base() {
    QB=(qb --plain -o "$TAG" -i "$QB_PROJECT/$TAG" --pod-running-timeout=10m)
}

# Sets CMD to the qb command line for leg $1, or for its report task with $2.
build_cmd() {
    local i=$1 task=${2:-${L_TASK[$1]}} a v
    local -a vars=()

    qb_base
    CMD=("${QB[@]}")
    while read -r a; do
        CMD+=(-a "$a")
    done < <(leg_archs "$i")
    CMD+=(-v "$task")
    [[ -z ${2:-} && -n ${L_VARS[i]} ]] && IFS=$'\x1f' read -r -a vars <<<"${L_VARS[i]}"
    for v in "${vars[@]}"; do
        CMD+=(-V "$v")
    done
}

# Print a command line in a form that pastes back into a shell.
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

list_legs() {
    local i archs

    printf '%-31s %-8s %-24s %-12s %s\n' LEG SET "QB TASK" ARCH VARIABLES
    for i in "${SEL[@]}"; do
        archs=$(leg_archs "$i" | paste -sd,)
        printf '%-31s %-8s %-24s %-12s %s\n' "${L_NAME[i]}" "${L_SET[i]}" \
            "${L_TASK[i]}" "$archs" "${L_VARS[i]//$'\x1f'/  }"
    done
}

dry_run() {
    local i

    if ((!OPT_NO_BUILDER)); then
        echo "# chimera-dev"
        builder_cmd
        fmt_cmd "${CMD[@]}"
    fi
    for i in "${SEL[@]}"; do
        echo "# ${L_NAME[i]}"
        build_cmd "$i"
        fmt_cmd "${CMD[@]}"
        if [[ -n ${L_REPORT[i]} ]]; then
            build_cmd "$i" "${L_REPORT[i]}"
            echo "#   on failure: $(fmt_cmd "${CMD[@]}")"
        fi
        [[ -z ${L_ASSETS[i]} ]] || echo "#   then downloads: ${L_ASSETS[i]}"
    done
}

# ---------------------------------------------------------------------------
# Running

BUILDER_ARCHS=()

# chimera-dev has to exist for every arch any selected leg runs on.
builder_cmd() {
    local a i

    BUILDER_ARCHS=()
    for a in "${KNOWN_ARCHS[@]}"; do
        for i in "${SEL[@]}"; do
            if leg_archs "$i" | grep -qx "$a"; then
                BUILDER_ARCHS+=("$a")
                break
            fi
        done
    done
    qb_base
    CMD=("${QB[@]}")
    for a in "${BUILDER_ARCHS[@]}"; do
        CMD+=(-a "$a")
    done
    CMD+=(-v chimera-dev)
}

preflight() {
    local bad

    command -v qb >/dev/null || die "qb is not on PATH"
    # qb packs the whole tree for each leg and fails every task on a path it
    # cannot read.  The usual culprit is the root-owned, mode-0700
    # smbtorture<random> directories a local smbtorture run leaves behind.
    bad=$(find . ! -readable -prune -print 2>/dev/null | head -5)
    [[ -z $bad ]] || die "qb cannot pack unreadable paths; fix them (e.g. sudo chmod a+rx, or remove empty smbtorture* dirs):
$bad"
    [[ -z $(git status --porcelain 2>/dev/null) ]] ||
        warn "the working tree has uncommitted changes; the legs test them as they are when each leg starts"
}

# Waits for a qb run to log that it packed its context (or to exit without
# needing it any more), then marks it done.  Whichever leg completes the set
# announces that the working tree is free again.
note_packed() {
    local log=$1 pid=$2 mark=$3 i

    until grep -q 'Packed context' "$log" 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    : >"$mark"
    for i in "${SEL[@]}"; do
        [[ -e $STATE/${L_NAME[i]}.packed ]] || return 0
    done
    # mkdir is atomic, so only one leg announces.
    mkdir "$STATE/all-packed" 2>/dev/null || return 0
    say "packed  every leg has its copy of the working tree; edits no longer reach this run$PACK_CAVEAT"
}

# qb exits on SIGINT without deleting its pod (seen with qb v2.0.36: the pod
# stays Running, its runner idle), so after an interrupt delete the pods the
# run's log names.
delete_pods() {
    local log=$1 ns pod

    command -v kubectl >/dev/null || return 0
    ns=$(grep -m1 -oE '\(namespace [^)]+\)' "$log" | sed -E 's/^\(namespace (.*)\)$/\1/')
    for pod in $(grep -oE 'Launching pod name qb-[0-9a-f-]+' "$log" | awk '{print $4}' | sort -u); do
        echo "--- qb-ci: interrupted; deleting pod $pod"
        kubectl delete pod -n "${ns:-default}" --wait=false "$pod"
    done >>"$log" 2>&1
}

# Runs a qb command line with its output in a log, and records the pid so a
# SIGTERM can be passed on.  Background jobs of a script start with SIGINT
# ignored, and qb would inherit that; env --default-signal restores it, so a
# Ctrl-C reaches every qb process.  With PACK_MARK set, also notes when the
# run has packed the working tree.
run_qb() {
    local log=$1 id=$2 pid rc

    shift 2
    env --default-signal=INT,QUIT "$@" >"$log" 2>&1 &
    pid=$!
    echo "$pid" >"$STATE/$id.pid"
    [[ -n ${PACK_MARK:-} ]] && note_packed "$log" "$pid" "$PACK_MARK" &
    while :; do
        wait "$pid"
        rc=$?
        # A trapped signal interrupts wait with a status above 128 while qb
        # may still be running; wait for it again.
        ((rc > 128)) && kill -0 "$pid" 2>/dev/null && continue
        break
    done
    rm -f "$STATE/$id.pid"
    ((rc != 0)) && interrupted && delete_pods "$log"
    return "$rc"
}

# The asset revision a qb run published under: its start time in UTC, which is
# also the timestamp on the first line qb logs (in local time).
qb_revision() {
    local ts

    ts=$(grep -m1 -oE '^[0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}' "$1") || return 0
    date -u -d "${ts//\//-}" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null
}

# Download the assets a run published into a directory.  Tries the run's own
# revision first, then 'latest'; returns 1 if any asset could not be fetched.
download_assets() {
    local log=$1 dest=$2 rev asset rc=0

    shift 2
    rev=$(qb_revision "$log")
    mkdir -p "$dest"
    for asset in "$@"; do
        {
            echo "--- qb-ci: downloading $asset (revision ${rev:-latest})"
            qb assets download --strip -p "$QB_PROJECT" -t "$TAG" ${rev:+--revision "$rev"} \
                --asset-name "$asset" --output-dir "$dest" ||
                qb assets download --strip -p "$QB_PROJECT" -t "$TAG" \
                    --asset-name "$asset" --output-dir "$dest"
        } >>"$log" 2>&1 || rc=1
    done
    return "$rc"
}

interrupted() {
    [[ -e $STATE/interrupted ]]
}

# One leg, run in the background: the qb run with its retries, the report task
# when a static analysis leg fails (as the pipeline does), and the assets.
# The outcome goes to a status file for the main loop.
run_leg() {
    local i=$1 name=${L_NAME[$1]} start=$SECONDS attempt=0 rc result note=""
    local log=$LOG_DIR/${L_NAME[$1]}.log
    local -a assets

    build_cmd "$i"
    while :; do
        attempt=$((attempt + 1))
        if ((attempt == 1)); then
            PACK_MARK=$STATE/$name.packed run_qb "$log" "$name" "${CMD[@]}"
        else
            run_qb "$log" "$name" "${CMD[@]}"
        fi
        rc=$?
        ((rc == 0)) && break
        interrupted && break
        ((attempt > OPT_RETRY)) && break
        mv "$log" "$log.try$attempt"
        say "retry   $name (attempt $((attempt + 1)) of $((OPT_RETRY + 1)))"
    done
    ((attempt > 1)) && note="$attempt attempts"

    if ((rc != 0)) && [[ -n ${L_REPORT[i]} ]] && ! interrupted; then
        build_cmd "$i" "${L_REPORT[i]}"
        read -r -a assets <<<"${L_REPORT_ASSETS[i]}"
        if run_qb "$LOG_DIR/$name.report.log" "$name.report" "${CMD[@]}" &&
            download_assets "$LOG_DIR/$name.report.log" "$LOG_DIR/$name" "${assets[@]}"; then
            note+="${note:+, }report in $name/"
        else
            note+="${note:+, }report failed, see $name.report.log"
        fi
    fi
    if ((rc == 0)) && [[ -n ${L_ASSETS[i]} ]]; then
        read -r -a assets <<<"${L_ASSETS[i]}"
        if download_assets "$log" "$LOG_DIR/$name" "${assets[@]}"; then
            note+="${note:+, }assets in $name/"
        else
            note+="${note:+, }assets download failed, see $name.log"
        fi
    fi

    if ((rc == 0)); then
        result=PASS
    elif interrupted; then
        result=STOPPED
    else
        result=FAIL
    fi
    printf '%s %d %s\n' "$result" $((SECONDS - start)) "$note" >"$STATE/$name.status"
}

INTERRUPTED=0
declare -A PID_LEG=() RESULT=() SECS=() NOTE=()
RUNNING=0 DONE=0

on_int() {
    ((INTERRUPTED)) && return
    INTERRUPTED=1
    : >"$STATE/interrupted"
    say "interrupted: starting no more legs; stopping the running ones and deleting their pods"
}

# A SIGTERM reaches only this script, so pass it on to the qb processes as the
# SIGINT a terminal would have sent them.
on_term() {
    local f

    on_int
    for f in "$STATE"/*.pid; do
        [[ -e $f ]] && kill -INT "$(<"$f")" 2>/dev/null
    done
}

# Wait for any leg to finish and report it.  Returns early, having reaped
# nothing, when a trapped signal interrupts the wait.
reap() {
    local pid rc

    # wait unsets pid when a trapped signal interrupts it.
    wait -n -p pid
    rc=$?
    pid=${pid:-}
    if [[ -z $pid ]]; then
        # 127 means no children are left, which RUNNING says cannot happen;
        # collect whatever the remaining legs recorded rather than spin.
        if ((rc == 127)); then
            for pid in "${!PID_LEG[@]}"; do
                finish_leg "$pid"
            done
        fi
        return 0
    fi
    [[ -n ${PID_LEG[$pid]:-} ]] && finish_leg "$pid"
}

finish_leg() {
    local i=${PID_LEG[$1]} st secs note

    unset "PID_LEG[$1]"
    RUNNING=$((RUNNING - 1))
    DONE=$((DONE + 1))
    if ! read -r st secs note <"$STATE/${L_NAME[i]}.status" 2>/dev/null; then
        st=FAIL secs=0 note="the leg runner died; see ${L_NAME[i]}.log"
    fi
    RESULT[$i]=$st SECS[$i]=$secs NOTE[$i]=$note
    say "$(printf '%-7s %-31s %8s' "$st" "${L_NAME[i]}" "$(dur "$secs")")  ($RUNNING running, $((${#SEL[@]} - DONE - RUNNING)) queued)"
}

summary() {
    local i st n_bad=0 need_all=0 a
    local -a rerun=()

    # A brace group, not a pipe into tee: n_bad has to survive it.
    {
        echo
        printf '%-8s %9s  %-31s %s\n' RESULT DURATION LEG NOTE
        for i in "${SEL[@]}"; do
            st=${RESULT[$i]:-SKIPPED}
            printf '%-8s %9s  %-31s %s\n' "$st" "$(dur "${SECS[$i]:-0}")" "${L_NAME[i]}" "${NOTE[$i]:-}"
            if [[ $st != PASS ]]; then
                n_bad=$((n_bad + 1))
                rerun+=(--only "${L_NAME[i]}")
                [[ ${L_SET[i]} == all ]] && need_all=1
            fi
        done
        echo
        echo "Logs: $LOG_DIR/<leg>.log"
        if ((n_bad == 0)); then
            echo "All ${#SEL[@]} legs passed."
        else
            echo "$n_bad of ${#SEL[@]} legs did not pass.  Rerun them against the same chimera-dev with:"
            ((need_all)) && rerun=(--all "${rerun[@]}")
            for a in "${OPT_ARCHS[@]}"; do
                rerun=(-a "$a" "${rerun[@]}")
            done
            echo "  $(fmt_cmd "$SELF" -o "$TAG" --no-builder "${rerun[@]}")"
        fi
    } >"$LOG_DIR/summary.txt"
    cat "$LOG_DIR/summary.txt"
    return $((n_bad > 0))
}

run_all() {
    local i rc start
    local -a again=()

    mkdir -p "$STATE" || die "cannot create $LOG_DIR"
    trap on_int INT
    trap on_term TERM

    # What packs the tree again after the "packed" line.
    ((OPT_RETRY > 0)) && again+=(retries)
    for i in "${SEL[@]}"; do
        if [[ -n ${L_REPORT[i]} ]]; then
            again+=("static_analysis failure reports")
            break
        fi
    done
    PACK_CAVEAT=""
    ((${#again[@]} == 1)) && PACK_CAVEAT=" (except ${again[0]})"
    ((${#again[@]} == 2)) && PACK_CAVEAT=" (except ${again[0]} and ${again[1]})"

    say "tag $TAG: ${#SEL[@]} legs, logs in $LOG_DIR"
    say "each leg packs the working tree when it starts; leave the tree alone until the 'packed' line"

    if ((OPT_NO_BUILDER)); then
        say "chimera-dev  reusing chimera-dev:$TAG (--no-builder)"
    else
        builder_cmd
        say "chimera-dev  building for ${BUILDER_ARCHS[*]}"
        start=$SECONDS
        run_qb "$LOG_DIR/chimera-dev.log" chimera-dev "${CMD[@]}"
        rc=$?
        if ((rc != 0)); then
            if ((INTERRUPTED)); then
                say "chimera-dev  STOPPED after $(dur $((SECONDS - start)))"
            else
                say "chimera-dev  FAIL after $(dur $((SECONDS - start))); see $LOG_DIR/chimera-dev.log"
            fi
            summary
            ((INTERRUPTED)) && exit 130
            exit 1
        fi
        say "chimera-dev  PASS $(dur $((SECONDS - start)))"
    fi

    for i in "${SEL[@]}"; do
        while ((OPT_JOBS > 0 && RUNNING >= OPT_JOBS && !INTERRUPTED)); do
            reap
        done
        ((INTERRUPTED)) && break
        run_leg "$i" &
        PID_LEG[$!]=$i
        RUNNING=$((RUNNING + 1))
        say "start   ${L_NAME[i]}"
    done
    ((INTERRUPTED)) || say "all ${#SEL[@]} legs started"
    while ((RUNNING > 0)); do
        reap
    done

    summary
    rc=$?
    ((INTERRUPTED)) && exit 130
    exit "$rc"
}

main() {
    parse_args "$@"
    cd "$ROOT" || die "cannot cd to $ROOT"

    ((${#OPT_ARCHS[@]})) && ARCHS=("${OPT_ARCHS[@]}") || ARCHS=("${KNOWN_ARCHS[@]}")
    TAG=${OPT_TAG:-$(default_tag)}
    [[ $TAG =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]] || die "'$TAG' is not a valid image tag"
    LOG_DIR=${OPT_LOG_DIR:-${CHIMERA_BUILD_DIR:-/tmp}/qb-ci/$TAG/$(date -u +%Y%m%dT%H%M%SZ)}
    STATE=$LOG_DIR/.state

    load_jobs
    load_legs

    if ((OPT_VERIFY)); then
        verify_ci || die "the leg table does not match $CI_FILE (diff above)"
        echo "$SELF: leg table matches $CI_FILE (${#L_NAME[@]} legs)"
        exit 0
    fi
    if ! verify_ci; then
        ((OPT_LIST || OPT_DRY)) || die "the leg table does not match $CI_FILE (diff above); update $SELF to follow the pipeline"
        warn "the leg table does not match $CI_FILE (diff above)"
    fi

    select_legs
    if ((${#SEL[@]} == 0)); then
        ((OPT_ALL)) && die "no legs selected"
        die "no legs selected (the nightly and coverage legs need --all)"
    fi
    if ((OPT_LIST)); then
        list_legs
        exit 0
    fi
    if ((OPT_DRY)); then
        dry_run
        exit 0
    fi

    preflight
    run_all
}

main "$@"
