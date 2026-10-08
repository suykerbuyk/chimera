<!--
SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
SPDX-License-Identifier: Unlicense
-->

# chimera-dev image and user-mode Podman

`builders/dev/Dockerfile` is the `chimera-dev` image: the toolchain and test
dependencies the qb CI legs build and test in (`qb.yaml`, task
`chimera-dev`).  The `podman-*` targets in the top-level Makefile (defined in
`mk/vp-facade.mk`) build an image locally and run the build and the quick
test tier in it **unprivileged**.

Two images can back those targets, selected by `PODMAN_FLAVOR`:

- `thin` (the default) is `builders/thin/Dockerfile`.  It is a pinned,
  quick-tier-only toolchain that can be recreated from public inputs.  See
  [`builders/thin/README.md`](../thin/README.md).
- `dev` is this kitchen-sink image.

Everything below about privilege, seccomp and skips applies to both.

With `PODMAN_FLAVOR=dev` the local run uses the same image as qb, but runs
it differently.  The qb test legs run as root, run the extended tier
(`ctest -C extended`, sharded), and the `ctest_release` /
`ctest_debug` / `ctest_coverage` legs configure with
`-DCHIMERA_MBT_CORPUS=OFF`.  Local `podman-test` runs unprivileged, runs the
quick tier only, and configures with the MBT corpus on (quint is in the
image).  A green `podman-test` is therefore not a local copy of a qb leg.

## Targets

```bash
make podman-image        # build PODMAN_IMAGE for PODMAN_FLAVOR (network)
make podman-build        # configure + build BUILD_TYPE (default Debug) in the container
make podman-test         # build, then run the quick ctest tier in the container
make podman-shell        # interactive bash -l in the container, same user and caps
make podman-clean        # remove this flavor's PODMAN_BUILD_DIR (the image is left alone)
make podman-image-bump   # re-resolve the thin image's pins and show the diff (no build)
```

Add `PODMAN_FLAVOR=dev` to any of them to use this image.

`podman-image` is never an implicit prerequisite.  `podman-build`,
`podman-test` and `podman-shell` stop with "run make podman-image first" when
the image is missing.

| Variable | Default | Meaning |
|---|---|---|
| `PODMAN_FLAVOR` | `thin` | `thin` or `dev`; picks the defaults below |
| `PODMAN_DOCKERFILE` | `builders/$(PODMAN_FLAVOR)/Dockerfile` | Dockerfile `podman-image` builds |
| `PODMAN_IMAGE` | `localhost/chimera-$(PODMAN_FLAVOR):local` | image tag built and run |
| `PODMAN_BUILD_DIR` | `~/.cache/chimera/podman-$(PODMAN_FLAVOR)` | host dir bind-mounted at `/build` |
| `PODMAN_BUILD_FLAGS` | empty | extra `podman build` flags, e.g. `--no-cache` |
| `BUILD_TYPE` | `Debug` | `Debug`, `Release` or `Coverage` |
| `PODMAN` | `podman` | container CLI |

The source tree is mounted at `/chimera`, so inside the container the
Makefile builds under `/build/<BUILD_TYPE>`, which is `PODMAN_BUILD_DIR` on
the host.  `HOME` is `/build/home`.  Builds and tests run with
`--network=none`; `podman-shell` keeps the default network.

## Disk

With this image (`PODMAN_FLAVOR=dev`), the image plus a Debug build can take
12–25 GB across podman storage (`~/.local/share/containers`) and
`PODMAN_BUILD_DIR`.  The thin image is much smaller: see
`builders/thin/README.md`.  `podman-image` prints
`df` for both before it starts.  Point `PODMAN_BUILD_DIR` somewhere roomier if
`/home` is tight.

## Reproducibility: the apt+base lock

The dev image carries the `builders/thin` reproducibility discipline for its
**apt+base layer, as the image ships by default (`ENABLE_DOCA=0`)**: the base is
digest-pinned, every apt package is pinned to an exact version, and a lock gate
fails the build if the installed set drifts from a committed closure. This is
narrower than thin's guarantee, and the limits are spelled out below — read them
before trusting a green build.

