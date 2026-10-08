<!--
SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
SPDX-License-Identifier: Unlicense
-->

# Opt-in privileged Podman test route (runbook)

This is the operator runbook for the `podman-priv-*` make targets
(`builders/priv/priv-ci.sh`). The route runs the CI **test legs** locally, as
root in a scoped Podman container, the way `.gitlab-ci.yml` runs them, **without
qb**. It is opt-in local privileged evidence, **not** a CI substitute: qb and the
GitLab pipeline remain the gate (see
[`docs/architecture/user-mode-podman.md`](../../docs/architecture/user-mode-podman.md),
section "Privileged legs (opt-in)").

The unprivileged `podman-*` family and the default-target SKIP-77 contract are
untouched. None of these targets is a prerequisite of any default target, and
none runs from `make test` or `make check`.

## What the family is

| Target | Sudo? | What it does |
|---|---|---|
| `make podman-priv-list` | no | Select the legs `PRIV_LEGS` picks and print them. |
| `make podman-priv-image` | yes | Pull `PRIV_IMAGE` rootless, stage with `save`, `load` into the scoped root store, assert `.Id` matches, write `image.lock`. |
| `make podman-priv-test` | yes | Build and run the selected legs in one privileged container. |
| `make podman-priv-compare` | yes | Run `PRIV_BASE` and `PRIV_HEAD` (each its own container), then compare with `tools/privci/compare.py`. |
| `make podman-priv-diff A=<dir> B=<dir>` | no | Compare two existing run dirs (rootless `compare.py` in the locked image). |
| `make podman-priv-clean` | only with `PRIV_CLEAN_STORE=1` | Remove `PRIV_DIR`'s own run dirs; with `PRIV_CLEAN_STORE=1` also the root store and the scoped `/run` dirs. |

