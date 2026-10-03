#!/bin/sh
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# make podman-image-bump: re-resolve the three pins in builders/thin/Dockerfile
# (base index digest, apt snapshot, quint image digest), rewrite them, and show
# the diff.  It never builds, commits or pushes; review the diff, then rebuild
# with `make podman-image PODMAN_BUILD_FLAGS=--no-cache` and run
# `make podman-test` before committing.
#
# Needs the network plus skopeo and curl.  Only this script needs them:
# podman cannot report a remote digest without pulling (`podman manifest
# inspect` re-serialises the JSON, so its hash is not the digest), and neither
# podman nor skopeo speaks apt.  Run from the repository root.
#
# Every check refuses before anything is written.  DRY_RUN=1 runs every
# check and resolution, rewrites a temporary copy instead, and prints its diff.

set -eu

DOCKERFILE=${DOCKERFILE:-builders/thin/Dockerfile}
BASE_NAME=${BASE_NAME:-public.ecr.aws/ubuntu/ubuntu}
BASE_TAG=${BASE_TAG:-26.04}
QUINT_REPO=${QUINT_REPO:-ghcr.io/chimera-nas/quint}
SNAPSHOT_URL=${SNAPSHOT_URL:-https://snapshot.ubuntu.com/ubuntu}
ARCHES=${ARCHES:-"amd64 arm64"}

die() { echo "podman-image-bump: $*" >&2; exit 1; }

for tool in skopeo curl git sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found (install it; only this target needs it)"
done
[ -f "$DOCKERFILE" ] || die "$DOCKERFILE not found (run from the repository root)"

# 1. Clean inputs, so the diff shown is the bump alone.
# git diff is silent about an untracked file, so require it tracked first.
git ls-files --error-unmatch -- "$DOCKERFILE" >/dev/null 2>&1 || die "$DOCKERFILE is not tracked by git"
git diff --quiet HEAD -- "$DOCKERFILE" || die "$DOCKERFILE has uncommitted changes"
case "$(git submodule status ext/specs 2>/dev/null)" in
    " "*) ;;
    *) die "ext/specs is not checked out at its recorded commit (git submodule update --init ext/specs)" ;;
esac
git -C ext/specs diff --quiet HEAD -- .quint-version || die "ext/specs/.quint-version has uncommitted changes"

# 2. quint version, as the specs submodule pins it.
V=$(tr -d '[:space:]' < ext/specs/.quint-version)
echo "$V" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "ext/specs/.quint-version is empty or malformed: '$V'"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 3. The digest of the bytes the registry serves for REF, cross-checked
#    against skopeo's own view.  Prints sha256:<hex>.
resolve() {
    ref=$1 raw=$tmp/raw
    skopeo inspect --raw "docker://$ref" > "$raw" || die "skopeo inspect --raw $ref failed"
    [ -s "$raw" ] || die "empty manifest for $ref"
    grep -Eq '"mediaType"[[:space:]]*:[[:space:]]*"application/vnd\.(oci\.image\.(index|manifest)\.v1|docker\.distribution\.manifest\.(list\.)?v2)\+json"' "$raw" ||
        die "no manifest or index mediaType in the response for $ref"
    d=$(sha256sum < "$raw" | cut -c1-64)
    echo "$d" | grep -Eq '^[0-9a-f]{64}$' || die "bad digest for $ref: '$d'"
    check=$(skopeo inspect --format '{{.Digest}}' "docker://$ref") || die "skopeo inspect --format $ref failed"
    [ "$check" = "sha256:$d" ] || die "digest mismatch for $ref: raw sha256:$d, skopeo $check (tag moved?)"
    echo "sha256:$d"
}

base_digest=$(resolve "$BASE_NAME:$BASE_TAG")
quint_digest=$(resolve "$QUINT_REPO:$V")

