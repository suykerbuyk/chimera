#!/bin/bash -e
# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
#
# Prove that the KVM shard set the pipeline declares still covers what the
# build is configured to test.
#
# The ctest_kvm legs shard by guest image: each leg selects one image's tests
# with `-L kvm -L "^<image>$"`, and on the images that carry nfstest/ltp those
# two suites are peeled into a shard of their own because they cost more than
# everything else on the image combined.  There is no catch-all shard, so a
# guest image with no shard -- or one that gained nfstest/ltp without a
# -nfstest shard -- runs nowhere and fails nothing.  That is the failure this
# check exists to make loud: silent loss of coverage, which is exactly what
# the previous hand-maintained exclude list was prone to.
#
# Derives the required set from the configured build tree (the kvm_<image>_image
# targets, and which of those images actually register nfstest/ltp tests) and
# compares it against the declared list.  Mirrors the check upstream runs in
# .github/workflows/extended.yml so the two pipelines fail on the same drift.
#
# Usage: verify_kvm_shards.sh <build-dir> <declared-shards>
#
#   <declared-shards> is a space-separated list of "<image>" and
#   "<image>-nfstest" names, e.g. "ubuntu2604 ubuntu2604-nfstest ubuntu2404".
#
# Run from ctest_kvm in qb.yaml straight after cmake, before the build and
# before the ~3.4 GB image pull, so a stale matrix costs seconds rather than
# minutes.  Deliberately not a ctest case: what is under test is the CI
# matrix, which ctest cannot see.

BUILD_DIR=${1:?usage: $0 <build-dir> <declared-shards>}
DECLARED=${2?usage: $0 <build-dir> <declared-shards>}

if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "ERROR: ${BUILD_DIR} is not a configured ninja build tree" >&2
    exit 1
fi

cd "${BUILD_DIR}"

# The image targets are the authority on which guests this configure can run.
images=$(ninja -t targets all 2>/dev/null |
         grep -oE '^kvm_[a-z0-9_]+_image' |
         sed -E 's/^kvm_(.*)_image$/\1/' |
         sort -u)

if [ -z "${images}" ]; then
    echo "ERROR: no kvm_<image>_image targets in ${BUILD_DIR}; was it configured with -DKVM_TESTING=ON?" >&2
    exit 1
fi

# A -nfstest shard is required exactly where the image registers nfstest or ltp
# tests.  The label selectors are anchored because ctest -L takes a regex and
# ANDs repeated -L flags: unanchored, ubuntu2404 would also match
# ubuntu2404_hwe and the two images would be indistinguishable here.
want=""
for img in ${images}; do
    want="${want} ${img}"
    n=$(ctest -C extended -N -L '^kvm$' -L "^${img}$" -R '/(nfstest|ltp)/' 2>/dev/null |
        sed -n 's/^Total Tests: \([0-9]*\)$/\1/p')
    if [ -n "${n}" ] && [ "${n}" != "0" ]; then
        want="${want} ${img}-nfstest"
    fi
done

# Compare as sorted sets so neither list's order is load-bearing.
want_sorted=$(echo ${want} | tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' ')
have_sorted=$(echo ${DECLARED} | tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' ')

if [ "${want_sorted}" != "${have_sorted}" ]; then
    echo "ERROR: the declared KVM shards do not cover this configure." >&2
    echo "  declared: ${have_sorted}" >&2
    echo "  required: ${want_sorted}" >&2
    echo "Update KVM_SHARDS in qb.yaml and the ctest_kvm / ctest_kvm_images" >&2
    echo "matrices in .gitlab-ci.yml together." >&2
    exit 1
fi

echo "KVM shard coverage OK: ${want_sorted}"
