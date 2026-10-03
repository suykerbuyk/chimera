<!--
SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
SPDX-License-Identifier: Unlicense
-->

# chimera-dev image and user-mode Podman

`builders/dev/Dockerfile` is the `chimera-dev` image: the toolchain and test
dependencies the qb CI legs build and test in (`qb.yaml`, task
`chimera-dev`).  The `podman-*` targets in the top-level Makefile (defined in
`mk/vp-facade.mk`) build the same image locally and run the build and the
quick test tier in it **unprivileged**.

Same image, different run.  The qb test legs run as root, run the extended
tier (`ctest -C extended`, sharded), and the `ctest_release` /
`ctest_debug` / `ctest_coverage` legs configure with
`-DCHIMERA_MBT_CORPUS=OFF`.  Local `podman-test` runs unprivileged, runs the
quick tier only, and configures with the MBT corpus on (quint is in the
image).  A green `podman-test` is therefore not a local copy of a qb leg.

## Targets

```bash
make podman-image   # build localhost/chimera-dev:local (network, slow)
make podman-build   # configure + build BUILD_TYPE (default Debug) in the container
make podman-test    # build, then run the quick ctest tier in the container
make podman-shell   # interactive bash -l in the container, same user and caps
make podman-clean   # remove PODMAN_BUILD_DIR (the image is left alone)
```

`podman-image` is never an implicit prerequisite.  `podman-build`,
`podman-test` and `podman-shell` stop with "run make podman-image first" when
the image is missing.

| Variable | Default | Meaning |
|---|---|---|
| `PODMAN_IMAGE` | `localhost/chimera-dev:local` | image tag built and run |
| `PODMAN_BUILD_DIR` | `~/.cache/chimera/podman-build` | host dir bind-mounted at `/build` |
| `BUILD_TYPE` | `Debug` | `Debug`, `Release` or `Coverage` |
| `PODMAN` | `podman` | container CLI |

The source tree is mounted at `/chimera`, so inside the container the
Makefile builds under `/build/<BUILD_TYPE>`, which is `PODMAN_BUILD_DIR` on
the host.  `HOME` is `/build/home`.  Builds and tests run with
`--network=none`; `podman-shell` keeps the default network.

## Disk

The image plus a Debug build can take 12–25 GB across podman storage
(`~/.local/share/containers`) and `PODMAN_BUILD_DIR`.  `podman-image` prints
`df` for both before it starts.  Point `PODMAN_BUILD_DIR` somewhere roomier if
`/home` is tight.

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
