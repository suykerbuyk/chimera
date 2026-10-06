#!/bin/sh
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# make podman-image-bump PODMAN_FLAVOR=dev: re-resolve builders/dev's pins and
# regenerate the frozen apt closure.  Unlike builders/thin/bump-pins.sh (which
# only rewrites three ARG lines and never builds), dev has no immutable snapshot
# to verify against -- its reference IS the committed closure -- so this script
# MUST run a reference build to capture the realized closure from dpkg-query.
#
# It does two things, refusing before it writes anything if a check fails:
#   1. Re-resolves the base index digest and rewrites the one `FROM ...@sha256:`
#      line (cross-checked against skopeo's own {{.Digest}}, like thin).
#   2. Builds a reference image with the lock gate OFF (DEV_LOCK_GATE=off), runs
#      a container, and writes the installed closure to builders/dev/packages.lock
#      in the apt Packages-stanza format the gate's read_lists() consumes.
#
# It NEVER builds the verify image, commits or pushes.  After it runs: review
# the packages.lock diff, re-pin any changed `pkg=version` in the Dockerfile
# install lines to match the new closure, then run a normal
# `make podman-image PODMAN_FLAVOR=dev PODMAN_BUILD_FLAGS=--no-cache` (gate ON)
# and commit the result yourself.
#
# Needs the network plus skopeo and podman.  Run from the repository root.
# DRY_RUN=1 re-resolves and reports but does not rewrite the Dockerfile or build.

set -eu

DOCKERFILE=${DOCKERFILE:-builders/dev/Dockerfile}
BASE_NAME=${BASE_NAME:-repository.mdh.quantum.com/ubuntu}
BASE_TAG=${BASE_TAG:-26.04}
PODMAN=${PODMAN:-podman}
REF_IMAGE=${REF_IMAGE:-localhost/chimera-dev-bumpref:local}
LOCKFILE=${LOCKFILE:-builders/dev/packages.lock}

die() { echo "dev bump-pins: $*" >&2; exit 1; }

for tool in skopeo "$PODMAN" git sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found (install it; only this target needs skopeo)"
done
[ -f "$DOCKERFILE" ] || die "$DOCKERFILE not found (run from the repository root)"

# 1. Clean inputs, so the diff shown is the bump alone.
git ls-files --error-unmatch -- "$DOCKERFILE" >/dev/null 2>&1 || die "$DOCKERFILE is not tracked by git"
git diff --quiet HEAD -- "$DOCKERFILE" || die "$DOCKERFILE has uncommitted changes"

tmp=$(mktemp -d)
# Clean up the scratch dir AND the multi-GB reference image on any exit, so a
# bump never leaks localhost/chimera-dev-bumpref:local into podman storage.
trap 'rm -rf "$tmp"; "$PODMAN" rmi -f "$REF_IMAGE" >/dev/null 2>&1 || true' EXIT

# 2. Resolve the base index digest (the bytes the registry serves), cross-checked
#    against skopeo's own view, exactly as thin's resolve() does.
raw=$tmp/raw
skopeo inspect --raw "docker://$BASE_NAME:$BASE_TAG" > "$raw" || die "skopeo inspect --raw failed"
[ -s "$raw" ] || die "empty manifest for $BASE_NAME:$BASE_TAG"
grep -Eq '"mediaType"[[:space:]]*:[[:space:]]*"application/vnd\.(oci\.image\.(index|manifest)\.v1|docker\.distribution\.manifest\.(list\.)?v2)\+json"' "$raw" ||
    die "no manifest or index mediaType in the response for $BASE_NAME:$BASE_TAG"
d=$(sha256sum < "$raw" | cut -c1-64)
echo "$d" | grep -Eq '^[0-9a-f]{64}$' || die "bad digest: '$d'"
check=$(skopeo inspect --format '{{.Digest}}' "docker://$BASE_NAME:$BASE_TAG") || die "skopeo inspect --format failed"
[ "$check" = "sha256:$d" ] || die "digest mismatch: raw sha256:$d, skopeo $check (tag moved?)"
new_base="$BASE_NAME:$BASE_TAG@sha256:$d"

