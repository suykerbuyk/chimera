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
`Dockerfile.rocky10` has no Quantum builder — ignore it.  The plain
`Dockerfile` is upstream's runtime image (the one `chimera-docker.json`
configures); nothing downstream mirrors it, so a change there is a
no-op to record, not to port (2026-10-02: `VOLUME /export`).

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

Expect 20 SDK headers (18 plus `vfs_access.h`, which joined in the
2026-09-16 sync, plus `vfs_mode.h` from the 2026-09-28 sync -- recount when
upstream adds one) plus the three external ones, and `#define OTEL_TRACING 0`. Then
compile a probe that includes only `"vfs/sdk/chimera_vfs_sdk.h"` with
`-I<stage>/include/chimera -I<stage>/include`, and run
`qtm_build/verify_test_includes.sh <stage>/<prefix>` for the test package's
caller-side headers.

Restore `-DCMAKE_INSTALL_PREFIX=/usr/local` afterwards — it is baked into
`CHIMERA_STATE_DIR` and changing it forces a rebuild.  When no install rule
changed this cycle (`git diff <last-tag>..origin/main -- '*CMakeLists.txt'
'cmake/*' | grep '^[+-].*install('` is empty), skip the prefix switch and
stage the tree already built: `sudo env DESTDIR=<stage> cmake --install
/build/Release`.  The layout under `include/` does not depend on the prefix,
and both rebuilds are avoided.

Out-of-tree consumers (myriad) must track layout changes here.  The only
runtime guard is `CHIMERA_VFS_SDK_VERSION`, checked at registration, and
upstream does not bump it for every layout change: at the 2026-09-23 sync
`struct chimera_vfs_request` gained pNFS fields mid-struct and
`CHIMERA_VFS_GATE_SCRATCH_SIZE` went 400 -> 704 with the version still 2.
Diff `src/vfs/sdk/vfs_request.h` every sync and say in the merge message
when modules must be rebuilt; a stale module passes the gate and then reads
a skewed request.

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

### Commits carried ahead of upstream

develop sometimes cherry-picks commits from open upstream PRs (CHIM-209
carried seven, from #1697 and #1698).  When upstream later merges them, the
pre-flight lists their files as conflict candidates.  Compare
`git show <sha> | git patch-id --stable` for each carried commit against its
upstream counterpart before merging: if every pair matches, the merge takes
upstream's later work cleanly, and afterwards
`git diff sync-code-<new> -- <file>` should be empty for each file only
those commits touched on develop.  A pair that differs means upstream
reshaped the PR; resolve toward upstream's version.

Carried branches for open Jira work go stale the same way.  At the
2026-09-23 sync the CHIM-211 fix and probes arrived upstream in a later
form (691121b1, 227fccdd, 1dd8a5ed, 1048c466), leaving that branch
nothing to merge.  Check open topic branches against the merged tree the
same way before anyone merges one into develop on top of the sync.

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

### Static analysis covers chimera, not the submodules

Upstream's analyzer jobs skip `ext/` (`analyze_cached.py --exclude ext/`,
TUs only) and analyze Release only; the qb `static_analysis_*` legs run
scan-build in both modes.  Until the 2026-09-23 sync they also analysed
`ext/`, and the new libevpl pin's first finding (an unchecked `fopen()` in
`src/core/pread/tests/shutdown_close.c`) failed all four legs while every
upstream job stayed green.  They now pass `--exclude /context/ext/` on the
ninja step (qb.yaml has the why).  A leg that fails with a report under
`src/` is ours; one Debug-only is a thing upstream cannot see, so fix it
upstream-first rather than suppressing it.

