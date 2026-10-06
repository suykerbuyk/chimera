# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
# Base image, pinned by its multi-arch index digest and re-resolved by
# builders/dev/bump-pins.sh.  Declared before FROM so FROM can use it; re-declared
# after FROM so the lock-gate RUN can record it in the manifest.
ARG BASE_IMAGE=repository.mdh.quantum.com/ubuntu:26.04@sha256:f3d28607ddd78734bb7f71f117f3c6706c666b8b76cbff7c9ff6e5718d46ff64
FROM ${BASE_IMAGE}
ARG BASE_IMAGE

# DEV_LOCK_GATE=off skips the apt+base lock gate; used by the capture/reference
# builds in builders/dev/bump-pins.sh that regenerate packages.lock.  A normal
# `make podman-image PODMAN_FLAVOR=dev` leaves it on.
ARG DEV_LOCK_GATE=on

ARG ENABLE_XLIO=1
ARG ENABLE_DOCA=0
ARG ENABLE_FIO=1
ARG ENABLE_ELBENCHO=1
ARG ENABLE_CLAUDE=0
ARG ENABLE_CODEX=0
ARG ENABLE_UV=0
ARG ENABLE_WPTS=1
# Pinned to an exact chimera-nas/pynfs commit (on branch chimera-fast-ci).  Using a
# SHA rather than a moving branch makes the image reproducible and busts the Docker
# layer cache whenever pynfs is bumped.  Keep this in sync with .devcontainer/Dockerfile.
ARG PYNFS_REF=b861f7b8211b4dc798b88a7e81b300fb4615189c
# Pinned Microsoft Windows Protocol Test Suites (WPTS) FileServer release.
ARG WPTS_VERSION=4.26.3.0
ARG ELBENCHO_REF=96b1bf29c35e437715c0527194e69e4c96ed9922
# oras CLI, used by the KVM suites to pull the VM images from the registry.
ARG ORAS_VERSION=1.2.0
# Pinned quint release, used to generate the model-based-test trace corpus.
# Trace generation is seeded and so reproducible for a given release, but
# different releases may sample the state space differently, which is why
# ext/specs records the version it generated against and warns on a mismatch.
# KEEP IN SYNC with ext/specs/.quint-version (.devcontainer/alain/Dockerfile
# and ext/libevpl/.devcontainer/Dockerfile pin the same release).
ARG QUINT_VERSION=0.32.0

ENV APT_MIRROR=http://repository.mdh.quantum.com/repository/pkgs/ubuntu
ENV DEBIAN_FRONTEND=noninteractive

RUN if [ -n "$APT_MIRROR" ]; then \
    echo "deb $APT_MIRROR resolute main universe" > /etc/apt/sources.list.d/local-mirror.list && \
    echo "deb $APT_MIRROR resolute-updates main universe" >> /etc/apt/sources.list.d/local-mirror.list && \
    echo "deb $APT_MIRROR resolute-security main universe" >> /etc/apt/sources.list.d/local-mirror.list && \
    rm -f /etc/apt/sources.list.d/ubuntu.sources; \
    fi

