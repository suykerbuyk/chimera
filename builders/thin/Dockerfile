# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# Thin toolchain image for the user-mode Podman loop (make podman-build /
# podman-test, PODMAN_FLAVOR=thin).  Only what the quick ctest tier needs:
# gcc, cmake, ninja, the libraries configure requires, python3, and node +
# quint for the MBT corpus.  The kitchen sink qb builds and tests in is
# builders/dev/Dockerfile; nothing here replaces it.
#
# Recreatable from public inputs, every one pinned:
#   - the base by index digest (Canonical's ECR build of ubuntu:26.04, not
#     byte-identical to the Docker Official image; ECR also avoids Docker
#     Hub's anonymous rate limit),
#   - quint and node by the digest of upstream's toolchain image
#     (Dockerfile.quint),
#   - every apt package by one snapshot.ubuntu.com timestamp, the only apt
#     source; lock-gate.py fails the build if anything installed is not in it.
# `make podman-image-bump` re-resolves the three pins and shows the diff.
# Hosts a rebuild needs: public.ecr.aws, ghcr.io, snapshot.ubuntu.com.
#
# There is deliberately no APT_MIRROR: a mirror would replace the snapshot
# and silently drop the lock.
#
# Every ARG a FROM uses must be declared before the first FROM; the stage
# re-declares all three so the pins file is never written with empty values.
ARG UBUNTU_IMAGE=public.ecr.aws/ubuntu/ubuntu:26.04@sha256:cd11a24d38f395f018457869ea3dab0c21546450787e63d60b52e680de159bb7
ARG QUINT_IMAGE=ghcr.io/chimera-nas/quint:0.32.0@sha256:16fb1fdb045556f46a9440f4f491d081ac5328392b3468e4bf37cd5805a6e8fd
ARG UBUNTU_SNAPSHOT=20261001T000000Z

FROM ${QUINT_IMAGE} AS quint

FROM ${UBUNTU_IMAGE}
ARG UBUNTU_IMAGE
ARG QUINT_IMAGE
ARG UBUNTU_SNAPSHOT

ENV DEBIAN_FRONTEND=noninteractive

COPY lock-gate.py /usr/local/lib/chimera-thin/lock-gate.py