The closure is **amd64-specific**. dev is amd64 in practice (the StorNext step is
amd64-only); an arm64 build would install arm64 package versions that are not in
`packages.lock` and so would fail the gate wholesale. Re-bumping on arm64 would
produce a different lock.

Because of this, `chimera-dev` is built **amd64-only** (`qb.yaml` declares
`architecture: [ amd64 ]`): `packages.lock` carries no arm64 closure and no arm64
build infra exists to generate one.

### What is and is not locked

**Locked (apt+base, `ENABLE_DOCA=0`):** the base image and every `apt-get install`
across all layers — the internal mirror and `download.docker.com` (`docker-ce`
et al.).

**Not locked (a separate follow-on):** the from-source and out-of-band tools —
`fio`/`libdpcp`/`libxlio` git branches, `elbencho`/`pynfs` clones, the
`dotnet-install --channel 8.0` SDK, `pip`/`gem`/`npm` installs, oras, WPTS, and
the StorNext `…-master` tarball. **Also not locked: `ENABLE_DOCA=1`.** That build
adds `cuda-toolkit-12-8` from the NVIDIA repo, unpinned and absent from
`packages.lock`, so a `DOCA=1` gate-on build is not reproducible and would fail
the gate — pinning cuda is deferred to the external-source follow-on. The lock
gate says **"apt+base layer frozen,"** never "the image reproduces." A green gate
is not a whole-image reproducibility claim.

### Pins

| Pin | What | Where |
|---|---|---|
| `BASE_IMAGE` | `repository.mdh.quantum.com/ubuntu:26.04@sha256:<index>` — the base by its multi-arch index digest | `ARG BASE_IMAGE` at the top of the Dockerfile (used by `FROM`) |
| apt versions | every named package as `pkg=version` on the install lines | the `apt-get install` lines |
| `packages.lock` | the full realized apt+base closure (all 964 installed packages, transitive deps and base residuals included), in apt `Packages`-stanza format | `builders/dev/packages.lock` |

Two names stay unpinned on purpose: `debhelper-compat` and `libboost-system-dev`
are virtual packages with no installable version of their own (provided by
`debhelper` and `libboost1.90-dev`, which the closure — and so the gate — still
covers).

### How apt is locked — and the mutable-mirror caveat

Unlike thin, dev has **no dated/immutable apt snapshot**. The internal mirror
(`repository.mdh.quantum.com`, "Artifact Depot") and `download.docker.com` serve
a **live, mutable** suite; there is no snapshot URL to freeze. So determinism is
achieved by pinning exact versions and committing the realized closure as the
frozen reference — the frozen point is the checked-in `packages.lock`, not the
mirror.