# Quantum note. We do not need to unminimize since it is only a build_container
# Every apt package is pinned to the exact version captured in packages.lock
# (see builders/dev/README.md).  No blind `apt upgrade`: the frozen set is the
# only thing installed, so the resolved closure stays put until a conscious
# bump.  debhelper-compat and libboost-system-dev are left unpinned on purpose:
# they are virtual packages with no installable version of their own (provided
# by debhelper and libboost1.90-dev, which the lock gate covers via the closure).
RUN apt-get -y update && \
    apt-get -y --no-install-recommends install \
    clang=1:21.1.6-71 clang-tools=1:21.1.6-71 cmake=4.2.3-2ubuntu2 ninja-build=1.13.2-1 git=1:2.53.0-1ubuntu1 flex=2.6.4-8.2build2 bison=2:3.8.2+dfsg-1build4 lldb=1:21.1.6-71 gdb=17.1-2ubuntu1 less=668-1build1 vim=2:9.1.2141-1ubuntu4.1 psmisc=23.7-2ubuntu2 uncrustify=0.78.1+dfsg1-1build1 \
    net-tools=2.10-2ubuntu1 tshark=4.6.4-1 tcpdump=4.99.6-1 uuid-dev=2.41.3-3ubuntu2 iproute2=6.19.0-1ubuntu1 man-db=2.13.1-1build1 manpages-dev=6.17-1 ca-certificates=20260223 ssh=1:10.2p1-2ubuntu3.2 libjansson-dev=2.14-2build4 libclang-rt-21-dev=1:21.1.8-6ubuntu1 llvm=1:21.1.6-71 wget=1.25.0-2ubuntu4 \
    libxxhash-dev=0.8.3-2build1 liburcu-dev=0.15.6-1 librdmacm-dev=61.0-2ubuntu3 liburing-dev=2.14-1 libaio-dev=0.3.113-8build1 libunwind-dev=1.8.3-0ubuntu1 librocksdb-dev=9.11.2-1 libsqlite3-dev=3.46.1-9 libcurl4-openssl-dev=8.18.0-1ubuntu2.1 clangd=1:21.1.6-71 uthash-dev=2.3.0-2build1 libnuma-dev=2.0.19-1build1 \
    libboost-dev=1.90.0.1ubuntu3 libboost-program-options-dev=1.90.0.1ubuntu3 libboost-thread-dev=1.90.0.1ubuntu3 libboost-system-dev \
    build-essential=12.12ubuntu2 ruby-full=1:3.3build1 autoconf=2.72-3.1ubuntu2 automake=1:1.18.1-3build1 make=4.4.1-3 libtool=2.5.4-9 pkg-config=2.5.1-4 libs3-dev=2.0-4ubuntu4 gh=2.46.0-4 npm=9.2.0~ds3-1 reuse=5.0.2-1build1 libssl-dev=3.5.5-1ubuntu3 openssl=3.5.5-1ubuntu3 \
    libkrb5-3=1.22.1-2ubuntu4 libkrb5-dev=1.22.1-2ubuntu4 libgssapi-krb5-2=1.22.1-2ubuntu4 libwbclient-dev=2:4.23.6+dfsg-1ubuntu2 libcrypt-dev=1:4.5.1-1 curl=8.18.0-1ubuntu2.1 jq=1.8.1-4ubuntu2 libicu-dev=78.2-2ubuntu1 awscli=2.31.35-1 \
    krb5-kdc=1.22.1-2ubuntu4 krb5-admin-server=1.22.1-2ubuntu4 krb5-user=1.22.1-2ubuntu4 python3-pip=25.1.1+dfsg-1ubuntu2 python3-setuptools=78.1.1-0.1build1 python3-venv=3.14.3-0ubuntu2 \
    dh-python=7.20260309 pybuild-plugin-pyproject=7.20260309 debhelper-compat \
    samba=2:4.23.6+dfsg-1ubuntu2 samba-ad-dc=2:4.23.6+dfsg-1ubuntu2 samba-ad-provision=2:4.23.6+dfsg-1ubuntu2 samba-vfs-modules=2:4.23.6+dfsg-1ubuntu2 smbclient=2:4.23.6+dfsg-1ubuntu2 samba-testsuite=2:4.23.6+dfsg-1ubuntu2 && \
    gem install jekyll bundler

RUN if [ "$ENABLE_CLAUDE" = "1" ] ; then \
    npm install -g @anthropic-ai/claude-code ; \
    fi

RUN if [ "$ENABLE_CODEX" = "1" ] ; then \
    npm install -g @openai/codex ; \
    fi

# --disable-native: fio's configure otherwise adds -march=native, so the
# binary is tuned to whichever runner happened to build this image.  The qb
# cluster is a heterogeneous pool, so an image built on an AVX-512 node ships
# EVEX instructions that die with SIGILL at startup on a node without them --
# before fio has parsed a single argument.  Matches upstream's Dockerfiles.
RUN if [ "$ENABLE_FIO" = "1" ] ; then \
    git clone --depth 1 --branch fio-3.40 https://github.com/axboe/fio.git /fio && \
    cd /fio && \
    ./configure --disable-libnfs --disable-native && \
    make -j8 && \
    make install ; \
    fi