Append `PRIV_DRY_RUN=1` to any of them to preview: every `sudo` line (and, for
`test`, the run dir, `legs.tsv`, each leg's `leg.sh` and `meta.txt`) is printed,
**nothing runs as root, and no container starts.** Always dry-run first.

## Prerequisites

- A published **`chimera-dev`** image reachable at `PRIV_IMAGE` (default
  `repository.mdh.quantum.com/chimera/chimera-dev:develop`). This route does not
  build the image; it pulls it. Anonymous pull is used.
- **Disk** on `PRIV_DIR`'s filesystem (all on `/home` by default):
  - `image` refuses below **25 GiB** free.
  - `test` refuses below **10 GiB** free.
  - Peak for a two-sided release compare is about **22 GB** (image ~4.4 GB,
    staged tar 4.4 GB deleted after load, root store ~4.4 GB, ~8 GB of release
    build trees, < 1 GB of runs). A KVM leg adds ~3.4 GB.
- `sudo` (a privileged run needs it; see `PRIV_SUDO`), and rootless **and**
  rootful Podman available to your user through `sudo`.

## Variables

| Variable | Default | Meaning |
|---|---|---|
| `PRIV_IMAGE` | `repository.mdh.quantum.com/chimera/chimera-dev:develop` | Image to pull and load. |
| `PRIV_DIR` | `$HOME/.cache/chimera/podman-priv` | Scoped store + runs + build trees. Guarded (below). |
| `PRIV_LEGS` | `ctest_*_pynfs ctest_*_smbtorture ctest_*_wpts ctest_*_rest ctest_quint_*` | Space-separated globs, matched by `qb-ci.sh --only`. |
| `PRIV_MODE` | `release` | `release` or `debug`; handed to `qb-ci.sh -m`. |
| `PRIV_SUDO` | `sudo` | Privilege-raising command. **Empty is refused**, no override. |
| `PRIV_BASE`, `PRIV_HEAD` | (unset) | The two commits a `compare` runs. Both **required** for `compare`. |
| `PRIV_EXPECT` | `builders/priv/section3/expect.txt` | Pre-registered transitions for `compare`/`diff`. |
| `PRIV_REQUIRE` | `builders/priv/section3/require.txt` | Presence checks for `compare`/`diff`. |
| `PRIV_REF` | (unset) | Source: unset exports the working tree; a rev does a cached `git archive`. |
| `PRIV_FRESH` | (unset) | `1` empties the shared build tree before each leg. |
| `PRIV_DRY_RUN` | (unset) | `1` previews without sudo or a container. |
| `PRIV_CLEAN_STORE` | (unset) | `1` makes `clean` also remove the root store and `/run` dirs (sudo). |

### `PRIV_DIR` guard rules

`PRIV_DIR` is checked by every subcommand that writes there (including a
dry-run), so a dangerous value is refused before anything is written. It must:

- be an **absolute** path;
- not be `/` or `$HOME`;
- not contain or equal the unprivileged Podman build dir
  (`$HOME/.cache/chimera/podman-<flavor>`), `$HOME/.cache/chimera`, or the repo
  top level;
- not be **inside** the repo (with `PRIV_REF` unset the working-tree export would
  otherwise try to copy the root-owned `storage/` back into the context).

A marker file `.chimera-podman-priv` proves the dir is ours: `image` (or the
first `test`) creates it, and `test`, `compare` and `clean` refuse a `PRIV_DIR`
without it. `image`, `test`, `compare`, and `clean` with `PRIV_CLEAN_STORE=1`
are refused when run as **uid 0** — run them unprivileged and let `PRIV_SUDO`
raise privilege.

## P4 — privileged smoke and interrupt tests (operator present)

Run these on the build host, in the worktree. Review each dry-run before its live
run.

```
# 1. Preview the image build (no sudo, nothing runs).
make podman-priv-image PRIV_DRY_RUN=1 PRIV_DIR="$HOME/.cache/chimera/podman-priv"

# 2. Build the scoped root store for real.  Check: the self-check passes
#    (graphRoot, runRoot, tmpdir); `df /` moves by less than 100 MB.
make podman-priv-image PRIV_DIR="$HOME/.cache/chimera/podman-priv"

# 3. A single-leg smoke test.  Check: netns configure succeeds; ctest.xml is
#    non-empty; the *_linux / *_io_uring probes run; no root-owned files outside
#    storage/; aio-max-nr restored; no container left.
make podman-priv-test PRIV_LEGS=ctest_release_rest PRIV_MODE=release \
  PRIV_DIR="$HOME/.cache/chimera/podman-priv"
```

**Interrupt tests.** Start the smoke test (step 3) again and, in turn:

- (a) send a single Ctrl-C;
- (b) send a double Ctrl-C;
- (c) from a second terminal, stop the container with the scoped wrapper:
  `sudo env TMPDIR=$PRIV_DIR/tmp podman --root $PRIV_DIR/storage --runroot /run/chimera-priv-$USER/storage --tmpdir /run/chimera-priv-$USER/libpod stop chimera-priv-$USER-<runid>`.

Each must print the `aio-max-nr: restored …` line, leave **no** container, leave
**no** stray root-owned files, and restore `aio-max-nr`. If anything is left, use
the [Recovery](#recovery) lines the driver prints.

**Clean check.** Then:

```
make podman-priv-clean PRIV_CLEAN_STORE=1 PRIV_DIR="$HOME/.cache/chimera/podman-priv"
```

and confirm `$PRIV_DIR/storage` and `/run/chimera-priv-$USER/{storage,libpod}`
are gone, and that a fresh `image` then `test` still succeeds (no stale `/run`
state trips the pre-run refusal).

## P5 — the SMB section-3 comparison (the first real use)

One prepared command. It runs each side in its own container, then compares.

```
make podman-priv-compare PRIV_BASE=03e81248 PRIV_HEAD=61d14614 PRIV_MODE=release \
  PRIV_DIR="$HOME/.cache/chimera/podman-priv" \
  PRIV_LEGS='ctest_*_pynfs ctest_*_smbtorture ctest_*_wpts ctest_*_rest' \
  PRIV_EXPECT=builders/priv/section3/expect.txt \
  PRIV_REQUIRE=builders/priv/section3/require.txt
```

`PRIV_EXPECT` and `PRIV_REQUIRE` already default to those two files, so they may
be omitted; they are spelled out here for the record.

- **Runtime: roughly 1.5–2.5 h** (two release builds plus the pynfs, smbtorture,
  WPTS and rest legs, run sequentially).
- `03e81248` and `61d14614` are the decision-9 before/after subjects. They are
  deliberately behind the branch point, so the `builders/dev` drift warning fires
  for **both** sides; this is expected and is recorded with the run.
- The run writes `compare.md` into the head run dir and prints it.

**Exit codes** (passed through by `podman-priv-compare`):

| Exit | Meaning |
|---|---|
| 0 | Clean: no unexpected new failures. |
| 1 | Regression: an unexpected new failure (`Passed->Failed` not covered by a satisfied `expect` entry), a required pattern missing on a side, or the image `.Id`s differ. |
| 2 | **Operator review required**: an `expect` entry is a mismatch or stale, with no exit-1 condition. The driver prints `OPERATOR REVIEW REQUIRED: …; see <head>/compare.md`. |

A `Passed->Failed` outside `expect.txt`, or an exit-2 mismatch, should be re-run
alone (`PRIV_LEGS=<leg>`) to classify it as reproducible or intermittent. An
intermittent failure is a finding, never a waiver.

> **Pending operator privileged run.** The section-3 numbers (counts, the
> `compare.md` body, image `.Id`/RepoDigest and run paths) are recorded in this
> task's `Execution` section only after the operator runs P5. Do not fill them in
> here before then.

## P6 — KVM trial (does not block P5)

```
make podman-priv-test PRIV_LEGS=kvm_release_ubuntu2604 PRIV_MODE=release \
  PRIV_DIR="$HOME/.cache/chimera/podman-priv"
```

The KVM leg uses a **bridge** network (the only leg that does; it adds a host
`podman0` interface and netavark rules for the run) and about **3.4 GB** of extra
disk.

> **Pending operator privileged run (decision D6).** Whether KVM is supported or
> documented out of scope depends on this run: support it if green; otherwise
> record it as out of scope with the exact error. Leave this open until the
> operator runs it.

## Recovery

The host-side interrupt/recovery net records `fs.aio-max-nr` unprivileged before
a run, so even if the in-container cleanup is bypassed (SIGKILL, a wedged
container) the driver can print — **never run** — the exact lines to restore the
host. They are, with real values substituted:

```
<scoped-wrapper> rm -f chimera-priv-$USER-<runid>
sudo sysctl -w fs.aio-max-nr=<before>
sudo chown -R <uid>:<gid> <run-dir> <build-root>
```

where `<scoped-wrapper>` is
`sudo env TMPDIR=$PRIV_DIR/tmp podman --root $PRIV_DIR/storage --runroot /run/chimera-priv-$USER/storage --tmpdir /run/chimera-priv-$USER/libpod --volumepath $PRIV_DIR/storage/volumes`.

For a full manual cleanup of the root-owned scoped `/run` state the non-sudo
`clean` cannot remove:

```
sudo rm -rf /run/chimera-priv-$USER
```

A full `make podman-priv-clean PRIV_CLEAN_STORE=1` does the equivalent in order:
`<scoped-wrapper> rmi -a`, then `sudo rm -rf $PRIV_DIR/storage`, then
`sudo rm -rf /run/chimera-priv-$USER/storage /run/chimera-priv-$USER/libpod`.

## What this route does not reproduce

See the honesty caveats in `docs/architecture/user-mode-podman.md`
("What it does not reproduce"): ceph S3 (no s3-tests venv in `chimera-dev`), the
shared build dir (qb gives each leg a fresh pod), sequential rather than sharded
legs, pod resource/`--pids-limit` differences, the executor-shell assumption
(legs run under `bash -e -o pipefail`, recorded in `meta.txt` as a deviation),
and image currency (the image tracks develop, not a source commit; runs pin by
`.Id` and record the `builders/dev` drift warning).