**Retention-horizon, loud fail (operator decision).** Reproducibility holds only
while the mirror still **retains** every pinned version. When a pinned version
ages out of the mirror, `apt-get install pkg=version` **fails the build loudly**,
naming the package — it never silently installs a newer one. That is the signal
to do a conscious **re-bump** (below), which is a deliberate mutation of the
frozen set. This is weaker than thin's immutable-snapshot guarantee by design:
thin can always rebuild its exact set from `snapshot.ubuntu.com`; dev cannot once
the mirror has moved. No `.deb` vendoring and no switch to `snapshot.ubuntu.com`
(which would change qb's egress contract and not cover docker-ce) were taken.

### Lock gate

The last layer runs `builders/dev/lock-gate.py`, a **byte-identical copy** of
`builders/thin/lock-gate.py` (keep the two in sync; a drift is a bug — their
sha256 must match). It is fed the committed `packages.lock` as `--lists` and the
live `dpkg-query` status as `--status`, and asserts every installed
`(Package, Arch, Version)` is in the closure. Exit codes: `0` all present; `2`
bad input; `3` some installed package is outside the closure (it prints exactly
those `Package:Arch=Version` triples). The same step then re-runs the gate with
an injected `openssl:amd64=0.0-leak` row and fails unless that run exits `3` and
names exactly that row — proving on every build that the gate can fail.

On success the image records its inputs under `/usr/share/chimera-dev`
(`packages.txt`, `pins`).

### Rebuild

```bash
make podman-clean PODMAN_FLAVOR=dev
make podman-image PODMAN_FLAVOR=dev PODMAN_BUILD_FLAGS=--no-cache   # lock gate must print "apt+base layer frozen"
```

A `--no-cache` build proves the pinned set is installable and the gate is green;
it does **not** prove bit-for-bit time-reproducibility (the non-apt sources
above are not frozen).

### Bumping the pins

```bash
make podman-image-bump PODMAN_FLAVOR=dev              # re-resolve base digest, regenerate packages.lock
make podman-image-bump PODMAN_FLAVOR=dev DRY_RUN=1    # resolve + report only, no rewrite, no build
```

`builders/dev/bump-pins.sh` re-resolves the base index digest (skopeo,
cross-checked like thin's) and rewrites the one `ARG BASE_IMAGE=` line, then
runs a reference build with the gate off (`DEV_LOCK_GATE=off`) and regenerates
`packages.lock` from that build's `dpkg-query` — **not** from the live mirror
index, which would not match the real resolution, base residuals or the
non-mirror (docker.com) packages. It never builds the verify image, commits or
pushes. After it runs: review the `packages.lock` diff, re-pin any changed
`pkg=version` on the install lines to match, run a normal `--no-cache` build so
the gate verifies, and commit yourself. Needs the network, `skopeo` and
`podman`.

The pins never move on their own. A security update reaches this image only
when someone bumps it.

## Privilege model

The container runs as your uid with no added privilege:

- `--userns=keep-id --user $(id -u):$(id -g)`
- `--cap-drop=ALL --security-opt no-new-privileges`
- Podman's default seccomp profile minus its `io_pgetevents` deny (see the
  libaio paragraph below); the io_uring syscalls stay blocked
- `--init --pids-limit=0`, `EVPL_IO_URING_ENTRIES=1024`

There is no `--privileged`, no `CAP_DAC_READ_SEARCH`, no `CAP_NET_ADMIN`, and
no seccomp change beyond that one overlay, and none should be added to these
targets.  Privileged
and root runs belong to the qb CI legs (`qb.yaml`, `.devcontainer/quantum/qb-ci.sh`)
and the VS Code devcontainer.

There is also a **separate, opt-in** way to run those privileged CI test legs
locally: the `podman-priv-*` family (`builders/priv/priv-ci.sh`,
`builders/priv/README.md`), which runs them as root in a scoped Podman container
via `sudo`.  It is deliberately a different target family — it adds no privilege
to the unprivileged `podman-*` targets above — and it is local evidence, not a
gate.

## What skips, and why

Tests that need privilege SKIP (ctest exit code 77) instead of failing.  A
pass under `podman-test` therefore says nothing about those tests.  The CI
legs run them.

| Backend | Native unprivileged host | `podman-test` |
|---|---|---|
| `linux` passthrough | SKIP: `open_by_handle_at` is EPERM without `CAP_DAC_READ_SEARCH` | same: SKIP on EPERM |
| `io_uring` passthrough | SKIP: `open_by_handle_at` EPERM, as above | SKIP: backend unavailable (`CHIMERA_VFS_ENXIO`). `io_uring_setup` is ENOSYS under seccomp, so the module fails to initialize |
| everything else in the quick tier (memfs, …) | runs | runs |

How many quick-tier tests skip depends on whether the MBT corpus is
configured, because the corpus adds `*_linux` and `*_io_uring` replay batches
on top of the probes.  Measured 2026-10-03 on `develop` @ `19c2796a`, Debug,
each in its own build dir:

| Run | Corpus | ctest total | Failed | Skipped | Skip cause |
|---|---|---|---|---|---|
| Native unprivileged `make test` | off (no quint on the host) | 90 | 0 | 11 (6 linux, 5 io_uring) | EPERM on both |
| `make podman-test` | on | 245 | 0 | 39 (20 linux, 19 io_uring) | linux EPERM; io_uring ENXIO, because `io_uring_setup` is denied |

Neither run failed a test.  The two runs register different test sets, so
their totals are not a ranking.  The SKIP line names the cause: "needs
CAP_DAC_READ_SEARCH" versus "backend unavailable".  ENXIO covers any host
where io_uring cannot initialize, including `kernel.io_uring_disabled=2`.

Some suites are not skipped but simply absent.  At configure time CMake
probes for `ip netns add` (network-namespace tests) and for raising
`memlock` / `aio-max-nr` (raised-limit tests).  Unprivileged, both probes fail
and those tests are never registered, so `ctest -N` lists fewer tests than
CI does.

One substitution is neither a skip nor an absence.  The diskfs test harnesses
pick their libevpl block backend at configure time
(`CHIMERA_DISKFS_DEVICE_TYPE`).  Since libaio 0.3.113, `io_getevents()` is
implemented with the `io_pgetevents` syscall.  Podman's default seccomp
profile lists `io_pgetevents` and `io_pgetevents_time64` in an explicit deny
group that returns EPERM, so libaio links but can never reap a completion.
CMake runs a small probe that submits and reaps one real read.  When the
probe fails, CMake sets `CHIMERA_DISKFS_DEVICE_TYPE=pread`, and the diskfs
tests run on libevpl's pread backend; the configure output says `diskfs
device type: pread`.  Native unprivileged hosts pass the probe and keep
`libaio`.  The 2026-10-03 scorecard above was measured that way: Podman
`pread`, native `libaio`.

The `podman-build`, `podman-test` and `podman-shell` targets now run with a
seccomp overlay instead of the default profile.  Before each run,
`builders/dev/seccomp-libaio.py` (run inside the image, so the host needs no
python) reads `PODMAN_SECCOMP_BASE` (default
`/usr/share/containers/seccomp.json`) and writes
`$(PODMAN_BUILD_DIR)/seccomp.json`.  The overlay is the default profile with
exactly two names removed from its lists, `io_pgetevents` and
`io_pgetevents_time64`.  Nothing is added to any allow list.  The two calls
now fall to the profile's default action, ENOSYS, and with that answer
libaio reaps completions (measured with the CMake probe).  The other 33 names
in that deny group (`vmsplice`, `userfaultfd`, `swapon`, …) stay denied, and
`io_uring_setup`, `io_uring_enter` and `io_uring_register` stay blocked.  The
generator refuses, and the target fails, if the base profile is missing, if
it does not list both names, if either is still listed afterwards, or if any
`io_uring_*` name would be allowed.  It never falls back to the default
profile.

With the overlay, configure reports `diskfs device type: libaio`.  A green
`podman-test` therefore covers the diskfs code over libevpl's libaio backend
on a regular file, in every test that takes `CHIMERA_DISKFS_DEVICE_TYPE`.
Tests built on `src/vfs/diskfs/tests/diskfs_test_harness.h` still hard-code
the pread backend.  It still proves nothing about a real block device or
storage array (the device is an image file in the build dir), and nothing
about io_uring, which remains SKIP.

There is no `podman-check` or `podman-integration`.  The extended tier
(`make integration`) needs privilege and is not supported under Podman.

## AddressSanitizer and `ptrace_scope`

Debug builds use ASan, and LeakSanitizer runs at exit by attaching to the
process with ptrace.  This works in the no-caps container when the host's
`kernel.yama.ptrace_scope` is 0 (measured).  It should also work at 1, because
LSan registers its tracer with `PR_SET_PTRACER`.  At `ptrace_scope >= 2`,
attaching needs `CAP_SYS_PTRACE`, which the container does not have, so the
leak checker fails.  `ptrace_scope` is a host setting the container cannot
change.  On such hosts, disable leak checking from a shell:
`make podman-shell`, then `ASAN_OPTIONS=detect_leaks=0 make test`.
Alternatively, use `BUILD_TYPE=Release`.