RUN if [ "$ENABLE_ELBENCHO" = "1" ] ; then \
    git clone https://github.com/chimera-nas/elbencho.git /elbencho && \
    cd /elbencho && \
    git checkout "$ELBENCHO_REF" && \
    make -j8 && \
    make install ; \
    fi

RUN if [ "$ENABLE_DOCA" = "1" ] ; then \
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/sbsa/cuda-keyring_1.1-1_all.deb && \
    dpkg -i cuda-keyring_1.1-1_all.deb && \
    apt-get update && \
    apt-get -y --no-install-recommends install cuda-toolkit-12-8 ; \
    fi

RUN if [ "$ENABLE_XLIO" = "1" ] ; then \
    git clone --depth 1 --branch gcc_15.2.0_build_fix https://github.com/arenaud16/libdpcp.git /libdpcp && \
    cd /libdpcp && \
    ./autogen.sh && \
    ./configure && \
    make -j8 && \
    make install && \
    git clone --depth 1 --branch 3.60.2-nlfix https://github.com/benjarvis/libxlio.git /libxlio && \
    cd /libxlio && \
    ./autogen.sh && \
    ./configure --with-dpcp=/usr/local && \
    make -j8 && \
    make install ; \
    fi

RUN apt-get -y update && \
    apt-get -y --no-install-recommends install qemu-system-x86=1:10.2.1+ds-1ubuntu3 qemu-utils=1:10.2.1+ds-1ubuntu3 && \
    install -m 0755 -d /etc/apt/keyrings && \
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc && \
    chmod a+r /etc/apt/keyrings/docker.asc && \
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu resolute stable" \
        > /etc/apt/sources.list.d/docker.list && \
    apt-get -y update && \
    apt-get -y --no-install-recommends install docker-ce=5:29.8.2-1~ubuntu.26.04~resolute docker-ce-cli=5:29.8.2-1~ubuntu.26.04~resolute containerd.io=2.3.6-1~ubuntu.26.04~resolute docker-buildx-plugin=0.37.1-1~ubuntu.26.04~resolute && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# oras: the OCI artifact client the KVM suites use to pull the VM images
# (vmlinuz, initrd, rootfs.qcow2) from the internal registry -- see
# kvm/fetch_vm_image.sh.  Baked into the image rather than fetched at job start:
# the qb ctest_kvm task runs in a cluster pod with no egress to github.com, so a
# runtime download made every KVM leg depend on an external host purely to obtain
# a tool that then talks to our own registry.  CMake picks it up off PATH via
# find_program(ORAS_EXECUTABLE oras); with -DKVM_TESTING=ON a missing oras is a
# configure-time FATAL_ERROR, so its absence can never be mistaken for a test
# failure.  dpkg's arch names (amd64/arm64) match the oras asset names exactly.
# --fail matters: without it curl pipes an HTTP error page into tar, which then
# reports a bogus "not in gzip format" and hides the real status.
RUN curl -fsSL \
        "https://github.com/oras-project/oras/releases/download/v${ORAS_VERSION}/oras_${ORAS_VERSION}_linux_$(dpkg --print-architecture).tar.gz" \
        | tar -xz -C /usr/local/bin oras && \
    chmod +x /usr/local/bin/oras && \
    oras version

RUN apt-get -y update && \
    apt-get -y --no-install-recommends install python3-ply=3.11-10 python3-gssapi=1.10.1-1build1 python3-pip=25.1.1+dfsg-1ubuntu2 && \
    pip3 install --break-system-packages xdrlib3 && \
    git clone https://github.com/chimera-nas/pynfs.git /opt/pynfs && \
    cd /opt/pynfs && git checkout "$PYNFS_REF" && python3 ./setup.py build && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

