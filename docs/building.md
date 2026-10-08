---
title: Building from Source
layout: default
nav_order: 2
permalink: /building
---

# Building from Source

Chimera is built with CMake and Ninja. There are three supported paths,
roughly in order of convenience:

1. The **devcontainer** at `.devcontainer/` — recommended for development.
2. A **native build** on a supported Linux host, using the per-OS Dockerfiles
   in the repository root as a reference for required packages.
3. The **top-level Makefile**, which wraps the CMake invocations for both
   devcontainer and native workflows.

For MSVC builds on Windows x64 or ARM64, see [Native Windows development](windows.md).

## Using the devcontainer

The repository ships a [development container](https://containers.dev/) at
`.devcontainer/` configured for VS Code and Cursor. It is the easiest way to
get a working build environment because every dependency, including optional
ones like XLIO and FIO, is preinstalled.

To use it:

1. Open the repository in VS Code or Cursor with the Dev Containers extension
   installed.
2. When prompted, choose **Reopen in Container**. The container is built from
   `.devcontainer/Dockerfile` on first launch.
3. The repository is mounted read-write at `/chimera` and used as the
   workspace root.

Inside the container, the build directory defaults to `/build` (set via
`CHIMERA_BUILD_DIR=/build` in `devcontainer.json`) so that build outputs live
on a docker volume rather than the source bind mount.

The devcontainer is `privileged: true` and ships with KVM, Docker-in-Docker,
Samba, MIT Kerberos, and FIO available, which is enough to run the full test
matrix locally.

## Reference Dockerfiles per OS

In addition to the runtime `Dockerfile` (which produces the published
`ghcr.io/chimera-nas/chimera` image), the repository root contains a set of
per-OS Dockerfiles whose `apt`/`dnf` invocations enumerate the dependencies
required to build chimera on each platform:

| File                       | Base image           |
|----------------------------|----------------------|
| `Dockerfile.ubuntu22.04`   | `ubuntu:22.04`       |
| `Dockerfile.ubuntu24.04`   | `ubuntu:24.04`       |
| `Dockerfile.ubuntu26.04`   | `ubuntu:26.04`       |
| `Dockerfile.rocky9`        | `rockylinux:9`       |
| `Dockerfile.rocky10`       | `rockylinux:10`      |

These are not meant to produce the final runtime image; they exist as
authoritative, CI-tested package lists for each distribution. If you are
building chimera natively on Ubuntu 24.04, for example, the package list in
the first `RUN apt-get install` block of `Dockerfile.ubuntu24.04` is the
canonical reference. The same applies to the `dnf install` block in the
Rocky Linux variants.

## The top-level Makefile

The `Makefile` at the repository root is a thin wrapper over CMake and Ninja.
It picks a build directory automatically — `/build` when the source tree is
at `/chimera` (devcontainer convention), and `./build` everywhere else — and
exposes the targets you'll typically use.

### Build and test

```bash
make                # list targets (help); does not build
make release        # build_release + test_release
make debug          # build_debug + test_debug

make build_release  # release build, no tests
make build_debug    # debug build, no tests
make test_release   # ctest on the release build
make test_debug     # ctest on the debug build
```

The debug build enables AddressSanitizer; the release build is what CI ships
and what the published container image runs.

Build outputs land under `${CHIMERA_BUILD_DIR}/Release` or
`${CHIMERA_BUILD_DIR}/Debug`. You can rebuild incrementally without going
through `make` by running `ninja -C build/Release` (or `build/Debug`)
directly.

### Code quality

```bash
make syntax         # auto-format all C sources with uncrustify
make syntax-check   # verify formatting without modifying files
make check          # run the full CI check matrix locally
```

`make check` runs syntax-check, both release and debug builds and their test
suites, Clang static analysis (`scan-build`), the REUSE / SPDX license lint,
and the copyright-year check. Passing `make check` locally clears the
equivalent CI gates only with the privileges CI has. Unprivileged, the tests
that need privilege skip (exit 77) or are not registered at all, and
`make check` covers only the quick tier (`make check_extended` covers the
full suite).

### Unprivileged local loop (user-mode Podman)

A host with only `podman`, `make` and `git` can build and run the quick tier
in a container without installing chimera's build dependencies. Build the
image once with `make podman-image`, then use `make podman-build` and
`make podman-test`. The container runs as your uid with no added
capabilities, so tests that need privilege skip.

The default image (`PODMAN_FLAVOR=thin`, `builders/thin/Dockerfile`) is
pinned and can be recreated from public inputs. Building it needs no
Quantum registry. `PODMAN_FLAVOR=dev` uses the `chimera-dev` CI image
instead, and building that one needs the internal registry. See
[User-mode Podman build and test](architecture/user-mode-podman.md) for the
design, the skip list, how the loop differs from the CI legs, and how to
maintain it.

To run the privileged CI test legs locally (off the gate, as root in a scoped
Podman container, for evidence rather than as a CI substitute), there is a
separate opt-in route: `make podman-priv-*`, documented in
[`builders/priv/README.md`](../builders/priv/README.md) and
[Privileged legs (opt-in)](architecture/user-mode-podman.md#privileged-legs-opt-in).

### Other targets

```bash
make clean          # remove all build outputs under ${CHIMERA_BUILD_DIR}
make copyright      # update copyright years on files changed vs. main
make reuse-lint     # check SPDX license headers
make docs           # print pointers to the API docs and OpenAPI spec
```

The `BASE` variable controls the diff base for `make copyright` and
`make copyright-check` (default `main`); override it with
`make copyright BASE=origin/main` when working on a feature branch with a
different upstream.
