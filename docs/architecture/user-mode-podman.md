---
title: User-Mode Podman Build and Test
layout: default
nav_order: 6
permalink: /architecture/user-mode-podman
---

# User-mode Podman build and test

This document describes the unprivileged Podman loop that builds chimera
and runs its quick test tier on a Linux host. The host needs:

- rootless `podman`, with Podman's default seccomp profile installed;
- `make` and `git`;
- network access, but only to build the image;
- `kernel.yama.ptrace_scope` at 0 or 1 for ASan leak checking.

The full list is under [Host dependencies](#host-dependencies). This
document covers:

- what the loop is;
- how it came to exist, and the problem each step solved;
- what it improves on, and what it cannot prove;
- how to keep it working.

The loop is a downstream (Quantum) addition. It lives in:

- `mk/vp-facade.mk`
- `builders/dev/` (the `dev` flavor and the seccomp overlay)
- `builders/thin/` (the `thin` flavor)

The upstream `Makefile` gains exactly one `include` line. The qb CI image,
`builders/dev/Dockerfile`, is unchanged.

Line references are to `develop` at `560311f4`.

## Contents

1. [What it is](#what-it-is)
2. [How it came to be](#how-it-came-to-be)
3. [Why it may be better](#why-it-may-be-better-than-what-was-there)
4. [What it has already found](#what-it-has-already-found)
5. [Maintaining it](#maintaining-it)
6. [References](#references)

## What it is

### Three environments, deliberately different

| Environment | How it runs | What it is for |
|---|---|---|
| Native host (`make test`) | Your uid, no added capabilities, whatever libraries the host has | The everyday edit-build-test loop |
| User-mode Podman (`make podman-*`) | Your uid mapped into the container, all capabilities dropped, `no-new-privileges`, a seccomp profile derived from Podman's default, no network for build and test | An unprivileged quick-tier run in a known image. With the default `thin` flavor, the image can be recreated from public inputs; the `dev` flavor cannot (see [below](#the-qb-kitchen-sink-image-as-a-local-loop)). |
| qb CI and the VS Code devcontainers | `privileged: true` on every qb test task (for example `qb.yaml:427`) and in both devcontainers (`.devcontainer/devcontainer.json:35`, `.devcontainer/quantum/devcontainer.json:52`) | The real gate. It runs the extended tier and everything the unprivileged runs skip. |

The Podman loop is not a local copy of a qb leg, and it differs in three
ways:

- **Privilege and tier.** qb's test legs run as root and run the extended
  tier, sharded. The Podman loop runs the quick tier, unprivileged.
- **The MBT corpus.** qb's `ctest_release`, `ctest_debug` and
  `ctest_coverage` legs configure with `-DCHIMERA_MBT_CORPUS=OFF`. The
  Podman loop keeps the model-based (quint) corpus on.
- **The test set.** Its job is a reproducible, privilege-free signal on the
  quick tier. It does not replace the gate.

### The facade

`Makefile:4` is the only line downstream adds to the upstream `Makefile`:

```make
include $(dir $(lastword $(MAKEFILE_LIST)))mk/vp-facade.mk
```

Everything else lives in `mk/vp-facade.mk`. Upstream's recipes stay
byte-identical, so an upstream sync merges cleanly. The facade does three
things:

- Bare `make` prints help and never builds (`override .DEFAULT_GOAL :=
  help`).
- The standard targets (`build`, `test`, `integration`, `install`, `clean`)
  call upstream's recipes.
- It adds the `podman-*` targets.

Nothing in the facade may fail at parse time, because qb's syntax job runs
`make syntax-check`, which reads the facade too. Invalid `BUILD_TYPE` and
`PODMAN_FLAVOR` values are therefore rejected inside recipes, never while
make parses the file (`mk/vp-facade.mk:39-40`).

### Targets and variables

| Target | What it does |
|---|---|
| `make podman-image` | Builds `PODMAN_IMAGE` from `PODMAN_DOCKERFILE`. Prints `df` first. Needs the network. It is never an implicit prerequisite of another target. |
| `make podman-build` | Runs `make build BUILD_TYPE=…` inside the container with `--network=none` |
| `make podman-test` | Runs `make test BUILD_TYPE=…` (build, then the quick tier) inside the container with `--network=none` |
| `make podman-shell` | An interactive `bash -l` with the same user, caps and seccomp, on the default network |
| `make podman-clean` | Removes this flavor's `PODMAN_BUILD_DIR`. The image is left alone. |
| `make podman-image-bump [DRY_RUN=1]` | Re-resolves the thin image's three pins and shows the diff. Never builds, commits or pushes. |

| Variable | Default | Meaning |
|---|---|---|
| `PODMAN_FLAVOR` | `thin` | `thin` or `dev`. Sets the defaults below. |
| `PODMAN_DOCKERFILE` | `builders/$(PODMAN_FLAVOR)/Dockerfile` | The Dockerfile `podman-image` builds |
| `PODMAN_IMAGE` | `localhost/chimera-$(PODMAN_FLAVOR):local` | The image tag that is built and run |
| `PODMAN_BUILD_DIR` | `~/.cache/chimera/podman-$(PODMAN_FLAVOR)` | Host dir bind-mounted at `/build` |
| `PODMAN_BUILD_FLAGS` | empty | Extra `podman build` flags, for example `--no-cache` |
| `PODMAN_SECCOMP_BASE` | `/usr/share/containers/seccomp.json` | The default profile the overlay is derived from |
| `BUILD_TYPE` | `Debug` | `Debug` (ASan), `Release` or `Coverage` |

These are defined at `mk/vp-facade.mk:42-56`. `make help` prints the current
values.

**Why each flavor has its own build dir.** CMake records configure-time
choices in the build tree, and those choices depend on the image. Two
examples:

- whether libaio can reap completions (see
  [diskfs device type](#diskfs-device-type));
- which optional tools were found.

The dev image finds XLIO, oras, qemu and smbtorture. The thin image has none
of them. Sharing one tree between images would carry one image's choices
into the other.

**Coverage needs clang.** Upstream's Coverage build forces clang
(`Makefile:20`). The thin image is gcc only. Before running, `podman-build`
and `podman-test` ask the image itself for `clang`
(`mk/vp-facade.mk:85-93`), so any `PODMAN_IMAGE` that has clang works.

### The run line

`mk/vp-facade.mk:62-68`:

```make
PODMAN_RUN = $(PODMAN) run --rm --init \
	--userns=keep-id --user $(shell id -u):$(shell id -g) \
	--cap-drop=ALL --security-opt no-new-privileges \
	--security-opt seccomp=$(PODMAN_SECCOMP) \
	--pids-limit=0 \
	-v $(CURDIR):/chimera:Z -v $(PODMAN_BUILD_DIR):/build:Z -w /chimera \
	-e HOME=/build/home -e EVPL_IO_URING_ENTRIES=1024
```

| Option | Effect |
|---|---|
| `--userns=keep-id --user uid:gid` | Files in the bind mounts stay owned by you, and nothing in the container is root. |
| `--cap-drop=ALL`, `no-new-privileges` | Nothing can regain privilege, including via setuid binaries. |
| `--network=none` (added by `podman-build` and `podman-test`) | The build and the tests cannot reach the network. |
| Source at `/chimera`, build at `/build` | Upstream's `CHIMERA_BUILD_DIR` defaults to `/build` when `$(CURDIR)` is `/chimera`, so the same `make build` works unchanged in the container. |
| `HOME=/build/home` | Persistent and writable. Tools such as quint can keep caches there. |

Never add `--privileged`, capabilities or a relaxed seccomp profile to these
targets. A test that needs privilege must skip (see the
[skip contract](#skip-contract)), not demand privilege.

### The seccomp overlay

Before each `podman-build`, `podman-test` and `podman-shell`, the facade
generates `$(PODMAN_BUILD_DIR)/seccomp.json` from `PODMAN_SECCOMP_BASE`
(`mk/vp-facade.mk:72-83`). It does so by running
`builders/dev/seccomp-libaio.py` with the image's `python3`, so the host
needs no python.

**The problem it solves.** libaio 0.3.113, the version in Ubuntu 26.04,
implements `io_getevents()` with the `io_pgetevents` syscall. The host's
default profile (`/usr/share/containers/seccomp.json`, checked on the build
host) has these rules:

| Calls | Rule |
|---|---|
| `io_pgetevents`, `io_pgetevents_time64` | In an explicit group of 35 names with `SCMP_ACT_ERRNO`, `errnoRet: 1`, so the call fails with **EPERM** |
| `io_setup`, `io_submit`, `io_getevents`, `io_cancel`, `io_destroy` | Allowed |
| Anything not listed, including every `io_uring_*` | The `defaultAction`, `SCMP_ACT_ERRNO` with `defaultErrnoRet: 38`, i.e. **ENOSYS** |

libaio treats EPERM from `io_pgetevents` as a failed reap. libevpl's poller
reads `n <= 0` as "no completions yet". Under the default profile, a diskfs
test configured for libaio therefore waits until the ctest timeout.

**The fix.** The generator removes the two names from every list and adds
nothing.
- The two calls then fall through to the profile default, ENOSYS.
- libaio treats ENOSYS as "this syscall does not exist" and reaps through the
  already-allowed `io_getevents`.
- It was measured: a direct `io_pgetevents` returns 38, and the CMake libaio
  probe exits 0.

Allowing the two names is not required, and the generator does not allow
them.

**What stays denied:**
- the other 33 names in that EPERM group;
- `io_uring_setup`, `io_uring_enter` and `io_uring_register`, which stay
  unlisted and so still get ENOSYS.

**When the generator refuses** (`builders/dev/seccomp-libaio.py:49-65`). It
exits non-zero and writes nothing to stdout if:

- the base profile cannot be read;
- the base profile no longer lists both names;
- either name is still listed after the edit;
- any `io_uring_*` name would be allowed.

The facade then stops with "Cannot generate …; not running without it". It
never falls back to the default profile, and no copy of the full profile is
vendored into the repo.

Commit `2d48281c` described the denial as ENOSYS. That was wrong, and
`73ace820` corrected the analysis to EPERM-in-a-group. The comment at
`CMakeLists.txt:612-614` still says "(ENOSYS)".

### The thin image

`builders/thin/Dockerfile` is the default image. It carries what
`make build` and the quick tier need:

- gcc (`build-essential`), cmake, ninja, make, flex, bison, pkgconf and git;
- the libraries configure requires or that the measured run linked: liburing,
  rocksdb, OpenSSL, jansson, uthash, curl, xxhash, unwind, uuid, numa, krb5,
  crypt, urcu, libaio, rdmacm, wbclient and nghttp2;
- python3;
- node and quint.

It has no clang, samba, KDC, qemu, docker, oras, XLIO, pynfs, fio, elbencho,
.NET/WPTS or StorNext.

**Pins.** Three `ARG`s sit before the first `FROM`
(`builders/thin/Dockerfile:27-29`), and the stage re-declares all three
(`:34-36`) so they are not empty inside it.

| Pin | Value at `560311f4` | Notes |
|---|---|---|
| `UBUNTU_IMAGE` | `public.ecr.aws/ubuntu/ubuntu:26.04@sha256:cd11a24d…` | Canonical's ECR build, pinned by its multi-arch OCI index digest. It is not byte-identical to the Docker Official `ubuntu:26.04`. ECR also avoids Docker Hub's anonymous pull rate limit. |
| `QUINT_IMAGE` | `ghcr.io/chimera-nas/quint:0.32.0@sha256:16fb1fdb…` | Upstream's toolchain image (`Dockerfile.quint`): the node tarball plus an npm-layout `/opt/quint`. Only `/opt/quint` is copied. Trace generation uses the TypeScript backend, and `HOME=/build/home` would never see a Rust evaluator cache in `/root/.quint`. |
| `UBUNTU_SNAPSHOT` | `20261001T000000Z` | A `snapshot.ubuntu.com` timestamp. This is apt's only source, for every pocket including security. |

**apt** runs in one `RUN` (`builders/thin/Dockerfile:57-127`):

1. **Sources.** The guards refuse to continue if `/etc/apt/sources.list` has
   any `deb` line, or if `sources.list.d` holds anything besides
   `ubuntu.sources`. Then every `URIs:` line is rewritten to the snapshot,
   and the step refuses if any source is not the snapshot. `Suites`,
   `Components` and `Signed-By` are left as the base set them.
2. **CA bootstrap.** The base has no CA store, and the snapshot redirects
   HTTP to HTTPS. So `ca-certificates` is installed **from the snapshot**,
   with `Acquire::https::Verify-Peer=false` on those two apt commands only
   (`:66-73`). Integrity comes from the InRelease signature checked against
   the base's `Signed-By` keyring, not from TLS. If the base ever ships a CA
   store, the step is skipped.
3. **Install.** A normal, TLS-verified `apt-get update`, then `full-upgrade`
   and `install` with `APT::Get::Always-Include-Phased-Updates=true`
   (`:74-104`).
   - `full-upgrade` means no package is held back for needing new
     dependencies.
   - Always including phased updates means the outcome does not depend on
     the container's machine-id.
4. **Lock gate.** See [the lock gate](#the-lock-gate) below.
5. **Clean up.** The apt lists are removed.

The image then copies `/opt/quint` from the quint stage, puts it on `PATH`,
and runs a smoke check that fails if quint, node, python3 or `cc` is
missing or broken (`:133-139`).

**What the image records.** Every image carries
`/usr/share/chimera-thin/pins` (the three pins) and
`/usr/share/chimera-thin/packages.txt` (every installed
`Package:Arch=Version`). The pins say what it was built from; the package
list says what it contains.

### The lock gate

`builders/thin/lock-gate.py` is a standalone, stdlib-only script. It checks
that every installed `(Package, Architecture, Version)` appears in the
snapshot's `Packages` indexes.

| Input | Content |
|---|---|
| `--status FILE` | `dpkg-query -W -f '${db:Status-Status}\t${Package}\t${Architecture}\t${Version}\n'`. Only rows whose first field is exactly `installed` count (`lock-gate.py:48`). |
| `--lists FILE` (repeatable) | Plain-text `Packages` stanzas |
| `--pin NAME=VALUE` (repeatable) | An empty value is an input error. |
| `--manifest DIR` | On success, writes `packages.txt` and `pins` |

| Exit | Meaning |
|---|---|
| 0 | Every installed package is in the snapshot. |
| 2 | Usage or input error: no lists, no stanzas, no installed rows, or an empty pin |
| 3 | At least one installed package is missing. stdout carries exactly the missing `Package:Arch=Version` triples and nothing else. |

**How the indexes are read.** The base image keeps apt's lists
gzip-compressed (`Acquire::GzipIndexes`). The Dockerfile therefore finds
them with `apt-get indextargets --format '$(FILENAME)' 'Created-By:
Packages'` and decompresses each with `/usr/lib/apt/apt-helper cat-file`
(`builders/thin/Dockerfile:105-113`).

The gate never asks `apt-cache policy` or `apt-cache show`. Both count
`/var/lib/dpkg/status` as a source, so every installed version would appear
to "exist" and the gate could never fail.

**The self-test.** After the gate passes, the same `RUN` appends one row,
`installed\topenssl\tamd64\t0.0-leak`, to a copy of the status and runs the
gate again. The build fails unless that run exits exactly 3 and its stdout
is exactly `openssl:amd64=0.0-leak` (`:119-126`). Every build therefore
proves the gate can fail, and fail for the right reason.

### Refreshing the pins

`make podman-image-bump` runs `builders/thin/bump-pins.sh`. It is the only
part of the loop that needs `skopeo` and `curl`.

- skopeo is needed because podman cannot report a remote digest without
  pulling: `podman manifest inspect` re-serializes the JSON, so its hash is
  not the digest.
- curl is needed because neither podman nor skopeo speaks apt.

Every check refuses, writing nothing, if it fails:

1. **Clean inputs** (`bump-pins.sh:36-48`):
   - the Dockerfile is tracked and unmodified;
   - `ext/specs` is at its recorded commit;
   - `ext/specs/.quint-version` is clean and matches
     `^[0-9]+\.[0-9]+\.[0-9]+$`.
2. **Digests** (`:55-68`), for the base tag and for
   `ghcr.io/chimera-nas/quint:$(cat ext/specs/.quint-version)`:
   - `skopeo inspect --raw` must exit 0 and return a non-empty body with an
     OCI or Docker index/manifest mediaType;
   - its sha256 must match `^[0-9a-f]{64}$`;
   - `skopeo inspect --format '{{.Digest}}'` must agree.
3. **Snapshot** (`:75-91`):
   - The snapshot ID is the current UTC time.
   - The base's `Created` time is read for amd64 and arm64 as
     `{{.Created.UTC.Format "20060102T150405Z"}}`, and each value must match
     `^[0-9]{8}T[0-9]{6}Z$`.
   - The snapshot must not be earlier than the latest of those times.
   - The four pocket InRelease files must be reachable. This is only a
     reachability check: the service answers for any timestamp after it
     started.
4. **Rewrite** (`:95-111`). Each `ARG` pin line must appear exactly once.
   The three lines are rewritten and re-read to confirm the new values.
   - With `DRY_RUN=1`, a temporary copy is rewritten instead and its diff
     printed.

### Skip contract

Unprivileged runs must be green with SKIPs, never red. A test whose
precondition needs privilege exits **77**, and its registration sets
`SKIP_RETURN_CODE 77`. Without that property, ctest counts the 77 as a
failure.

**Why linux and io_uring passthrough skip.** On mount, the linux and io_uring
backends probe `open_by_handle_at` (`linux_probe_open_by_handle`,
`src/vfs/linux/linux_common.h:388`). That call needs `CAP_DAC_READ_SEARCH`,
so unprivileged it returns EPERM. Under seccomp, io_uring's module cannot
initialize at all; `chimera_vfs_mount` then returns ENXIO
(`src/vfs/vfs_proc_mount.c:231-236`). The MBT harnesses map both to exit 77:

- `nfs3_mbt_common.h`
- `posix_driver.c` (`:1537-1551`)
- `s3_mbt_common.h` (`:759-776`)
- `vfs_xattr_dac_test.c`

**Gates that turn suites off instead.** Some configure-time gates register
fewer tests rather than skipping them:

- the `ip netns` probe (`CMakeLists.txt:766-800`);
- the raised-limits probe (`:811-841`);
- `KVM_TESTING=AUTO` (`:688-762`).

**Expected result** for the quick tier with the corpus on, in either flavor:

| Measure | Value |
|---|---|
| Registered | 245 |
| Skipped | 39 |
| Skipped because of linux passthrough | 20, each with "needs `CAP_DAC_READ_SEARCH`" |
| Skipped because of io_uring passthrough | 19, each with "backend unavailable" (ENXIO) |

Any other skip, or any failure, is a finding to investigate. It is not host
noise.

**Known intermittent failures, until CHIM-231 is fixed:**
`chimera/fuse/mbt/strict_memfs` and its sibling
`chimera/fuse/mbt/batch_memfs`. Both have been seen failing intermittently
under `ctest -j` with ESTALE (116) on `readdir`, on the same trace (see
[What it has already found](#what-it-has-already-found)). Record such a
failure against CHIM-231 rather than treating it as new. Any other failure
is new.

### diskfs device type

At configure time, CMake picks the block backend for the diskfs tests that
take `CHIMERA_DISKFS_DEVICE_TYPE` (`CMakeLists.txt:604-672`).

**How it decides.** When libaio links, configure compiles and runs a probe.
The probe submits one real `pread` on `/proc/self/exe` and reaps it with a
5-second timeout. An empty ring with a zero timeout would never reach the
denied syscall.

- If the probe succeeds, `HAVE_LIBAIO_USABLE` is true and the device type is
  `libaio`.
- If it fails, configure prints "libaio is linked but unusable here" and
  uses `pread`.
- The explicit `diskfs_aio` test variants are registered only when
  `HAVE_LIBAIO_USABLE` is true (`96a796ae`).

The choice is made once, at configure time. There is no runtime fallback.

| Environment | Device type |
|---|---|
| Native unprivileged host | `libaio` |
| Podman with the default profile (before `73ace820`) | `pread` |
| Podman with the libaio overlay (now; both flavors) | `libaio` (`LIBAIO_PROBE_RUN=0`) |

Configure in the environment you test in. A tree configured natively and
then run under Podman, or the other way round, carries the wrong choice.

White-box tests built on `src/vfs/diskfs/tests/diskfs_test_harness.h` still
hard-code `pread`.

## How it came to be

| Commit | Date | Change | The problem it solved |
|---|---|---|---|
| `0c8d77c0` | 2026-10-02 | linux/io_uring mounts probe `open_by_handle_at`. `chimera_vfs_mount` returns ENXIO when io_uring cannot initialize. Harnesses map EPERM and ENXIO to exit 77. | Unprivileged, NFS MNT returned SERVERFAULT, and io_uring returned ESTALE from a NULL module, so tests failed rather than skipped. |
| `118f3c30` | 2026-10-02 | The facade (`mk/vp-facade.mk`) and the first `podman-*` targets, on the qb `chimera-dev` image | There was no unprivileged way to run chimera's build and tests in a known image, and bare `make` built Debug. |
| `449f923a` | 2026-10-02 | `SKIP_RETURN_CODE 77` on the NFS aux and DRC linux/io_uring batches | Those replays already exited 77, but CMake did not set the property, so `podman-test` reported them as failed. |
| `2d48281c` | 2026-10-02 | The configure-time libaio probe, with a `pread` fallback | Under Podman's default seccomp, libaio could not reap, and diskfs tests hung until the timeout. |
| `96a796ae` | 2026-10-03 | Register `diskfs_aio` variants only when `HAVE_LIBAIO_USABLE` | "libaio linked" is not "libaio works". Those variants would still hang. |
| `19c2796a` | 2026-10-03 | Document the pread fallback | Under Podman, diskfs ran over pread only. Readers needed to know which device they were testing. |
| `fbbf2e1c` | 2026-10-03 | Record the measured scorecard | Skip counts and the corpus difference needed a measured baseline to compare later runs against. |
| `73ace820` | 2026-10-03 | The seccomp overlay: drop the `io_pgetevents` deny | The pread fallback meant user-mode runs never exercised libevpl's libaio backend. The overlay restores it without allowing anything new. |
| `560311f4` | 2026-10-03 | The thin pinned image as the default flavor, with the lock gate, `podman-image-bump` and per-flavor build dirs | The dev image could not be rebuilt without Quantum hosts, and nothing in it was pinned. |

### Design catches in the thin image

The thin image went through several plan and code review rounds before
implementation. Each item below is a failure the review caught on paper,
and the design now prevents it:

- **The CA bootstrap leaked live packages.** The first plan installed
  `ca-certificates` from the base's own archive sources before switching to
  the snapshot.
  - Pinning only `ca-certificates/resolute` still let its `openssl`
    dependency resolve to the live archive's newer version.
  - The upgrade from the snapshot never downgrades, so the image would have
    shipped a package that is not in the lock, and which version it got
    would have depended on the build day.
  - **Fix:** rewrite the sources first, then bootstrap from the snapshot
    with peer verification off for those two commands only.
- **A naive gate could never pass, or could never fail.** The base keeps
  apt's lists gzip-compressed, so a glob over `*_Packages` matches nothing,
  and every package would look missing. The tempting fixes were all wrong:
  - widening the glob reads gzip bytes as text;
  - skipping when no lists exist makes the gate vacuous;
  - asking `apt-cache` counts dpkg's own status as a source.

  **Fix:** `indextargets` plus `apt-helper cat-file`, and an input error on
  zero lists, zero stanzas or zero installed rows.
- **The self-test could pass vacuously.** Two problems:
  - an injected row written in `dpkg`'s `${Status}` format would be ignored
    by a gate that reads `${db:Status-Status}`;
  - a gate that echoes its inputs would satisfy a `grep` for the injected
    row on any failure.

  **Fix:** inject in the right format, give "missing" its own exit code (3),
  print only missing triples, and require both exit 3 and exact output.
- **The bump could pin the hash of an empty input.**
  `skopeo inspect --raw … | sha256sum` exits 0 even when skopeo fails, and
  prints `e3b0c442…`, the sha256 of empty input. A rate-limit error page
  hashes just as happily. **Fix:** check skopeo's exit status, the body and
  its mediaType, the digest shape, and agreement with `{{.Digest}}`.
- **Snapshot older than the base.** A snapshot of "today at 00:00Z" can
  predate a base Canonical rebuilt later the same day. Then the base can
  carry versions the snapshot lacks. **Fix:** use the current time, and
  refuse unless it is at or after the base's latest `Created`.
- **Smaller catches:**
  - `skopeo` rejects a reference with both a tag and a digest;
  - `git diff --quiet` is silent about an untracked file;
  - an `ARG` used in a `FROM` is empty inside the stage unless re-declared;
  - `cc --version | head` reports `head`'s status, not `cc`'s.

## Why it may be better than what was there

### The qb kitchen-sink image as a local loop

`builders/dev/Dockerfile` is qb's `chimera-dev` image. It serves qb well:
it carries every tool the extended tier and the privileged legs use. As a
local loop it has these properties:

- **It cannot be rebuilt without Quantum hosts.** It is
  `FROM repository.mdh.quantum.com/ubuntu:26.04` by tag (`:4`), with
  `APT_MIRROR` on the same host (`:31`). StorNext comes from
  `qsan.mdh.quantum.com` (`:210`).
- **Nothing pins what it installs:**
  - `apt-get upgrade` is unpinned (`:43`);
  - XLIO and libdpcp build from moving branches (`:92`, `:98`);
  - elbencho and pynfs are cloned from GitHub (`:77`, `:138`);
  - docker-ce comes from `download.docker.com` (`:109-111`);
  - .NET comes from `dot.net` (`:152`);
  - quint and gems come from npm and RubyGems without a lockfile (`:53`,
    `:200`).
- **It is large.** The local build measured 2026-10-03 (image
  `990d690d61ea`, since deleted) was about 4.3 GB. With a Debug tree, its
  own runbook budgets 12–25 GB.

### Native only

A native host builds against whatever libraries it happens to have. Without
quint on `PATH`, the MBT corpus is off. The measured native run registered
90 quick tests against 245 in the container.

### What the thin image changes

| Property | Thin image, measured 2026-10-03 (see the note below the table) |
|---|---|
| Hosts a rebuild contacts | `public.ecr.aws`, `ghcr.io`, `snapshot.ubuntu.com` only. No `*.quantum.com`, archive or security hosts, npm, nodejs.org or GitHub clones. |
| Pinned inputs | Base by index digest, quint by digest, apt by snapshot timestamp |
| Verified lock | The lock gate found all 262 installed packages in the snapshot. The self-test rejected the injected leak with exit 3. |
| Privilege | Unprivileged by construction: keep-id, no caps, no new privileges, no network for build and test |
| Size | 905 MB (`32eda4368f5e`) |
| Cold build | 3.6 minutes (19:15:26Z to 19:19:03Z) |
| Debug `podman-test` | 245 registered, 205 passed, 1 failed (`fuse/mbt/strict_memfs`, a known intermittent failure tracked as CHIM-231), 39 skipped |

The measured image was built from the tree committed as `560311f4`, with one
difference: the final smoke-check line changed after that build (the commit
message says so). The commit's own smoke line has not yet been through a
full build.

**Parity with the dev-image run.** The thin run was compared with the
`dev`-image run at `fbbf2e1c` + overlay:

- **Test names:** the same 245.
- **Skips:** the same 39, with the expected reasons.
- **Configure:** the same `diskfs device type: libaio` and
  `quint 0.32.0 (pinned 0.32.0)`.
- **CMakeCache:** a found/NOTFOUND diff differs only by the tools the thin
  image drops: `ELBENCHO_BIN`, `ORAS_EXECUTABLE`, `QEMU_EXECUTABLE`,
  `SMBTORTURE_EXECUTABLE` and `XLIO_*`. The node and quint paths move to
  `/opt/quint/bin`.
- **MBT corpus:** all 1458 trace files are identical once each file's
  `#meta.description` and `#meta.timestamp` (the generation time) are
  removed. So node 20.18.1 and the distro's node 22 generate the same
  traces.

### Limits: what a green run does not prove

- **It skips the privileged paths.** linux and io_uring passthrough skip.
  FUSE runs over an in-process socketpair harness
  (`src/server/fuse/tests/quint/fuse_sim.h`), not a kernel mount. The netns
  suites, raised-limit backends, KVM suites and the extended tier are not
  registered at all.
- **It does not test real storage.** The diskfs device is an image file in
  the build dir, not an array or block device, and io_uring stays denied.
- **qb remains the gate.** A change to passthrough, FUSE, KVM, netns-only or
  raised-limit code needs privileged evidence:
  `.devcontainer/quantum/qb-ci.sh` or the GitLab pipeline.
- **It is not bit-for-bit hermetic.** The inputs are pinned, but layer
  timestamps are not normalised and `SOURCE_DATE_EPOCH` is not set.
  "Recreatable" here means the same inputs, verified, not identical bytes.
- **The pins age.** Security updates reach the image only when someone runs
  the bump.
- **It is gcc only.** Clang-specific warnings, scan-build and the Coverage
  build need `PODMAN_FLAVOR=dev`.
- **Some evidence is only on the build host.** The measurements above are
  recorded in the commit message of `560311f4`. Fuller logs, the package
  manifest and normalised corpus hash lists are kept in
  `~/.cache/chimera/evidence/thin-reproducible-image-2026-10-03/` and
  `~/.cache/chimera/baselines/podman-libaio-2026-10-03/`, but only on that
  host.

## What it has already found

**CHIM-231: FUSE opendir ESTALE.**
- **Symptom.** In the thin image's first full Debug run,
  `chimera/fuse/mbt/strict_memfs` failed at step 145 of a trace identical to
  the passing baseline's: `readdir` returned ESTALE (116) where the model
  expected success.
- **First look.** Replayed alone, the trace passed 5 of 5 times, which looks
  like a flake.
- **Reproduction.** Forcing the VFS idle close sweep reproduces the failure:
  `CHIMERA_CLOSE_SWEEP_MIN_AGE_MS=0 CHIMERA_CLOSE_SWEEP_INTERVAL_MS=10`
  (the variables are read in `src/vfs/vfs.c:287,341`). CHIM-231 records a
  control run that passed and a forced-sweep run that failed at the same
  step with ESTALE.
- **Likely cause, not yet confirmed.** CHIM-231's code trace says:
  - a directory held open through FUSE `opendir` takes no backend
    reference;
  - after the directory is removed, the close sweep can drop the last
    reference;
  - the next `readdir` on the held stream then resolves a stale handle.

  The issue itself says "confirm before fixing".
- **Probably not the image.** The same trace passed in the dev-image
  baseline, and the failure reproduces under the forced sweep. Both are
  consistent with a race in the code rather than a difference in the image.
- **Sibling test.** Later load runs saw `chimera/fuse/mbt/batch_memfs` fail
  the same way, with ESTALE on the non-strict copy of the same trace
  (`posix/fuse/posixFuse_step_512_0x5_0`).

**CHIM-230: NFSv4 READ count not clamped.** Found by code reading in the
same effort, not by the loop, and not yet reproduced on the wire:
- NFSv4 READ passes the client's `count` to the VFS unclamped
  (`src/server/nfs/nfs4_proc_read.c:97`).
- NFSv3 clamps it (`nfs3_proc_read.c:128-133`).
- By the code, one oversized READ could make the VFS read-buffer allocation
  fail, and the VFS aborts when that allocation fails. No test has shown
  this yet.

**The principle.** A failing test in inherited code is a finding to file and
fix, not a flake to waive. The value of a reproducible, unprivileged loop is
that "it failed once" can be turned into "it fails under these conditions",
and then into a fix.

## Maintaining it

### Host dependencies

| Task | Needs |
|---|---|
| Build the image; build and test in it | `podman`, `make`, `git`, and network access for `podman-image` only |
| `podman-image-bump` | Also `skopeo` and `curl` |
| Seccomp overlay | Podman's default profile at `PODMAN_SECCOMP_BASE` (python runs in the image) |
| ASan leak checking | `kernel.yama.ptrace_scope` at 0 or 1. At 2 or above, run with `ASAN_OPTIONS=detect_leaks=0` or use `BUILD_TYPE=Release`. |

### Bumping the pins, end to end

1. Start from a clean checkout: `builders/thin/Dockerfile` committed,
   `ext/specs` at its recorded commit.
2. `make podman-image-bump DRY_RUN=1`. Read the report: the quint version,
   the base `Created` time, and the old and new value of each pin.
3. `make podman-image-bump`. This rewrites the three `ARG` lines.
4. `make podman-image PODMAN_BUILD_FLAGS=--no-cache`. The build log must
   show:
   - the `bootstrap:` line;
   - `lock: <N> installed packages, all in … snapshot <id>`;
   - `lock gate self-test: injected openssl:amd64=0.0-leak rejected (exit 3)`;
   - the four smoke versions.
5. Save the previous run as the comparison base **before** cleaning.
   `podman-clean` deletes `Testing/`, `CMakeCache.txt` and `specs-corpus/`.
   Skip this step if you compare against a fixed saved baseline instead.
   ```bash
   T=~/.cache/chimera/podman-thin/Debug
   B=~/.cache/chimera/baselines/thin-$(date -u +%Y%m%d)
   mkdir -p "$B"
   cp "$T"/Testing/Temporary/CTestCostData.txt \
      "$T"/Testing/Temporary/LastTestsDisabled.log \
      "$T"/CMakeCache.txt "$B"/
   python3 builders/thin/corpus-normhash.py "$T"/specs-corpus \
       > "$B"/specs-corpus.normsha256
   (cd "$B" && sha256sum * > SHA256SUMS) && chmod -R a-w "$B"
   ```
6. Run the tests from a fresh tree, with exactly one ctest run:
   ```bash
   make podman-clean
   make podman-test 2>&1 | tee podman-test.log
   ```
7. Compare the new tree against `$B` (see
   [Verifying parity](#verifying-parity)).
8. Review the Dockerfile diff and commit it. Record in the message: the
   image size, the build time, the lock line, and the test totals.

### Verifying parity

Compare a new run with a previous one. The previous one can be an earlier
thin run, or a `PODMAN_FLAVOR=dev` run when you are checking the thin image
itself.

| Compare | How | Expected |
|---|---|---|
| Test names | `Testing/Temporary/CTestCostData.txt` rows before `---` | Identical sets |
| Skips | `Testing/Temporary/LastTestsDisabled.log` | Only `*linux*` and `*io_uring*`, with the reasons in [Skip contract](#skip-contract) |
| Failures | The CostData section after `---` | Empty, except the known intermittent `fuse/mbt/strict_memfs` or `fuse/mbt/batch_memfs` (CHIM-231) until that issue is fixed |
| Configure | The test log | `diskfs device type: libaio`; `specs: quint X (pinned X)` and no "does not match the pinned" warning; `specs: generating the MBT corpus …` |
| CMakeCache | For each `*_LIB`, `*_LIBRARY`, `*_INCLUDE_DIR`, `*_EXECUTABLE`, `*_BIN`, `*_FOUND` key: found or NOTFOUND | Thin against dev differs only by the tools thin drops, plus the node and quint paths |
| MBT corpus | `python3 builders/thin/corpus-normhash.py <tree>/specs-corpus`, which hashes each trace after dropping `#meta.description` and `#meta.timestamp` (raw hashes change on every generation); `cmp` against the saved `specs-corpus.normsha256` | Identical (a quint version bump may legitimately change it) |

Two cautions:
- `ctest -N` overwrites `Testing/Temporary/LastTest.log`. Read the test
  names from CostData instead of running it in a tree you are measuring.
- Re-running ctest in the same tree makes the CostData counts cumulative.

### When quick-tier dependencies change

Add a package when configure starts to fail in the thin image, or when a
new quick-tier feature needs one. Remove one only after checking that
nothing still finds it. To see what configure needs:

- **The probes.** Search for `find_package`, `find_library`, `find_path`,
  `find_program` and `pkg_check_modules` under `CMakeLists.txt`, `cmake/`,
  `ext/libevpl/` and `src/`. Note which are `REQUIRED` and which only turn a
  feature off.
- **Linked bare.** Some libraries are linked without any probe (rocksdb for
  cairn, xxhash, unwind, uuid, numa, krb5, crypt). They fail at build time,
  not at configure time.
- **The CMakeCache diff against a dev-image tree.** Anything newly NOTFOUND
  in thin is a candidate.

Edit the sorted `install` list in `builders/thin/Dockerfile`. Then rebuild
cold and run the parity check. The lock gate covers any new package
automatically. A package that is optional but changes which tests register,
such as `libsqlite3-dev` for the KV variants, changes the expected test
set. Record that change.

### Bumping quint

1. Update `ext/specs/.quint-version` in the `ext/specs` submodule, and
   update the submodule pointer.
2. Make sure upstream has published `ghcr.io/chimera-nas/quint:<version>`.
   If it has not, the bump refuses at the skopeo step.
3. Run `make podman-image-bump`. It reads the version from
   `.quint-version`, so the quint tag and digest move together.
4. Rebuild and run the parity check. Corpus hashes may change legitimately
   when quint changes how it samples. Compare test outcomes rather than
   expecting identical traces.

The dev image pins quint separately (`builders/dev/Dockerfile:29`) and must
be kept in step with `.quint-version`.

### When the lock gate fails

The gate prints each missing `Package:Arch=Version`, one per line, and the
build stops with exit 3. The usual causes:

- **Snapshot capture lag.** "Now" resolves to the service's latest capture,
  which can predate a base rebuilt minutes earlier. The base then carries a
  version the snapshot does not list yet. Wait for a later capture, then
  re-run the bump.
- **The base and snapshot drifted apart** after a manual edit of one pin.
  Re-run the bump so they move together.
- **A source other than the snapshot appeared.** The earlier guards should
  have refused first. Check `ubuntu.sources` and `sources.list.d`.

Exit 2 means the inputs were wrong: no lists, an empty status or an empty
pin. That usually means apt's on-disk layout changed. Do not widen or skip
the gate to get past it.

### When the seccomp base profile changes

The overlay is derived from the host's profile every run, so profile
updates flow through by themselves. The generator refuses if a new profile:

- no longer lists both `io_pgetevents` names;
- would allow any `io_uring_*` call.

If that happens, read the new profile before changing anything. Do not
point `PODMAN_SECCOMP_BASE` at a hand-edited copy, and do not drop the
overlay. If a future libaio stops using `io_pgetevents`, or a new profile
stops denying it, configure reports `diskfs device type: libaio` either
way. The overlay is then harmless, and can be removed in a separate change.

### Upstream sync

The upstream sync procedure
(`.claude/skills/sync-main-to-develop/SKILL.md`) merges `origin/main` into
`develop`. Three things keep this loop out of its way:

- **The upstream `Makefile`.** Its only downstream change is the `include`
  at `Makefile:4`. Keep it there; do not edit recipes in `Makefile`.
- **The downstream files.** These are downstream-only:
  - `mk/`
  - `builders/dev/seccomp-libaio.py`
  - `builders/dev/README.md`
  - `builders/thin/`
  - this document, `docs/architecture/user-mode-podman.md`
  - the "Unprivileged local loop" subsection of `docs/building.md`, which
    otherwise belongs to upstream
- **Upstream changes to watch for:**
  - New quick-tier dependencies: re-check configure in the thin image after
    each sync.
  - Changes to `Dockerfile.quint` or the quint image tag scheme.
  - Changes to the libaio probe block in `CMakeLists.txt`.

### Disk budget and cleanup

| Item | Size |
|---|---|
| Thin image | 905 MB, plus the pulled base and quint layers |
| A Debug build dir | about 3–4 GB (`podman-thin` measured 3.6 GB right after the run) |
| Dev image | about 4.3 GB, and 12–25 GB with a build |

Cleanup commands:

```bash
make podman-clean                        # this flavor's build dir only
make podman-clean PODMAN_FLAVOR=dev      # the other flavor's
podman rmi localhost/chimera-thin:local  # an image you no longer need
podman images                            # find base/quint layers to rmi
```

`podman-clean` never touches build dirs from before the flavor split, such
as `~/.cache/chimera/podman-build`. Remove those by hand.

### Troubleshooting

| Message or symptom | Meaning | Action |
|---|---|---|
| `Image … not found: run make podman-image first` | No image for this flavor | `make podman-image` (with the matching `PODMAN_FLAVOR`) |
| `PODMAN_FLAVOR must be thin or dev` | Typo | Fix the variable |
| `BUILD_TYPE=Coverage needs clang in …` | Thin image, Coverage build | `PODMAN_FLAVOR=dev`, or another `BUILD_TYPE` |
| `Seccomp profile … not found` | `PODMAN_SECCOMP_BASE` points nowhere | Point it at Podman's default profile |
| `Cannot generate …/seccomp.json; not running without it`, preceded by `seccomp-libaio: …` | The generator refused (see [The seccomp overlay](#the-seccomp-overlay)) | Read the base profile; do not bypass |
| `a source is not the snapshot` / `unexpected files in /etc/apt/sources.list.d` | The base image's apt layout changed | Inspect the new base before bumping |
| `lock-gate: N installed package(s) not in the snapshot` | See [When the lock gate fails](#when-the-lock-gate-fails) | |
| `lock gate did not detect injected leak` | The gate's self-test failed | Treat as a broken gate; do not ship the image |
| `podman-image-bump: … is not tracked by git` / `has uncommitted changes` / `not checked out at its recorded commit` | Dirty inputs | Commit, stash, or `git submodule update --init ext/specs` |
| `podman-image-bump: digest mismatch … (tag moved?)` | The tag moved between two lookups | Re-run |
| `podman-image-bump: snapshot … is earlier than the base` | The clock is behind, or the base is newer than now | Check the host clock |
| `skopeo inspect … failed` | Network, rate limit, or no such tag | Check access to the registry; for quint, check the tag exists |
| diskfs tests hang until timeout, or configure says `diskfs device type: pread` in Podman | The tree was configured without the overlay, or under another environment | `make podman-clean` and re-run |
| `specs: quint … does not match the pinned …` warning | The image's quint differs from `.quint-version` | Run the bump so `QUINT_IMAGE` follows `.quint-version` |
| LeakSanitizer fails at exit | `kernel.yama.ptrace_scope` ≥ 2 | `ASAN_OPTIONS=detect_leaks=0` from `make podman-shell`, or `BUILD_TYPE=Release` |
| `fuse/mbt/strict_memfs` or `fuse/mbt/batch_memfs` fails with ESTALE (116) on `readdir` | Known intermittent failure, CHIM-231 (open) | Record it against CHIM-231; no new issue needed |
| A test outside `*linux*`/`*io_uring*` skips, or any other test fails | A regression or a real bug | Investigate and file it (as with CHIM-231) |

## References

| Path | What it is |
|---|---|
| `mk/vp-facade.mk` | Targets, variables, the run line, seccomp prep, flavor and Coverage checks |
| `builders/thin/Dockerfile` | The pinned thin image |
| `builders/thin/lock-gate.py` | The snapshot lock gate |
| `builders/thin/bump-pins.sh` | Re-resolves the pins |
| `builders/thin/corpus-normhash.py` | Normalised hash list of an MBT corpus, for parity checks |
| `builders/thin/README.md` | Thin image runbook |
| `builders/dev/Dockerfile` | qb's `chimera-dev` image (the `dev` flavor) |
| `builders/dev/seccomp-libaio.py` | Seccomp overlay generator |
| `builders/dev/README.md` | Podman runbook: privilege model, skips |
| `CMakeLists.txt:596-672` | libaio probe and diskfs device type |
| `CMakeLists.txt:766-841` | netns and raised-limits gates |
| `src/vfs/linux/linux_common.h:388` | `open_by_handle_at` probe |
| `src/vfs/vfs_proc_mount.c:231-236` | ENXIO for an uninitialised module |
| `docs/quint-testing.md` | Where model-based checks belong |

The commits are listed under [How it came to be](#how-it-came-to-be). The
Jira issues are CHIM-230 and CHIM-231.