RUN if [ "$ENABLE_UV" = "1" ] ; then \
    curl -LsSf https://astral.sh/uv/install.sh | sh ; \
    fi

# .NET 8 + Microsoft Windows Protocol Test Suites (WPTS) FileServer suite, used
# by the MS-SMB2 server conformance ctests (src/server/smb/tests). WPTS speaks
# SMB2 from its own managed stack, so it runs host-side in a netns against the
# chimera daemon -- no VM needed. CMake auto-detects /opt/dotnet and /opt/wpts.
ENV DOTNET_ROOT=/opt/dotnet
RUN if [ "$ENABLE_WPTS" = "1" ] ; then \
    curl -sSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh && \
    bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /opt/dotnet && \
    ln -sf /opt/dotnet/dotnet /usr/local/bin/dotnet && \
    rm -f /tmp/dotnet-install.sh && \
    mkdir -p /opt/wpts && \
    curl -sSL "https://github.com/microsoft/WindowsProtocolTestSuites/releases/download/${WPTS_VERSION}/FileServer-TestSuite-ServerEP.tar.gz" -o /tmp/wpts.tar.gz && \
    tar -C /opt/wpts -xzf /tmp/wpts.tar.gz && \
    rm -f /tmp/wpts.tar.gz ; \
    fi

#
# Quantum specific addons
#

# quint drives the model-based tests.  Since upstream adb54a5c the whole
# trace corpus is GENERATED at build time -- cmake/Specs.cmake hands each
# quint suite's configs to ext/specs' generator, and there is no prebuilt
# bundle to fetch any more -- so without quint (and the node it ships with)
# on PATH, cmake disables every MBT replay ctest with a STATUS line and no
# leg reports anything missing.  The same was always true of the two models
# no bundle ever covered: src/vfs/diskfs/tests/quint, which lives in this
# repo, and the control-plane model under src/server/rest/tests/quint.  The
# ctest_quint_* legs in qb.yaml are what runs the replays; they configure
# with REQUIRE_CTL_MBT and REQUIRE_DISKFS_MBT so a quint that goes missing
# from this image is a configure error there rather than a quietly smaller
# suite.
#
# quint was omitted from this image while the upstream batch replayer wedged
# (trace stepFds_128_0x1_2 preceding another trace left the reset's umount
# waiting on a VFS close that never completed), because it would have
# re-registered the replays that qb.yaml was suppressing by hiding oras.
# That hang is fixed and the ctest_quint_* legs have run the replays since
# 2026-09-01.  The legs that must not pay for the replays twice (the extended
# ctest legs, static analysis, smbtorture, kvm) turn the corpus off with
# -DCHIMERA_MBT_CORPUS=OFF instead, which quint cannot defeat, so the two no
# longer interact.
#
# Generation defaults to quint's TypeScript backend (SPECS_QUINT_BACKEND in
# ext/specs/cmake/SpecsCorpus.cmake), which needs nothing beyond the npm
# install.  The Rust evaluator is still warmed below for a build that selects
# SPECS_QUINT_BACKEND=rust: quint downloads it on first run, into ~/.quint,
# and cluster pods reach the network through internal mirrors only, so that
# download arrives truncated and every generation fails.  Warm the cache
# here, where the network is available and the result is baked into the
# image.  Double quotes, so quint's prime operator (n') needs no escaping:
# inside single quotes a backslash is literal and would end the string early.
# The closing ls is the check -- if the evaluator did not land, fail here
# where it is fixable rather than in every downstream build job.
RUN npm install -g @informalsystems/quint@${QUINT_VERSION} && \
    printf "module warm {\n  var n: int\n  action init = n' = 0\n  action step = n' = n + 1\n}\n" \
        > /tmp/warm.qnt && \
    { quint run /tmp/warm.qnt --max-steps=1 --max-samples=1 > /dev/null 2>&1 || true ; } && \
    rm -f /tmp/warm.qnt && \
    ls -d /root/.quint/rust-evaluator-*/quint_evaluator && \
    quint --version

