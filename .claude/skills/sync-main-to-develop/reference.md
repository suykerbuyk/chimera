# Upstream sync reference

Per-artifact detail for the `sync-main-to-develop` skill. Read the relevant
section before resolving, not after building.

## What mirrors what

| Downstream artifact | Mirrors | Relationship | Drift check |
|---|---|---|---|
| `.devcontainer/quantum/Dockerfile` | `.devcontainer/Dockerfile` | **Verbatim prefix** + Quantum tail below the `# Quantum Specific addon` marker | `diff <(head -n "$(wc -l < .devcontainer/Dockerfile)" .devcontainer/quantum/Dockerfile) .devcontainer/Dockerfile` |
| `.devcontainer/quantum/devcontainer.json` | `.devcontainer/devcontainer.json` | Full rewrite, not a copy | Judgment: did upstream add a build arg worth mirroring? |
| `builders/<os>/Dockerfile` | `Dockerfile.<os>` | Curated port (internal mirrors, no XLIO, etc.) | `git diff <last-tag>..origin/main -- 'Dockerfile*'` |
| `builders/dev/Dockerfile` | `.devcontainer/Dockerfile` | **Curated variant, NOT a prefix copy** | Judgment; see "quint" below |
| `src/vfs/snfs/` | `src/vfs/linux/` | Downstream copy, Rocky-only, heavily drifted | `sync-report.sh post` + a Rocky9 build |
| `qtm_build/CMakeLists.txt` | `src/vfs/CMakeLists.txt` install rules | Packaging overlay | Staged install, below |

In-scope OS matrix: `Rocky9`, `Ubuntu22`, `Ubuntu24`, `Ubuntu26`. Upstream's
`Dockerfile.rocky10` has no Quantum builder — ignore it.

## Recurring hazards

### The module SDK boundary (`src/vfs/sdk/`)

Upstream defines the SDK as the compile-time contract for VFS backends. A
module includes `vfs/sdk/chimera_vfs_sdk.h` and nothing else from the tree.

When a header moves into or within `sdk/`, **nothing conflicts** — git
renames it and downstream files that include the old path break at compile
time. `src/vfs/snfs/` is the usual casualty; check every `"vfs/..."` include
in it, including the ones in `snfs_cvapi.h`, which is easy to forget because
it is not a mirror of any `linux` file.

### `vfs_fh_magic.h` vendor slots

Upstream names slots 240-255 positionally (`VENDOR0`..`VENDOR15`) and its own
`examples/vfs_module` selects `VENDOR0` **by name**. Quantum needs names for
two of those slots (myriad = 240, snfs = 241).

**Add aliases, never rename:**

```c
CHIMERA_VFS_FH_MAGIC_VENDOR0  = 240,
...
CHIMERA_VFS_FH_MAGIC_MYRIAD   = CHIMERA_VFS_FH_MAGIC_VENDOR0,
CHIMERA_VFS_FH_MAGIC_SNFS     = CHIMERA_VFS_FH_MAGIC_VENDOR1,
```

A rename makes this header conflict on every sync and breaks the upstream
example. Worse, the rename historically kept `// CHIMERA_VFS_FH_MAGIC_VENDOR0`
as a trailing comment, so a substring `grep -q` reports the tree healthy. Any
check here must anchor on the enumerator:
`grep -E '^[[:space:]]*CHIMERA_VFS_FH_MAGIC_VENDOR0[[:space:]]*='`.

### Packaged includes and the `OTEL_TRACING` pin

Upstream installs the SDK itself:

```
include/chimera/vfs/sdk/*.h
include/chimera/{prometheus-c,stopwatch,oteltracing}.h
```

Consumers use `-I<prefix>/include/chimera` with
`#include "vfs/sdk/chimera_vfs_sdk.h"`.

`qtm_build/` adds exactly one thing: an `oteltracing.h` with `OTEL_TRACING`
pinned to the value the packaged binaries were built with. The upstream
header defaults to `1` (a 4 KiB `otel_span` inline in
`chimera_vfs_request`); chimera packages build with `0`. A module compiled
against the wrong value gets a skewed request layout and segfaults on its
first mount — silently, because the dlopen boundary has no ABI check.

The pin works by installing over upstream's copy, which requires
`add_subdirectory(qtm_build)` to stay **last** in the top-level
`CMakeLists.txt`. Both ends carry a comment; keep them together.