[ "$(grep -c '^ARG BASE_IMAGE=' "$DOCKERFILE")" = 1 ] || die "expected exactly one 'ARG BASE_IMAGE=' line in $DOCKERFILE"
old_base=$(sed -n 's/^ARG BASE_IMAGE=//p' "$DOCKERFILE")
echo "base image (ARG BASE_IMAGE)"
echo "  old: $old_base"
echo "  new: $new_base"

if [ "${DRY_RUN:-0}" = 1 ]; then
    echo "DRY_RUN=1: not rewriting $DOCKERFILE and not building."
    exit 0
fi

sed -i "s#^ARG BASE_IMAGE=.*#ARG BASE_IMAGE=$new_base#" "$DOCKERFILE"
[ "$(sed -n 's/^ARG BASE_IMAGE=//p' "$DOCKERFILE")" = "$new_base" ] || die "ARG BASE_IMAGE rewrite did not take; inspect $DOCKERFILE"

# 3. Reference build with the gate OFF, so apt resolves against the just-pinned
#    base and the current install-line pins, and the gate does not fire against a
#    stale lock.  The apt closure does not depend on the source-build ENABLE_*
#    flags (they build from git/dotnet/npm, not apt), so turn them off for speed;
#    ENABLE_DOCA stays at its Dockerfile default so cuda is in/out of the closure
#    exactly as the committed recipe ships it.
echo "reference build (gate off) -> $REF_IMAGE ..."
$PODMAN build --no-cache --build-arg DEV_LOCK_GATE=off \
    --build-arg ENABLE_FIO=0 --build-arg ENABLE_XLIO=0 \
    --build-arg ENABLE_ELBENCHO=0 --build-arg ENABLE_WPTS=0 \
    -t "$REF_IMAGE" -f "$DOCKERFILE" "$(dirname "$DOCKERFILE")" \
    || die "reference build failed (a pinned pkg=version may have aged out of the mirror -- that is the retention-horizon loud fail; update the pin it names and retry)"

# 4. Capture the realized installed closure as Packages stanzas (read_lists
#    format): only rows dpkg marks as installed, sorted for a stable diff.
#    POSIX sh has no pipefail, so a failed `podman run` that prints nothing would
#    still write a valid header-only (zero-package) lock and pass `|| die`.  Guard
#    against that: write to a temp file, require a non-empty floor, and only then
#    mv it into place.  A real dev closure is ~900+ packages.
out=$tmp/packages.lock
$PODMAN run --rm --network=none --cap-drop=ALL --security-opt no-new-privileges "$REF_IMAGE" \
    dpkg-query -W -f '${db:Status-Status}\t${Package}\t${Architecture}\t${Version}\n' \
    | awk -F'\t' '$1=="installed"{print $2"\t"$3"\t"$4}' | LC_ALL=C sort \
    | awk -F'\t' 'BEGIN{print "# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors"
                        print "# SPDX-License-Identifier: Unlicense"
                        print "# builders/dev/packages.lock -- frozen apt+base closure for the dev image."
                        print "# Generated by builders/dev/bump-pins.sh from a reference build'\''s dpkg-query."
                        print "# Packages-stanza format consumed by builders/dev/lock-gate.py read_lists()."
                        print "# Edit only via the bump script; a hand edit desyncs the lock gate.\n"}
                  {print "Package: "$1"\nArchitecture: "$2"\nVersion: "$3"\n"}' > "$out" \
    || die "writing the closure failed"

n=$(grep -c '^Package: ' "$out" || true)
[ "$n" -gt 100 ] || die "captured only $n packages (expected ~900+); dpkg-query/podman run likely failed -- $LOCKFILE left unchanged"
mv "$out" "$LOCKFILE"
echo "wrote $LOCKFILE: $n installed packages"
echo
echo "Next: review the diff, re-pin any changed pkg=version in $DOCKERFILE to match,"
echo "      then  make podman-image PODMAN_FLAVOR=dev PODMAN_BUILD_FLAGS=--no-cache"
echo "      (lock gate ON must print 'apt+base layer frozen'), and commit yourself."
echo "      This script does not build the verify image, commit or push."