# install install snfs on amd64 arch
RUN if [ "`dpkg --print-architecture`" = "amd64" ] ; then \
        curl -s http://qsan.mdh.quantum.com/sw/rels/sn/7.2.5_E/GA/delivery/saveobjs/Ubuntu25_x86_64/objs-snfs-130-master.tar.gz | \
        tar zxvf - -C / ./usr/cvfs/include ./usr/cvfs/lib ; \
    fi

# ---------------------------------------------------------------------------
# apt+base lock gate.  Runs last, after every apt install across all RUN layers
# (nothing below the pynfs layer touches dpkg: the git/dotnet/npm/StorNext steps
# install to /usr/local, /opt and /usr/cvfs, not the dpkg database).  It is the
# dev counterpart of builders/thin's lock gate, but dev has no immutable apt
# snapshot: the internal mirror (and download.docker.com) are mutable, so the
# frozen reference IS the committed builders/dev/packages.lock, captured from a
# reference build's dpkg-query (see builders/dev/bump-pins.sh).  lock-gate.py is
# byte-identical to builders/thin/lock-gate.py; only what feeds its --lists
# differs (the committed closure, not a live snapshot index).
#
# The gate asserts every installed (Package, Arch, Version) is in packages.lock.
# That freezes the apt+base layer ONLY -- it says NOTHING about the git/dotnet/
# npm/StorNext sources, so its success line must never claim the image reproduces.
# DEV_LOCK_GATE=off skips it (the bump/reference capture builds).
COPY lock-gate.py /usr/local/lib/chimera-dev/lock-gate.py
COPY packages.lock /usr/local/lib/chimera-dev/packages.lock
RUN set -eu; \
    if [ "$DEV_LOCK_GATE" = off ]; then \
        echo "lock gate: SKIPPED (DEV_LOCK_GATE=off; reference/bump capture build)"; \
    else \
        gate=/usr/local/lib/chimera-dev/lock-gate.py; \
        lists=/usr/local/lib/chimera-dev/packages.lock; \
        dpkg-query -W -f '${db:Status-Status}\t${Package}\t${Architecture}\t${Version}\n' > /tmp/status; \
        rc=0; python3 "$gate" --status /tmp/status --lists "$lists" \
            --pin BASE_IMAGE="${BASE_IMAGE}" --manifest /usr/share/chimera-dev > /tmp/gate.out 2>/tmp/gate.err || rc=$?; \
        if [ "$rc" != 0 ]; then \
            echo "lock gate: FAILED (exit $rc) -- installed packages not in builders/dev/packages.lock (re-bump: make podman-image-bump PODMAN_FLAVOR=dev):" >&2; \
            cat /tmp/gate.out /tmp/gate.err >&2; \
            exit "$rc"; fi; \
        n=$(grep -c '^Package: ' "$lists"); \
        echo "lock gate: apt+base layer frozen -- $n packages, every installed package present in builders/dev/packages.lock"; \
        echo "lock gate: covers the DEFAULT (ENABLE_DOCA=0) apt+base layer only -- internal mirror + download.docker.com; it does NOT cover ENABLE_DOCA=1 cuda packages, nor the git/dotnet/npm/StorNext sources, and is NOT a full-image reproducibility claim"; \
        cp /tmp/status /tmp/status.leak; \
        printf 'installed\topenssl\tamd64\t0.0-leak\n' >> /tmp/status.leak; \
        rc=0; python3 "$gate" --status /tmp/status.leak --lists "$lists" > /tmp/leak.out 2>/dev/null || rc=$?; \
        if [ "$rc" != 3 ] || [ "$(cat /tmp/leak.out)" != 'openssl:amd64=0.0-leak' ]; then \
            echo "lock gate self-test FAILED (exit $rc): $(cat /tmp/leak.out)" >&2; exit 1; fi; \
        echo "lock gate self-test: injected openssl:amd64=0.0-leak rejected (exit 3)"; \
        rm -f /tmp/status /tmp/status.leak /tmp/gate.out /tmp/gate.err /tmp/leak.out; \
    fi
