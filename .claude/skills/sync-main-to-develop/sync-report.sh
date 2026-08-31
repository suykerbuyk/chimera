#!/usr/bin/env bash
#
# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
#
# sync-report.sh - read-only pre/post-flight report for an upstream
# origin/main -> develop sync.  Changes nothing; safe to run at any point.
#
# Usage:
#   sync-report.sh pre  [<last-sync-tag>]   what the merge is about to bring
#   sync-report.sh post                     the drift checks, after merging
#   sync-report.sh commits <from> <to>      commit list for the merge message
#
# <last-sync-tag> defaults to the newest sync-code-* tag.

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 1

# The newest sync-code tag already merged into HEAD: the checklist tags
# origin/main (step 1) before running this report (step 2), so the newest
# tag overall is the one being synced, not the previous sync's baseline.
last_tag() { git tag --list 'sync-code-*' --sort=-creatordate --merged HEAD | head -1; }

hr() { printf '\n=== %s ===\n' "$1"; }

pre() {
    local base="${1:-$(last_tag)}"

    hr "upstream head"
    git rev-parse --short origin/main
    git log --oneline -1 origin/main

    hr "commits since $base"
    git rev-list --count "$base..origin/main"

    hr "files changed on BOTH sides (conflict candidates)"
    local mb
    mb=$(git merge-base HEAD origin/main)
    comm -12 \
        <(git diff --name-only "$mb..HEAD" | sort) \
        <(git diff --name-only "$mb..origin/main" | sort)

    hr "submodule pointers (base / develop / main)"
    git config -f .gitmodules --get-regexp path | awk '{print $2}' | while read -r m; do
        [ -e "$m" ] || continue
        printf '%-14s %s %s %s\n' "$m" \
            "$(git rev-parse --short "$mb:$m"          2>/dev/null)" \
            "$(git rev-parse --short "HEAD:$m"         2>/dev/null)" \
            "$(git rev-parse --short "origin/main:$m"  2>/dev/null)"
    done

    hr "upstream Dockerfile.<os> changes (-> builders/<os>/)"
    git diff --stat "$base..origin/main" -- 'Dockerfile*' | cat
    echo "(empty = nothing to port into builders/)"

    hr "upstream .devcontainer changes (-> .devcontainer/quantum/)"
    git diff --stat "$base..origin/main" -- .devcontainer | cat
    echo "(empty = prefix re-copy not needed)"
}

post() {
    local rc=0

    hr "devcontainer prefix in sync?"
    if diff <(head -n "$(wc -l < .devcontainer/Dockerfile)" \
                   .devcontainer/quantum/Dockerfile) \
            .devcontainer/Dockerfile > /dev/null; then
        echo "IN SYNC"
    else
        echo "DRIFTED -- re-copy the prefix above the 'Quantum Specific addon' marker"
        rc=1
    fi

    hr "snfs include surface vs linux"
    # Every vfs/ header snfs includes must exist.  The SDK move renamed
    # these out from under it once already and the merge said nothing.
    grep -rho '"vfs/[a-z0-9_/]*\.h"' src/vfs/snfs/ | tr -d '"' | sort -u |
        while read -r h; do
            if [ -f "src/$h" ]; then printf '  ok      %s\n' "$h"
            else printf '  MISSING %s\n' "$h"; rc=1; fi
        done

    hr "vendor fh-magic slots keep upstream's positional names"
    # Match the enumerator, not a mention: the broken state this catches
    # kept "VENDOR0" as a trailing comment on the renamed line, so a bare
    # substring grep reports success on exactly the tree it must reject.
    if grep -qE '^[[:space:]]*CHIMERA_VFS_FH_MAGIC_VENDOR0[[:space:]]*=' \
            src/vfs/sdk/vfs_fh_magic.h &&
       grep -qE '^[[:space:]]*CHIMERA_VFS_FH_MAGIC_VENDOR1[[:space:]]*=' \
            src/vfs/sdk/vfs_fh_magic.h; then
        echo "OK -- VENDOR0/VENDOR1 present (examples/vfs_module compiles)"
    else
        echo "BROKEN -- a downstream rename replaced them; add Quantum names"
        echo "          as aliases instead (see reference.md)"
        rc=1
    fi

    hr "packaged include layout (needs a staged install first)"
    echo "sudo env DESTDIR=/tmp/chimera-stage ninja -C /build/Release install"
    echo "then: find /tmp/chimera-stage/opt/quantum/chimera/include -type f"
    echo "expect include/chimera/vfs/sdk/*.h + oteltracing.h pinned to 0"

    return $rc
}

commits() {
    git log --format='    %h %s' --reverse "$1..$2"
}

case "${1:-}" in
    pre)     shift; pre "$@" ;;
    post)    shift; post "$@" ;;
    commits) shift; commits "$@" ;;
    *)       sed -n '5,14p' "$0"; exit 1 ;;
esac