**Never install a header set by globbing a source directory.** The previous
`file(GLOB src/vfs/*.h)` did not fail when nine headers moved into `sdk/` —
it just stopped matching them, and would have shipped an include tree missing
the parts that matter.

**Acceptance test:**

```bash
sudo cmake -DCMAKE_INSTALL_PREFIX=/opt/quantum/chimera -S . -B /build/Release
sudo ninja -C /build/Release
sudo env DESTDIR=/tmp/chimera-stage ninja -C /build/Release install
find /tmp/chimera-stage/opt/quantum/chimera/include -type f | sort
head -5 /tmp/chimera-stage/opt/quantum/chimera/include/chimera/oteltracing.h
```

Expect 19 SDK headers (18 plus `vfs_access.h`, which joined in the
2026-09-16 sync -- recount when upstream adds one) plus the three external
ones, and `#define OTEL_TRACING 0`. Then compile a probe that includes only
`"vfs/sdk/chimera_vfs_sdk.h"` with `-I<stage>/include/chimera -I<stage>/include`.

Restore `-DCMAKE_INSTALL_PREFIX=/usr/local` afterwards — it is baked into
`CHIMERA_STATE_DIR` and changing it forces a rebuild.

Out-of-tree consumers (myriad) must track layout changes here; the module ABI
is unversioned, so a stale module against a new server fails at runtime, not
at build.

### quint and the MBT corpus

Upstream generates the whole model-based-test corpus at build time
(`cmake/Specs.cmake`, since adb54a5c / the 2026-09-16 sync): each quint
suite's configs go to ext/specs' generator, and there is no prebuilt bundle,
no `cmake/SpecsBundle.cmake`, no `oras` fetch. What that means downstream:

- A builder that should *run* the replays needs quint and node on PATH.
  `builders/dev/Dockerfile` installs quint from npm, pinned to
  `ext/specs/.quint-version`. The OS builders (`builders/<os>/`) do not and
  need not: they only build and package, and Specs.cmake disables the
  replays with a STATUS line when quint is absent. Upstream's
  `Dockerfile.rocky9` / `Dockerfile.ubuntu22.04` gained quint for their CI
  cells; that is deliberately not mirrored.
- Legs that configure the tree but never run a replay pass
  `-DCHIMERA_MBT_CORPUS=OFF` (the extended ctest legs, static analysis,
  smbtorture, kvm). It replaced `-DSPECS_BUNDLE=NONE`, which became an
  unused variable the moment upstream deleted SpecsBundle.cmake -- and the
  merge said nothing, because qb.yaml is downstream-only and cmake does not
  fail on an unknown `-D`. `sync-report.sh post` now checks that every `-D`
  option qb.yaml passes is still named by a CMake file in the tree.
- The `ctest_quint_*` legs keep the corpus on and pass `REQUIRE_CTL_MBT` /
  `REQUIRE_DISKFS_MBT`, so a quint missing from chimera-dev fails configure
  instead of shrinking the suite.

