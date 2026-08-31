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

Expect 18 SDK headers plus the three external ones, and `#define
OTEL_TRACING 0`. Then compile a probe that includes only
`"vfs/sdk/chimera_vfs_sdk.h"` with `-I<stage>/include/chimera -I<stage>/include`.

Restore `-DCMAKE_INSTALL_PREFIX=/usr/local` afterwards — it is baked into
`CHIMERA_STATE_DIR` and changing it forces a rebuild.

Out-of-tree consumers (myriad) must track layout changes here; the module ABI
is unversioned, so a stale module against a new server fails at runtime, not
at build.

### quint, oras, and the MBT replay hang

Upstream's devcontainer installs quint. `.devcontainer/quantum/Dockerfile`
takes it as part of the verbatim prefix. **`builders/dev/Dockerfile` must
not**, while the batch-replayer hang is unfixed.

`qb.yaml`'s ctest legs hide the `oras` binary before cmake runs so
`cmake/SpecsBundle.cmake` cannot fetch the prebuilt trace corpus, which
leaves the model-based replays unregistered. quint would let SpecsBundle's
`local` provider build that corpus from `ext/specs` instead, re-registering
the replays — and they still wedge (trace `stepFds_128_0x1_2` before another
trace leaves the reset's umount waiting on a VFS close that never completes).
Every ctest leg would burn its timeout instead of reporting.

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