# 1. Point every source at the snapshot before apt fetches anything.  Only
#    URIs change; Suites, Components and Signed-By stay as the base set them.
#    Refuse any other source, so nothing can resolve outside the snapshot.
# 2. The base has no CA store and the snapshot redirects http to https, so
#    ca-certificates comes from the snapshot with peer verification off for
#    those two commands only.  Integrity rests on the InRelease signature
#    checked against the Signed-By keyring, not on TLS.
# 3. Then a normal, verified update.  full-upgrade so nothing is held back,
#    and phased updates always included so the result does not depend on a
#    machine-id.
# 4. lock-gate.py: every installed package must be in the snapshot indexes.
#    The lists are gzip on this base (docker-gzip-indexes), so they are found
#    with apt-get indextargets and read with apt-helper cat-file.  Then prove
#    the gate can fail: one injected row must give exit 3 naming exactly
#    that row.
RUN set -eu; \
    if [ -e /etc/apt/sources.list ] && grep -Eq '^[[:space:]]*deb' /etc/apt/sources.list; then \
        echo "unexpected deb line in /etc/apt/sources.list" >&2; exit 1; fi; \
    if [ "$(ls -A /etc/apt/sources.list.d)" != ubuntu.sources ]; then \
        echo "unexpected files in /etc/apt/sources.list.d: $(ls -A /etc/apt/sources.list.d)" >&2; exit 1; fi; \
    sed -E -i "s#^URIs:[[:space:]]*https?://[^/]+/(ubuntu(-ports)?)/?[[:space:]]*\$#URIs: https://snapshot.ubuntu.com/\\1/${UBUNTU_SNAPSHOT}#" \
        /etc/apt/sources.list.d/ubuntu.sources; \
    grep -q '^URIs:' /etc/apt/sources.list.d/ubuntu.sources; \
    if grep '^URIs:' /etc/apt/sources.list.d/ubuntu.sources | grep -v "^URIs: https://snapshot.ubuntu.com/ubuntu\(-ports\)\?/${UBUNTU_SNAPSHOT}\$"; then \
        echo "a source is not the snapshot" >&2; exit 1; fi; \
    if [ -s /etc/ssl/certs/ca-certificates.crt ]; then \
        echo "bootstrap: CA store present, skipping"; \
    else \
        echo "bootstrap: no CA store, installing ca-certificates from snapshot ${UBUNTU_SNAPSHOT}"; \
        apt-get -o Acquire::https::Verify-Peer=false update; \
        apt-get -o Acquire::https::Verify-Peer=false -y --no-install-recommends install ca-certificates; \
    fi; \
    apt-get update; \
    apt-get -o APT::Get::Always-Include-Phased-Updates=true -y --no-install-recommends full-upgrade; \
    apt-get -o APT::Get::Always-Include-Phased-Updates=true -y --no-install-recommends install \
        bison \
        build-essential \
        ca-certificates \
        cmake \
        flex \
        git \
        libaio-dev \
        libcrypt-dev \
        libcurl4-openssl-dev \
        libjansson-dev \
        libkrb5-dev \
        libnghttp2-dev \
        libnuma-dev \
        librdmacm-dev \
        librocksdb-dev \
        libssl-dev \
        libunwind-dev \
        liburcu-dev \
        liburing-dev \
        libwbclient-dev \
        libxxhash-dev \
        make \
        ninja-build \
        openssl \
        pkgconf \
        python3 \
        uthash-dev \
        uuid-dev; \
    mkdir -p /tmp/lists; n=0; \
    for f in $(apt-get indextargets --format '$(FILENAME)' 'Created-By: Packages'); do \
        found=; \
        for c in "$f" "$f.gz" "$f.xz" "$f.lz4" "$f.zst"; do \
            if [ -e "$c" ]; then found=$c; break; fi; \
        done; \
        if [ -z "$found" ]; then echo "index target $f has no file" >&2; exit 1; fi; \
        /usr/lib/apt/apt-helper cat-file "$found" > /tmp/lists/$n; n=$((n+1)); \
    done; \
    set --; for i in $(seq 0 $((n-1))); do set -- "$@" --lists /tmp/lists/$i; done; \
    dpkg-query -W -f '${db:Status-Status}\t${Package}\t${Architecture}\t${Version}\n' > /tmp/lists/status; \
    gate=/usr/local/lib/chimera-thin/lock-gate.py; \
    python3 $gate --status /tmp/lists/status "$@" \
        --pin UBUNTU_IMAGE="${UBUNTU_IMAGE}" --pin QUINT_IMAGE="${QUINT_IMAGE}" \
        --pin UBUNTU_SNAPSHOT="${UBUNTU_SNAPSHOT}" --manifest /usr/share/chimera-thin; \
    cp /tmp/lists/status /tmp/lists/status.leak; \
    printf 'installed\topenssl\tamd64\t0.0-leak\n' >> /tmp/lists/status.leak; \
    rc=0; python3 $gate --status /tmp/lists/status.leak "$@" --pin UBUNTU_SNAPSHOT="${UBUNTU_SNAPSHOT}" \
        > /tmp/lists/leak.out 2>/dev/null || rc=$?; \
    if [ "$rc" != 3 ] || [ "$(cat /tmp/lists/leak.out)" != 'openssl:amd64=0.0-leak' ]; then \
        echo "lock gate did not detect injected leak (exit $rc): $(cat /tmp/lists/leak.out)" >&2; exit 1; fi; \
    echo "lock gate self-test: injected openssl:amd64=0.0-leak rejected (exit 3)"; \
    rm -rf /var/lib/apt/lists/* /tmp/lists

# node + quint from upstream's toolchain image.  Only /opt/quint: the warmed
# Rust evaluator (/quint-home) is not copied, because corpus generation uses
# the TypeScript backend (SPECS_QUINT_BACKEND) and podman runs with
# HOME=/build/home, where a copy in /root/.quint would never be read.
COPY --from=quint /opt/quint /opt/quint
ENV PATH=/opt/quint/bin:${PATH}

# Smoke check.  cc's output is captured first so a missing or broken cc fails
# the step; piped straight into head, only head's status would count.
RUN set -eu; quint --version; node --version; python3 --version; \
    cc_version=$(cc --version); printf '%s\n' "$cc_version" | head -n 1