**Reproducing a leg locally: disable ccache.** The top-level CMakeLists
wraps every compile in ccache when it is installed (the devcontainer has
it, chimera-dev does not), and ccache wraps `ccc-analyzer` too.  A cache hit
replays the analyzer's warning on stderr but never writes the HTML report
scan-build counts, so a warm cache prints the finding *and* reports "No
bugs found", rc=0.  Run local scan-build as `sudo env LC_ALL=C
CCACHE_DISABLE=1 scan-build ...`, and treat any local "0 bugs" produced
without it as unproven.  To re-check one TU cheaply, `touch` it and re-run
`scan-build ... ninja` in the already-configured tree.

### Verifying with qb: qb-ci.sh first, rocky9 beside it

`.devcontainer/quantum/qb-ci.sh` (CHIM-220, on develop since the 2026-09-28
sync) is the verification entry point.  It builds `chimera-dev` once under
its own tag (`$USER-<branch>` by default), then starts every merge-train
leg as its own `qb -i` process -- four ctest shards per mode, the two KVM
shards, quint, static analysis, syntax -- and prints a PASS/FAIL line per
leg with the log path.  Compared with `qb check` that means a failure
names a shard, a re-run costs one shard (`--no-builder --only <leg>`),
and the leg table is checked against `.gitlab-ci.yml` before anything
runs, so a pipeline change not mirrored here stops the run.  Pass
`-a amd64` (only static analysis has arm64 cells and the ctest legs are
amd64-only in CI anyway); `--all` adds the nightly legs and is not needed
for a sync.  It warns when the tree is dirty and says when every leg has
packed its copy; the `tee` log itself is untracked and harmless.

It does not run the Rocky9 build legs: the pipeline builds Rocky9 in its
package jobs, outside the leg table.  Rocky9 is the only place snfs
compiles, so always add `qb -o arenaud-claude --plain rocky9`.  The two
runs use different output tags and do not collide.

### The builder image is not rebuilt by a bare single-leg qb

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

**Keep the tree clean for the whole run, not just the launch.** qb packs
each task's context when that task *starts*, and the ctest, syntax and
static-analysis legs only start once chimera-dev is built, minutes in.  An
edit made after launching lands in those legs, marks their
`QB_BUILD_GIT_SHA` `-dirty`, and means the legs no longer test one tree.
Stage follow-up edits outside the repo until every run_container task has
logged `Executing` (tasks with an identical context reuse one pack, so not
each logs its own `Packed context` line).

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
- **The capability word.** Upstream adds opt-in `CHIMERA_VFS_CAP_*` bits,
  declares them on linux, and derives protocol behaviour from them; snfs
  keeps the old word and nothing conflicts or fails to build.  At the
  2026-09-23 sync e417751a made the SMB server derive
  FileFsAttributeInformation from the capabilities, and the new
  `CHIMERA_VFS_CAP_SPARSE` would have silently withdrawn
  FILE_SUPPORTS_SPARSE_FILES from every snfs share.  `sync-report.sh post`
  now fails on any bit linux declares that snfs does not, except the three
  it lacks by design (RPL, XATTR, DELEGATES_DAC; reasons in the script).
  For a new bit, check snfs implements what the bit stands for before
  declaring it -- declaring DELEGATES_DAC unverified, for one, would turn
  off the VFS access gate for snfs.

Not every linux.c change is snfs's business on a sync.  At the 2026-09-23
sync two were left for a behavioural-catch-up ticket: the O_WRONLY mapping
for write-only opens (a9652f75; snfs_set_open_flags does not even
distinguish READ_ONLY|WRITE_ONLY yet) and the ATTR_FH echo in getattr
(0efe7bee).

## Merge commit message

Compose in a file, then `git commit -F`. Include:

- Subject `CHIM-<n>: merge origin/main into develop (sync-code-<date>)`
- Upstream sha + new tag, previous tag + its sha
- The commit list, **grouped by area** rather than a flat dump
  (`sync-report.sh commits <from> <to>` emits the raw list)
- Submodule moves, old → new
- What was resolved and why, for any non-obvious resolution
- `Signed-off-by: Alain Renaud <alain.renaud@quantum.com>`

## Jira tickets the sync resolves

Upstream-first fixes are tracked twice: a CHIM ticket here and a PR on
chimera-nas/chimera.  When the PR merges, the next sync carries the fix
onto develop, and that sync is the event that resolves the ticket -- not
the upstream merge (develop did not have it yet) and not the MR merge
alone (nobody reading the ticket sees that).

Before writing the merge message, map the range to tickets: match each
upstream commit against the open CHIM tickets' PR numbers and branch
names (memory and the Jira "In Review" list are the sources; `git log
--grep` on the develop topic branches confirms a match by patch-id).
Record the mapping in the merge message's "Jira coverage" paragraph.

Then, for each ticket, with the Atlassian tools (site symform.jira.com):

- `createIssueLink` type `Relates`, inward = the resolved ticket,
  outward = the sync ticket.
- `addCommentToJiraIssue` in Markdown, one paragraph: the upstream PR
  and commit shas, "merged on origin/main and arrives on develop with the
  <date> code drop, [CHIM-<sync>](url)", the tag, the merge commit and a
  link to the branch (`https://gitlab.mdh.quantum.com/chimera/chimera/-/commits/<branch>`)
  or the MR once it exists.  Close with what happens next: "resolve once
  the CHIM-<sync> MR merges into develop", and the downstream ticket it
  unblocks (MD-xxxx) when there is one.
- Leave the status alone until the MR merges; the ticket moves to Done
  then, with the MR link.

Finally give the sync ticket a description in the CHIM-214 shape:
snapshot/tag/branch/MR, what upstream brings, downstream changes, the
verification done, follow-ups, and the list of tickets it resolves.

Partial coverage counts too: a ticket whose scope upstream only partly
delivered (CHIM-190 got the REST docs at the 2026-10-02 sync, not the
GET endpoints) gets a comment saying what arrived, no link to Done.

## Commit splitting

- Downstream-only changes take the `CHIM-<n>:` prefix.
- Upstream-bound changes never carry a Jira key.
- `qtm_build/` and `qb.yaml` are Quantum-only and get their own commit,
  never mixed with code that could go upstream.
- Every commit gets the `Signed-off-by` trailer.
