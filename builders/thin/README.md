<!--
SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
SPDX-License-Identifier: Unlicense
-->

# chimera-thin image

`builders/thin/Dockerfile` is the default image for the user-mode Podman
targets (`PODMAN_FLAVOR=thin`).  It carries what `make build` and the quick
ctest tier need, and nothing else:

- gcc, cmake, ninja, make, flex, bison, pkgconf, git
- the libraries configure requires or that the measured run linked
- python3
- node and quint, for the MBT trace corpus

It can be recreated from public inputs, and every input is pinned.  It does
not use `repository.mdh.quantum.com` or any other internal host.

The qb CI image is still `builders/dev/Dockerfile` (`chimera-dev`), and nothing
here changes it.  How the Podman targets run (user, caps, seccomp overlay,
skips) is described in [`builders/dev/README.md`](../dev/README.md); both
flavors run the same way.

## Flavors

| `PODMAN_FLAVOR` | Dockerfile | Image | Build dir |
|---|---|---|---|
| `thin` (default) | `builders/thin/Dockerfile` | `localhost/chimera-thin:local` | `~/.cache/chimera/podman-thin` |
| `dev` | `builders/dev/Dockerfile` | `localhost/chimera-dev:local` | `~/.cache/chimera/podman-dev` |

`PODMAN_DOCKERFILE`, `PODMAN_IMAGE` and `PODMAN_BUILD_DIR` can each be
overridden on their own.

Each flavor has its own build dir, because a tree configured in one image
carries that image's configure results into the other.  For example, the dev
image finds XLIO, oras, qemu and smbtorture, and the thin image has none of
them.  `podman-clean` removes only the current flavor's dir.  Trees from
before the split, such as `~/.cache/chimera/podman-build` or
`podman-libaio`, are not removed for you.

`BUILD_TYPE=Coverage` forces clang, and the thin image has only gcc.
`podman-build` and `podman-test` check for `clang` in `PODMAN_IMAGE` and stop
with a pointer to `PODMAN_FLAVOR=dev` when it is missing.

## Pins

All three pins are `ARG` lines at the top of the Dockerfile.

| Pin | What | Why |
|---|---|---|
| `UBUNTU_IMAGE` | `public.ecr.aws/ubuntu/ubuntu:26.04@sha256:<index>` | Canonical's ECR build of 26.04, pinned by its multi-arch index digest.  It is not byte-identical to the Docker Official `ubuntu:26.04`.  ECR also avoids Docker Hub's anonymous pull limit. |
| `QUINT_IMAGE` | `ghcr.io/chimera-nas/quint:<ver>@sha256:<index>` | Upstream's node + quint toolchain image (`Dockerfile.quint`).  Only `/opt/quint` is copied.  Corpus generation uses the TypeScript backend, so the Rust evaluator cache is not needed. |
| `UBUNTU_SNAPSHOT` | a `snapshot.ubuntu.com` timestamp | apt's only source.  It covers every pocket, security included. |

### How apt is locked

Every apt source is pointed at the snapshot before apt fetches anything, and
any other source is refused.

The base image has no CA store.  The snapshot redirects HTTP to HTTPS, so
`ca-certificates` is installed from the snapshot with
`Acquire::https::Verify-Peer=false` on those two apt commands only.  Integrity
comes from the InRelease signature, checked against the `Signed-By` keyring in
the base image.

After that, apt runs a normal TLS-verified `update`, then `full-upgrade` and
`install`, both with `APT::Get::Always-Include-Phased-Updates=true`.

### Lock gate

`lock-gate.py` then checks that every installed `(Package, Arch, Version)`
appears in the snapshot's `Packages` indexes.  The indexes are read with
`apt-get indextargets` and `apt-helper cat-file`, because the base keeps them
gzipped.  The gate's exit codes:

- `0`: every installed package is in the snapshot.
- `2`: bad input (no lists, no entries, no installed rows, an empty pin).
- `3`: some packages are missing.  The gate prints exactly those
  `Package:Arch=Version` triples.

The same build step then runs the gate a second time with an injected
`openssl:amd64=0.0-leak` row.  That run must exit `3` and name exactly that
row, or the build fails.  This proves on every build that the gate can fail.

The image records its inputs in two files:

- `/usr/share/chimera-thin/packages.txt`: every installed package.
- `/usr/share/chimera-thin/pins`: the three pins.

## Rebuild

```bash
make podman-image                               # thin (default)
make podman-image PODMAN_BUILD_FLAGS=--no-cache # cold rebuild
make podman-test                                # Debug quick tier in localhost/chimera-thin:local
```

A rebuild contacts only these hosts:

- `public.ecr.aws`: the base image.
- `ghcr.io`, with blobs from `pkg-containers.githubusercontent.com`: the quint image.
- `snapshot.ubuntu.com`: apt.

It does not contact `archive.ubuntu.com`, `security.ubuntu.com`, npm,
nodejs.org, github.com source clones, or any `*.quantum.com` host.

Disk: the image is about 0.9 GB (905 MB measured 2026-10-03) and a Debug tree
about 3.6 GB.
`podman-image` prints `df` before it starts.

## Bumping the pins

```bash
make podman-image-bump            # rewrite the pins, print the diff
make podman-image-bump DRY_RUN=1  # same checks, Dockerfile untouched
```

`podman-image-bump` needs the network, `skopeo` and `curl`.  No other target
needs those two tools; the other targets need only podman, make and git.
skopeo is used because podman cannot report a remote digest without pulling.
`podman manifest inspect` re-serializes the JSON, so its hash is not the
digest.

What it does, refusing before it writes anything if any check fails:

1. It refuses if `builders/thin/Dockerfile` is untracked or has uncommitted
   changes.  It also refuses if `ext/specs` is not at its recorded commit, or
   if `ext/specs/.quint-version` is dirty, empty or malformed.
2. It resolves the base index digest and the digest of
   `ghcr.io/chimera-nas/quint:$(cat ext/specs/.quint-version)`.  For each
   digest, the raw manifest must be non-empty and carry an index or manifest
   mediaType, the digest must match `^[0-9a-f]{64}$`, and skopeo's own
   `{{.Digest}}` must agree.
3. It sets the snapshot to the current UTC time, and refuses if that is
   earlier than the newest base `Created` time across amd64 and arm64.
4. It rewrites the three `ARG` lines and prints the diff.  It never builds,
   commits or pushes.

Then:

1. Run `make podman-image PODMAN_BUILD_FLAGS=--no-cache`.  The lock gate and
   its self-test must pass.
2. Run `make podman-test` and compare the result with the scorecard in
   `doc/TESTING.md`.
3. Review the diff and commit it yourself.

The pins do not move on their own.  Security updates reach this image only
when someone bumps it, for example alongside an upstream sync.

**Snapshot capture lag.** The snapshot service publishes a capture some time
after the archive changes.  If the base was rebuilt shortly before a bump, the
latest capture can predate it.  The base can then carry a package version
that the snapshot does not list yet.  The lock gate fails the build and names
that package.  That is the gate working as intended, not a bug.  Wait for the
next capture, then bump again.