The batch-replayer hang that once justified hiding oras (trace
`stepFds_128_0x1_2` wedging the reset's umount) is fixed; it is history, not
a constraint.

### Upstream merges are gated on the quick tier only

Since upstream 5295fc89/54a1451c (2026-08-31 sync), a bare `ctest` runs
only the *quick* tier (the quint model-based tests); everything else is
`-C extended`.  Two standing consequences:

- Every downstream ctest invocation (qb.yaml legs, ad-hoc local runs)
  must pass `-C extended` or it runs almost nothing and passes.
- Upstream's merge queue gates on the quick tier; the extended tier runs
  in a periodic workflow.  An upstream regression in extended-tier tests
  (pjd, pynfs, unit suites) can land on main green and arrive here via
  the sync -- the 2026-08-31 sync brought one (nfs4 client open gate,
  96 pjd failures).  Expect the qb ctest legs, not the merge, to be the
  step that finds these; diagnose before assuming a downstream cause.

### Attributing an extended-tier failure before fixing it

The qb ctest legs report only the test names without `-v`; the failure
detail appears in the run's final error block, or in a re-run of the one
leg with `-v` (`qb -o arenaud-claude-sa --plain -a amd64 -v chimera-dev
ctest_release`, ~40 min, separate tag so it does not collide with the
check run).  Then decide whose bug it is:

- **Upstream's own Extended runs**: `gh run list --repo chimera-nas/chimera
  --workflow extended.yml --branch main`, then `gh run view <id> --json jobs`
  for the per-category job conclusions and `gh api
  repos/chimera-nas/chimera/actions/jobs/<job>/logs` for the log (the
  `gh run view --log` form comes back empty).  Extended has been red since
  at least 2026-09-02 and hits its 6 h timeout, so a job that is absent or
  cancelled there proves nothing.
- **develop before the sync**: `glab ci list -R chimera/chimera`, the failed
  jobs of a nightly pipeline via `glab api
  "projects/chimera%2Fchimera/pipelines/<id>/jobs?scope[]=failed"`, and
  `glab ci trace <job>` grepped for `(Failed)`.
- **Locally**: the smbtorture and WPTS drivers run one case in a few seconds
  (`smbtorture_test -b <backend> <subtest>` under
  `scripts/netns_test_wrapper.sh`, `SMBTORTURE_LOG_LEVEL=debug`;
  `scripts/wpts_smb_test_wrapper.sh <chimera> memfs <Case>` with
  `CHIMERA_DEBUG=1`).  A failure at ~0.3 s with
  `io_uring_queue_init_params() failed: Cannot allocate memory` is this
  box, not the code: export `EVPL_IO_URING_ENTRIES=1024`.

The 2026-09-07 sync brought four such regressions (WPTS ChangeStreamName,
smbtorture dup_extents on linux, KVM nfstest_alloc on diskfs, two analyzer
findings); all but the KVM one were fixed as upstream-bound commits, the
KVM one gated in `.gitlab-ci.yml` with the reason beside it.

### Verifying with qb: the builder image is not rebuilt by default

Every `ctest_*`, `syntax` and `static_analysis_*` leg declares
`dependencies: builder: {task: chimera-dev, tag: develop}`. Naming a single
task therefore runs against the chimera-dev image **tagged develop**, not one
built from the sync branch. A sync usually touches `builders/dev/Dockerfile`
(see quint above), so name the image task too:

```bash
qb -o arenaud-claude --plain -a amd64 \
   -V CTEST_FILTER="-L pynfs" chimera-dev ctest_debug
```

The `check` alias lists `chimera-dev` first, so a full
`qb -o arenaud-claude --plain check` already rebuilds it.

### snfs lag

`src/vfs/snfs` is a Rocky-only copy of `src/vfs/linux`, drifted by roughly
1300 lines since 2026-02-26. Default scope on a sync is **make it compile**;
porting behavioural changes is its own ticket. It only builds where
`/usr/cvfs/include` exists, so the `rocky9` qb leg is the real gate.

Two things in it are deliberate copies of `linux.c` rather than drift, and
have to move when upstream moves them:

- **The byte-range lock projection.** Upstream 5ea192f3 (2026-09-07 sync)
  removed the `OP_LOCK` wire that `snfs_lock()` mirrored; snfs now carries
  `linux.c`'s `CHIMERA_VFS_CAP_CLAIM_RANGE` code instead -- the
  `range_file`/`range` registry on `snfs_shared`, `snfs_claim_acquire()`
  and `snfs_claim_release()`, OFD locks on one descriptor per (fh, owner)
  -- renamed `chimera_linux_` -> `snfs_`, `CHIMERA_LINUX_LOCK_` ->
  `SNFS_LOCK_`, `linux_open_by_handle` -> `snfs_open_by_handle`.
  `sync-report.sh post` diffs the two blocks modulo whitespace and reports
  DRIFTED when upstream changes its side; re-port rather than patch.
- **`.sdk_version = CHIMERA_VFS_SDK_VERSION`** in `vfs_snfs`. Registration
  aborts a module without it (`vfs.c`), which no build leg sees -- snfs
  lacked the stamp from the 2026-08-28 SDK commit until the 2026-09-07 sync.
  Keep it whenever the module struct is touched.

## Merge commit message

Compose in a file, then `git commit -F`. Include:

- Subject `CHIM-<n>: merge origin/main into develop (sync-code-<date>)`
- Upstream sha + new tag, previous tag + its sha
- The commit list, **grouped by area** rather than a flat dump
  (`sync-report.sh commits <from> <to>` emits the raw list)
- Submodule moves, old → new
- What was resolved and why, for any non-obvious resolution
- `Signed-off-by: Alain Renaud <alain.renaud@quantum.com>`

## Commit splitting

- Downstream-only changes take the `CHIM-<n>:` prefix.
- Upstream-bound changes never carry a Jira key.
- `qtm_build/` and `qb.yaml` are Quantum-only and get their own commit,
  never mixed with code that could go upstream.
- Every commit gets the `Signed-off-by` trailer.