# 4. Snapshot: now, and not earlier than any base child we build.  The
#    service answers for any timestamp after it started, so the InRelease
#    fetch below is only a reachability check; the Created comparison is the
#    real one.
S=$(date -u +%Y%m%dT%H%M%SZ)
created=
for a in $ARCHES; do
    c=$(skopeo inspect --override-arch "$a" --format '{{.Created.UTC.Format "20060102T150405Z"}}' \
        "docker://$BASE_NAME@$base_digest") || die "skopeo inspect Created ($a) failed"
    echo "$c" | grep -Eq '^[0-9]{8}T[0-9]{6}Z$' || die "bad Created for $a: '$c'"
    created=$(printf '%s\n%s\n' "$c" "$created" | LC_ALL=C sort | tail -n 1)
done
[ -n "$created" ] || die "no base Created timestamp"
# Same fixed-width UTC shape, so a byte-wise sort orders them.
if [ "$(printf '%s\n%s\n' "$S" "$created" | LC_ALL=C sort | tail -n 1)" != "$S" ]; then
    die "snapshot $S is earlier than the base ($created)"
fi
for pocket in resolute resolute-updates resolute-security resolute-backports; do
    curl -fsSLI -o /dev/null "$SNAPSHOT_URL/$S/dists/$pocket/InRelease" ||
        die "snapshot $S: $pocket/InRelease not reachable"
done

# 5. Rewrite exactly one line per pin.
new_base="$BASE_NAME:$BASE_TAG@$base_digest"
new_quint="$QUINT_REPO:$V@$quint_digest"
for n in UBUNTU_IMAGE QUINT_IMAGE UBUNTU_SNAPSHOT; do
    [ "$(grep -c "^ARG $n=" "$DOCKERFILE")" = 1 ] || die "expected exactly one 'ARG $n=' line in $DOCKERFILE"
done
target=$DOCKERFILE
if [ "${DRY_RUN:-0}" = 1 ]; then
    target=$tmp/Dockerfile
    cp "$DOCKERFILE" "$target"
fi
old() { sed -n "s/^ARG $1=//p" "$target"; }
old_base=$(old UBUNTU_IMAGE) old_quint=$(old QUINT_IMAGE) old_snap=$(old UBUNTU_SNAPSHOT)

sed -i -e "s#^ARG UBUNTU_IMAGE=.*#ARG UBUNTU_IMAGE=$new_base#" \
       -e "s#^ARG QUINT_IMAGE=.*#ARG QUINT_IMAGE=$new_quint#" \
       -e "s#^ARG UBUNTU_SNAPSHOT=.*#ARG UBUNTU_SNAPSHOT=$S#" "$target"
[ "$(old UBUNTU_IMAGE)" = "$new_base" ] && [ "$(old QUINT_IMAGE)" = "$new_quint" ] &&
    [ "$(old UBUNTU_SNAPSHOT)" = "$S" ] || die "rewrite of $target did not take; inspect it"

# 6. Report.
echo "quint version (ext/specs/.quint-version): $V"
echo "base Created (latest of $ARCHES): $created"
echo "UBUNTU_IMAGE    $old_base"
echo "             -> $new_base"
echo "QUINT_IMAGE     $old_quint"
echo "             -> $new_quint"
echo "UBUNTU_SNAPSHOT $old_snap -> $S"
echo
if [ "$old_base" = "$new_base" ] && [ "$old_quint" = "$new_quint" ]; then
    echo "Base and quint digests are current; only the snapshot moved."
fi
if [ "$target" = "$DOCKERFILE" ]; then
    git --no-pager diff -- "$DOCKERFILE"
else
    echo "DRY_RUN=1: $DOCKERFILE is unchanged; the rewrite would be:"
    diff -u "$DOCKERFILE" "$target" || true
fi
echo
echo "Next: make podman-image PODMAN_BUILD_FLAGS=--no-cache   (lock gate must pass)"
echo "      make podman-test                                  (compare the scorecard)"
echo "      review and commit $DOCKERFILE yourself; this target does not."
