<!--
SPDX-FileCopyrightText: 2026 Quantum Corporation

SPDX-License-Identifier: LicenseRef-Quantum-Proprietary
-->

# Chimera Compliance Suites: Inventory and Proposed Full Runs

| | |
|---|---|
| Status | Draft 0.1. Not yet reviewed. |
| Date | 2026-10-04 |
| Verified at | `develop` @ `d96e33a8` |
| Answers | [Q-SUC-2](prd.md#12-open-questions), as a **proposal** for the operator |
| Requirements | SUC-1, SUC-2 (`prd.md:390-400`); roadmap E1 (`roadmap.md:33`) |

## Status

This document lists what each SUC-2 suite runs today, what it skips or
excludes and why, and what a full run would need. The suites are pynfs,
cthon, smbtorture, the Ceph S3 tests, pjdfstest and MinIO warp. It also
proposes a definition of "full" for each suite.

Every definition of "full" here is a **proposal**. The operator accepts or
amends it when answering Q-SUC-2. Facts carry a citation (`path:line` at
`d96e33a8`, or a CI job with quoted output). Judgements are labelled
PROPOSAL.

Headline facts:

- **No suite gates an MR push.** Every SUC-2 suite is in the `extended`
  ctest tier. The GitLab merge train gates pynfs (non-diskfs), smbtorture,
  the cthon port, pjdfstest and cthon04 in KVM. The scheduled pipeline, which
  runs three times a week, adds the rest.
- **MinIO warp never runs: it is not in the tree. The Ceph S3 tests never run in GitLab CI.** The test is not registered there, because the system Python lacks boto3 (job 3490972: "boto3 not found - S3 tests will be skipped"). Upstream GitHub `extended` was not measured; registration there is unlikely for the same reason.
- **No suite runs against the shipped configuration.**
  - The harnesses enable features the defaults leave off.
  - They shorten the NFSv4 timers.
  - Every diskfs row runs with `unsafe_async`, so no durable write path is
    tested.
- **pjdfstest never runs through FUSE**, although SUC-2 names that path.
- **Several "passes" are not passes.**
  - pynfs WARN and NOTSUP results count as passes.
  - smbtorture exits 0 on a skip.
  - One GitLab retry turned a real timeout green (pipeline 205316).

### Decisions requested

The operator decides each of these when answering Q-SUC-2. Every proposal
below carries a recommendation and its evidence.

- **D2: the backend a full run uses.** See "Proposed definition of a full run", Backend.
- **D4: running pjdfstest through FUSE**, as SUC-2 states. See "pjdfstest", No FUSE path, and its proposed full run.
- **D6: warp parameters and pass criterion.** See "MinIO warp".
- **OQ-1 to OQ-9.** See "Open questions": advertised features, suite list, cthon over SMB, retries, interim exclusions, gate depth, the S3 public ACL, profile deviations, and delegation coverage.
- **The per-suite selections**, in each suite's "Proposed full" subsection: pynfs, cthon, pjdfstest, smbtorture, Ceph s3-tests and MinIO warp.

## Scope and method

### Terms

| Term | Meaning here |
|---|---|
| run | registered in ctest and executed by a CI leg named in "CI coverage" |
| skipped | registered, but at runtime it exits 77, reports a whole-file or subtest skip, or is a pynfs OMIT |
| excluded | deliberately not selected, with a reason in the source |
| missing | in the pinned upstream suite but not wired, or the wiring is absent |
| shipped default | The installed `src/daemon/chimera.json`, including its mounts, export and shares, applied over `chimera_server_config_init()` in `src/server/server.c` and the backend mount parsers. Where the two disagree, `chimera.json` wins: 16 core threads and 1 sync delegation thread, not `server.c`'s 8 and 8 |

### Pins

| Input | Pin | Source |
|---|---|---|
| pynfs (shipped harness) | `chimera-nas/pynfs` fork @ `b861f7b8` | `builders/dev/Dockerfile:17` |
| pynfs (universe) | `git://git.linux-nfs.org/projects/cdmackay/pynfs.git` master @ `cd4701827a` (merge-base with the fork `3c14d9e3`) | scratch clone, 2026-10-04 |
| Samba / smbtorture | `samba-testsuite 2:4.23.6+dfsg-1ubuntu2` in `repository.mdh.quantum.com/chimera/chimera-dev:ci-chimera-442` (index `sha256:b6ce8991f5ca10997ee563fe5c3a5ebf13eb0310b7358a1537c4e39513590a1d`) | unprivileged `podman pull`, then `podman run --network=none` |
| Ceph s3-tests | `ceph/s3-tests` @ `5522d1c3` | `.devcontainer/Dockerfile:59` |
| cthon04 (universe) | `git://git.linux-nfs.org/projects/steved/cthon04.git` @ `86a4501` | scratch clone |
| cthon04 (KVM images) | `chimera-nas` fork @ `13f4336`, built into `kvm-test-base` `v1.9.0` | `kvm/CMakeLists.txt:31`; image build recipe |
| pjdfstest (universe) | `github.com/pjd/pjdfstest` @ `85a8aea` (not pinned in tree) | scratch clone |
| MinIO warp | v1.8.2, `warp.v1.8.2` linux-amd64, sha256 `fc7661a6b952800fbc8422cf0f72ae82726f82aa80065d15f50125f8bf5ca5fa` | dl.min.io, checked against the published checksum |

### Evidence

All CI facts come from three GitLab pipelines of project `chimera/chimera`
(id 1715). The document quotes the job IDs, counts and exact output lines it
relies on, because job artifacts expire:

- pipeline 205316: 2026-10-09
- pipeline 205343: 2026-10-09
- pipeline 205408: 2026-10-11

| Pipeline | Kind | Date | Commit |
|---|---|---|---|
| 205408 | scheduled (schedule 123) | 2026-10-04 | `3ea255e0`, an ancestor of `d96e33a8` |
| 205343 | merge train, MR 114 | 2026-10-02 | `5593f580` |
| 205316 | scheduled; the retry example | 2026-10-02 | `966c105e` |

The traces and artifacts of every `ctest*` and `smbtorture_explore` job in
those pipelines were saved on 2026-10-04, together with the `chimera-dev`
build trace (job 3490948) and the `smbtorture --list` output: 138 files.

**Where the evidence is kept.** The evidence is in the vibe-palace vault at
`Projects/chimera/doc/evidence/compliance-suite-inventory/`. Its `README.md`
indexes the following:

- `ci/excerpts-<pipeline>.txt`: every CI line this document quotes, plus each
  job's ctest summary, failure, skip and label-total lines, with their line
  numbers in the original trace or log;
- `ci/SHA256SUMS`: the sha256 of all 138 saved files. The file's own sha256 is
  `db7dcf704c6961a0bb6eca77402bfe6f90a0cae8963969e74a87cdfbd8f18e07`. The full
  files themselves (about 13 MB) are not retained; the excerpts keep the lines
  this document relies on;
- `data/s3-classification.tsv`: the per-case Ceph ACL-4/SEC-1 classification;
- `data/skip71-gates.txt`: the 71 pjdfstest whole-file gates;
- the smbtorture list and explore-report summaries;
- the `repro/*.repro.sh` scripts and their helpers, which regenerate the counts;
- the citation lists and checker.

After a pipeline's artifacts expire, its facts can be checked against the
excerpts and checksums, but not re-fetched from GitLab. They are "not
reproducible after" that pipeline's expiry date.

No local privileged run was made, and no image was built. Counts not taken
from CI come from scratch clones of the pinned upstreams and from replaying
the CMake registration logic.

## Registration gates

A suite counts as "run" only if CMake registers it and ctest then selects it. Registration depends on what the configuring environment can do. Every SUC-2 suite is in the **extended** tier. A plain `ctest` (the quick tier) selects none of them.

### Gates in the build

| Gate | Where | What it needs | What it gates |
|---|---|---|---|
| Tier | `CMakeLists.txt:357` `set(CHIMERA_TEST_TIER "extended")`; extended tests are registered with `CONFIGURATIONS extended` (`CMakeLists.txt:433`) | `ctest -C extended` | Every SUC-2 suite |
| netns probe | `CMakeLists.txt:777` (`ip netns add`); result at :786-799 | `CAP_NET_ADMIN` | smbtorture `smb2.*` matrix (`src/server/smb/tests/CMakeLists.txt:5239`), WPTS (:160, :423), pike (:476) |
| `chimera_add_netns_test` | `CMakeLists.txt:850-863` | nothing. With netns it wraps the test in a namespace; without it, the test runs on the host network under `RESOURCE_LOCK chimera_net` (:862) | Ceph (`src/server/s3/tests/ceph/CMakeLists.txt:18`) and smbtorture `rpc.*` (`src/server/smb/tests/CMakeLists.txt:638`): **registered with or without netns** |
| Limits probe | `CMakeLists.txt:819` (`ulimit -l unlimited; test -w /proc/sys/fs/aio-max-nr`) | raised memlock and a writable `aio-max-nr` | cthon-port (`src/posix/tests/CMakeLists.txt:735`), pjd (:1560), fsx (:621) |
| KVM probe | `CMakeLists.txt:718-750` | `oras` (:732), qemu (:737), `/dev/kvm` (:742), uid 0 (:747) | cthon04-KVM, nfstest, LTP, KVM xfstests |
| pynfs | `CMakeLists.txt:883-886` | Python 3 and `/opt/pynfs/nfs4.0/testserver.py` (**no netns gate**) | pynfs. Its wrapper then runs `ip netns add` (`scripts/pynfs_test_wrapper.sh:193`) under `set -u` only (:16), with no fallback |
| Ceph | `src/server/s3/tests/CMakeLists.txt:27` (`if(BOTO3_FOUND)`) → :134; `ceph/CMakeLists.txt:17` (`${S3TESTS_DIR}/.venv`) | boto3 and the s3-tests venv | Ceph s3-tests |
| smbtorture binary | `src/server/smb/tests/smbtorture_test.c:512-515` | `smbtorture` on `PATH` at run time, otherwise the test exits 77 (Skipped) | `rpc.*` tests that are registered without it |

### Registration by environment

| Suite | Unprivileged thin (measured) | Unprivileged `chimera-dev` (derived, not run) | Privileged `chimera-dev`, GitLab (pipeline 205408, measured) |
|---|---|---|---|
| pynfs | 0 (no `/opt/pynfs`) | 1377 registered. Expected to **fail**: the netns gate does not apply, but the wrapper's `ip netns add` needs `CAP_NET_ADMIN` | 929 (job 3490966: `100% tests passed, 0 tests failed out of 929`) + 448 diskfs (job 3490974: `... out of 448`) = 1377 |
| cthon-port | 0 (limits gate) | 0 (limits gate) | 794 Passed in the `rest` leg (job 3490972 artifact) |
| cthon04-KVM | 0 (`oras` and qemu not found) | 0 (uid is not 0 under keep-id; no `/dev/kvm`) | 110 per image (72 NFS + 38 SMB), 4 images (jobs 3490981, 3490985, 3490989, 3490991) |
| smbtorture | 6 `rpc.*` (#498-503, one Disabled) + `chimera/tools/smbtorture/report` (#614). The `rpc.*` tests would exit 77, because thin has no Samba | 6 `rpc.*` (no netns gate); `smb2.*` matrix 0 (netns gate) | 1741 gated (job 3490968); 3930 cells explored (job 3490980) |
| Ceph s3-tests | 0 | 0 (no s3-tests in `builders/dev`) | 0. The `rest` leg log has no `ceph` test |
| pjdfstest | 0 (limits gate) | 0 (limits gate) | 2578 Passed in the `rest` leg (job 3490972 artifact) |
| MinIO warp | 0 | 0 | 0 |

Sources:
- **Thin** is measured read-only with `ctest -N -C extended --test-dir ~/.cache/chimera/podman-thin/Debug`, which gave `Total Tests: 615` for a tree configured at this commit (listing taken 2026-10-03). The build directory is shared between checkouts, so a tree reconfigured from another branch lists a different total. The cache records `ORAS_EXECUTABLE-NOTFOUND` and `QEMU_EXECUTABLE-NOTFOUND`.
- **Unprivileged dev** is derived from three things:
  - the Podman loop's privilege model ("Your uid mapped into the container, all capabilities dropped", `docs/architecture/user-mode-podman.md:53`);
  - the image contents: pynfs `builders/dev/Dockerfile:138`, `samba-testsuite` :52, `oras` :130-133, qemu :107; no s3-tests and no pike;
  - the gates above.
- **Privileged** is the CI evidence (below).

## CI coverage

### Schedule (read-only GitLab API, 2026-10-04)

`glab api projects/1715/pipeline_schedules` returns:
- schedule `id 123`, `"Schedule run"`;
- `ref refs/heads/develop`, `cron "30 2 * * 0,3,5"`, `cron_timezone "America/Chicago"`, `active True`;
- `next_run_at 2026-10-07T07:33:00Z`.

Its most recent pipelines are 204918 (09-27), 205136 (09-30), 205316 (10-02) and 205408 (10-04), all `success`. Earlier ones are 204520 (09-23) and 204809 (09-25), both `failed`. That is three runs a week, not nightly. `.gitlab-ci.yml:25` still says "Nightly / scheduled".

### Cadence by suite

GitLab rule sets:
- `rules_fast` (:61-63): every MR event and every schedule.
- `rules_full` (:71-76): runs automatically in the merge train and on a schedule. On an MR push it is `when: manual`, `allow_failure: true`.
- `rules_explore` (:78-79): schedule only.

GitHub:
- `pr-checks.yml:26` runs the "full quick test tier on devcontainer" on PRs.
- `build-test.yml:26` ("What gates a merge is the QUICK test tier") runs in the merge queue.
- `extended.yml:16` runs `cron: '0 1,7,13,19 * * *'`, which is 4×/day. Shards are listed at :742-745 and selectors at :821-849. No GitHub run was observed for this inventory; the GitHub column is configuration only.

| Suite (selector) | GitLab MR push | GitLab merge train | GitLab scheduled | GitHub PR / merge queue | GitHub extended 4×/day |
|---|---|---|---|---|---|
| pynfs non-diskfs (`ctest` `-L '^pynfs$' -E diskfs_`, :321-322) | manual, non-gating | yes (gating) | yes | no (quick tier) | `pynfs` shard (`-L ^pynfs$`, no `-E`) |
| pynfs diskfs (`ctest_explore` `-R diskfs_`, :378-379) | no | no | yes | no | in the `pynfs` shard |
| cthon-port (label `posix`) | manual | yes (`rest` leg, :333-334) | yes | no | `posix` shard |
| cthon04-KVM ubuntu2604 (`ctest_kvm`, :588) | manual | yes | yes | no | `kvm-ubuntu2604` |
| cthon04-KVM other 3 images (`ctest_kvm_images`, :649-666) | no | no | yes | no | `kvm-<image>` |
| smbtorture gated (`-L '^smbtorture$'`, :325-326) | manual | yes | yes | no | `smbtorture` shard |
| smbtorture full matrix (`smbtorture_explore`, :512-515) | no | no | yes, informational | no | `smbtorture-matrix` job (:1183), red only on a newly failing gated subtest (:1324-1330, :1371-1377) |
| smbtorture `rpc.*` (label `smbtorture_rpc`) | manual | yes (`rest` leg) | yes | no | `rest` shard |
| Ceph s3-tests (label `ceph-s3`) | never: not in `builders/dev` | never | never | no | configured in the `rest` shard (`ceph-s3` is not in `EXCL`, :821); not measured, and registration is unlikely without a system boto3 |
| pjdfstest (label `pjdfstest`) | manual | yes (`rest` leg) | yes | no | `pjd` shard (:826) |
| MinIO warp | not wired | not wired | not wired | not wired | not wired |
| WPTS (label `wpts`) | manual | yes (`wpts` leg, :329-330) | yes | no | `wpts` shard |
| pike (label `pike`) | never: not in `builders/dev` | never | never | no | `rest` shard |
| nfstest and LTP (`-R '/(nfstest|ltp)/'`) | manual | yes (ubuntu2604-nfstest, :613) | yes (+ ubuntu2404-nfstest, :656) | no | `kvm-*-nfstest` |
| KVM xfstests programs | manual | yes (ubuntu2604) | yes (4 images) | no | `kvm-<image>` |
| FUSE errno test, in-process fsx | manual | yes (`rest` leg) | yes | no | `rest` / `posix` |

### Retry triggers

Every GitLab test job carries `retry: max: 1`.

| Job (definition line) | Rules | `script_failure` | `runner_system_failure` / `stuck_or_timeout_failure` |
|---|---|---|---|
| `ctest` (:256; `retry:` :260) | `rules_full` | **yes** (:263) | yes |
| `ctest_explore` (:364; :368) | `rules_explore` | **yes** | yes |
| `ctest_quint` (:395; :399) | `rules_fast` | **yes** | yes |
| `quint_coverage` (:428; :432) | `rules_fast` | no | yes |
| `coverage` (:464; :468) | `rules_full` | no | yes |
| `smbtorture_explore` (:512; :521) | `rules_explore` | no | yes |
| `ctest_kvm` (:549; :553) | `rules_full` | **yes** | yes |
| `ctest_kvm_images` (:634; :638) | `rules_explore` | **yes** | yes |

GitHub has no retry: "There is no flaky-test rescue. A test that fails, fails: no re-run" (`.github/workflows/build-test.yml:37-40`). GitLab judges a job by its exit code alone.

**Retry masking example (pipeline 205316, `develop`, sha `966c105e`, scheduled 2026-10-02, pipeline status `success`):**
- Job 3488415, `ctest: [release, amd64, smbtorture, -L '^smbtorture$']`, status `failed` (`script_failure`).
  - Trace line 1030: `1741/1741 Test #2272: chimera/server/smb/smbtorture_smb2_notify_linux ...***Timeout 600.10 sec`.
  - Line 1068: `99% tests passed, 1 tests failed out of 1741`.
  - Line 1094: `ERROR: Job failed: exit status 1`.
- Job 3488440 has the same name and started at 07:52:27, one second after 3488415 ended.
  - Line 1074: `100% tests passed, 0 tests failed out of 1741`.
  - Line 1094: `Job succeeded`.

### Evidence register

Pinned pipelines:
- **205408:** `schedule`, `develop`, sha `3ea255e0`, an ancestor of HEAD d96e33a8. 2026-10-04.
- **205343:** `merge_request_event`, ref `refs/merge-requests/114/train`, sha `5593f580`. 2026-10-02.
- **205316:** `schedule`, `develop`, sha `966c105e`, an ancestor of HEAD. 2026-10-02.

Artifacts expire 2026-10-09 (205343, 205316) and 2026-10-11 (205408). The trace, artifact zip, `jobs-<p>.json` and `pipeline-<p>.json` copies were saved on 2026-10-04; excerpts and checksums are in the vault evidence directory named in "Scope and method". Its `SHA256SUMS` lists 138 files (the CI copies plus the `smbtorture --list` output), all verified OK on 2026-10-04. `sha256sum SHA256SUMS` = `db7dcf704c6961a0bb6eca77402bfe6f90a0cae8963969e74a87cdfbd8f18e07`.

Each "Summary" line below is quoted from the job trace after the `[chimera/<task>]` prefix. The per-test tallies come from the job's artifact log.

**Pipeline 205408 (scheduled)**

| Job | Name | Status | Summary | Artifact tally |
|---|---|---|---|---|
| 3490948 | `chimera-dev` (builder) | success | none. The trace records `output:    repository.mdh.quantum.com/chimera/chimera-dev_amd64:ci-chimera-442` | n/a (no artifact saved) |
| 3490966 | `ctest: [release, amd64, pynfs, -L '^pynfs$' -E diskfs_]` | success | `100% tests passed, 0 tests failed out of 929` | 929 Passed |
| 3490967 | `ctest: [debug, amd64, pynfs, ...]` | success | `... out of 929` | 929 Passed |
| 3490968 | `ctest: [release, amd64, smbtorture, -L '^smbtorture$']` | success | `... out of 1741` | 1741 Passed |
| 3490969 | `ctest: [debug, amd64, smbtorture, ...]` | success | `... out of 1741` | 1741 Passed |
| 3490970 | `ctest: [release, amd64, wpts, -L '^wpts$']` | success | `... out of 60` | 60 Passed |
| 3490971 | `ctest: [debug, amd64, wpts, ...]` | success | `... out of 60` | 60 Passed |
| 3490972 | `ctest: [release, amd64, rest, -LE '^pynfs$\|^smbtorture$\|^wpts$\|^kvm$']` | success | `... out of 5532` | 5518 Passed, 14 Skipped, 1 Not Run |
| 3490973 | `ctest: [debug, amd64, rest, ...]` | success | `... out of 5532` | 5518 / 14 / 1 |
| 3490974 | `ctest_explore: [release, amd64, pynfs_diskfs, -L '^pynfs$' -R diskfs_]` | success | `... out of 448` | 448 Passed |
| 3490975 | `ctest_explore: [debug, ...]` | success | `... out of 448` | 448 Passed |
| 3490976 | `ctest_quint: [release, amd64]` | success | `... out of 195` | 195 Passed |
| 3490977 | `ctest_quint: [debug, amd64]` | success | `... out of 195` | 195 Passed |
| 3490980 | `smbtorture_explore` | success | `== totals: 3930/3930 cells measured, failing (disabled) 53, newly passing 108, still failing 614, timing out 20` | n/a (report) |
| 3490981 | `ctest_kvm: [Release, amd64, ubuntu2604, ubuntu2604, -E '/(nfstest\|ltp)/']` | success | `... out of 382` | 382 Passed, 12 Not Run |
| 3490982 | `ctest_kvm: [Debug, ... ubuntu2604 ...]` | success | `... out of 382` | 382 / 12 Not Run |
| 3490983 | `ctest_kvm: [Release, ... ubuntu2604-nfstest, -R '/(nfstest\|ltp)/' -E 'nfstest_alloc_diskfs']` | success | `... out of 92` | 92 Passed |
| 3490984 | `ctest_kvm: [Debug, ... ubuntu2604-nfstest ...]` | success | `... out of 92` | 92 Passed |
| 3490985 / 3490986 | `ctest_kvm_images: [Release / Debug, ... ubuntu2404 ...]` | success | `... out of 388` | 388 Passed, 6 Not Run |
| 3490987 / 3490988 | `ctest_kvm_images: [Release / Debug, ... ubuntu2404-nfstest ...]` | success | `... out of 92` | 92 Passed |
| 3490989 / 3490990 | `ctest_kvm_images: [Release / Debug, ... ubuntu2404_hwe ...]` | success | `... out of 388` | 388 / 6 Not Run |
| 3490991 / 3490992 | `ctest_kvm_images: [Release / Debug, ... ubuntu2204_hwe ...]` | success | `... out of 388` | 388 / 6 Not Run |

**Pipeline 205343 (merge train, MR 114).** Every listed job has status `success`.

| Job | Name | Summary | Artifact tally |
|---|---|---|---|
| 3489331 / 3489332 | `ctest: [release / debug, amd64, pynfs, ...]` | `... out of 929` | 929 Passed |
| 3489333 / 3489334 | `ctest: [..., smbtorture, ...]` | `... out of 1741` | 1741 Passed |
| 3489335 / 3489336 | `ctest: [..., wpts, ...]` | `... out of 60` | 60 Passed |
| 3489337 / 3489338 | `ctest: [..., rest, ...]` | `... out of 5532` | 5518 / 14 Skipped / 1 Not Run |
| 3489339 / 3489340 | `ctest_quint: [release / debug, amd64]` | `... out of 195` | 195 Passed |
| 3489343 / 3489344 | `ctest_kvm: [Release / Debug, ... ubuntu2604 ...]` | `... out of 382` | 382 / 12 Not Run |
| 3489345 / 3489346 | `ctest_kvm: [Release / Debug, ... ubuntu2604-nfstest ...]` | `... out of 92` | 92 Passed |

The train has no `ctest_explore`, `smbtorture_explore` or `ctest_kvm_images` jobs, as `rules_explore` predicts.

**Pipeline 205316 (scheduled, the retry example).** Every listed job has status `success` except 3488415.

| Job | Name | Summary | Artifact tally |
|---|---|---|---|
| 3488413 / 3488414 | `ctest: [release / debug, amd64, pynfs, ...]` | `... out of 928` | 928 Passed |
| **3488415** | `ctest: [release, amd64, smbtorture, ...]` (**failed**) | `99% tests passed, 1 tests failed out of 1741` | 1740 Passed, 1 Timeout |
| 3488416 | `ctest: [debug, amd64, smbtorture, ...]` | `... out of 1741` | 1741 Passed |
| 3488417 / 3488418 | `ctest: [..., wpts, ...]` | `... out of 60` | 60 Passed |
| 3488419 / 3488420 | `ctest: [..., rest, ...]` | `... out of 5528` | 5514 / 14 Skipped / 1 Not Run |
| 3488421 / 3488422 | `ctest_explore: [..., pynfs_diskfs, ...]` | `... out of 448` | 448 Passed |
| 3488423 / 3488424 | `ctest_quint: [...]` | `... out of 195` | 195 Passed |
| 3488427 | `smbtorture_explore` | `== totals: 3930/3930 cells measured, failing (disabled) 53, newly passing 108, still failing 614, timing out 20` | n/a |
| 3488428 / 3488429 | `ctest_kvm: [..., ubuntu2604 ...]` | `... out of 382` | 382 / 12 Not Run |
| 3488430 / 3488431 | `ctest_kvm: [..., ubuntu2604-nfstest ...]` | `... out of 92` | 92 Passed |
| 3488432 / 3488433 | `ctest_kvm_images: [..., ubuntu2404 ...]` | `... out of 388` | 388 / 6 Not Run |
| 3488434 / 3488435 | `ctest_kvm_images: [..., ubuntu2404-nfstest ...]` | `... out of 92` | 92 Passed |
| 3488436 / 3488437 | `ctest_kvm_images: [..., ubuntu2404_hwe ...]` | `... out of 388` | 388 / 6 Not Run |
| 3488438 / 3488439 | `ctest_kvm_images: [..., ubuntu2204_hwe ...]` | `... out of 388` | 388 / 6 Not Run |
| **3488440** | `ctest: [release, amd64, smbtorture, ...]` (retry of 3488415) | `100% tests passed, 0 tests failed out of 1741` | 1741 Passed |

The pynfs gating leg had 928 tests in 205316 and 929 in 205408. 929 + 448 = 1377 matches the registered count. The `.gitlab-ci.yml` comments still say 1376 (:297, :314).

## Harness deviations from shipped defaults

SUC-1 asks for the suites to be "run against the product's shipping configuration" (`docs/product/prd.md:390-392`).

The shipped configuration has two layers:
- the code defaults in `chimera_server_config_init` (`src/server/server.c:233-476`);
- the installed `src/daemon/chimera.json` (`install(FILES chimera.json DESTINATION etc)`, `src/daemon/CMakeLists.txt:10`). That file (`src/daemon/chimera.json:1-48`) sets:
  - `common`: `tcp_flavor` plain, sync delegation on with **1** sync delegation thread, async delegation off (8 threads if enabled), tracing off (`:2-14`);
  - `server`: NFS and SMB enabled, **16** core threads (`:15-19`);
  - a `memfs` filesystem (`:20-24`);
  - two mounts, `linux_dir` (the `linux` passthrough module on `/var/tmp`) and `memfs_dir` (`:25-34`);
  - one NFS export `/nfs1` on `memfs_dir` (`:35-39`);
  - two SMB shares, `smb1` on `linux_dir` and `smb2` on `memfs_dir` (`:40-47`).

  These mounts, exports and shares are part of "shipped". The daemon applies `server.threads` over the `server.c` default of 8 (`src/daemon/daemon.c:471-474`, `src/server/server.c:240`). It applies `common.sync_delegation_threads` last, over both the `server.c` default of 8 and any `server`-section key (`src/daemon/daemon.c:525-534`, `src/server/server.c:243`). **The shipped values are therefore 16 core threads and 1 sync delegation thread.** Every other feature default below comes from `server.c`.

A diskfs backend that is not configured defaults to `unsafe_async` false (`src/vfs/diskfs/diskfs_mount.c:1157`, `json_is_true(...)`) and a 1 GiB intent log (`src/vfs/diskfs/space_map.h:240`).

### Deviations, all suites

This table consolidates the deviations. The per-suite sections give the full smbtorture, pynfs, s3 and posix detail.

| Harness (suite) | Setting | Harness value | Shipped default | Cite |
|---|---|---|---|---|
| pynfs wrapper (pynfs, diskfs rows) | diskfs `unsafe_async` / `intent_log_size` | `true` / 64 MiB | false / 1 GiB | `scripts/pynfs_test_wrapper.sh:134` |
| pynfs wrapper (pynfs) | `nfs4_lease_time` | 5 s by default; 20 / 30 / 40 s for the listed codes; 90 s for the combined runs | 90 s (`NFS4_LEASE_TIME_DEFAULT_S`, `src/server/nfs/nfs4_lease.h:27`) | wrapper :38, :168; `pynfs/CMakeLists.txt:116`, :266, :345 |
| pynfs wrapper (pynfs) | `nfs4_grace_time` | 10 s | 180 s (`nfs4_lease.h:28`) | wrapper :39, :169 |
| pynfs wrapper (pynfs `deleg*` flags, `PYNFS_FORCE_DELEG`) | `nfs4_delegations` | `true` | off (`server.c:455`) | wrapper :79-89, :167 |
| pynfs cold-restart wrapper | `nfs4_drc`; lease / grace | `true`; 4 s / 6 s | off (`server.c:463`); 90 / 180 s | `scripts/pynfs_cold_restart_test_wrapper.sh:86`, :41-42 |
| pynfs pNFS wrapper (flex tests) | `pnfs.enabled` | `true` | off (`server.c:382`) | `scripts/pynfs_pnfs_test_wrapper.sh:88` |
| NFS Kerberos wrappers (pynfs krb5 tests; KVM `kerberos/`) | NFS Kerberos acceptor | on | off (`server.c:364`) | `scripts/nfs_kerberos_test_wrapper.sh:249`; `kvm/kvm_nfs_kerberos_test_wrapper.sh:235` |
| smbtorture driver | persistent handles; leases + oplocks; dirleases; 2 NICs | all on | off; off; off; 0 NICs | `src/server/smb/tests/smbtorture_test.c:564`, :571-572, :577, :588-592; `server.c:265`, :296-297, :271, :261 |
| smbtorture driver (named subtests) | streams; `acl_inherited_canonicalize=0`; Windows replay profile; signing required; notify disabled; encryption | on for those subtests | off; 1; 0; 0; 0; 0 | `smbtorture_test.c:609`, :620, :666, :767, :778, :796; `server.c:276`, :310, :344, :281, :301, :285 |
| smbtorture driver (diskfs rows) | `unsafe_async` / intent log | `true` / 64 MiB | false / 1 GiB | `smbtorture_test.c:717`, :720 |
| WPTS wrapper (all WPTS configs) | `smb_named_streams`, `smb_leases`, `smb_oplocks` | `true` | off | `scripts/wpts_smb_test_wrapper.sh:264-266` |
| WPTS profile `CHIMERA_SMB_PERSISTENT` | persistent handles; `FileShare` CA | on | off | wrapper :215, :162-163; env `src/server/smb/tests/CMakeLists.txt:214` |
| WPTS profile `CHIMERA_SMB_COMPRESSION` | `smb_compression` | `true` | off (`server.c:289`) | wrapper :223; CMake :215 |
| WPTS profile `CHIMERA_SMB_MULTICHANNEL` | 3 NICs advertised; `smb_min_dialect "2.0.2"` | on | 0 NICs | wrapper :231, :236; CMake :216 |
| WPTS profile `CHIMERA_SMB_ENCRYPTION` / `_PERSHARE_ENCRYPTION` | `smb_encryption "enabled"`; `encrypt_data` on `SMBEncrypted` | on | off | wrapper :244, :188; CMake :218-219 |
| WPTS profile `CHIMERA_SMB_DIRLEASE` | `smb_directory_leases` | `true` | off | wrapper :252; CMake :227 |
| WPTS profile `CHIMERA_SMB_SYMLINK` | reparse-point fixtures seeded | on | n/a (fixtures) | wrapper :323-324; CMake :232 |
| WPTS (all) | `ShareForceLevel2` share `force_level2_oplock` | on, that share only | off | wrapper :196 |
| pike wrapper (all configs) | `smb_named_streams`, `smb_leases`, `smb_oplocks` | `true` | off | `scripts/pike_smb_test_wrapper.sh:155-157` |
| pike profile `persistent` | persistent handles + CA share + 3 NICs (multichannel) | on | off / 0 NICs | wrapper :134, :123, :142; CMake :512 |
| pike profile `encryption` | `smb_encryption "enabled"` + share `encrypt_data` | on | off | wrapper :138, :129; CMake :513 |
| Ceph harness | 3 access keys → one user; `s3_bucket_root "/share"`; no pool mode | as listed | S3 off in the shipped config (`server.c:392`, `chimera.json:15-19`) | `src/server/s3/tests/ceph/run_ceph_s3tests.sh:79-90` |
| boto3 S3 harness (diskfs rows) | `unsafe_async` / intent log | `true` / 64 MiB | false / 1 GiB | `src/server/s3/tests/s3_test.py:133` |
| posix common (cthon-port, pjd and posix diskfs rows) | `unsafe_async` / intent log | `true` / 64 MiB | false / 1 GiB | `src/posix/tests/posix_test_common.h:589`, :593 (included by `cthon_common.h:37`, `pjd_common.h:36`) |
| fsx (in-process) | `unsafe_async` | `true` | false | `src/posix/tests/run_fsx.sh:126`; `src/posix/tests/fsx.c:4037` |
| KVM NFS wrapper (cthon04-KVM, xfstests) | diskfs `unsafe_async`; NFSv3 `nolock` (client side) | `true`; `nolock` | false; n/a | `kvm/kvm_nfs_test_wrapper.sh:165`, :270 |
| KVM SMB wrapper (cthon04-KVM SMB, xfstests SMB) | diskfs `unsafe_async`; `smb_min_dialect` = dialect under test | `true`; pinned | false; 2.0.2 floor (`server.c:254-259`) | `kvm/kvm_smb_test_wrapper.sh:172`, :206 |
| KVM nfstest wrappers | diskfs `unsafe_async`; `nfs4_delegations` (2-client) | `true`; `true` except `nfstest_cache` | false; off | `kvm/kvm_nfstest_test_wrapper.sh:175`; `kvm/kvm_nfstest_2client_test_wrapper.sh:155`, :130-131, :178 |
| All the above daemons | `threads` 4, `rcu_reclaim_threads` 4; the `delegation_threads` key they write is read by nothing, so sync delegation threads stay at the `server.c` default of 8 | 4 / 8 | 16 core (`chimera.json:18`) / 1 sync delegation (`chimera.json:5`) / 4 RCU (`server.c:440`) | e.g. `scripts/pynfs_test_wrapper.sh:161`, :165, :166 |

### The diskfs `unsafe_async` effect

`unsafe_async` is documented as "config opt-in: submit block writes without FUA/sync (no crash safety)" (`src/vfs/diskfs/diskfs_internal.h:1718`). It turns off the FUA/sync flag on several write paths:
- the mount helper (`diskfs_mount.c:550`, :609);
- clone copy-on-write (`diskfs_clone.c:400`);
- the data and metadata I/O paths (`diskfs_io.c:67`, :3580, :4570);
- the intent log (`diskfs_log.c:2061`).

22 test files in `scripts/`, `kvm/` and `src/` (outside the diskfs module itself) reference it, and none sets it false. `src/vfs/diskfs/tests/diskfs_test_harness.h` takes the value from a field that both of its init paths set to 1 (:367, :410). Every diskfs harness of a SUC-2 suite (pynfs, smbtorture, cthon-port, pjd) and of an adjacent suite (cthon04-KVM, nfstest, xfstests, fsx, boto3 S3) is among them. So no diskfs row in any of today's runs exercises a durable (FUA/sync) write path. These rows prove nothing about durability. WPTS and pike are registered on memfs only (`src/server/smb/tests/CMakeLists.txt:268`, :529), so this setting does not reach them.

### Limit on the "shipping configuration" claim

No SUC-2 suite runs against the shipped configuration today. The table above lists every deviation found, and they fall into five groups:
- feature enablement: SMB leases, persistent handles, streams, multichannel, encryption, dirleases, compression, delegations, pNFS, Kerberos, DRC;
- security posture: signing required, and encryption on;
- shortened NFSv4 lease and grace timers;
- non-durable diskfs;
- harness-specific identities and shares.

A green result therefore shows conformance of the configured profile, not of the shipped default.


## Pass semantics today

What a green ctest result means today, suite by suite. Each suite section below gives the detail and the citations; in particular "pynfs result semantics", "smbtorture client options" and "smbtorture server-config overrides".

| Suite | Counts as pass today | Counts as skip | Not detected | Configuration |
|---|---|---|---|---|
| pynfs | No FAIL in the JUnit file. WARN and NOTSUP (an unsupported operation) pass, and an OMIT run passes (`scripts/pynfs_test_wrapper.sh:310-333`) | Only the 5 Kerberos tests have SKIP 77, and they passed in CI | Per-code WARN, NOTSUP and OMIT: GitLab keeps no JUnit | Short lease and grace, 4 threads, diskfs `unsafe_async`. Delegations and the unauthenticated REST debug endpoint are on in the combined runs (derived from the wrapper code, not observed in CI; see Findings 1) |
| cthon (port) | Program exit 0 | None (no SKIP 77) | — | posix harness: diskfs `unsafe_async`, 4 threads |
| cthon04 (KVM) | `runtests` exit 0 in `-f` (functional, one iteration) mode | In-run sub-test skips (SMB `CIFS=yes`), not measured | Guest output of passing tests (`--output-on-failure`) | KVM wrappers: diskfs `unsafe_async`; SMB dialect floor pinned to the dialect under test |
| smbtorture | smbtorture exit 0, **including a subtest that skips**. 96 `bind_negative` cells and 103 off-memfs `copy_chunk` cells pass by skipping | SKIP 77 only when the binary is missing | Subtest-level skips: the logs carry no subtest output | Persistent handles, leases, oplocks, dirleases and 2 multichannel NICs on for every run; further per-subtest overrides; `samba3=yes`; fixed seed |
| Ceph s3-tests | pytest pass on a 214-case allowlist | SKIP 77 when the suite is missing | Not run in GitLab CI: not registered, because there is no system boto3 (job 3490972). GitHub `extended` not measured; registration unlikely for the same reason | Three access keys aliased to one user; no TLS |
| pjdfstest (port) | Program exit 0 | None (SKIP 77 is set but never returned) | — | posix harness: diskfs `unsafe_async` |
| warp | — | — | Not in the tree; never runs | — |
| all (GitLab) | Job exit status. `ctest`, `ctest_explore`, `ctest_kvm` and `ctest_kvm_images` retry once on `script_failure` (`.gitlab-ci.yml:260-265`) | — | A failure masked by the retry: job 3488415 failed with "2272 - chimera/server/smb/smbtorture_smb2_notify_linux (Timeout) smbtorture" and its retry, job 3488440, reported "100% tests passed, 0 tests failed out of 1741" | — |

## pynfs

pynfs is the Linux NFS community's NFSv4 server conformance suite. Each test has a short code (for example `OPEN1`), a list of flags (for example `open`, `all`) and, in the 4.1 tree, an optional version range (`VERS:`). A run selects tests by naming flags or codes; `no<flag>`/`no<code>` deselects them. The last matching argument wins.

Every count below is reproduced by `repro/pynfs.repro.sh` in the vault evidence directory (see "Scope and method"). Lines in the pynfs trees are cited as `fork:<path>:<line>` (chimera-nas/pynfs @ b861f7b8) or `upstream:<path>:<line>` (cd470182). They are not files in this repository.

### Source and pins

| Item | Value | Source |
|---|---|---|
| Harness copy | chimera-nas/pynfs, commit `b861f7b8211b4dc798b88a7e81b300fb4615189c` (branch `chimera-fast-ci`), cloned to `/opt/pynfs` and built with `setup.py build` | `builders/dev/Dockerfile:17` "ARG PYNFS_REF=b861f7b8211b4dc798b88a7e81b300fb4615189c"; :138 "git clone https://github.com/chimera-nas/pynfs.git /opt/pynfs"; :139 "python3 ./setup.py build" |
| Pin in the CI image | `/opt/pynfs` HEAD is `b861f7b8…`. The image lists 679 codes for 4.0 and 267 for 4.1 | Unprivileged `podman run --network=none` of `chimera-dev:ci-chimera-442`, digest `sha256:b6ce8991…a1d`. That is the digest job 3490966 resolved: "Resolved image digest sha256:b6ce8991f5ca10997ee563fe5c3a5ebf13eb0310b7358a1537c4e39513590a1d" |
| Upstream | `git://git.linux-nfs.org/projects/cdmackay/pynfs.git`, HEAD = master = `cd4701827a` on 2026-10-04 | `git ls-remote` |
| Merge-base | `3c14d9e3ad12272ab2f6092c85bb28ab03951484` | `git merge-base b861f7b8 cd4701827a` |
| Result module | `testmod.py` is byte-identical in fork 4.0 (`nfs4.0/lib/`), fork 4.1 and upstream 4.1 | `cmp` |
| Code counts | 4.0: fork 679, upstream 689. 4.1 tree: fork 267, upstream 266 (merge-base 264) | `testserver.py --showcodesflags` |

### Fork delta

The fork has 5 commits that are not upstream. Upstream has 12 commits after the merge-base that are not in the fork.

| Change | Fork commit / upstream commit | Effect on what runs | Evidence |
|---|---|---|---|
| Adds BIND1–3 (`st_bind_conn_to_session.py`, flags `bind_conn_to_session all`) | fork `b861f7b` | 3 fork-only codes. They run in the 4.1 and 4.2 combined runs and in the Kerberos tests | `fork:nfs4.1/server41tests/st_bind_conn_to_session.py:17,49,78` "CODE: BIND1/2/3" |
| WRT15 (`testSizes`) samples 24 write lengths instead of every length 0..max-1, unless `PYNFS_WRITE_FULL_SIZES` is set | fork `c4bf18d` | Narrower than upstream. The tree never sets the variable (`grep -c` = 0 in `pynfs/CMakeLists.txt` and the wrapper), so CI always samples | `fork:nfs4.0/servertests/st_write.py:416` "if os.environ.get(\"PYNFS_WRITE_FULL_SIZES\"):" |
| flex LAYOUTSTATS sleeps are scaled by `PYNFS_LAYOUTSTATS_SLEEP_SCALE`, default 0.1 | fork `6cd5c83` | Shorter pacing for FFLS\* | `fork:nfs4.1/server41tests/st_flex.py:19` "LAYOUTSTATS_SLEEP_SCALE = float(os.environ.get(\"PYNFS_LAYOUTSTATS_SLEEP_SCALE\", \"0.1\"))" |
| EID9 sleep clamped at ≥0 for a lease shorter than 10 s | fork `944c7a8` | Lets EID9 run under the harness's 5 s lease | `git diff 3c14d9e3 b861f7b8 -- nfs4.1/server41tests/st_exchange_id.py` "time.sleep(max(0, min(lease - 10, 1)))" |
| Python 3 fixes in `st_read.py` and `st_write.py` (bytes, `//`) | fork `063eabe` | Test-code fixes only | same diff |
| GATT11a–j ("tests for unsupported fattr4 attributes") | upstream `cd47018` | 10 codes missing from the pin (4.0) | code-set diff |
| CALLBACK1 (CB_NOTIFY_LOCK, expired client), DELEG26 (READ with a delegation stateid after CLOSE) | upstream `0a2d3c4`, `23d5c46` | 2 codes missing from the pin (4.1 tree) | code-set diff |
| Slot handling in the 4.1 client | upstream `11472e1` ("mark slot free after compound op complete"), `10ff905` (reuse a slot only when SEQUENCE itself gets NFS4ERR_DELAY) | Client behaviour differs from upstream. Not measured | `git log 3c14d9e3..cd4701827a` |

Diffstat for fork over merge-base: "6 files changed, 109 insertions(+), 7 deletions(-)". No test's FLAGS, DEPEND or VERS metadata differs between pin and upstream for the codes they share.

### Wiring

| Dimension | Today | Cite |
|---|---|---|
| Registration gate | Registered when `CHIMERA_ENABLE_PYNFS_TESTS` (default ON) and `/opt/pynfs/nfs4.0/testserver.py` exists. There is no netns gate. The stale comment above it still says unpassing combinations are "marked DISABLED" | `CMakeLists.txt:871` "option(CHIMERA_ENABLE_PYNFS_TESTS \"Register PyNFS test suite ctests\" ON)"; :886 "AND EXISTS \"/opt/pynfs/nfs4.0/testserver.py\")"; :867 "the rest are marked DISABLED" |
| netns | Every wrapper creates a network namespace and has no fallback, so an unprivileged tree registers 1377 tests that fail | `scripts/pynfs_test_wrapper.sh:193` "ip netns add \"${NETNS_NAME}\"" |
| Tier | extended (directory default) | `CMakeLists.txt:357` "set(CHIMERA_TEST_TIER \"extended\")" |
| Backends | memfs, linux, io_uring, diskfs_io_uring; plus diskfs_aio when libaio is usable and cairn when `CAIRN_ENABLED` (default ON). CI registers all 6 | `pynfs/CMakeLists.txt:9` "set(PYNFS_BACKENDS memfs linux io_uring diskfs_io_uring)"; :11 "if(HAVE_LIBAIO_USABLE)"; :15 "if(CAIRN_ENABLED)"; `CMakeLists.txt:105` |
| Minor versions | 4.0, 4.1, 4.2. 4.0 uses `nfs4.0/testserver.py`; 4.1 and 4.2 use `nfs4.1/testserver.py --minorversion=1/2` | `pynfs/CMakeLists.txt:36` "set(PYNFS_MINOR_VERSIONS 0 1 2)"; `scripts/pynfs_test_wrapper.sh:283` "--minorversion=1 --maketree"; :289 "--minorversion=2 --maketree" |
| Flag lists | 4.0: 38 flags including `acl`. 4.1: 19 flags. 4.2: the 4.1 list plus `copy sparse` | `pynfs/CMakeLists.txt:20` "set(PYNFS_FLAGS_0"; :21 "access acl close commit compound create delegations"; :28; :34 "set(PYNFS_FLAGS_2 ${PYNFS_FLAGS_1} copy sparse)" |
| Combined runs | One ctest per (backend, minor) for every non-isolated flag, with `no<isolated-flag>` appended. Timeout 900 s (wrapper 800 s), lease 90 s, JUnit to `pynfs-junit/` | `pynfs/CMakeLists.txt:328` "set(name chimera/pynfs/combined_${backend}_nfs4.${minor})"; :307 "list(APPEND excludes \"no${flag}\")"; :343 "TIMEOUT 900"; :345 "PYNFS_TIMEOUT=800;PYNFS_NFS4_LEASE_TIME=90" |
| Per-code runs | The isolated flags (4.0: `lock lockt locku close renew replay delegations`; 4.1/4.2: `deleg courteous destroy_session exchange_id`) get one ctest per code per backend, with a fresh daemon each. Codes come from `--showcodesflags`, which ignores VERS; this replay found no code whose VERS excludes its minor version. 4 codes (LOCK1, LOCKHELD, LOCKRNG, LKU2) are registered twice per 4.0 backend, under two flags (24 duplicate entries in all) | `pynfs/CMakeLists.txt:48` "set(PYNFS_ISOLATED_FLAGS"; :51; :53; :198 "python3 ${testserver} --showcodesflags"; :247 "set(name chimera/pynfs/${flag}/${code}_${backend}_nfs4.${minor})" |
| Dependencies | Every run passes `--rundeps --force`: dependencies of a selected test run, and a failed dependency does not stop the dependent test | `pynfs/CMakeLists.txt:252` "\"PYNFS_RUNDEPS=1\""; `scripts/pynfs_test_wrapper.sh:248` "PYNFS_DEP_ARGS=(--rundeps --force)"; `fork:nfs4.1/testmod.py:307` "runfilter = lambda x, y : True"; :324 "elif (not options.force) and \\" |
| flex (pNFS flex files) | 27 per-code tests, memfs MDS plus memfs data server, 4.1 only | `pynfs/CMakeLists.txt:371` "pynfs_codes_for_flag(flex 1 PYNFS_FLEX_CODES)"; :377 "LABELS \"pynfs;pynfs_flex;memfs;nfs4.1\"" |
| Kerberos | 5 tests, memfs, 4.1, running 12 codes (`EID1 EID2 CSESS1 CSESS2 SEQ1 SEQ4 BIND1 BIND2 BIND3 RECC1 OPEN1 OPEN2`) under krb5 or krb5p. These are the only pynfs tests with SKIP 77 (missing krb5 tooling or python gssapi). In CI they **passed, not skipped** (job 3490966: "Test #925: chimera/pynfs/kerberos/krb5_smoke_memfs_nfs4.1 ..............   Passed    1.98 sec") | `pynfs/CMakeLists.txt:389` "set(NFS_KRB_CODES EID1 EID2 …)"; :394 "SKIP_RETURN_CODE 77"; `scripts/nfs_kerberos_test_wrapper.sh:94` "exit 77" |
| cold_restart | 1 test on cairn with a persistent KV store and 4.1 DRC; driven by an in-tree client on pynfs's library, not by testserver codes | `pynfs/CMakeLists.txt:444` "if(CAIRN_ENABLED)"; :447 "add_test(NAME chimera/pynfs/cold_restart/cairn_nfs4.1" |
| Labels | per-code: `pynfs;pynfs_<flag>;<backend>;nfs4.<minor>`; combined: `pynfs;<backend>;nfs4.<minor>` | `pynfs/CMakeLists.txt:280`; :342 |
| JUnit | Combined runs write per-code JUnit to `pynfs-junit/`. Only GitHub's workflow sets the directory and reads it; GitLab keeps only the ctest log as artifact | `pynfs/CMakeLists.txt:59`; `.github/workflows/build-test.yml:582` "-DPYNFS_JUNIT_DIR=/workspace/ctest-results/pynfs-junit"; `scripts/ci_test_report.py:65`; `.gitlab-ci.yml:341-342` "paths:" / "- ctest_${MODE}_${GROUP}_${ARCH}.log" |

**Registered total (derived by replaying `pynfs/CMakeLists.txt` against the pinned code list): 1377.**

| Part | Count | Derivation |
|---|---|---|
| Combined | 18 | 3 minors × 6 backends |
| Per-code, 4.0 | 702 | 117 per backend (close 13, delegations 26, lock 30, lockt 16, locku 15, renew 3, replay 14) × 6 |
| Per-code, 4.1 | 312 | 52 per backend (courteous 7, deleg 12, destroy_session 7, exchange_id 27 − EID50) × 6 |
| Per-code, 4.2 | 312 | same as 4.1 |
| flex | 27 | memfs, 4.1 |
| Kerberos | 5 | memfs, 4.1 |
| cold_restart | 1 | cairn, 4.1 |
| **Total** | **1377** | each of the 6 backends has 224 combined + per-code entries (6 × 224 = 1344), plus 33 |

**CI selectors.** The label partitions in two:

| Leg | Selector | Tests | Cadence | Cite |
|---|---|---|---|---|
| `ctest` pynfs (gating) | `-L '^pynfs$' -E diskfs_` | 929 = 4 backends × 224 + 27 flex + 5 Kerberos + 1 cold_restart | merge train and schedule (`rules_full`); manual on MR push | `.gitlab-ci.yml:322` "CTEST_FILTER: [ \"-L '^pynfs$' -E diskfs_\" ]"; :72 |
| `ctest_explore` pynfs_diskfs (informational) | `-L '^pynfs$' -R diskfs_` | 448 = 2 diskfs backends × 224 | schedule only (`rules_explore`) | `.gitlab-ci.yml:364`, :379 "CTEST_FILTER: [ \"-L '^pynfs$' -R diskfs_\" ]"; :79 |

The `.gitlab-ci.yml` comment still says "both select 1376" and "(928 + 448 = 1376, verified by direct count)" (:297, :314). The pinned pipelines measure 929 + 448 = 1377. Both legs retry once on `script_failure` (:260, :368).

### Run, skipped, excluded, missing

Configuration used: the harness's own config, not the shipped default (see "pynfs harness configuration vs shipped defaults").

| Bucket | What | Count | Reason | Evidence |
|---|---|---|---|---|
| **Run (gating)** | `ctest: [release\|debug, amd64, pynfs, -L '^pynfs$' -E diskfs_]` | 929 ctests per job, 929 passed | merge-train gate | job 3490966 (pipeline 205408, 2026-10-04, sha 3ea255e0): "100% tests passed, 0 tests failed out of 929" (trace :1052); job 3490967: "[chimera/ctest_debug] 100% tests passed, 0 tests failed out of 929" (trace :1052); merge train 205343 jobs 3489331/3489332 (MR 114): "out of 929" (trace :1052) |
| **Run (explore)** | `ctest_explore: [release\|debug, amd64, pynfs_diskfs, -L '^pynfs$' -R diskfs_]` | 448 ctests per job, 448 passed | schedule only, does not gate | job 3490974 (pipeline 205408): "[chimera/ctest_release] 100% tests passed, 0 tests failed out of 448" (trace :1058); job 3490975: "[chimera/ctest_debug] 100% tests passed, 0 tests failed out of 448" (trace :1058) |
| Run: distinct codes run (derived) | 4.0: 624 codes (622 on linux and io_uring). 4.1: 216 on memfs (189 + 27 flex), 189 on the other 5 backends. 4.2: 204 on every backend | — | combined selection + per-code + `--rundeps` dependencies + flex | replay (`pynfs.repro.sh` §3). The combined runs select 510 (4.0 memfs; 508 linux/io_uring), 137 (4.1) and 152 (4.2) codes |
| **Skipped (ctest)** | `***Skipped` / `Not Run` lines in the saved ctest logs | 0 in all four jobs (929/929, 929/929, 448/448, 448/448 `Passed`) | — | `artifacts-3490966.zip` … `artifacts-3490975.zip` (pipeline 205408) |
| Skipped (per code: OMIT, NOTSUP, WARN) | per-code results inside a ctest | **not measured** | GitLab artifacts hold only `ctest_*.log`. The pynfs JUnit is not collected, and ctest prints test output only on failure | `.gitlab-ci.yml:341-342`; `unzip -l` shows no `.xml` |
| **Excluded** | CSID7 (4.1, 4.2) | all 6 backends | Tagged `pnfs` (LAYOUTGET). Excluded through `PYNFS_EXTRA_currentstateid nopnfs`, which drops every `pnfs`-tagged code from the combined runs; CSID7 is the only such code selected | `pynfs/CMakeLists.txt:66` "set(PYNFS_EXTRA_currentstateid nopnfs)"; commit 40bb1368 |
| Excluded | WRT18, SATT15 (4.0) | linux, io_uring | The change attribute must advance on every rapid op. On these backends it is the host ctime, and "Our CI runner's kernel predates multigrain" | `pynfs/CMakeLists.txt:75`, :79-80 "set(PYNFS_SKIP_BACKENDS_WRT18  linux io_uring)"; commit 52622f9a |
| Excluded | EID50 (4.1, 4.2) | all 6 | SP4_SSV: the server "declines SP4_SSV in EXCHANGE_ID" | `pynfs/CMakeLists.txt:83`, :91 "set(PYNFS_SKIP_BACKENDS_EID50  ${PYNFS_BACKENDS})"; commit d72c1e04 |
| Excluded | SEC7 (4.0) | all 6 | Requires SECINFO to list RPCSEC_GSS. Kerberos is "off by default and off in this" harness | `pynfs/CMakeLists.txt:95`, :102, :323; commit ee8c5281 |
| **Missing, 4.0** | 65 of 689 upstream codes never run on any backend (list below) | 65 | not in a selected flag, absent from the pin, or excluded | replay against upstream cd470182 |
| **Missing, 4.1** | 39 of 252 upstream codes valid at minor 1 | 39 | same | same |
| **Missing, 4.2** | 65 of 266 upstream codes valid at minor 2 (the 27 flex codes run only at 4.1) | 65 | same | same |

Missing codes by test module (upstream cd470182; "flags" are each group's distinguishing pynfs flags):

| Minor | Module (count) | Codes | Flags / why not run |
|---|---|---|---|
| 4.0 | st_delegation (7) | DELEG11 DELEG12 DELEG13a–e | `writedelegations`, which is not in `PYNFS_FLAGS_0` |
| 4.0 | st_fslocations (13) | FSLOC1 FSLOC2 FSLOC3 FSLOC4a/b FSLOC5a/b FSLOC6a/b FSLOC7a/b FSLOC8a/b | `fslocations`, not selected |
| 4.0 | st_getattr (10) | GATT11a–j | not in the pin |
| 4.0 | st_gss (8) | GSS1–GSS8 | `gss`, not selected (no Kerberos in the harness) |
| 4.0 | st_secinfo (1) | SEC7 | excluded (above) |
| 4.0 | st_reboot (8) | REBT1 REBT2 REBT3 REBT5 REBT6 REBT7 REBT10 REBT11 | `reboot`, not selected (REBT8 runs, via `delegations`) |
| 4.0 | st_lock (7) | LOCK18 LOCK19 LOCK21 LOCK22; LOCK11g LOCK13a; LOCK8c | `fairlocks` (4); `ganesha` (2); LOCK8c has no flag but `everything` (1) |
| 4.0 | st_locku, st_read, st_write, st_setclientidconfirm (5) | LKU6c, RD4 RD9g, WRT10g, CIDCF2 | `ganesha` (all 5) |
| 4.0 | st_lookupp (2) | LOOKP5 LOOKP6 | `special` (LOOKP6 also `ganesha`) |
| 4.0 | st_spoof (3) | SPOOF1–3 | `spoof` |
| 4.0 | st_setattr (1) | SATT1a | no flag other than `everything` |
| 4.1, 4.2 | st_reboot (11) | REBT1 REBT2a–c REBT3a–c REBT4a–c REBT5 | `reboot` |
| 4.1, 4.2 | st_secinfo_no_name (4) | SECNN1–4 | `secinfo_no_name`, not in `PYNFS_FLAGS_1` |
| 4.1, 4.2 | st_secinfo (2) | SEC1 SEC2 | `all` only (`secinfo` is not a 4.1 flag in the list) |
| 4.1, 4.2 | st_getdevicelist (8), st_block (4) | GETDINFO1 GETDLIST1 GETLAYOUT1 GETLAYOUT100 LAYOUTCOMMIT1 LAYOUTRET1–3; BLOCK1–4 | pNFS (`pnfs`, `block`) |
| 4.1, 4.2 | st_current_stateid (1), st_exchange_id (1) | CSID7, EID50 | excluded (above) |
| 4.1, 4.2 | st_trunking (2) | TRUNK1 TRUNK2 | `trunking` |
| 4.1, 4.2 | st_create_session (2) | CSESS20 CSESS21 | `ganesha` |
| 4.1, 4.2 | st_lookupp (1) | LKPP4 | `special` |
| 4.1, 4.2 | st_callback (1), st_delegation (1) | CALLBACK1, DELEG26 | not in the pin |
| 4.1 | st_copy (1) | COPY5 | `copy` is a 4.2-only flag in the list, and COPY5 has no VERS limit |
| 4.2 | st_flex (27) | FFGDI1 FFLA1 FFLG2 FFLOOS FFLOR1 FFLOR100 FFLORACCESSREAD FFLORACCESSWRITE FFLORDELAYREAD FFLORDELAYWRITE FFLORFBIG FFLORFBIGWRITE FFLORIOREAD FFLORIOWRITE FFLORNOSPC FFLORNOSPCWRITE FFLORNXIOREAD FFLORNXIOWRITE FFLORSERVERFAULTREAD FFLORSERVERFAULTWRITE FFLORSTALEREAD FFLORSTALEWRITE FFLS1 FFLS2 FFLS3 FFLS4 FFST1 | flex runs only at 4.1 |

Per-backend gaps inside "run": WRT18 and SATT15 do not run on linux or io_uring; flex runs only on memfs.

### Protocol coverage

| Version | Covered? | How |
|---|---|---|
| NFSv4.0 | yes | `nfs4.0/testserver.py`, 624 of 689 upstream codes |
| NFSv4.1 | yes | `nfs4.1/testserver.py --minorversion=1`, 213 of 252 upstream codes, plus BIND1–3 |
| NFSv4.2 | yes | `--minorversion=2`, 201 of 266 upstream codes, plus BIND1–3. The 14 codes with `VERS: 2-` (ALLOC1–3, XATT1–11) run only here; in the 4.1 runs they are filtered out by VERS and count as OMIT |
| NFSv3 | **no** | pynfs has no NFSv3 server tests. Upstream has only `nfs4.0/` and `nfs4.1/` server suites; `nfs3client.py` is a helper library no server test imports |
| RPCSEC_GSS | partial | No GSS code in the main matrix (4.0 GSS1–8 and SEC7 never run). The 5 Kerberos tests run 12 4.1 codes under krb5/krb5p on memfs |
| pNFS | partial | 27 flex-files codes on memfs at 4.1. Block, SCSI and generic layout codes never run |

### Proposed full subset (PROPOSAL for Q-SUC-2)

Proposal, pynfs row: "Upstream `cd470182`: 4.0, 4.1@1 and 4.2@2, plus BIND1–3. **Prerequisite:** bump the fork. (N) pNFS. (O) FSLOC, TRUNK. (P) ACL\* (ACL-1), GSS/krb5p (SEC-6), REBT/reclaim and delegations (HA-5): interim only, tied to SEC-10, `shipped-defaults-safe` and an E1 restart-harness child. (M) SECNN. `PYNFS_WRITE_FULL_SIZES=1`." Pass criterion: "PASS only. NOTSUP fails unless (O)/(N). WARN fails unless listed. OMIT fails unless it is the filter-OMIT of an excluded test. No retry." Corruption oracle: "Partial (READ/WRITE read-back)".

Universe per run: 4.0 = 689 codes; 4.1@1 = 252 + BIND1–3 = 255; 4.2@2 = 266 + BIND1–3 = 269. The class lists below apply the class names of "Proposed definition of a full run" to the pynfs flags and modules:
- (N) is `pnfs` or `flex`, or modules st_flex, st_block and st_getdevicelist.
- (O) is `fslocations` and `trunking`.
- (P) is `acl`, `gss`, `reboot`/st_reboot plus `reclaim_complete`, and `delegations`/`writedelegations`/`deleg`.
- (M) is `secinfo_no_name`.
- Every other code is in the full set by the principle in "Proposed definition of a full run" ("every test in the pinned upstream suite … minus a written exclusion list") and has no class.

| Minor | Class | Codes (count) | Run today | Not run today |
|---|---|---|---|---|
| 4.0 | (N) pNFS (PROTO-2, `prd.md:84`) | none (no pNFS tests in the 4.0 suite) | — | — |
| 4.0 | (O) FSLOC | FSLOC1–8b (13) + GATT8 (tagged `fslocations`) = 14 | 1 (GATT8) | 13 |
| 4.0 | (P) ACL-1 (`prd.md:239`) | ACL0 ACL5 ACL10 (3) | 3 | 0 |
| 4.0 | (P) SEC-6 (`prd.md:273`) | GSS1–8, SEC7 (9) | 0 | 9 |
| 4.0 | (P) HA-5 reboot/reclaim (`prd.md:371`) | REBT1 2 3 5 6 7 8 10 11 (9) | 1 (REBT8) | 8 |
| 4.0 | (P) HA-5 delegations | DELEG1–22 family (32) | 25 | 7 (DELEG11 12 13a–e) |
| 4.0 | (M) | none | — | — |
| 4.0 | no class (in full set) | 622 | 594 | 28: CIDCF2 GATT11a–j LKU6c LOCK8c LOCK11g LOCK13a LOCK18 LOCK19 LOCK21 LOCK22 LOOKP5 LOOKP6 RD4 RD9g SATT1a SPOOF1–3 WRT10g |
| 4.1 | (N) pNFS | 27 flex + BLOCK1–4 + GETDINFO1 GETDLIST1 GETLAYOUT1 GETLAYOUT100 LAYOUTCOMMIT1 LAYOUTRET1–3 + CSID7 = 40 | 27 (flex, memfs) | 13 |
| 4.1 | (O) TRUNK | TRUNK1 TRUNK2 (2) | 0 | 2 |
| 4.1 | (P) HA-5 reboot/reclaim | REBT1 REBT2a–c REBT3a–c REBT4a–c REBT5 + RECC1–4 (15) | 4 (RECC1–4) | 11 |
| 4.1 | (P) HA-5 delegations | DELEG1–9 DELEG23–26 (13) | 12 | 1 (DELEG26) |
| 4.1 | (P) ACL-1, SEC-6 | no `acl` or `gss` codes in the 4.1 suite. SEC-6 coverage comes from the Kerberos tests | — | — |
| 4.1 | (M) SECNN | SECNN1–4 (4) | 0 | 4 |
| 4.1 | no class (in full set) | 181 | 173 | 8: CALLBACK1 COPY5 CSESS20 CSESS21 EID50 LKPP4 SEC1 SEC2 |
| 4.2 | (N) pNFS | same 40 | 0 | 40 |
| 4.2 | (O) TRUNK | 2 | 0 | 2 |
| 4.2 | (P) HA-5 reboot/reclaim | 15 | 4 | 11 |
| 4.2 | (P) HA-5 delegations | 13 | 12 | 1 |
| 4.2 | (M) SECNN | 4 | 0 | 4 |
| 4.2 | no class (in full set) | 195 | 188 | 7: CALLBACK1 CSESS20 CSESS21 EID50 LKPP4 SEC1 SEC2 |

**Spec-permitted tuning (PROPOSAL, tied to OQ-6).** The harness changes these values from what ships. Each is a tuning parameter the protocol leaves to the server, not a behaviour change. They are declared deviations under the category "spec-permitted tuning":

| Parameter | Harness | Shipped | Spec basis |
|---|---|---|---|
| NFSv4 lease (`nfs4_lease_time`) | 5 s default; 20/30/40 s per-code overrides; 90 s combined; 4 s cold_restart | 90 s | Lease length is the server's choice, reported to clients in the `lease_time` attribute (RFC 7530 §5.8.1.11 and §9.5; RFC 8881 §5.8.1.11 and §8.3) (from spec knowledge) |
| NFSv4 grace (`nfs4_grace_time`) | 10 s; 6 s cold_restart | 180 s | The grace period is server-chosen, and the RFCs expect it to be at least as long as the lease period (RFC 7530 §9.6.2; RFC 8881 §8.4.2). The strength of that requirement (MUST or SHOULD) has not been checked (from spec knowledge). The combined runs use a 10 s grace with a 90 s lease, so their grace is shorter than their lease. That combination is therefore not declared as spec-permitted tuning; it counts as a deviation to remove |
| Core threads | 4 | 16 (`src/daemon/chimera.json:18`) | Implementation tuning; no protocol significance |
| Sync delegation threads | 8 (the `delegation_threads` key the harness writes is unread, so the `server.c:243` default applies) | 1 (`src/daemon/chimera.json:5`) | Implementation tuning; no protocol significance |

Proposal: the **release-gate full run uses the shipped timers and thread count**. The **per-change gate may use the declared tuning above**, as part of OQ-6 (gate depth versus time).

Gap between today and the proposal:
- Bump the fork to upstream to get GATT11a–j, CALLBACK1, DELEG26 and the slot fixes.
- Set `PYNFS_WRITE_FULL_SIZES=1`.
- Read per-code JUnit (pass needs PASS, not WARN or NOTSUP).
- Drop the GitLab retry.
- Remove the undeclared configuration deviations listed in "pynfs harness configuration vs shipped defaults", above all diskfs `unsafe_async`.
- Today's exclusions sit in these classes: CSID7 (N); EID50 (SP4_SSV; its spec status is unresolved, see "Requirements and features"; no class); SEC7 (P, SEC-6); WRT18/SATT15 on linux/io_uring (no class; platform: kernel multigrain timestamps).

### pynfs result semantics

| Layer | Behaviour | Cite |
|---|---|---|
| Outcomes | `TEST_OMIT=3` ("Test skipped"), `TEST_FAIL=4`, `TEST_NOTSUP=5` ("Counts as WARN, but considered a failed dependency"), `TEST_WARN=6` ("Technically a PASS, but there was a better way"), `TEST_PASS=7` | `fork:nfs4.1/testmod.py:28-32` (identical in fork 4.0 `lib/testmod.py` and upstream) |
| OMIT sources | A test not selected by the run filter is OMIT ("Failed runfilter"). In 4.1/4.2 a test whose VERS range excludes `--minorversion` is also filtered out. With `--force`, a failed dependency does **not** turn into OMIT | `fork:nfs4.1/testmod.py:304` "t.result = t._omit_result"; `fork:nfs4.1/testserver.py:193` "if not (test.versions[0] <= options.minorversion <= test.versions[1]):" |
| JUnit (`--xml`) | Every test in the suite gets a `<testcase>`, selected or not (`tests = len(tests)`). Only FAIL becomes `<failure>`; every OMIT, including filter-OMIT, becomes `<skipped>`; PASS, WARN and NOTSUP become plain testcases; `errors` is hard-coded "0" | `fork:nfs4.1/testmod.py:518` "testsuite.setAttribute(\"tests\", str(len(tests)))"; :519 "testsuite.setAttribute(\"errors\", \"0\")"; :532 "if t.result == TEST_FAIL:"; :539 "elif t.result == TEST_OMIT:" |
| Runner exit code | Does not reflect test failures. 4.1 `main()` ends without `sys.exit`; 4.0 exits 3 only `if nfail < 0`, which never happens (nfail is a count). It is non-zero only on an exception (for example a failed init) | `fork:nfs4.1/testserver.py:364-371`; `fork:nfs4.0/testserver.py:388` "if nfail < 0:" |
| Chimera wrapper gate | A non-zero pynfs exit is propagated (124/137 is reported as a wall-clock timeout). Otherwise the JUnit `<testsuite>` must have `failures == 0` and `errors == 0`. A missing or unparsable XML fails | `scripts/pynfs_test_wrapper.sh:296` "if [ \"$PYNFS_EXIT\" -eq 124 ] \|\| [ \"$PYNFS_EXIT\" -eq 137 ]; then"; :310 "exit \"$PYNFS_EXIT\""; :323 "print(ts.get(\"failures\", \"1\"), ts.get(\"errors\", \"1\"))"; :328 "if [ \"${FAILURES:-1}\" != \"0\" ] \|\| [ \"${ERRORS:-1}\" != \"0\" ]; then"; :333 "exit 0" |
| Net effect | A ctest passes when pynfs reports no FAIL. WARN and NOTSUP (unsupported operation) pass. OMIT passes, so a run that selects nothing passes. Today's ctest results cannot tell PASS from WARN or NOTSUP | as above |
| flex wrapper | Same logic, but on `--json` `.failures` via `jq` | `scripts/pynfs_pnfs_test_wrapper.sh:161` "FAILURES=$(jq '.failures' \"$RESULTS_FILE\" 2>/dev/null)" |
| ctest | CI runs `ctest -C extended --output-on-failure --test-output-size-failed 65536 --timeout 300 -j 12` plus the selector (job 3490966 artifact log :142, "Executing ctest -C extended --output-on-failure"). Kerberos tests: SKIP 77 is "Skipped". Wrapper timeout per code 60 s by default (180 s for delegations, courteous, destroy_session and readdir); combined runs 900 s | `pynfs/CMakeLists.txt:394`; `scripts/pynfs_test_wrapper.sh:245` "PYNFS_TIMEOUT=\"${PYNFS_TIMEOUT:-60}\""; `pynfs/CMakeLists.txt:343` |
| CI | Judged by job exit status; `ctest` and `ctest_explore` retry once on `script_failure` | `.gitlab-ci.yml:260`, :368 |

### pynfs harness configuration vs shipped defaults

The wrapper writes its own `chimera.json` (`generate_config`, `scripts/pynfs_test_wrapper.sh:92-188`). The shipped defaults are those of `chimera_server_config_init()` in `src/server/server.c` and the diskfs mount parser.

| Setting | pynfs harness value | Shipped default | Cite |
|---|---|---|---|
| diskfs `unsafe_async` (writes without FUA/sync) | `true` on diskfs_io_uring and diskfs_aio | off ("Off by default"); FUA/sync on every write | `scripts/pynfs_test_wrapper.sh:134` "\"unsafe_async\":true,\"intent_log_size\":67108864"; `src/vfs/diskfs/diskfs_mount.c:1153`, :1157; effect at `diskfs_mount.c:550` "!io->shared->unsafe_async", `diskfs_clone.c:400` |
| diskfs `intent_log_size` | 64 MiB | 1 GiB | `scripts/pynfs_test_wrapper.sh:134`; `src/vfs/diskfs/space_map.h:240` "#define SM_INTENT_LOG_SIZE_DEFAULT (1ULL << 30)" |
| diskfs devices | 10 sparse 1 GiB image files in the build tree | deployment-specific | `scripts/pynfs_test_wrapper.sh:124` "truncate -s 1G \"$device_path\"" |
| `nfs4_lease_time` | 5 s by default. Per-code overrides: 30 s / 40 s (delegations), 40 s (DSESS9002/9003), 20 s (courteous). Combined runs 90 s. flex wrapper 5 s; Kerberos wrapper 5 s; cold_restart 4 s | 90 s | `scripts/pynfs_test_wrapper.sh:38`, :168; `pynfs/CMakeLists.txt:116`, :118, :151, :345; `scripts/pynfs_cold_restart_test_wrapper.sh:41` "LEASE_S=4"; `src/server/nfs/nfs4_lease.h:27` "#define NFS4_LEASE_TIME_DEFAULT_S    90"; `src/server/server.c:474` |
| `nfs4_grace_time` | 10 s (cold_restart 6 s) | 180 s | `scripts/pynfs_test_wrapper.sh:39`, :169; `scripts/pynfs_cold_restart_test_wrapper.sh:42` "GRACE_S=6"; `src/server/nfs/nfs4_lease.h:28` "#define NFS4_GRACE_TIME_DEFAULT_S    180"; `src/server/server.c:475` |
| `nfs4_delegations` | `true` for per-code `delegations`/`deleg` runs and DSESS9002/9003. **Derived: also `true` for all 18 combined runs.** The wrapper's match `*"delegations "*` / `*"deleg "*` fires on the trailing `nodelegations ` / `nodeleg ` deselect tokens. That contradicts the comment that the combined run "has delegations off". `false` for other per-code runs | off (`nfs4_delegations = 0`) | `scripts/pynfs_test_wrapper.sh:79` "DELEG_ENABLE=\"false\""; :86 "*\" deleg\"* \| *\" delegations\"* \| *DELEG* \| *\"delegations \"* \| *\"deleg \"*)"; :167; `pynfs/CMakeLists.txt:170`, :295 "does not leak into the combined run -- which has delegations off -- and"; `src/server/server.c:455` "config->nfs4_delegations = 0;" |
| REST debug fs-ops, REST auth | on whenever delegations are on (`rest_debug_fsops: true`, `rest_auth_enabled: false`, port 7780; `--serverhelper` passed to pynfs). By the derivation above, that includes every combined run | `rest_debug_fsops = 0`, `rest_auth_enabled = 1` | `scripts/pynfs_test_wrapper.sh:104` "\\\"rest_debug_fsops\\\": true,"; :105; :256 "SERVERHELPER_ARGS=(--serverhelper"; `src/server/server.c:250-251` |
| `threads` (core threads) | 4 (flex data server 2) | 16 (`src/daemon/chimera.json:18`) | `scripts/pynfs_test_wrapper.sh:165` "\"threads\": 4,"; `src/daemon/daemon.c:471`; `src/server/server.c:240` "config->core_threads             = 8;" |
| `delegation_threads` | 4, but no code reads this key (`grep '"delegation_threads"' src` = 0), so sync delegation threads stay at the `server.c:243` default of 8 | sync delegation threads 1 (`src/daemon/chimera.json:5`, applied at `src/daemon/daemon.c:525-534`) | `scripts/pynfs_test_wrapper.sh:166` |
| NFS Kerberos (`nfs_auth.kerberos_enabled`) | off in the main matrix (same as the default). On only in the 5 Kerberos tests | off | `src/server/server.c:364` "config->nfs_auth.kerberos_enabled            = 0;"; `scripts/nfs_kerberos_test_wrapper.sh:249` "\"kerberos_enabled\": true," |
| pNFS | on in the flex wrapper's MDS, plus a second daemon in `data_server` mode | off (`pnfs_enabled = 0`) | `scripts/pynfs_pnfs_test_wrapper.sh:88` "\"pnfs\": { \"enabled\": true,"; :70; `src/server/server.c:382` |
| KV store, 4.1 DRC | memkv everywhere except cold_restart (cairn KV, `nfs4_drc: true`) | DRC off | `pynfs/CMakeLists.txt:440` "Every other pynfs run uses memkv"; `scripts/pynfs_cold_restart_test_wrapper.sh:85-86`; `src/server/server.c:463` "config->nfs4_drc = 0;" |
| Export | one export `/share` with no `sec=`. The Kerberos tests set `NFS_EXPORT_SEC` | — | `scripts/pynfs_test_wrapper.sh:181-185` |

Effect on the "shipping configuration" claim: no pynfs row today runs against the shipped default. The diskfs rows prove nothing about durability (`unsafe_async`). The short lease and grace times shorten every expiry path. The combined runs exercise the server with delegations and the unauthenticated REST debug endpoint enabled.

## cthon

"cthon" refers to two different things in this tree. They share a name and nothing else.

| Name used here | What it is | Client | Registered by |
|---|---|---|---|
| **cthon-port** | A C re-implementation of the cthon04 basic, special and lock programs. It runs in-process against the Chimera POSIX client library, either directly on a VFS backend or through the in-process Chimera NFS client and server | Chimera's own client library, not a kernel client | `src/posix/tests/CMakeLists.txt:744-1029` |
| **cthon04-KVM** | The real cthon04 shell suite, run inside a QEMU guest against a Chimera daemon. The guest uses the Linux kernel NFS or CIFS client | Linux kernel `nfs` / `cifs` | `kvm/tests/CMakeLists.txt:107-128,367-394,723-758` |

### Upstream universe (cthon04 @ 86a4501)

Source: `git://git.linux-nfs.org/projects/steved/cthon04.git`, HEAD `86a4501a6e1e415844dc632894a85a5253cc1505` (2026-06-01). Every path in this subsection is a path in that clone.

| Category | Programs in tree | Run by `runtests` | Not run by `runtests` | Mode flag honoured? |
|---|---|---|---|---|
| basic | 14: `test1`–`test9` plus `test4a 5a 5b 7a 7b` (`basic/Makefile:9-10`) | 9: test1, 2, 3, 4 (4a if `CIFS=yes`), 5, 6, 7 (7a if `HARDLINKS=no`), 8, 9 (`basic/runtests:28-78`) | 5a, 5b, 7b are commented out (`basic/runtests:51-54,69-70`) | yes: `TESTARG=$1` (`basic/runtests:9-11`). `-f` sets count 1 and negates `-t` (`basic/test5.c:60`); test1/test2 shrink the tree to 2 levels × 2 files × 2 dirs (`basic/test1.c:140-144`) |
| general | 6 timed workloads: Small Compile, Tbl, Nroff, Large Compile, Four simultaneous large compiles, Makefile (`general/runtests.wrk:53-138`), plus a `stat` helper | all 6 | — | **no**: `general/runtests` never reads `$1`; it always sources `../tests.init` if present (`general/runtests:13-15`) |
| special | 20 (`special/Makefile:6-8`) | 15: op_unlk, op_ren, op_chmod, dupreq, excltest, negseek, rename, truncate, holey, nfsidem, rewind, telldir, freesp, bigfile, bigfile2 (`special/runtests.wrk:24-105`) | fstat ("protocol bug", commented, `special/runtests.wrk:50-53`); nstat, stat, stat2, touchn are never invoked. `CIFS=yes` skips op_ren (when `MNTOPTIONS` has `vers=2`/`vers=3`, :28-33) and nfsidem (:72-77) | **no**: the mode flag is not read (`special/runtests:16-18`) |
| lock | `tlock.c` built as `tlocklfs` and `tlock64` (`LOCKTESTS`, `tests.init:57`; `lock/Makefile:23-29`); 15 numbered tests (`lock/tlock.c:1580-1624`) | tests 1–7 and 11–15; test 8 only with `-r` | test 9 (mandatory locking) needs `-m`, which `runtests` never passes (`lock/tlock.c:1604`); test 10 runs only for CIFS (:1607) | yes, but remapped: `-t`→`-r` (rate test 8 on), `-f`→ no flag (`lock/runtests:34-37`) |

**How `runtests` reads its arguments** (`runtests:16-53`):
- Leading `-N <passes>` is consumed first (:16-25).
- After that, 1 argument sets the category; 2 set category and mode; 3 set category, mode and test path (:27-39).
- `./tests.init` is sourced only when 0 (or more than 3) arguments remain (:40-46). In that case `TESTARG="-t"` (`tests.init:11`).
- If the mode is still unset, it falls back to `-a` (:87-90). `-a` is not a mode any sub-script recognises.
- `$NFSTESTDIR` is deleted before each category and at the end (:103-106, :120).
- The sub-scripts load defaults on their own:
  - `lock/runtests` always sources `../tests.init` (:12), then overrides `TESTARG` with `$1` (:14-17).
  - `general` and `special` source it whenever it exists.

**cthon04 in the KVM image.**
- `kvm/CMakeLists.txt:31` pins `kvm-test-base` `v1.9.0`.
- In `github.com/chimera-nas/kvm-test-base`, commit `0d25de4f` sets `VERSION` to `v1.9.0`. Its `Dockerfile.kvm` clones `https://github.com/chimera-nas/cthon04.git` and checks out `CTHON04_COMMIT=13f4336cc95475d877d8f0376bbacda68a937711` ("Force -std=gnu89 in tests.init for GCC 14+ compatibility", 2026-06-16).
- The published artifact `ghcr.io/chimera-nas/kvm-test-base:ubuntu2604-v1.9.0-amd64` has manifest annotation `org.opencontainers.image.created: 2026-06-20T12:15:07Z`. That is after 0d25de4 (2026-06-20 11:54 UTC) and before the v1.10.0 commit. It carries no source-revision annotation, and the 3.5 GB rootfs was not downloaded. So the cthon04 commit is **derived from the image's build recipe, not read from the image**.
- `13f4336` shares no history with upstream 86a4501 (`git merge-base` is empty). Differences from upstream that affect results:
  - `general/runtests.wrk` drops every `set -e` around `./stat …`. A failing `stat` no longer fails the general category; only compile failures do (`|| eval $errexit`). Tbl and Nroff failures already only `cat` their logs upstream.
  - `basic/runtests` prints `TESTARG=…` in place of a blank line.
  - `lock/tlock.c` uses `signal()` instead of `sigaction()`.
  - `tests.init` is rewritten. `TESTARG="-t"` is unchanged, `LOCKTESTS` is still `tlocklfs tlock64`, and `CFLAGS` gains `-std=gnu89`.
  - `runtests`, `lock/runtests` and `special/runtests.wrk` are unchanged.

### Wiring and matrix

**cthon-port** (794 tests; `CHIMERA_LIMITS_TESTING` gate, `src/posix/tests/CMakeLists.txt:735`)

| Programs | Backends × path | Tests | Cite |
|---|---|---|---|
| 14 basic (`cthon_basic_1`…`_9`, `4a 5a 5b 7a 7b`) + 19 special = 33 | direct VFS, no protocol: memfs, diskfs_io_uring, diskfs_aio, cairn, linux, io_uring | 33 × 6 = 198 | `src/posix/tests/CMakeLists.txt:745-821` |
| same 33 | in-process **NFSv3** client → in-process server, same 6 backends | 198 | :866-913 |
| same 33 | in-process **NFSv4.1** client (`nfs4_*`) | 198 | :982-1029 |
| same 33 | in-process NFSv3 over TCP-RDMA (`nfs3rdma_*`) — **E10/PROTO-1, not counted for Q-SUC-2** | 198 | :924-971 |
| `cthon_lock_tlock` (tlock tests 1–5 only) | direct linux, io_uring only; never over NFS | 2 | :828-840; `src/posix/tests/cthon_lock_tlock.c:503-509` |
| **Total** | | **794** (596 without nfs3rdma) | |

- The port runs each program with only `-b <backend>` (:736, :878, :994). So it uses the default counts (`CTHON_DCOUNT` 10, `src/posix/tests/cthon_basic_5.c:40`), not functional mode (`-f` sets count 1, :102-104).
- "nfs4" means NFSv4.1. The client accepts only `vers=3|4` (`src/vfs/nfs/nfs.c:33-35`) and sends `minorversion = 1` (`src/vfs/nfs/nfs4_mount.c:394`).
- Compared with the upstream programs:
  - basic: 14 of 14 programs ported.
  - special: 19 of 20 ported. `freesp` is missing; the port adds the five programs upstream never runs.
  - general: not ported.
  - lock: 5 of 15 tlock tests.

**cthon04-KVM** (110 per image; KVM gate `CMakeLists.txt:698-761`)

| Axis | Values | Cite |
|---|---|---|
| Images | ubuntu2404, ubuntu2404_hwe, ubuntu2204_hwe, ubuntu2604 | `kvm/CMakeLists.txt:39-44` |
| Backends | memfs, linux, io_uring, diskfs_io_uring, diskfs_aio, + cairn | `kvm/tests/CMakeLists.txt:19-30`, :382-394 |
| NFS | versions 3, 4.0, 4.1, 4.2 × basic/general/special × 6 backends = **72** | :107, :110, :367-394 |
| NFS command | `cd /opt/cthon04 && ./runtests -b\|-g\|-s -f /mnt/test` | :111-113 |
| NFS mount | `vers=<v>,tcp,nconnect=16`; NFSv3 adds `nolock` unless `NFS_ENABLE_NLM` is set (no cthon test sets it) | `kvm/kvm_nfs_test_wrapper.sh:260-271` |
| SMB | general on 2.0.2, 2.1, 3.0, 3.0.2, 3.1.1 × 5 backends + cairn at 3.1.1; basic and special at 3.1.1 × 6 = **38** | `kvm/tests/CMakeLists.txt:128`, :723-758 |
| SMB command | `CIFS=yes MNTOPTIONS=vers=<d> ./runtests <flag> -f /mnt/test` | :737, :750 |
| SMB mount | `username=root,password=secret,vers=<d>,nobrl,modefromsid,cache=loose`; secmode `none` | `kvm/kvm_smb_test_wrapper.sh:298`; `kvm/tests/CMakeLists.txt:736` |
| Not wired | lock (`-l`), `-t`/timing mode, `-N` > 1, NFSv3 with NLM | — |

What the KVM commands execute, derived from the upstream scripts above:
- **NFS:**
  - basic runs 9 programs at count 1 with a reduced tree.
  - general runs 6 workloads; `-f` is ignored.
  - special runs all 15 programs; `-f` is ignored.
- **SMB** (`CIFS=yes`):
  - basic uses test4a instead of test4.
  - special skips op_ren on every dialect, because `vers=2.x`/`3.x` matches `vers=2|vers=3`. It also skips nfsidem.
- CI runs ctest with `--output-on-failure` (job 3490981 trace: `Executing ctest -C extended --output-on-failure --no-tests=error -L kvm -L "^ubuntu2604$" -E '/(nfstest|ltp)/' -j 12`). A passing test's guest output is not kept, so in-run sub-test skips are **not measured**.

### Server configuration used (harness deviations from shipped defaults)

| Setting | cthon-port harness | cthon04-KVM harness | Shipped default | Cite |
|---|---|---|---|---|
| diskfs `unsafe_async` (FUA/sync skipped on block writes) | `true` | `true` | off (`json_is_true` of an absent key) | `src/posix/tests/posix_test_common.h:589`; `kvm/kvm_nfs_test_wrapper.sh:165`; `kvm/kvm_smb_test_wrapper.sh:172`; `src/vfs/diskfs/diskfs_mount.c:1157`, effect :550 |
| diskfs `intent_log_size` | 64 MiB | 64 MiB | 1 GiB | `src/posix/tests/posix_test_common.h:593`; `kvm/kvm_nfs_test_wrapper.sh:165`; `src/vfs/diskfs/space_map.h:240` |
| core threads / sync delegation threads | 4 / 4 | 4 core; `"delegation_threads": 4` is not a key the daemon reads, so 8 sync delegation threads (`server.c:243` default) | 16 / 1 (`src/daemon/chimera.json:18`, `:5`) | `src/posix/tests/posix_test_common.h:659-660`; `kvm/kvm_nfs_test_wrapper.sh:196-197`; `src/daemon/daemon.c:471,505`; `src/server/server.c:240,243` |
| Transport | in-process (`CHIMERA_TCP_FLAVOR_INPROC`) except forking tests | TCP over a TAP device in a netns | plain TCP | `src/posix/tests/posix_test_common.h:674-677`; `src/server/server.c:252` |
| NFSv4 lease / grace | not set → default | not set → default | 90 s / 180 s | `src/server/server.c:474-475`; `src/server/nfs/nfs4_lease.h:27-28` |
| NFS Kerberos | not set → off | not set → off | off | `src/server/server.c:364` |
| SMB dialect floor | n/a | `smb_min_dialect` = the dialect under test | 2.0.2–3.1.1 offered | `kvm/kvm_smb_test_wrapper.sh:206`; `src/server/server.c:254-259` |
| SMB encryption / signing | n/a | secmode `none`; server not changed | off / optional | `kvm/kvm_smb_test_wrapper.sh:299-302`; `src/server/server.c:281,285` |

Consequences:
- Every cthon row on diskfs runs in a **non-durable** configuration: 198 cthon-port rows (excluding `nfs3rdma`) and 38 cthon04-KVM rows per image.
- The NFS-side protocol settings (lease, grace, Kerberos) are the shipped defaults.
- Client-side, NFSv3 KVM runs use `nolock`. SMB runs use `nobrl`, which keeps byte-range locks local to the client.

### CI evidence (pipeline 205408, scheduled, 2026-10-04, sha 3ea255e0)

The CI sha is an ancestor of d96e33a8. Between the two, `src/posix/tests/CMakeLists.txt` and `kvm/tests/CMakeLists.txt` differ only by `HAVE_LIBAIO` → `HAVE_LIBAIO_USABLE` on the same lines (`git diff 3ea255e0 d96e33a8`).

| Suite | Job (pipeline 205408) | Registered → result | Quoted summary line (artifact log) |
|---|---|---|---|
| cthon-port | 3490972 `ctest: [release, amd64, rest, …]` | 794 Passed, 0 skipped (198 of them `nfs3rdma`) | `ctest_release_rest_amd64.log:11210` "100% tests passed, 0 tests failed out of 5532" |
| cthon-port | 3490973 `ctest: [debug, amd64, rest, …]` | 794 Passed, 0 skipped | `ctest_debug_rest_amd64.log:11216` "100% tests passed, 0 tests failed out of 5532" |
| cthon04-KVM ubuntu2604 | 3490981 (Release), 3490982 (Debug) `ctest_kvm` | 110 Passed each (72 NFS, 38 SMB), 0 skipped | `ctest_kvm_Release_amd64_ubuntu2604.log:951` and `…Debug…:957` "100% tests passed, 0 tests failed out of 382" |
| cthon04-KVM ubuntu2404 | 3490985 / 3490986 `ctest_kvm_images` | 110 Passed each | `ctest_kvm_Release_amd64_ubuntu2404.log:951` / `…Debug…:957` "…out of 388" |
| cthon04-KVM ubuntu2404_hwe | 3490989 / 3490990 | 110 Passed each | `ctest_kvm_*_ubuntu2404_hwe.log:951/957` "…out of 388" |
| cthon04-KVM ubuntu2204_hwe | 3490991 / 3490992 | 110 Passed each | `ctest_kvm_*_ubuntu2204_hwe.log:951/957` "…out of 388" |

- Merge train 205343 (MR 114) shows the same:
  - jobs 3489337/3489338 (rest): 794 cthon-port Passed;
  - jobs 3489343/3489344 (`ctest_kvm` ubuntu2604): 110 Passed each.
- Every job above has `retry: max: 1` on `script_failure` (`.gitlab-ci.yml:260`, :553, :638).
- All are status `success` with one attempt in `jobs-205408.json`.
- Artifacts expire 2026-10-11 (205408) and 2026-10-09 (205343). Saved copies and their sha256 are in the evidence register.

### Run / skipped / excluded / missing

| Item | Status today | Reason | Cite |
|---|---|---|---|
| cthon-port basic + special, 6 backends × {direct, NFSv3, NFSv4.1} | **run** (596), all pass | in-process client; default counts | `src/posix/tests/CMakeLists.txt:745-1029`; job 3490972 |
| cthon-port `nfs3rdma` rows | run (198), **out of scope** | E10 / PROTO-1 | :924-971 |
| cthon-port lock | run on 2 direct backends only (tlock 1–5) | the forking test needs storage shared across processes; memfs is per-process | :823-840 |
| cthon-port over NFS, lock | **missing** | not registered | :828-840 |
| cthon04-KVM NFS basic/general/special, 3/4.0/4.1/4.2 × 6 backends × 4 images | **run** (72/image), all pass | kernel client, `-f` | `kvm/tests/CMakeLists.txt:367-394`; jobs 3490981–3490992 |
| cthon04-KVM SMB (general × 5 dialects; basic, special @3.1.1) | run (38/image), all pass | beyond SUC-2 (cthon is listed for NFS only) | :723-758 |
| cthon04-KVM timing mode (`-t`, upstream default counts) | **excluded** | `-f` hard-coded | :111-113 |
| cthon04-KVM lock (`-l`) | **missing** | category not in `CTHON_CATEGORIES` | :110 |
| NFSv3 with NLM | **missing** | `nolock` mount | `kvm/kvm_nfs_test_wrapper.sh:270` |
| SMB op_ren, nfsidem; test4 | skipped in-run | upstream `CIFS=yes` branches | upstream `special/runtests.wrk:28-33,72-77`, `basic/runtests:40-45` |
| tlock test 9 (mandatory), test 10 (CIFS) | not run in any upstream invocation over NFS | needs `-m` / CIFS | upstream `lock/tlock.c:1604-1609` |
| general category in the port | **missing** | not ported | `src/posix/tests/CMakeLists.txt:744-790` |
| special `freesp` in the port | **missing** | not ported | :763-783 |
| Durable diskfs configuration | **not tested** | harness sets `unsafe_async` | table above |

**Protocol coverage.**
- NFSv3:
  - in-process client;
  - kernel client with `nolock`.
- NFSv4.0: kernel client only.
- NFSv4.1:
  - in-process client;
  - kernel client.
- NFSv4.2: kernel client only. Note that the cthon programs issue no 4.2-only operations.
- SMB 2.0.2–3.1.1: kernel cifs client; general on every dialect, basic and special at 3.1.1 only.

### Proposed full run (PROPOSAL for Q-SUC-2)

The proposal is to run the real cthon04 over kernel clients, for NFSv3 (NLM on), 4.0, 4.1 and 4.2. These are the literal commands:

```
./runtests -N 1 -b -t /mnt/test
./runtests -N 1 -g -t /mnt/test
./runtests -N 1 -s -t /mnt/test
./runtests -N 1 -l -t /mnt/test     # lock/runtests:36 maps -t to -r
```

- Every argument is explicit, because the `tests.init` defaults apply only when no arguments are given.
- `-f` is used only for the merge-train gate (OQ-6).
- SMB is the operator's option (OQ-3).
- The port is supplementary, and `nfs3rdma` goes to E10.
- Pass criterion: exit 0 for each category. Corruption oracle: partial.

Facts the operator should read with this proposal (they do not change it):
- `-t` changes behaviour only in basic (counts) and lock (enables rate test 8). general and special ignore the flag.
- Even with `-l -t`, tlock test 9 (mandatory locking) is never run.
- The KVM image's cthon04 is the `chimera-nas` fork at 13f4336, not upstream 86a4501. In the fork, a failed `./stat` in general does not fail the category.

### Findings (candidate E1 children)

- cthon04-KVM runs in functional mode (`-f`), with no lock category and with NFSv3 `nolock`. See the Run table.
- The KVM image pins a cthon04 fork (13f4336) whose `general/runtests.wrk` has no `set -e` around `./stat`, which weakens the category's exit status. The universe for this inventory is upstream 86a4501.
- The KVM harness config contains `"delegation_threads": 4`, a key the daemon never reads. It has no effect.
- diskfs rows in both harnesses run with `unsafe_async: true` and prove nothing about durability.

## pjdfstest

### Port wiring and the 2578 count

- The port is a C re-implementation of upstream `.t` files: one program per file, sharing `src/posix/tests/pjd_common.h`. It runs through the Chimera POSIX client library, not through a kernel mount.
- A program passes when every `EXPECT` passed (`pjd_common.h:27`).
- No pjd source returns exit status 77, although ctest would treat 77 as a skip (`SKIP_RETURN_CODE 77`, `src/posix/tests/CMakeLists.txt:1567`, :1585).
- Gate: `CHIMERA_LIMITS_TESTING` (:1560, :1578).

| List | Programs | Backends per program | Tests | Cite |
|---|---|---|---|---|
| `PJD_TESTS` | 141 | 6 direct (memfs, cairn, diskfs_io_uring, diskfs_aio, linux, io_uring) + 12 NFS (`nfs3_*`, `nfs4_*` of the same 6) = 18 | 2538 | `src/posix/tests/CMakeLists.txt:1392`, :1594-1637 |
| `PJD_GRANULAR_TESTS` (`granular/00 03 04 05`) | 4 | memfs, cairn, diskfs_io_uring, diskfs_aio; **direct only**, because the NFS client cannot set `FATTR4_ACL` | 16 | :1536-1548, :1642-1655 |
| `PJD_LOCAL_TESTS` (`posix_fallocate/00`, `sparse/00`) | 2 | 6 direct + 6 `nfs4_*` (no NFSv3: no ALLOCATE) | 24 | :1659-1686 |
| **Total** | **147** | | **2578** (874 direct, 1704 over in-process NFS) | |

The "nfs4" pjd rows are NFSv4.1 (`src/vfs/nfs/nfs4_mount.c:394`).

**Label defect.**
- Tests are named `chimera/posix/pjd/<path>/<backend>` (:1562, :1580).
- The backend-label loop matches `_${_be}($|_)` (:1710-1711). That needs an underscore before the backend name, but direct pjd names have a `/` there.
- Replaying the loop with `cmake -P` over the simulated names gives:

| pjd tests | Backend label result | Count |
|---|---|---|
| over NFS (`nfs3_<be>`, `nfs4_<be>`) | correct | 1704 |
| direct memfs, cairn, diskfs_aio, linux, io_uring | **none** | 727 |
| direct diskfs_io_uring | **wrong** (`io_uring`) | 147 |

- The simulated name set equals the 2578 pjd names in job 3490972's log exactly (`helpers/pjd_sim.py` in the vault evidence directory; 0 missing, 0 extra).
- Two stale comments:
  - :1592 says "memfs and cairn pass 134/134; diskfs/passthrough/NFS are widened by follow-up PRs". Today there are 147 programs on every backend, including NFS.
  - `.github/workflows/extended.yml:827` says "pjd tests carry both labels". The `if/else` at :1705-1708 gives a pjd test `pjdfstest` and never `posix`.

### Upstream universe (pjd/pjdfstest @ 85a8aea)

- The tree does not pin upstream. Clone: `https://github.com/pjd/pjdfstest`, HEAD `85a8aea9e685999ef0540392fd80535f873d7ff7` (2026-08-07).
- 238 `.t` files in 17 categories.
- Plans were evaluated under Linux semantics by `helpers/pjd_plan.sh` in the vault evidence directory. It runs each `.t` up to its first `1..N` line, using upstream's own `supported`/`require`/`quick_exit`, with `os=Linux`, `fs=FUSE.CHIMERA`.
- On a non-FreeBSD OS, upstream turns a whole file into `1..1` / `ok 1` (`tests/misc.sh:164-168`) when any of these hold:
  - `require chflags` (misc.sh:179-187);
  - `require posix_fallocate` (:200-206);
  - the file is gated on `FreeBSD:UFS`, `FreeBSD:ZFS` or `FreeBSD`;
  - `nfsv4acls`, which always returns 1 off FreeBSD (:309-312).

| Category | Upstream `.t` | Runnable on Linux | Linux TAP plan sum (runnable files) | Ported | Ported ∩ runnable | Ported, but upstream quick_exits on Linux | Runnable, not ported |
|---|---|---|---|---|---|---|---|
| chflags | 14 | 0 | 0 | 0 | 0 | 0 | — |
| chmod | 13 | 11 | 325 | 9 | 9 | 0 | 10, 11 |
| chown | 11 | 9 | 1495 | 8 | 8 | 0 | 10 |
| ftruncate | 15 | 12 | 86 | 2 | 2 | 0 | 01–07, 09, 12, 14 |
| granular | 7 | 0 | 0 | 4 | 0 | 4 (00 03 04 05) | — |
| link | 18 | 12 | 353 | 11 | 11 | 0 | 17 |
| mkdir | 13 | 10 | 115 | 9 | 9 | 0 | 12 |
| mkfifo | 13 | 10 | 117 | 9 | 9 | 0 | 12 |
| mknod | 12 | 11 | 185 | 10 | 10 | 0 | 10 |
| open | 27 | 18 | 328 | 15 | 14 | 1 (13) | 17, 21, 24, 25 |
| posix_fallocate | 1 | 0 | 0 | 1 | 0 | 1 (00) | — |
| rename | 25 | 19 | 4851 | 18 | 17 | 1 (22) | 17, 23 |
| rmdir | 16 | 12 | 141 | 11 | 11 | 0 | 15 |
| symlink | 13 | 10 | 92 | 9 | 9 | 0 | 12 |
| truncate | 15 | 12 | 81 | 10 | 10 | 0 | 12, 14 |
| unlink | 15 | 12 | 437 | 11 | 11 | 0 | 13 |
| utimensat | 10 | 9 | 121 | 9 | 9 | 0 | — |
| `sparse` (port only) | 0 | — | — | 1 | — | — | — |
| **Total** | **238** | **167** | **8727** | **147** | **139** | **7** | **28** |

How the 71 Linux quick-exits break down by gate:

| Gate | Files |
|---|---|
| `require chflags` (incl. `chflags_SF_SNAPSHOT`, and chflags gates on 21 files in 13 other categories) | 35 |
| `FreeBSD:UFS` only | 18 |
| `FreeBSD` only | 8 |
| `FreeBSD:ZFS` | 5 |
| `nfsv4acls` | 2 |
| `posix_fallocate` | 1 |
| `stat_st_birthtime` | 1 |
| `rename_ctime` | 1 |

- The `rename_ctime` file (rename/22) runs when `fs` is EXT4, UFS or ZFS (:208-224). The count is fs-dependent for that file only.
- Over every Linux file the plan sum is 8798, counting each quick-exit as 1.
- "Runnable" means the file is not a whole-file quick-exit. In-file `supported` branches (for example `lchmod` in chmod/00) still trim assertions; the plan sums already reflect that.

### Whole-file skips on Linux (71 files)

A whole-file skip prints `1..1` / `ok 1` and exits 0 (`tests/misc.sh:164-168`). A TAP harness would read that as a pass. **This document counts every whole-file skip as a SKIP, never a pass.** Classified at 85a8aea from each file's gate and its `desc=` line:

- **44 files test behaviour Linux or FUSE cannot provide:** BSD file flags via chflags(2), FreeBSD ZFS NFSv4 ACLs, `st_birthtime`, and BSD-only `O_SHLOCK`/`O_EXLOCK`.
- **27 files test behaviour a Linux FUSE mount can provide.** Their gate is FreeBSD-specific setup tooling (`mdconfig`, `newfs`, `mount -u`) or a capability list that does not recognise Linux or the Chimera FUSE fs type. These are EROFS, ENOSPC, EMLINK, EXDEV, EBUSY and ETXTBSY errno checks, plus posix_fallocate/00 and rename/22.
- **Read-only and full filesystems are real NAS conditions**, so the 27 are in the full run as "would need X", where X is a Linux fixture. They are not platform exclusions.

#### Not runnable on Linux (44 files)

| File | Upstream gate | Reason |
|---|---|---|
| `chflags/00.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/01.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/02.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/03.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/04.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/05.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/06.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/07.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/09.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/10.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/11.t` | `require chflags_SF_SNAPSHOT;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/12.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chflags/13.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chmod/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `chown/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `ftruncate/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `granular/00.t` | `[ "${os}:${fs}" = "FreeBSD:ZFS" ] || quick_exit;` | FreeBSD ZFS NFSv4-ACL semantics; no Linux or FUSE interface sets NFSv4 ACLs |
| `granular/01.t` | `[ "${os}:${fs}" = "FreeBSD:ZFS" ] || quick_exit;` | FreeBSD ZFS NFSv4-ACL semantics; no Linux or FUSE interface sets NFSv4 ACLs |
| `granular/02.t` | `[ "${os}:${fs}" = "FreeBSD:ZFS" ] || quick_exit;` | FreeBSD ZFS NFSv4-ACL semantics; no Linux or FUSE interface sets NFSv4 ACLs |
| `granular/03.t` | `[ "${os}:${fs}" = "FreeBSD:ZFS" ] || quick_exit;` | FreeBSD ZFS NFSv4-ACL semantics; no Linux or FUSE interface sets NFSv4 ACLs |
| `granular/04.t` | `nfsv4acls || quick_exit;` | NFSv4 ACL tooling; `nfsv4acls` returns 1 off FreeBSD (`tests/misc.sh:309-312`) |
| `granular/05.t` | `[ "${os}:${fs}" = "FreeBSD:ZFS" ] || quick_exit;` | FreeBSD ZFS NFSv4-ACL semantics; no Linux or FUSE interface sets NFSv4 ACLs |
| `granular/06.t` | `nfsv4acls || quick_exit;	quick_exit;` | NFSv4 ACL tooling; `nfsv4acls` returns 1 off FreeBSD (`tests/misc.sh:309-312`) |
| `link/12.t` | `require chflags;require link;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `link/13.t` | `require chflags;require link;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `mkdir/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `mkfifo/10.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `mknod/09.t` | `require mknod;require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `open/09.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `open/10.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `open/11.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `open/13.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `open/18.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;` | tests `O_SHLOCK`/`O_EXLOCK` open flags, which exist only on BSD |
| `rename/06.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `rename/07.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `rename/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `rmdir/09.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `rmdir/10.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `symlink/09.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `truncate/08.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `unlink/09.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `unlink/10.t` | `require chflags;` | BSD file flags via chflags(2), which Linux lacks; upstream `supported chflags` is false off FreeBSD (`tests/misc.sh:179-187`) |
| `utimensat/03.t` | `require "utimensat";require stat_st_birthtime;` | needs `st_birthtime` in `struct stat`, which Linux lacks (`tests/misc.sh:228-234`) |

#### Skipped on Linux, would need X (27 files)

| File | Upstream gate | What it tests, and why it skips | Would need (X) |
|---|---|---|---|
| `chmod/09.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "chmod returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `chown/09.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "chown returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `ftruncate/10.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "truncate returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `ftruncate/11.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;noexec && quick_exit;` | ETXTBSY: "truncate returns ETXTBSY the file is a pure procedure (shared text) file that is being exe". Standard POSIX behaviour; gated on FreeBSD only | an executable on the Chimera mount that the test runs; the result depends on the client kernel's deny-write-on-exec behaviour |
| `link/05.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;require link;` | EMLINK: "link returns EMLINK if the link count of the file named by name1 would exceed {PC_LINK_MAX". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a backend whose link-count limit `{PC_LINK_MAX}` is reachable by the test |
| `link/14.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;require link;` | EXDEV: "link returns EXDEV if the source and the destination files are on different file systems". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a second Chimera mount on the same client, to cross |
| `link/15.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;require link;` | ENOSPC: "link returns ENOSPC if the directory in which the entry for the new link is being placed c". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount on a small or inode-limited backend that the test can fill |
| `link/16.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;require link;` | EROFS: "link returns EROFS if the requested link requires writing in a directory on a read-only fi". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `mkdir/09.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "mkdir returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `mkdir/11.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | ENOSPC: "mkdir returns ENOSPC if there are no free inodes on the file system on which the directory". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount on a small or inode-limited backend that the test can fill |
| `mkfifo/08.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "mkfifo returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `mkfifo/11.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | ENOSPC: "mkfifo returns ENOSPC if there are no free inodes on the file system on which the file is ". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount on a small or inode-limited backend that the test can fill |
| `open/14.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "open returns EROFS if the named file resides on a read-only file system, and the file is t". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `open/15.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "open returns EROFS when O_CREAT is specified and the named file would reside on a read-onl". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `open/19.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | ENOSPC: "open returns ENOSPC when O_CREAT is specified, the file does not exist, and there are no f". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount on a small or inode-limited backend that the test can fill |
| `open/20.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;noexec && quick_exit;` | ETXTBSY: "open returns ETXTBSY when the file is a pure procedure (shared text) file that is being ex". Standard POSIX behaviour; gated on FreeBSD only | an executable on the Chimera mount that the test runs; the result depends on the client kernel's deny-write-on-exec behaviour |
| `posix_fallocate/00.t` | `require posix_fallocate;` | Linux has posix_fallocate(3), but upstream `supported posix_fallocate` accepts only FreeBSD and not FUSEFS (`tests/misc.sh:200-206`) | a pjdfstest `supported()` that accepts Linux and the Chimera FUSE fs type (a fork patch), or running the in-tree port's posix_fallocate/00 |
| `rename/15.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;` | EXDEV: "rename returns EXDEV if the link named by 'to' and the file named by 'from' are on differe". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a second Chimera mount on the same client, to cross |
| `rename/16.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;` | EROFS: "rename returns EROFS if the requested link requires writing in a directory on a read-only ". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `rename/22.t` | `require rename_ctime;` | POSIX leaves ctime-on-rename optional; upstream runs it only for listed fs types (`tests/misc.sh:208-224`) | pjdfstest to recognise the Chimera FUSE fs type, and a product decision that Chimera updates ctime on rename |
| `rmdir/13.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;` | EBUSY: "rmdir returns EBUSY if the directory to be removed is the mount point for a mounted file s". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a second mount mounted on a directory inside the Chimera mount |
| `rmdir/14.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "rmdir returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `symlink/10.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "symlink returns EROFS if the file name2 would reside on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `symlink/11.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | ENOSPC: "symlink returns ENOSPC if there are no free inodes on the file system on which the symboli". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount on a small or inode-limited backend that the test can fill |
| `truncate/10.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "truncate returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |
| `truncate/11.t` | `[ "${os}" = "FreeBSD" ] || quick_exit;noexec && quick_exit;` | ETXTBSY: "truncate returns ETXTBSY the file is a pure procedure (shared text) file that is being exe". Standard POSIX behaviour; gated on FreeBSD only | an executable on the Chimera mount that the test runs; the result depends on the client kernel's deny-write-on-exec behaviour |
| `unlink/12.t` | `[ "${os}:${fs}" = "FreeBSD:UFS" ] || quick_exit;` | EROFS: "unlink returns EROFS if the named file resides on a read-only file system". Standard POSIX behaviour; only the fixture (`mdconfig`, `newfs`, `mount -u`, or a FreeBSD exec setup) is FreeBSD-specific | a Chimera mount that can be remounted (or exported) read-only on the Linux test host |

### CI evidence (pipeline 205408)

| Job | pjd tests | Quoted line |
|---|---|---|
| 3490972 `ctest: [release, amd64, rest, -LE '^pynfs$\|^smbtorture$\|^wpts$\|^kvm$']` | 2578 Passed, 0 skipped, 0 failed | `ctest_release_rest_amd64.log:11235` "pjdfstest          = 1130.91 sec*proc (2578 tests)"; :11210 "100% tests passed, 0 tests failed out of 5532" |
| 3490973 `ctest: [debug, …, rest, …]` | 2578 Passed, 0 skipped | `ctest_debug_rest_amd64.log:11241` "pjdfstest          = 1612.15 sec*proc (2578 tests)"; :11216 "100% tests passed, 0 tests failed out of 5532" |

Merge train 205343, jobs 3489337/3489338: 2578 Passed each.

### No FUSE path

- SUC-2 asks for "pjdfstest for POSIX behaviour through FUSE" (`docs/product/prd.md:394-395`).
- The only POSIX-semantics test over FUSE is `fuse_posix_test`, a hand-written errno test on memfs only (`src/server/fuse/tests/CMakeLists.txt:20`, :46-99).
- In job 3490972 it ran as `chimera/fuse/posix_memfs` and `posix_directio_memfs`, both Passed; `posix_uring_memfs` was Skipped.
- No pjdfstest program, port or upstream, runs over a FUSE mount.
- NFSv4 ACLs cannot be tested through FUSE on Linux. Upstream `granular` runs only on `FreeBSD:ZFS` or with `nfsv4acls` (`tests/granular/00.t:9`; `tests/misc.sh:309-312`).
- The in-tree granular port drives the VFS directly through `chimera_posix_{get,set}acl` (`src/posix/tests/CMakeLists.txt:1536-1543`). It covers ACL-1 at the VFS level only, with no protocol and no FUSE.

### Server configuration used

The pjd port uses the same posix harness as cthon-port: diskfs `unsafe_async: true` with a 64 MiB intent log, 4/4 threads, the in-process transport, and default lease, grace and Kerberos. See the cthon table above (`src/posix/tests/posix_test_common.h:589-593,659-660`). The 862 diskfs pjd rows are therefore non-durable runs.

### Run / skipped / excluded / missing

| Item | Status | Reason | Cite |
|---|---|---|---|
| 147 port programs × backends (2578) | **run**, all pass | in-process client | `src/posix/tests/CMakeLists.txt:1392-1686`; job 3490972 |
| 28 Linux-runnable upstream files | **missing** from port | not ported | comparison table |
| 44 upstream files | **not runnable on Linux**; upstream prints `1..1`/`ok 1`, which this document counts as **SKIP, never a pass** | non-Linux behaviour (see "Whole-file skips on Linux") | `tests/misc.sh:164-168` |
| 27 upstream files (25 FreeBSD-fixture errno files, posix_fallocate/00, rename/22) | **skipped, would need X** (counted as SKIP) | Linux fixture or gate change named per file in "Whole-file skips on Linux" | `tests/misc.sh:164-168`, `:200-224` |
| 7 port programs whose upstream file quick-exits on Linux (granular ×4, open/13, posix_fallocate/00, rename/22) | run in the port anyway | port ignores upstream gates | comparison table |
| pjdfstest over FUSE | **missing** | no wiring | `src/server/fuse/tests/CMakeLists.txt` |
| granular over NFS | **excluded** | NFS client cannot set `FATTR4_ACL` | `src/posix/tests/CMakeLists.txt:1541-1542` |
| Backend labels on 874 direct pjd tests | defect (727 none, 147 wrong) | label regex | :1710-1711 |
| Durable diskfs | not tested | `unsafe_async` | `src/posix/tests/posix_test_common.h:589` |

### Proposed full run (PROPOSAL for Q-SUC-2)

- Selection: upstream `tests/**/*.t` at a pinned commit, over a FUSE mount, run as root (D4).
- **Not runnable on Linux:** the 44 files listed under "Whole-file skips on Linux", each with its reason. They include all of `chflags`, all of `granular`, and open/18.
- **Skipped, would need X:** 27 files. Each stays in the selection and counts as SKIP until its fixture or gate change exists:
  - 25 errno checks need a Linux fixture: a read-only remount, a filled or inode-exhausted backend, a second mount, or an exec'd file;
  - posix_fallocate/00 and rename/22 need a gate change.
- ACL-1 (P) is covered by pynfs ACL\*, smbtorture/WPTS ACL and the in-process granular port. Gap: no NFSv4-ACL test through FUSE.
- **Pass criterion:** TAP `ok` for every assertion of every file that runs. A whole-file skip (`1..1` from `quick_exit`) is a SKIP, never a pass. A SKIP fails the run unless the file is on the not-runnable list.
- **Corruption oracle:** none. pjdfstest checks metadata and errno, not data. This remains a gap.

### Findings (candidate E1 children)

- The pjd backend-label regex misses 727 direct tests and mislabels 147 (`:1710-1711`).
- The comment at `:1592` is stale, and so is `extended.yml:827`.
- pjdfstest is not pinned upstream and does not run over FUSE (see "No FUSE path").
- The port lacks 28 Linux-runnable upstream files, including ftruncate 01–07, 09, 12, 14.
- diskfs pjd rows run with `unsafe_async: true`.

## smbtorture

smbtorture is Samba's protocol test client. Chimera runs it, unmodified, against an in-process Chimera server: one server process per ctest entry, inside a private network namespace. Each run uses a test profile, and that profile differs from the shipped SMB defaults. The overrides are listed under "Pass semantics today".

### Source, pin and universe

| Item | Value | Evidence |
|---|---|---|
| Package source | `samba-testsuite` from the distro mirror, installed **without a version pin** | `builders/dev/Dockerfile:52` "samba-vfs-modules smbclient samba-testsuite" |
| In-tree version note | "Samba 4.23" (a comment only) | `src/server/smb/tests/CMakeLists.txt:675` "Samba 4.23 devcontainer image" |
| CI image for pinned pipeline 205408 | `repository.mdh.quantum.com/chimera/chimera-dev:ci-chimera-442` (pipeline iid 442) | job 3490948 (pipeline 205408, `chimera-dev`), trace line 97: "output:    repository.mdh.quantum.com/chimera/chimera-dev:ci-chimera-442". The smbtorture legs ran in the same image: job 3490968 artifact `ctest_release_smbtorture_amd64.log` line 11, "image:     repository.mdh.quantum.com/chimera/chimera-dev:ci-chimera-442" |
| Image digest (pulled 2026-10-04, unprivileged) | index `sha256:b6ce8991f5ca10997ee563fe5c3a5ebf13eb0310b7358a1537c4e39513590a1d`; amd64 manifest `sha256:8fe0a876c445b22bdc843e54d641f1534f4f2547d13c9a31b8aaf4b83f216556`; created 2026-10-04 07:36:54 UTC | `skopeo inspect --raw … \| sha256sum`; `podman image inspect --format '{{.Digest}}'` |
| Samba version in that image | `samba-testsuite 2:4.23.6+dfsg-1ubuntu2`; `smbtorture --version` prints "Version 4.23.6-Ubuntu-4.23.6+dfsg-1ubuntu2" | `podman run --rm --network=none … dpkg-query -W samba-testsuite; smbtorture --version` |
| Universe | 2978 `smbtorture --list` entries in 25 top-level families (table below) | `smbtorture --list` output saved with the evidence as `smbtorture-list-ci-chimera-442.txt` (2979 lines, including the version header; sha256 `f776370d…aa5cd4`) |
| `smb2.*` catalog | 655 subtests. This set is identical to the image's `--list` (using regen.sh's de-duplication), to CI's `measured-catalog.txt` (655 lines, job 3490980), and to `SMBTORTURE_SUITES` | `tools/smbtorture/regen.sh:179` "smbtorture --list 2>&1 \| grep -E '^smb2\.'"; job 3490980 (pipeline 205408) trace line 231 "catalog: 655 subtests"; `src/server/smb/tests/CMakeLists.txt:682` "set(SMBTORTURE_SUITES" |

**Universe by top-level family.** Counts are raw `--list` lines at 4.23.6. For `smb2` one line is one subtest. Some other families list nested names.

| Family | Entries | Wired today |
|---|---|---|
| local | 684 | no |
| smb2 | 655 | yes: 655 × 6 backends |
| rpc | 561 | 6 entries: 5 gated, 1 disabled |
| krb5 | 522 | no |
| raw | 195 | no |
| base | 121 | no |
| vfs | 44 | no |
| winbind | 43 | no |
| net | 32 | no |
| rap | 24 | no |
| drs | 22 | no |
| libsmbclient | 15 | no |
| nbt | 14 | no |
| dlz_bind9 | 13 | no |
| ldap | 11 | no |
| dfs | 7 | no |
| netapi | 4 | no |
| unix, notifyd, dns_internal | 2 each | no |
| ntp, gpo, echo, dsdb, bench | 1 each | no |
| **Total** | **2978** | |

### Wiring

| Aspect | Fact | Cite |
|---|---|---|
| Registration gate | The `smb2.*` matrix and the race tests register only if netns testing is on **and** an `smbtorture` binary is found | `src/server/smb/tests/CMakeLists.txt:5239` "if(CHIMERA_NETNS_TESTING AND SMBTORTURE_EXECUTABLE)" |
| rpc tests | 5 enabled tests (`rpc.lsa.lookupsids`, `rpc.lsa.lookupnames`, `rpc.handles.lsarpc`, `rpc.srvsvc.srvsvc (admin access).NetShareEnumAll`, `rpc.wkssvc.wkssvc.NetWkstaGetInfo`) and 1 `DISABLED` test (`rpc.lsalookup`, "no trusts"). All run on memfs only, with label `smbtorture_rpc` and a 300 s timeout. They sit outside the smbtorture gate: `chimera_add_netns_test` always registers them | `:628` "set(SMBTORTURE_RPC_SUITES_ENABLED"; `:650` "set(SMBTORTURE_RPC_SUITES_DISABLED"; `:642` "LABELS \"smbtorture_rpc\""; `:661` "DISABLED TRUE" |
| Pipes the server serves | lsarpc, srvsvc, samr and wkssvc; any other pipe name gets OBJECT_NAME_NOT_FOUND | `src/server/smb/smb_proc_create.c:5755` "\"lsarpc\""; `:5761` "\"samr\"" |
| Backends | memfs, linux, diskfs_io_uring always; diskfs_aio, cairn and io_uring if their probes pass | `src/server/smb/tests/CMakeLists.txt:5291` "add_smbtorture_backend_tests(memfs)"; `:5294` "if(HAVE_LIBAIO_USABLE)" |
| Matrix | 655 subtests × 6 backends = 3930 cells | parser replay of `:682-4939` |
| Hand-disabled | 11 subtests, removed from every backend; all 11 also sit on every backend's allowlist | `:5039` "set(SMBTORTURE_DISABLED_SUBTESTS" |
| Never allowlisted | 74 subtests are on no backend's allowlist. 74 + 11 = 85 subtests that are gated nowhere | parser replay |
| Isolate suites | 7 parents run one daemon per subtest: `smb2.durable-open`, `durable-v2-delay`, `durable-v2-open`, `lease`, `oplock`, `replay`, `session`. Every other parent runs consolidated, one daemon per suite | `:4974` "set(SMBTORTURE_ISOLATE_SUITES" |
| ADS split | Named streams are turned on per server process for the subtests matching the 18 patterns in `smbtorture_ads_subtests.txt` (31 catalog subtests). A suite that mixes ADS and non-ADS subtests is split. Only memfs gates any ADS subtest (22). Its mixed parents are `smb2.fileid` and `smb2.kernel-oplocks`, which gives 4 split-out singleton entries | `:559` "smbtorture_ads_subtests.txt"; `:5173` "silently couples"; `smbtorture_test.c:609` "chimera_server_config_set_smb_named_streams(config, 1);" |
| Labels | `smbtorture` (gating, `:5111`/`:5125`); `smbtorture_explore` (opt-in, `SMBTORTURE_EXPLORE_TESTS` defaults to OFF, `:4955`/`:5149`); `smbtorture_rpc`. The 5 `*_disconnect_race_memfs` and 2 `*_prenotify_race_memfs` tests carry **no label**, so they run in the `rest` leg | `:5254` "foreach(_sub smb2.durable-open.lease"; `:5278` "foreach(_sub smb2.durable-open.open2-lease"; job 3490972 (pipeline 205408) artifact lines "smbtorture_smb2_durable_open_open2_lease_prenotify_race_memfs … Passed" |
| Timeouts | Consolidated suite: 600 s; `smb2.maxfid`: 1200 s; per-subtest, explore, race and rpc tests: 300 s. Explore cells use 300 s (1200 s for maxfid) | `:5105` "set(_timeout 600)"; `:5107` "set(_timeout 1200)"; `:5128` "TIMEOUT 300)"; `tools/smbtorture/regen.sh:269` "CELL_TIMEOUT=300" |
| RESOURCE_LOCK | `smb_durable_oplock_family` serializes `smb2.durable-(v2-)open.*` subtests and the 7 race tests against each other | `:5131` "durable-(v2-)?open"; `:5133` "RESOURCE_LOCK smb_durable_oplock_family)" |
| Exit 77 | The driver returns 77 only when `smbtorture` is missing from PATH. Otherwise it returns smbtorture's own exit status | `smbtorture_test.c:515` "return 77;"; `:388` "return WEXITSTATUS(rc);" |

**Allowlists per backend.** Gated = allowlisted minus the 11 hand-disabled.

| Backend | Allowlist starts | Allowlisted | Gated | Not gated | ctest entries |
|---|---|---|---|---|---|
| memfs | `:1407` | 578 | 567 | 88 | 308 |
| linux | `:2057` | 491 | 480 | 175 | 282 |
| io_uring | `:2600` | 493 | 482 | 173 | 283 |
| cairn | `:3145` | 539 | 528 | 127 | 290 |
| diskfs_io_uring | `:3740` | 545 | 534 | 121 | 289 |
| diskfs_aio | `:4341` | 542 | 531 | 124 | 289 |
| **Total** | | 3188 | **3122** | 808 | **1741** |

The replay gives 1741 ctest entries, which matches CI's "out of 1741" (below).

**Hand-disabled (11).** Reasons as recorded at `:5005-5071`. The explore column gives the per-backend outcome in job 3490980, in backend order memfs/linux/io_uring/cairn/diskfs_io_uring/diskfs_aio (P = pass, F = fail).

| Subtest | Recorded reason | Explore 3490980 |
|---|---|---|
| `smb2.compound_async.rename_last` | Client-side timing assumption under CI load; "see #733" (`:5016`) | PPPPPP |
| `smb2.ioctl.sparse_punch`, `sparse_hole_dealloc` | SET_ZERO_DATA must deallocate; project_smb_ioctl_sparse (`:5023`) | FFFFFF |
| `smb2.ioctl.sparse_lock` | Needs a range-lock conflict probe | FFFFFF |
| `smb2.ioctl.sparse_perms` | Residual access-check case | FFFFFF |
| `smb2.ioctl.sparse_qar_malformed` | Input validation | FFFFFF |
| `smb2.ioctl.sparse_qar_truncated` | Partial-emit path incomplete | PPPPPP |
| `smb2.ioctl.dup_extents_sparse_dest` | Destination-EOF guard | FFFPFF |
| `smb2.oplock.levelii500` | The Samba test contradicts MS-SMB2 3.3.5.22.1 (`:5050`) | FFFFFF |
| `smb2.lease.breaking3`, `v2_breaking3` | Assert Samba's step-per-ack choreography rather than MS-SMB2 3.3.4.7 (`:5057`) | FFFFFF |

### Pass semantics today

A ctest entry passes when smbtorture exits 0. smbtorture exits 0 when a subtest **skips**, so a self-skipped subtest shows as ctest `Passed`. The JUnit converter writes skips as `<skipped/>` (`scripts/smbtorture_to_junit.py:72`), but GitLab judges the job by exit code only. The explore runner classifies by exit code too (`tools/smbtorture/regen.sh:284-286`: 0 → PASS, 77 → SKIP, 124/137 → TIME).

### smbtorture client options

Every client invocation carries these options (`src/server/smb/tests/smbtorture_test.c:229-334`).

| Option | Cite | Effect today |
|---|---|---|
| `-U myuser%mypassword` | `:279` | No mechanism flag and no `-m`. The auth mechanism is not pinned and there is no dialect axis |
| `--option=torture:samba3=yes` | `:280` | Subtests that branch on the target expect Samba behaviour |
| `--option=torture:resume_key_support=%s` | `:281`, `:234` | `yes` on memfs only. Per the in-tree comment (`:231`), "resume_key_support=no smbtorture self-skips the whole copy_chunk family". 20–21 copy_chunk subtests are allowlisted on each non-memfs backend (103 cells), so they show as Passed while skipping |
| `filename=torture_test_file`, `offset=0`, `beyond_final_zero=4096` | `:288-290` | Parameters that set-sparse-ioctl and zero-data-ioctl abort without |
| `acl_xattr_name=user.NTACL` | `:291` | `ea.acl_xattr` targets a Samba-specific xattr |
| `use_iptables=yes` | `:300` | Simulates an unresponsive transport with iptables inside the per-test netns |
| `open_brlock_deadlock_timemout=2` | `:304` | Without it, `smb2.lock.open-brlock-deadlock` self-skips |
| `oplocktimeout=30` | `:310` | The batch22a/b break-timeout window is bracketed at 30 s, not the 35 s Windows default |
| `client smb3 signing algorithms=%s` | `:314`, `:250-257` | GMAC+CMAC+HMAC when the test name contains `session.signing`, `session.encryption`, or `bind_negative_smb3s…toG`; CMAC only otherwise (see "Encryption and signing subtests" below) |
| `--seed=4242` | `:325` | Every randomized input is fixed (cited cause: `smb2.replay.channel-sequence` drawing 0x7fff, which is legal per MS-SMB2 3.3.5.2.10) |
| `--fullname` | `:326` | Subtest names are reported in full |
| `localdir=<session dir>` (linux and io_uring only) | `:334` | Lets `samba3misc` take a POSIX lock on the share path |

**Encryption and signing subtests.**
- These subtests are allowlisted and gated on **all 6 backends**: `smb2.session.encryption-aes-{128,256}-{ccm,gcm}` (4), `smb2.session.signing-{aes-128-cmac,aes-128-gmac,hmac-sha-256}` (3), `anon-encryption1-3` (3), `bind_negative_smb3encGtoC{s,d}` (2) and `session-require-signing.bug15397` (1).
- The 4 `session.encryption-*` subtests passed in ctest. Example: job 3490968 artifact line 696, "smbtorture_smb2_session_encryption_aes_128_ccm_memfs … Passed".
- At samba-4.23.6, every `bind_negative_smb3sign*` and `bind_negative_smb3sne*` test skips with "Can't test without SMB 3.1.1 signing negotiation support" when the negotiated signing algorithm is below AES-128-GMAC. Source: `source4/torture/smb2/session.c`, for example :3363-3364.
- The driver offers GMAC only for names that contain `toG`, so the following **16 subtests are the self-skipping subset**. **All 16 are on every backend's allowlist** (96 gated cells) and appear as `Passed`. For example, job 3490968 artifact line 644: "smbtorture_smb2_session_bind_negative_smb3signGtoCd_memfs … Passed".

| Group | Subtests (each in `s` and `d` variants) | Count | Allowlisted |
|---|---|---|---|
| sign, Gto* | `bind_negative_smb3signGtoC`, `GtoC30`, `GtoH`, `GtoH2X` | 8 | all 6 backends |
| sign, CMAC↔HMAC | `bind_negative_smb3signCtoH`, `HtoC` | 4 | all 6 backends |
| sne, Gto* | `bind_negative_smb3sneGtoC`, `GtoH` | 4 | all 6 backends |

The `*toG` subtests (sign `CtoG`, `HtoG`, `C30toG`, `H2XtoG`; sne `CtoG`, `HtoG`; 12 in all) get GMAC and the encryption override, so they do not take the GMAC skip. The tree has no `smb2.encryption*` family.

### smbtorture server-config overrides

The driver starts every server with this test profile (`smbtorture_test.c:555-800`). The shipped defaults are in `src/server/server.c`.

| Setting | Scope | Cite | Shipped default |
|---|---|---|---|
| persistent/durable handles on | all runs | `:564` | off (`server.c:265` "smb_persistent_handles = 0") |
| leases on, oplocks on | all runs | `:571-572` | off (`server.c:296-297` "smb_leases  = 0") |
| directory leases on | all runs | `:577` | off (`server.c:271`) |
| 2 multichannel NICs advertised (127.0.0.1, 127.0.0.2) | all runs | `:585-593` | 0 NICs (`server.c:261` "smb_num_nic_info = 0") |
| named streams on | ADS subtest list (`smbtorture_ads_subtests.txt`) | `:601-609` | off (`server.c:276`) |
| `acl_inherited_canonicalize=0` | `smb2.acls_non_canonical` | `:620` | 1 (`server.c:310`) |
| Windows replay-pending profile | `smb2.replay.dhv2-pending*-windows` | `:666` | sane profile (`server.c:344` "smb_replay_pending_windows = 0") |
| signing required | `session-require-signing` | `:767` | not required (`server.c:281`) |
| change notify disabled | `change_notify_disabled` | `:778` | enabled (`server.c:301`) |
| encryption on | `session.encryption*`, `anon-encryption`, `bind_negative_smb3enc`, `bind_negative_smb3s*toG` | `:783-797` | off (`server.c:285` "smb_encryption = 0") |
| diskfs `unsafe_async: true` (FUA/sync skipped); 64 MiB intent log | diskfs backends | `:715-720` | sync on (`diskfs_mount.c:1157`, effect at `:550`); 1 GiB log (`space_map.h:240`) |
| diskfs `block_cache_blocks: 262144` | diskfs backends | `:736` | 2 × intent-log blocks (`diskfs_block.c:661`) |

Dialects are not overridden: the shipped 2.0.2–3.1.1 set applies (`server.c:254`). So no smbtorture cell today runs against the shipped default configuration. Every diskfs cell also runs without durable writes.

### Run, skipped, excluded, missing

| Bucket | Count | Reason | Evidence |
|---|---|---|---|
| **Run, gating** (merge train, and the scheduled pipeline's `ctest` legs) | 1741 ctest entries = 3122 subtest cells; 0 failed in both legs | Allowlisted and not hand-disabled | job 3490968 (pipeline 205408, release) trace line 1074 "100% tests passed, 0 tests failed out of 1741"; job 3490969 (debug) trace line 1074, same text |
| Run, rpc (`rest` leg) | 5 passed, 1 `Not Run (Disabled)` | `smbtorture_rpc` label is not excluded by `-LE '^pynfs$\|^smbtorture$\|^wpts$\|^kvm$'` | job 3490972 artifact: "smbtorture_rpc_lsalookup_memfs …***Not Run (Disabled)" |
| Run, race and prenotify (`rest` leg) | 7 passed | Unlabelled | job 3490972 artifact (see Wiring) |
| Skipped, ctest level | 0 | No `***Skipped` or `Not Run` lines in either smbtorture leg log | `grep -c 'Skipped\|Not Run'` = 0 for the 3490968 and 3490969 artifacts |
| Skipped, inside smbtorture (counted as Passed) | ≥ 96 cells (16 bind_negative subtests × 6) + 103 copy_chunk cells off memfs; any other self-skips **not measured** | Option-induced (client options above) | The ctest logs run without `-V`, so subtest output is not in the artifacts |
| Explore (scheduled, informational) | 3930/3930 cells measured. Totals: "failing (disabled) 53, newly passing 108, still failing 614, timing out 20"; the report has no `skipped` bucket (results.txt has 0 SKIP lines) | Non-gated cells measured nightly. The job does not fail on cell failures (`.gitlab-ci.yml:501` "Informational (the qb task swallows cell failures)") | job 3490980 (pipeline 205408) `smbtorture-report.txt` line 868 "== totals: 3930/3930 cells measured, failing (disabled) 53, newly passing 108, still failing 614, timing out 20" |
| **Excluded**, hand-disabled | 11 subtests (66 cells) | Recorded reasons above | `:5039-5072` |
| **Excluded**, never allowlisted | 74 subtests (444 cells). In explore, 66 fail or time out on all 6 backends and 8 pass on at least one backend | No per-test reason is recorded; allowlists are generated from a matrix run (`:673-676`) | job 3490980 `results.txt` |
| **Excluded**, per-backend complement | 808 cells not gated (includes the 2 rows above) | Failed at generation on that backend | parser replay |
| **Missing** | 2323 non-`smb2` entries, of which 6 rpc entries are wired | Not registered | universe table |

**Explore buckets per backend** (job 3490980, `smbtorture-report.txt` header lines).

| Backend | Header line | newly passing | timing out | still failing | failing (disabled) |
|---|---|---|---|---|---|
| memfs | :2 "655/655 cells measured, 578 in baseline" | 7 | 4 | 66 | 9 |
| linux | :100 "… 491 in baseline" | 19 | 4 | 141 | 9 |
| io_uring | :285 "… 493 in baseline" | 19 | 3 | 140 | 9 |
| cairn | :468 "… 539 in baseline" | 22 | 3 | 91 | 8 |
| diskfs_io_uring | :604 "… 545 in baseline" | 19 | 3 | 88 | 9 |
| diskfs_aio | :735 "… 542 in baseline" | 22 | 3 | 88 | 9 |

"newly passing" cells (108) pass but are not on the allowlist, so they are not gated. That is allowlist drift.

**Never allowlisted (74), by parent.** Pass patterns are explore outcomes across the 6 backends, in the order memfs/linux/io_uring/cairn/diskfs_io_uring/diskfs_aio (P = pass, F = fail, T = timeout).

| Parent | n | Parent | n |
|---|---|---|---|
| smb2.ioctl | 7 | smb2.session | 7 (bind_negative_smb202/210d/210s/2to3d/2to3s, reauth5, reconnect1) |
| smb2.lease | 5 | smb2.multichannel | 4 |
| smb2.compound_async, create, delete-on-close-perms, dir, durable-v2-open, getinfo, notify, oplock | 3 each | smb2.charset, compound, dirlease, durable-open, kernel-oplocks, name-mangling, rename | 2 each |
| acls_non_canonical, aio_delay, bench, dosmode, ea, fileid, hold-oplock, hold-sharemode, samba3misc, sdread, setinfo, timestamp_resolution, twrp | 1 each | | |

- Passing on all 6 backends but still not allowlisted (PPPPPP): `bench.oplock1`, `compound.compound-break`, `dir.large-files`, `lease.timeout-disconnect`, `rename.rename_dir_openfile`.
- Partial passes: `durable-open.alloc-size` (FFFFPP), `fileid.fileid-dir` (PFFFFF), `timestamp_resolution.resolution1` (FFPFFF).
- Timeouts: `aio_delay.aio_cancel`, `hold-oplock` and `hold-sharemode` time out on all 6 (TTTTTT); `notify.rec` is TTFFFF.

**Missing families.** These are the non-`smb2` entries. The class column restates the proposal and decision D7 (the full set is `smb2.*` plus the rpc pipes the product serves).

| Family | Entries | Proposed disposition |
|---|---|---|
| raw | 195 | (O) SMB1 |
| base | 121 | (O) SMB1 |
| vfs | 44 | (D) Samba VFS modules |
| rpc (served pipes: `rpc.lsa` 30, `lsalookup` 1, `lsa-getuser` 1, `srvsvc` 17, `samr` 20, `wkssvc` 29, `handles` 6) | 104 of 561 | in the full set ("rpc suites the product serves", D7); 6 wired. No `rpc.samr` entry is wired, though the server serves samr |
| rpc, other pipes | 457 | not served; outside the D7 full set |
| local, krb5, winbind, net, rap, drs, libsmbclient, nbt, dlz_bind9, ldap, dfs, netapi, unix, notifyd, dns_internal, ntp, gpo, echo, dsdb, bench | 1867 | outside the D7 full set (`smb2.*` + served rpc). the proposal gives these no class |

### Retry example

| Job | Result | Quoted line |
|---|---|---|
| 3488415 (pipeline 205316, 2026-10-02, `ctest: [release, amd64, smbtorture]`) | failed, `script_failure` | trace line 1068 "99% tests passed, 1 tests failed out of 1741"; line 1073 "2272 - chimera/server/smb/smbtorture_smb2_notify_linux (Timeout) smbtorture"; line 1030 "***Timeout 600.10 sec"; line 1094 "ERROR: Job failed: exit status 1" |
| 3488440 (same pipeline, automatic retry) | success | trace line 1074 "100% tests passed, 0 tests failed out of 1741"; line 1094 "Job succeeded" |

Pipeline 205316 finished `success`. The retry policy (`.gitlab-ci.yml:260-265`, `max: 1` on `script_failure`) hid the timeout from the pipeline status.

### Proposed full subset (PROPOSAL for Q-SUC-2)

The selection is every `smb2.*` subtest at the pinned Samba (4.23.6: 655 subtests) plus the served rpc entries (104 candidates). The pass criterion is `success`: a skip fails unless it is excluded, and there is no retry. The corruption oracle is partial (read-back subtests). The run uses the shipped defaults plus declared deviations; `unsafe_async` is not allowed.

| Class | Families / subtests (catalog counts at 4.23.6) |
|---|---|
| (P) ACL-2 | `smb2.acls.*` 14, `smb2.acls_non_canonical.flags` 1 (matches `smb2.acls*`, but asserts Samba's non-default canonicalisation; for review), `smb2.sdread` 1, `smb2.create.nulldacl` 1 |
| (P) SEC-6 | `smb2.session.encryption-*` 4 (they run today under the encryption override) |
| (P) HA-5 | Only the persistent/CA subset of `smb2.durable-v2-*` (36) and `smb2.replay.*` (60). By name only `durable-v2-open.persistent-open-{lease,oplock}` (2) are identifiable; the rest is **not classified** per subtest |
| (M)/(O) | Non-persistent durable and replay subtests (the remainder of the 96 above) |
| (M) or (O) | The 16 self-skipping `bind_negative_smb3sign`/`smb3sne` subtests (Gto* and CMAC↔HMAC). Proposed per-subtest rule: binding is processed only when `IsMultiChannelCapable` (MS-SMB2 3.3.5.5 step 4, "MUST perform the following"; 4.1.8 "MUST verify the signature … using the Session.SigningKey"). So all 16 are **(M) if multichannel is advertised (OQ-1), else (O)**. 3.3.5.5 does not state the specific status codes these subtests assert (see notes) |
| (M) | SMB1→SMB2 upgrade negotiate (no single `--list` entry identified) |
| (O) | `smb2.multichannel` 11; `smb2.streams` 14 (the ADS list matches 31 subtests); quota: `smb2.create.quota-fake-file` 1. The 4.23.6 catalog has no `smb2` transport-compression or DFS family (top-level `dfs` has 7). `raw.*` 195 and `base.*` 121 are SMB1 |
| (D) | `smb2.samba3misc` 1, `vfs.*` 44, `smb2.kernel-oplocks` 8, the 2 Samba-vs-spec lease subtests (`lease.breaking3`, `v2_breaking3`), `smb2.ea.acl_xattr` 1, `smb2.replay.channel-sequence` 1 (seed knob), and the 4 subtests that block by design and time out on every backend: `hold-oplock`, `hold-sharemode`, `lease.breaking4`, `aio_delay.aio_cancel` (`tools/smbtorture/regen.sh:259-262`) |

**Knob dispositions (PROPOSAL).**

| Knob | Disposition |
|---|---|
| `samba3=yes` | drop (P7) |
| `--seed` | fixed per run, rotated per scheduled run and recorded; `replay.channel-sequence` (D) |
| `oplocktimeout=30` | keep, citing the server deadline |
| `resume_key_support` | `yes` everywhere (this turns the 103 copy_chunk cells that pass by skipping into real runs) |
| signing algorithms | offer GMAC+CMAC+HMAC everywhere, so the 16 bind_negative subtests become visible results instead of skips |
| server overrides | each becomes a declared deviation (HA-5 persistent on CA shares; SEC-6 encryption; OQ-1 features such as leases, dirlease, multichannel, streams) or is dropped; `unsafe_async` is dropped |

## Ceph s3-tests

Ceph s3-tests is a boto3/pytest suite that exercises the S3 API. In Chimera it runs as **one ctest**, against **memfs only**, using a **214-case allowlist**. No GitLab pipeline runs it. GitHub `extended.yml` would put it in the `rest` shard, but only if it is registered, and on static evidence it is not (below). Every count below can be regenerated with `s3.repro.sh`.

### Source and pin

| Item | Value | Cite |
|---|---|---|
| Upstream | `https://github.com/ceph/s3-tests.git`, cloned into `/opt/s3-tests` with its own venv | `.devcontainer/Dockerfile:201-206` ("ENV S3TESTS_DIR=/opt/s3-tests") |
| Pin | `5522d1c351f75bc00ae0f64f742f3f095f5939d9` | `.devcontainer/Dockerfile:59` ("ARG S3TESTS_REF=5522d1c3…") |
| Build switch | `ENABLE_S3TESTS=1` | `.devcontainer/Dockerfile:56` |
| Python deps | `requirements.txt` at the pin (unpinned: `boto3 >=1.0.0`, `pytest`, …) plus `pytest-timeout`, in a `--python 3.12` venv | `.devcontainer/Dockerfile:205-206` |
| GitLab CI image (`builders/dev`) | not installed: the file has no `s3-tests` and no `boto3` (grep: 0 hits) | `builders/dev/Dockerfile` (absence; `repro/s3.repro.sh` in the vault evidence directory) |

The universe was enumerated from a scratch clone at the pin (`git rev-parse HEAD` = `5522d1c3…`). It ran under pytest 9.1.1, boto3 1.43.108 and Python 3.13.5. That is not the image's 3.12; collection does not depend on the difference.

### Wiring and registration gates

Registration needs all of the following:

| Gate | Effect | Cite |
|---|---|---|
| System `python3 -c "import boto3"` succeeds | Without it, the whole `src/server/s3/tests` body is skipped, including `add_subdirectory(ceph)` | `src/server/s3/tests/CMakeLists.txt:11`, `:27`, `:134` |
| `${S3TESTS_DIR}/.venv` exists (default `/opt/s3-tests`) | Otherwise prints "ceph s3-tests not found" | `src/server/s3/tests/ceph/CMakeLists.txt:12-17`, `:31` |
| netns | **Not a gate.** `chimera_add_netns_test` always registers. Without netns the test runs on the host network under `RESOURCE_LOCK chimera_net` | `CMakeLists.txt:856-862` |
| Tier | `extended` (directory default; registered with `CONFIGURATIONS extended`) | `CMakeLists.txt:357`, `:433` |

What gets registered:

| Property | Value | Cite |
|---|---|---|
| ctest name | `chimera/server/s3/ceph_s3tests` (exactly one test) | `src/server/s3/tests/ceph/CMakeLists.txt:18` |
| Backend | `--backend memfs` only | `…/ceph/CMakeLists.txt:22` |
| Label | `ceph-s3` | `…/ceph/CMakeLists.txt:26` |
| Timeout / skip code | `TIMEOUT 600`, `SKIP_RETURN_CODE 77` (the script exits 77 only when the venv is missing) | `…/ceph/CMakeLists.txt:27-28`; `run_ceph_s3tests.sh:51` |
| Selection | With no pytest args, the script runs the non-comment lines of `passing_tests.txt`: 214 node IDs, all in `test_s3.py`, all resolving at the pin, 3 of them marked `fails_on_rgw` | `run_ceph_s3tests.sh:168-175`; `passing_tests.txt:4` |
| Client | `AWS_MAX_ATTEMPTS=1` (no botocore retry), `--timeout=60 --timeout-method=signal` per case | `run_ceph_s3tests.sh:182-184` |

The README says the test is "gated on" `CHIMERA_NETNS_TESTING` (`src/server/s3/tests/ceph/README.md:38-39`), and the CMake comment says it registers "when … netns testing is enabled" (`…/ceph/CMakeLists.txt:7`). Neither is true of the code (see Findings).

**Pool mode:** none. Pool modes (MODE-1..3, `docs/product/prd.md:184`, `:195-198`) are not implemented, and the harness sets none. As a result, the ACL-4 refusal rows below are "not testable yet".

### CI coverage

| CI | Status | Evidence |
|---|---|---|
| GitLab merge train and schedule (`rest` leg, `-LE '^pynfs$\|^smbtorture$\|^wpts$\|^kvm$'`, `.gitlab-ci.yml:333-334`) | **Not run.** Neither the Ceph test nor any boto3 S3 test is registered | Job 3490972 (pipeline 205408, 2026-10-04, sha 3ea255e0), artifact `ctest_release_rest_amd64.log:121`: "boto3 not found - S3 tests will be skipped.  Install with: apt install"; `:11210`: "100% tests passed, 0 tests failed out of 5532". `grep -ci ceph` = 0 in that log and in the Debug leg's (job 3490973, `ctest_debug_rest_amd64.log:127`, same warning). Same in merge-train pipeline 205343 (jobs 3489337 `:121` and 3489338 `:127`). Saved artifact sha256: 3490972 `6ac5e8ab…3763`, 3490973 `86c7b253…7817`, 3489337 `44b875cb…08c8`, 3489338 `19c5eafd…7bae` |
| GitLab image, probed directly | Pipeline 205408 used `ci-chimera-442` (artifact line 2: `-i chimera/ci-chimera-442`) | `podman run --rm --network=none repository.mdh.quantum.com/chimera/chimera-dev:ci-chimera-442` (digest `sha256:8fe0a876c445b22bdc843e54d641f1534f4f2547d13c9a31b8aaf4b83f216556`) printed "ModuleNotFoundError: No module named 'boto3'" and "ls: cannot access '/opt/s3-tests': No such file or directory" |
| GitHub `extended.yml` (cron `0 1,7,13,19 * * *`, `:16`; privileged container, `:796`) | **Configured but unconfirmed.** `ceph-s3` is not in the shard exclusion set, so the test would run in `rest` | `.github/workflows/extended.yml:742` (shard list), `:821` (`EXCL=…`), `:836` (`rest)  SEL="-LE $EXCL"`). Not measured: GitHub Actions logs were not accessible with the allowed tools. **Static evidence against registration:** `.devcontainer/Dockerfile` installs the same apt set as `builders/dev` (diff: only `tmux`). It installs no system `boto3`: its only `pip3` installs are `xdrlib3` and pike's deps (`:176`, `:189`), and `boto3` goes only into the s3-tests venv (`:206`). The GitLab image with the same `awscli` (2.31.35-1, `.devcontainer/Dockerfile:103`, `builders/dev/Dockerfile:49`) has no `boto3`. So the `import boto3` gate (`src/server/s3/tests/CMakeLists.txt:11`) most likely fails upstream too |

The `test_s3.py` results quoted in `docs/design/s3_ceph_compliance.md:105` ("passed | 217 | 24%") were collected by hand. No CI job produces them.

### Harness configuration vs shipped defaults

`src/server/s3/tests/ceph/run_ceph_s3tests.sh:73-148` writes both the daemon config and `s3tests.conf`.

| Setting | Harness | Shipped default | Effect on what a pass proves |
|---|---|---|---|
| S3 service | `"s3_enabled": true`, port 5000 (`run_ceph_s3tests.sh:79`) | off (`src/server/server.c:392`); port 5000 (`:401`); `src/daemon/chimera.json:16-17` enables only NFS and SMB | Required to run at all |
| Backend | memfs only (`…/ceph/CMakeLists.txt:22`) | n/a (D2: facade; memfs/diskfs are interim proxies) | No diskfs, cairn or linux row. `unsafe_async` does not apply (memfs) |
| Identities | One user `s3test` with the runner's uid/gid (`:81`). Main, alt and tenant keys are all bound to it (`:73`, `:84-86`) | A key bound to no user acts as uid 65534 (`server.c:402`, `server.h:450`) | Alt and tenant **are the same principal as main**, so every "alt is denied" assertion is untestable, and alt "can" assertions pass vacuously |
| Canonical IDs | `s3tests.conf` declares `user_id = testid` / `altuserid` (`:109`, `:118`), but the daemon keys set no `user_id` | The canonical ID defaults to the access key (`src/server/s3/s3_cred_cache.h:307`) | Grants naming `get_alt_user_id()` name an ID the server does not know |
| IAM sections | `[iam]`, `[iam root]` and `[iam alt root]` keys written to `s3tests.conf` (`:130-147`) but not to the daemon | — | IAM-backed cases cannot authenticate |
| Bucket root | `/share` (`:90`) | first configured mount (`src/daemon/daemon.c:1657`) | Equivalent placement |
| Transport | `is_secure = False` (`:99`) | Plain TCP. S3 has no TLS flavor: `enum chimera_tcp_flavor` has PLAIN/IO_URING/XLIO/INPROC only (`src/vfs/sdk/vfs_tcp_flavor.h:23-26`) | SEC-6 "TLS for S3" (`prd.md:273-276`) is not testable yet |
| Anonymous | unsigned requests allowed by the client | rejected before authorisation as **400 MissingSecurityHeader** (`src/server/s3/s3.c:722`; `s3_status.c:148-150`), not 403 AccessDenied | Deny-asserting cases that send no `Authorization` header and assert `403` would get 400 instead. Not run. The server has no per-bucket anonymous enablement to declare as a deviation (SEC-1, `prd.md:255-257`) |
| Policy / unknown subresources | — | `?policy`, `?publicAccessBlock` and other unrecognised subresources get an empty 200 and are **not enforced** (`s3.c:1063`, `:1073-1074`) | Policy cases that pass do so without enforcement (ACL-4/SEC-4 → task `s3-unknown-subresources`) |
| `authenticated-read` | — | Treated as public-read (`src/server/s3/s3_acl.c:20`) | AUTHENTICATED@ semantics are not implemented |

### Universe

`pytest --collect-only` at `5522d1c3` collects **1053** cases. The collection was re-run for this document and matches the saved list byte for byte.

| File | Collected | On allowlist |
|---|---:|---:|
| `test_s3.py` | 838 | 214 |
| `test_iam.py` | 90 | 0 |
| `test_headers.py` | 48 | 0 |
| `test_sts.py` | 36 | 0 |
| `test_s3select.py` | 34 | 0 |
| `test_sns.py` | 5 | 0 |
| `test_s3control.py` | 1 | 0 |
| `test_utils.py` | 1 | 0 |
| **Total** | **1053** | **214** |

### Markers

`pytest.ini` declares **59** markers. **10** of them are on no collected case: `appendobject`, `auth_aws4`, `fails_on_s3`, `fails_with_subdomain`, `object_lock`, `s3website`, `s3website_routing_rules`, `s3website_redirect_location`, `storage_class`, `versioning`. In `test_s3.py`, **239** cases carry no marker at all and **360** carry none other than `fails_on_*` (all files together: 242 and 363). Some families have tests but no usable marker: 39 `test_object_lock*` cases (marker empty), 20 `test_versioning*` (marker empty), and 13 `test_cors*` (no CORS marker). A marker-based exclusion therefore cannot express the (O) families, and a node-ID denylist is needed (see "Per-file decision"). Among the populated markers relevant to (O) families: `encryption` 144, `bucket_logging` 113, `sse_s3` 56, `lifecycle` 48, `bucket_policy` 31, `tagging` 25, `auth_aws2` 21 (all in `test_headers.py`), `bucket_encryption` 12, `checksum` 11, `object_ownership` 8. The suite has no `xfail` anywhere.

### Run, skipped, excluded, missing (today)

| Set | Count | Reason | Cite |
|---|---:|---|---|
| Universe | 1053 | collected at the pin | repro |
| Wired | 214 | allowlist of "cases that pass against chimera on a single shared daemon" | `passing_tests.txt:4-5` |
| Run in GitLab CI | 0 | not registered: no `boto3`, no `/opt/s3-tests` | job 3490972 (pipeline 205408) |
| Run in GitHub CI | not measured | configured for `rest`, but registration is unlikely (no system `boto3`) | `extended.yml:821`, `:836` |
| Excluded | 839 | not on the allowlist (1053 − 214). Everything outside `test_s3.py` is excluded | `run_ceph_s3tests.sh:168-175` |
| Skipped (runtime) | not measured | 103 `test_s3.py` cases have a self-skip path whose trigger depends on config: bucket-logging extension 74, `[s3 cloud]` 12, `kms_keyid` 10, storage classes 6, `api_name` 1 | `s3.repro.sh` |
| Missing (harness cannot exercise) | — | distinct identities (alt = main); IAM/STS principals; KMS key; multiple storage classes; cloud tier; TLS; pool modes | harness table above |
| Passing for the wrong reason | 6 of the 16 policy/ACL allowlist cases | Four cases assert that alt may list or read: `test_bucket_policy`, `test_bucketv2_policy`, `test_bucket_policy_another_bucket` and `test_bucketv2_policy_another_bucket`. They pass while `?policy` gets an empty 200 and is not enforced (`s3.c:1063`, `:1073-1074`), and while alt is the same user as main. `test_object_copy_canned_acl` passes on the same identity alias. `test_block_public_policy_with_principal` passes while `?publicAccessBlock` and `?policy` are both accepted unenforced | `s3-classification.tsv` × allowlist |

Pass semantics today: the single ctest passes if and only if pytest exits 0. A pytest `skip` passes. A failure in any of the 214 cases fails the whole ctest, and ctest shows no per-case result.

### Per-file decision (PROPOSAL)

| File | Collected | Proposal |
|---|---:|---|
| `test_s3.py` | 838 | **in**, with a node-ID denylist; SSE, website, CORS, lifecycle and object lock are (O) |
| `test_headers.py` | 48 | **in**; the 21 `auth_aws2` (SigV2) cases are (O) |
| `test_iam.py` | 90 | (O) unless advertised |
| `test_sts.py` | 36 | (O) unless advertised |
| `test_s3select.py` | 34 | (O) unless advertised |
| `test_sns.py` | 5 | (O) unless advertised |
| `test_s3control.py` | 1 | (O) unless advertised |
| `test_utils.py` | 1 | **out**: it tests the suite's own `generate_random` helper and never contacts a server |

### Policy, ACL and anonymous cases (classification)

**Scope.** Every collected case in `test_s3.py` and `test_headers.py` that calls (directly or through a helper) an ACL, bucket-policy, ownership or public-access-block API, or makes an unsigned request. That is **285** cases: 279 in `test_s3.py` and 6 in `test_headers.py`, including the 4 `*_bad_authorization_{empty,none}` header cases found by name. The per-case list is `data/s3-classification.tsv` (node ID, class, reason) in the vault evidence directory (see "Scope and method"). Every class was assigned by reading the test body at the pin.

**Rules:**
- A grant or unconditioned policy whose principal and action map to an NFSv4 ACE is **canonical-mappable**. This includes owner/user grants, `AllUsers` mapped to EVERYONE@ and `AuthenticatedUsers` mapped to AUTHENTICATED@.
- Conditions, prefixes, tags, service principals, cross-tenant or cross-account principals, IAM account semantics and object ownership are **no-canonical-equivalent**.
- An unsigned request that the test asserts is refused is **deny-asserting**. One that the test asserts succeeds is **allow-asserting**.
- Invalid-credential cases are authentication cases, not anonymous ones, and are left **not classified**. So is anything doubtful.

| Class | Disposition (PROPOSAL) | test_s3 | test_headers | Total |
|---|---|---:|---:|---:|
| canonical-mappable (ACL-4) | (P), must pass in both modes | 73 | 2 | **75** |
| no-canonical-equivalent | must pass in native mode; must be refused with the standard error in multi-protocol mode (not testable yet: no pool modes) | 148 | 0 | **148** |
| deny-asserting anonymous | (P) SEC-1, must pass as shipped | 4 | 4 | **8** |
| allow-asserting anonymous | runs only under a declared per-bucket admin enablement (OQ-7). The server has no such setting today | 15 | 0 | **15** |
| not classified | reason recorded per case | 39 | 0 | **39** |
| **Total** | | **279** | **6** | **285** |

Notes on the classes:
- 113 of the 148 no-canonical-equivalent cases are bucket-logging cases. They are in scope only because their setup installs a log-delivery policy: service principal `logging.s3.amazonaws.com` with `aws:SourceArn`/`aws:SourceAccount` conditions. Their own feature class is decided separately.
- The other 35 are made up of these groups:
  - conditions: 19 (SSE and SSE-C headers, object tags, Referer, SourceIp, `s3:prefix`, copy-source, metadata-directive, grant header, x-amz-acl);
  - resource scoping: 2 (a key-prefix ARN, and an ARN for a key that does not exist yet);
  - `NotPrincipal`: 1;
  - Object Ownership: 8;
  - cross-tenant or cross-account: 3;
  - IAM account-root exemption: 2.
- The 39 not classified are:

| Count | Reason |
|---:|---|
| 11 | CORS preflight |
| 9 | PublicAccessBlock is a guard rather than a grant; how ACL-4 treats it is undecided |
| 6 | invalid credentials or presign expiry |
| 3 | tagging-action grants |
| 2 | e-mail grantee |
| 2 | wildcard cross-bucket resource |
| 2 | per-version ACL |
| 2 | missing bucket, which belongs to the SEC-3 row |
| 1 | `ExpectedBucketOwner` |
| 1 | `GetBucketPolicyStatus` with no construct |

- Today 16 of these cases are on the allowlist: 11 canonical-mappable and 5 not classified. None of the 8 deny-asserting cases is on it.

### Pass criterion

| | Today | Proposed full run (PROPOSAL) |
|---|---|---|
| Unit | one ctest over the 214-case allowlist | every case not on the written denylist, reported per node ID |
| Pass | pytest exit 0 | pytest pass. A skip fails unless it is listed. xfail fails unless listed (the suite has none). No retry. |
| Retry | client: none (`AWS_MAX_ATTEMPTS=1`). The GitLab `ctest` retry policy does not apply, because the test is not registered | none |
| Configuration | the harness deviations above | shipped default plus declared deviations. Multi-protocol mode, plus native mode for the ACL-4 rows. Over TLS (SEC-6). Distinct principals for main, alt and tenant |

### Corruption oracle

The oracle is partial and works case by case. A static scan of `test_s3.py` finds **305** cases that read an object body back: `_get_body`, `Body.read()`, or the range/multipart content-check helpers. Most of them assert equality with what was written. There are also **11** `checksum`-marked cases, which use explicit checksum algorithms. Together that is **316** cases. `test_headers.py` has none. **59** of the 214 allowlisted cases fall in that set. No case checks the backend after the run. The cross-suite read-back child proposed under "Proposed definition of a full run" still applies.

## MinIO warp

### Presence in tree

warp is absent. The repository mentions it only in `docs/product/prd.md:395-396` ("S3 is also load and stress tested with MinIO warp, which is a benchmark tool, not a conformance suite"). `git grep -i warp` finds no other hit apart from SMB `timewarp`, in `src/server/smb/smb_proc_create.c` and `src/server/smb/tests/phase0_contexts_test.c`. There is no harness, no ctest and no CI job.

### Pin (recorded for the proposal)

| Item | Value |
|---|---|
| Latest release | `v1.8.2`, published 2026-09-24 (`https://github.com/minio/warp/releases/latest` → `/tag/v1.8.2`), commit `d9f7173e8c9bebfcde20b36760883f3fb79055dc` |
| linux-amd64 binary | `https://dl.min.io/aistor/warp/release/linux-amd64/archive/warp.v1.8.2`. The GitHub release itself carries only `warp-rdma_*` tarballs, and its notes link the plain binaries on dl.min.io |
| sha256 | `fc7661a6b952800fbc8422cf0f72ae82726f82aa80065d15f50125f8bf5ca5fa`, which matches the published `warp.v1.8.2.sha256sum` |
| `--version` | `warp.v1.8.2 version v1.8.2 - d9f7173` |
| Licence | GNU AGPL v3 (`LICENSE` at the tag) |

### Modes (`warp --help` at v1.8.2; no server needed)

| Subcommand | Help text | Notes |
|---|---|---|
| `put` | benchmark put objects | |
| `get` | benchmark get objects | `--range`, `--versions` |
| `mixed` | benchmark mixed objects | |
| `delete` | benchmark delete objects | |
| `list` | benchmark list objects | |
| `stat` | benchmark stat objects (get file info) | |
| `multipart` | benchmark multipart object | |
| `multipart-put` | benchmark multipart upload | |
| `versioned` | benchmark mixed versioned objects | needs versioning |
| `atomic` | verify that concurrent overwrites are atomic and reads are not stale | **correctness check**; see below |
| `retention` | benchmark PutObjectRetention | needs object lock |
| `append` | benchmark appen[d] objects (s3 express) | S3 Express |
| `zip`, `snowball`, `fanout` | minio s3zip; snowball tar; fan-out on MinIO servers | MinIO extensions |
| `iceberg` | (ALPHA) Iceberg catalog operations | not S3 object API |
| `analyze`, `cmp`, `merge`, `client`, `run` | tooling, not benchmarks | |

### Does warp verify object contents?

- **The benchmark modes do not.** `get` reads each body into `io.Discard` and checks only the length (`pkg/bench/get.go:347`, `:355`: "unexpected download size"). `mixed` (`pkg/bench/mixed.go:282`, `:290`), `multipart` (`pkg/bench/multipart.go:248`, `:256`) and `versioned` (`pkg/bench/versioned.go:182`, `:190`) do the same. The README at the tag says so too: "Other warp benchmarks measure speed and check only object sizes" (`README.md:807`).
- **The `atomic` mode does.** It writes a stamp into each 4 KiB block. On every GET it hashes the body and checks each block against its expected content (`pkg/bench/atomic.go:232`, `:253`, `:281`: "does not match its content"; `:771`). It reports the violation categories `torn`, `corrupt`, `length`, `etag-mismatch`, `etag-md5`, `stale`, `list-*` and others (`atomic.go:51-68`). By default it also requires the ETag to be the MD5 of the body (`--no-etag-md5` turns that off).
- **Errors:** they are printed as `Errors: N` in the analysis output (`cli/analyze.go:231-233`). No code was found that turns a non-zero error count into a non-zero exit status; `cli/cli.go:84` exits 1 only when the app itself fails. Not verified at runtime.

### Proposed full definition (PROPOSAL for Q-SUC-2)

| Selection | Pass criterion | Corruption oracle |
|---|---|---|
| Pinned release (v1.8.2, sha256 `fc7661a6…a5fa`). Modes: put, get, mixed, multipart, list, delete and stat (+ versioned if advertised), **plus `atomic`**. Fixed parameters to be set under D6: duration, concurrency, object sizes, object count, TLS | Zero errors in every mode, and **zero `atomic` violations** (`torn`, `corrupt`, `length`, `etag-*`, `stale`, `list-*`). The gate must parse warp's output, because no code maps errors to the exit status (not verified at runtime). Throughput is recorded, not gated (D6) | **`atomic` is a read-after-write content oracle:** a block-by-block check of every GET body plus an ETag-is-MD5 check (`pkg/bench/atomic.go:232`, `:253`, `:281`). The other modes check only object sizes (`README.md:807`). The corruption oracle for S3 load is therefore `atomic`, and no E1 child is proposed for it |

## Adjacent suites

Facts only. These suites are not in SUC-2. Each one is an **OQ-2 candidate**.

| Suite | Wiring (cite) | Protocol covered | GitLab cadence | Count (registered / CSV) | Pinned result (205408) |
|---|---|---|---|---|---|
| WPTS MS-SMB2, MS-SMB2Model, MS-FSA, MS-FSAModel | `src/server/smb/tests/CMakeLists.txt:135-458`, `scripts/wpts_smb_test_wrapper.sh`; WPTS `4.26.3.0` (`builders/dev/Dockerfile:19`); gated on netns + dotnet + x86_64 (:160, :423) | SMB 2.0.2–3.1.1 server (MS-SMB2), MS-FSA file-system algorithms | `wpts` leg (train gating, scheduled; MR manual) | 60 ctest entries = 13 MS-SMB2 configs + 1 FSA + 1 FSAModel + 45 SMB2Model shards of ≤50 (`WPTS_SMB2MODEL_SHARD 50`, :367) | jobs 3490970/3490971: `100% tests passed, 0 tests failed out of 60` |
| pike | :460-540; `scripts/pike_smb_test_wrapper.sh`; pinned `PIKE_REF=bcbb343c` in `.devcontainer/Dockerfile:52` only | SMB 2/3 (pure-Python client) | **never on GitLab** (not in `builders/dev`); GitHub `rest` shard | 3 configs (base, persistent, encryption); 187 CSV rows | 0 pike tests in the job 3490972 `rest` log |
| KVM nfstest | `kvm/tests/CMakeLists.txt:425-566` (`NFSTEST_MATRIX` :443-452); 2-client delegation and cache tests :546, :560 | Linux kernel NFS client vs the server; v3, 4.0, 4.1, 4.2 per program | `ctest_kvm` ubuntu2604-nfstest (train); + ubuntu2404-nfstest (scheduled) | 53 per image | job 3490983: 53 nfstest Passed (in `... out of 92`) |
| KVM LTP | `LTP_MATRIX` `kvm/tests/CMakeLists.txt:212-216` (`dio`, `fcntl-locktests`, `chimera_nfs42`) | NFS (O_DIRECT, POSIX locks, 4.2 ops) | same legs as nfstest | 39 per image | job 3490983: 39 ltp Passed. Job 3490983 total: `100% tests passed, 0 tests failed out of 92` |
| KVM xfstests programs | `XFSTESTS_PROGRAMS` `kvm/tests/CMakeLists.txt:187` (fsstress, fsx, dirstress, nametest, fstest, rewinddir) | NFS v3–4.2 and SMB via kernel clients | `ctest_kvm` (train), `ctest_kvm_images` (scheduled) | 144 NFS + 76 SMB per image | jobs 3490981/3490985/3490989/3490991: 220 Passed each |
| FUSE errno test | `src/server/fuse/tests/CMakeLists.txt:59-98` (`fuse_posix_test.sh`; "plain syscalls and exact errnos", `fuse_posix_test.c:7`) | POSIX over kernel FUSE (memfs only, :46) | `rest` leg | 3 (`posix_memfs`, `posix_uring_memfs`, `posix_directio_memfs`) | job 3490972: 2 Passed, 1 Skipped (`posix_uring_memfs`) |
| In-process fsx | `src/posix/tests/CMakeLists.txt:614-700` (`FSX_NUM_OPS 10000`, :617); `run_fsx.sh` | posix client over local backends, NFS3, NFS3-RDMA, NFS4.1 | `rest` leg | 24 | job 3490972: 24 Passed |

**WPTS and pike case maps** (status column counted at d96e33a8; CI gates only on `green`, CMake :202, :495):

| CSV | green | xfail | skip | rows |
|---|---|---|---|---|
| `src/server/smb/tests/wpts/wpts_cases.csv` | 253 | 4 | 10 | 267 |
| `wpts/smb2model_cases.csv` | 2129 | 76 | 262 | 2467 |
| `wpts/fsa_cases.csv` | 185 | 56 | 122 | 363 |
| `wpts/fsamodel_cases.csv` | 154 | 67 | 122 | 343 |
| `pike/pike_cases.csv` | 179 | 7 | 1 | 187 |

Green cases by profile:
- **WPTS MS-SMB2:** base 97, persistent 51, compression 38, multichannel 17, encryption 15, dirlease 15, multichannel_persistent_encryption 5, multichannel_persistent 4, compression_encryption 4, symlink 3, dirlease_persistent 2, dirlease_appinstance 1, multichannel_encryption 1. Also, 21 base cases with `enc_path=1` re-run under encryption (CMake :206-207).
- **SMB2Model:** base 1113, persistent 347, multichannel 329, multichannel_persistent_encryption 236, encryption 104.
- **pike:** base 141, persistent 33, encryption 5.


## Package-level validation (sntest)

The tree also holds Quantum `sntest` suites under `test_suites/`. Unlike every ctest leg above, they install the **built Chimera packages** (`.deb` from the QB release assets) on a test host, start the `chimera` service, and drive it from separate client hosts. That is the closest thing in the tree to a run against an installed product. These are facts only; nothing here changes the proposal.

| Suite file | What it runs | Server configuration | Cite |
|---|---|---|---|
| `test_suites/validation/chimera-smbtorture.snt` | smbtorture from the client host's `smbclient`/Samba packages. It runs 17 `smb2` groups in one pass: read, compound_find, sharemode, delete-on-close-perms, deny, openattr, winattr, sdread, secleak, session-id, connect, bench-oplock, fileid, rename, rw, acls, check-sharemode. A run passes if the result file has no `FAILED` | Its own `/etc/chimera/sntest-chimera.json`: NFS and SMB enabled, 16 threads, one `linux` passthrough mount, one local user | `:50-75` "Create base chimera.json file"; `:116-135` "Create test smb2 group list"; `:141-147` "! grep \"FAILED\"" |
| `test_suites/validation/chimera-linux-tests.snt` | Through the Linux kernel client: cthon04 basic and special over NFSv3 and NFSv4, from a `cthon04.tar.gz` mirror (version not recorded); NFStest `nfstest_posix` over NFSv3 and NFSv4, and `nfstest_interop`; elbencho; and basic and rename checks over SMB | Same pattern, `linux` passthrough mount, 16 threads | `:162-168` "Run Connectathon basic test"; `:173-175` "nfstest_posix"; `:210-212` "nfstest_interop" |
| `test_suites/win_proto/win-proto-test-suite-chimera.snt` | Microsoft FileServer test suite (WPTS) MS-SMB2 cases from a fixed list that is "known to pass when using a Samba server". The working-list run is gated on `RUN_WORKING` | Its own `chimera.json`: `linux` passthrough mount, 16 threads | `:105` "known to pass when using a Samba server"; `:174` "%ifdef RUN_WORKING" |
| `test_suites/validation/chimera-snfs-tests.snt` | StorNext-backed (`snfs`) validation | — | `:50-53` "Install StorNext" |

How they are grouped and run:

- **Two groups.** `acceptance.sng` and `full-suite.sng` group the suites (`test_suites/validation/acceptance.sng:5-8`, `full-suite.sng:5-8`).
- **The acceptance group skips smbtorture.** `acceptance.sng` passes `SKIP_FAIL=1` to the smbtorture suite, and the smbtorture run and its pass check sit inside `%ifndef SKIP_FAIL` (`chimera-smbtorture.snt:140`). So the acceptance group runs no smbtorture tests at all.
- **No CI runs them.** No CI configuration in the tree references `test_suites/` or `sntest` (`.gitlab-ci.yml`, `qb.yaml` and `.github/workflows/`). How often they run, and with which results, is not recorded in the tree, and this document has no evidence of a run.
- **They do not use the shipped config.** None of them runs against the shipped `chimera.json`: each writes its own configuration, with a `linux` passthrough backend.

## Requirements and features

The PRD defines advertised *protocols* but not advertised *features*: see P7 (`docs/product/prd.md:115-119`) and SUC-1 (:390-392). This section states PRD text and code behaviour as fact. Every "advertised" judgement is marked PROPOSAL.

### PRD rows (fact)

| PRD ID (line) | Text (key phrase) | Suites that bear on it |
|---|---|---|
| P7 (:115) | "Every protocol the product advertises complies fully" | all |
| PROTO-2 (:84) and §11 (:416) | "NFSv4.1 parallel NFS (pNFS) is not in the first release" | pynfs pNFS/flex codes (N) |
| MODE-1 (:184) | "Each storage pool has a compatibility mode" | all, per mode |
| MODE-2 (:195-197) | "Multi-protocol mode only refuses requests that the standard allows a server to refuse" | all, both modes |
| MODE-3 (:198) | "A pool created without an explicit mode is multi-protocol" | default mode for the full run |
| ACL-1 (:239) | "NFSv4 ACLs are the canonical, stored permission model" | pynfs ACL\*, pjd `granular` port, smbtorture `smb2.acls*`, WPTS FSA ACL cases |
| ACL-2 (:240) | "SMB security descriptors map to and from NFSv4 ACLs" | smbtorture `smb2.acls*`, `sdread`, `create.nulldacl`; WPTS ACL cases |
| ACL-3 (:241) | "POSIX mode bits are derived from the canonical ACL" | pynfs ACL\*, pjd |
| ACL-4 (:242-245) | "an S3 permission construct with no canonical equivalent is refused" (multi-protocol); "accepted and enforced for S3 only" (native) | Ceph policy and ACL cases, split by mode |
| SEC-1 (:255-257) | "Anonymous or guest access is off unless an administrator turns it on" | Ceph anonymous cases (deny-asserting vs allow-asserting) |
| SEC-3 (:262-264) | "handle that does not exist, or no longer exists, is rejected" | missing bucket/share/export, stale-handle cases |
| SEC-4 (:265-269) | "Every operation that changes data or metadata is checked against the canonical ACL" | S3 silent-200 gap (below) |
| SEC-5 (:270-272) | "No request through any protocol can reach outside its share, export or bucket" | symlink and `..` rows |
| SEC-6 (:273-276) | "Kerberos privacy for NFS, SMB 3 encryption, and TLS for S3" | pynfs krb5p; smbtorture `session.encryption*`; Ceph over TLS |
| SEC-7 (:277-279) | "meets SEC-1 to SEC-6 and SEC-11" | shipped-default conflicts (below) |
| SEC-10 (:290-293) | "Active Directory and Kerberos for SMB and NFSv4" (Proposed) | smbtorture krb5 auth (not run), pynfs GSS |
| HA-5 (:371-376) | "SMB persistent handles, on continuously available shares only" | pynfs reclaim/delegation; smbtorture persistent/CA subset; Ceph multipart |
| SUC-1 (:390-392) | "run against the product's shipping configuration, with no data corruption" | harness deviations (above) |
| SUC-2 (:393-400) | "pjdfstest for POSIX behaviour through FUSE" | suite list |

**Pool mode (fact).** No code or harness implements or sets a pool compatibility mode. A case-insensitive grep for `pool_mode`, `compat(ibility)?_mode`, `multi_protocol` or `"native"` across `src`, `scripts` and `kvm` returns nothing. The Ceph harness config has no mode key (`run_ceph_s3tests.sh:77-92`). So any row that depends on ACL-4 refusal in a multi-protocol pool cannot be tested yet.

### Spec status

| Item | Spec status | Source |
|---|---|---|
| NFSv4.0 RPCSEC_GSS with Kerberos V5 | mandatory to implement | RFC 7530 §3.2.1 / §3.2.1.1 (from spec knowledge; the section is also named in `src/server/nfs/nfs_common.h:842`) |
| NFSv4.1 RPCSEC_GSS / Kerberos V5 | mandatory to implement | RFC 8881 §2.2.1.1 (from spec knowledge) |
| SECINFO_NO_NAME | REQUIRED in 4.1 | RFC 8881 §18.45 and the operations table in §17 (from spec knowledge) |
| Grace period and reclaim | mandatory server behaviour | RFC 7530 §9.6; RFC 8881 §8.4.2 (from spec knowledge) |
| pNFS (LAYOUT\* ops) | OPTIONAL | RFC 8881 §12, §17 table (from spec knowledge); PRD non-goal |
| `fs_locations` / migration, trunking offer | RECOMMENDED attribute / optional | RFC 8881 §11, §2.10.5 (from spec knowledge) |
| NFSv4.2 new operations (ALLOCATE, COPY, SEEK, …) | OPTIONAL | RFC 7862 §13 operations table (from spec knowledge) |
| Granting delegations | server's option | RFC 8881 §10.2 (from spec knowledge) |
| SP4_SSV state protection | **unresolved**: the REQUIRED/OPTIONAL status for servers was not checkable locally. Relevant sections: RFC 8881 §2.10.8.3 (state protection), §2.10.9 (SSV GSS mechanism), §18.35 (EXCHANGE_ID), §18.47 (SET_SSV) | Code fact: Chimera **declines SP4_SSV and negotiates SP4_NONE** (`src/server/nfs/nfs4_proc_exchange_id.c:45`, :81-88; `nfs4_proc_set_ssv.c:16-17`) |
| SMB2 signing | server MUST set `SMB2_NEGOTIATE_SIGNING_ENABLED` | MS-SMB2 §3.3.5.4 (from spec knowledge) |
| SMB1 negotiate carrying SMB2 dialects (upgrade) | handled by an SMB2 server | MS-SMB2 §3.3.5.3 / §3.3.5.3.1 (from spec knowledge) |
| Multichannel, persistent handles, directory leasing, encryption, compression, DFS | optional server capabilities | MS-SMB2 §2.2.4 capability flags, §3.3.5.4 (from spec knowledge) |
| S3 | no formal standard; Ceph s3-tests is the de-facto reference | — |

### Code advertisement (fact)

| Behaviour | Cite |
|---|---|
| SMB dialects 2.0.2, 2.1, 3.0, 3.0.2, 3.1.1 | `src/server/server.c:254-259` |
| SMB signing advertised enabled, not required | `server.c:281`; `src/server/smb/smb_proc_negotiate.c:159` |
| SMB `LEASING` capability advertised on 2.1+; leases and oplocks granted only when enabled (default off) | `src/server/smb/smb.c:155-156`, `smb_proc_negotiate.c:120-121`; `server.c:296-297` |
| SMB `MULTI_CHANNEL` only with ≥1 NIC configured (default 0) | `smb_proc_negotiate.c:130-131`; `server.c:261` |
| SMB persistent handles, directory leases, streams, encryption, compression off | `server.c:265`, :271, :276, :285, :289 |
| SMB Kerberos off (local NTLM only) | `server.c:357-359` |
| SMB1 negotiate without an SMB2 dialect is refused ("we don't support SMB1") | `src/server/smb/smb.c:2297-2301` |
| NFS Kerberos acceptor off; krb5 triples hidden from SECINFO while it is off ("RFC 7530 §3.2.1.1 / §16.31") | `server.c:364`; `src/server/nfs/nfs_common.h:842-850` |
| NFSv4 delegations off; pNFS off; DRC off | `server.c:455`, :382, :463 |
| NFSv4 lease 90 s, grace 180 s | `src/server/nfs/nfs4_lease.h:27-28` |
| S3 unknown subresources (`?policy`, `?encryption`, …) get an empty 200: "security-relevant configs (bucket policy, encryption) are silently NOT enforced" | `src/server/s3/s3.c:1063-1081` |
| Every protocol is off until configured | `server.c:390-393` |

### "Advertised" judgements (PROPOSAL)

- **PROPOSAL:** NFSv3, NFSv4.0, NFSv4.1 and NFSv4.2 are advertised protocols. SMB 2.0.2–3.1.1 is advertised. S3 is advertised. Each is enabled by config. NFSv4.2 is the client's choice; the server accepts it.
- **PROPOSAL:** SMB leasing counts as advertised, because the capability bit is sent on 2.1+ even though no lease is granted by default. OQ-1 must decide whether that bit commits the product to the `smb2.lease*` families.
- **PROPOSAL:** these are not advertised by the shipped default:
  - SMB multichannel, persistent handles, directory leases, streams, encryption and compression;
  - NFSv4 delegations, pNFS and Kerberos;
  - S3 bucket policy and encryption subresources.
  - Of these, SEC-6 (encryption, Kerberos) and HA-5 (persistent handles on CA shares) are PRD-required regardless. That makes them (P) rows, which conflict with the shipped defaults (SEC-7).
- **PROPOSAL:** SP4_SSV stays (O) until its spec status is resolved. If RFC 8881 makes server support REQUIRED, it moves to (M).

## Proposed definition of a full run

**PROPOSAL for the operator's answer to Q-SUC-2.** Nothing in this section is decided.

### Principle

A full run is every test in the pinned **upstream** suite, minus a written exclusion list. It runs against the **shipped default configuration**, changed only by a written list of declared deviations.

**Allowed deviations.** Every deviation is one row: the setting, its scope (all tests, one family, or one subtest) and a justification. There are two kinds:

- **Profile deviations.** These enable a feature the shipped default leaves off. Each is justified by a (P) row (for example HA-5 persistent handles on continuously available shares, SEC-6 encryption, or SEC-1 per-bucket admin enablement) or by an (O) feature the operator advertises under OQ-1 (for example leases, directory leases, multichannel or streams).
- **Spec-permitted tuning (T).** These change a value the protocol leaves to the server, without changing behaviour. Each is listed with its spec citation. Today that means the NFSv4 lease and grace times (5 s / 10 s in the harness, against 90 s / 180 s shipped) and the thread counts (4 core against 16 shipped; 8 sync delegation threads against 1 shipped). The pynfs section gives the table. The shipped thread values are those of the installed `src/daemon/chimera.json` (see "Harness deviations from shipped defaults").
  - **The release-gate full run uses the shipped timers and thread count.**
  - **The per-change gate may use the declared tuning** (OQ-6, gate depth against time).
  - A tuning value that leaves the spec's expectations is not tuning. For example, a grace period shorter than the lease is a deviation to remove.

**Not allowed.** No undeclared deviation. Above all, no `unsafe_async` and no other non-durable backend setting, because SUC-1 requires no data corruption.

**Backend (D2).** This decides which storage backend a full run exercises.

The facts:
- No suite runs against the backend the product will ship. That is the E14
  backend facade, which does not exist yet (`roadmap.md`, E14).
- Today's runs use memfs, linux, io_uring, cairn and diskfs. Every diskfs run
  has `unsafe_async` on (see "Harness deviations from shipped defaults").
- The installed `chimera.json` mounts the `linux` passthrough module and memfs.

The options:
- **(a) memfs only until E14.** Fastest. Tests no persistent storage, and so
  cannot show the absence of corruption.
- **(b) diskfs with durable settings plus memfs until E14.** Exercises a real
  block path with FUA/sync. Slower.
- **(c) the shipped `chimera.json` mounts (`linux` passthrough and memfs).**
  The closest to today's shipped configuration. It tests the host filesystem
  rather than Chimera's own storage.
- **(d) the E14 facade only.** Defers the release gate until E14 exists.

**Recommendation:** run (b) in the interim and switch to (d) once E14 lands.
Keep (c) as a supplementary run of the installed configuration; the
package-level `sntest` suites already do something close to it.

**Pool modes (MODE-2, MODE-3).** Every protocol suite runs in multi-protocol mode, which is the default and is required. S3 also runs in native mode for the ACL-4 rows that are native-only. No pool mode exists in code today, so those rows are "not testable yet".

### Classes

Every row carries one class, whether it is excluded or kept.

| Class | Meaning | Excludable? |
|---|---|---|
| (N) | PRD non-goal | Yes |
| (O) | Optional in the spec, not PRD-required, and not advertised (OQ-1) | Yes, conditional on OQ-1 |
| (M) | Mandatory in the spec of an advertised protocol version (P7, strict reading) | **No.** At most an interim exclusion, tied to a task |
| (P) | Required by the PRD (cite the ID) | **No.** At most an interim exclusion, tied to a task |
| (D) | Suite or Samba defect, or Samba-specific behaviour | Yes, with evidence |
| (L) | Not runnable on the test platform (Linux), with a reason | Yes, with the reason; never counted as a pass |
| (T) | Spec-permitted tuning (a configuration row, not a test exclusion) | Per-change gate only; not in the release-gate run |

### Per suite

The selection details, class counts and per-test lists are in each suite's "Proposed full" subsection. This table summarises them.

| Suite | Selection | Pass criterion | Corruption oracle |
|---|---|---|---|
| pynfs | Upstream `cd4701827a` at 4.0, 4.1@minor 1 and 4.2@minor 2, plus the fork's BIND1–3. Prerequisite: bump the fork to upstream. Classes: (N) pNFS; (O) FSLOC and TRUNK; (P) ACL\*, GSS/krb5p, REBT/reclaim and delegations, as interim exclusions only; (M) SECNN. `PYNFS_WRITE_FULL_SIZES=1`. Shipped timers for the release gate; (T) tuning allowed for the per-change gate | PASS only. NOTSUP fails unless (O) or (N). WARN fails unless listed. OMIT fails unless it is the filter-OMIT of an excluded test. No retry | Partial: READ/WRITE codes read the data back. Otherwise **no corruption oracle** |
| cthon | Real cthon04 over kernel clients on NFSv3 (with NLM), 4.0, 4.1 and 4.2: `./runtests -N 1 -b -t /mnt/test`, then the same with `-g`, `-s` and `-l`. `-f` only for the per-change gate (OQ-6). SMB is an operator option (OQ-3). The in-process port is supplementary; its `nfs3rdma` rows belong to E10 | Exit 0 for each category | Partial: basic and general verify the contents they wrote |
| smbtorture | Every `smb2.*` subtest at the pinned Samba, plus the rpc pipes the product serves. (M) SMB1→SMB2 upgrade; (O) SMB1 `raw.*` and `base.*`; (P) ACL-2, SEC-6 and the HA-5 persistent/CA subset; (D) Samba-specific subtests and the 4 tests that block by design (`tools/smbtorture/regen.sh:259-262`); knob dispositions as in the smbtorture section | `success`. A skip fails unless the subtest is excluded. No retry | Partial: read-back subtests only. Otherwise **no corruption oracle** |
| Ceph s3-tests | All 1053 collected cases, decided per file. Policy, ACL and anonymous cases are split per ACL-4 and SEC-1. Runs over TLS. A node-ID denylist replaces the allowlist | pytest pass. skip and xfail fail unless listed. No retry | Partial: about 316 cases read back data or check checksums |
| pjdfstest | Upstream `tests/**/*.t` over FUSE as root. (L) the 44 files that test non-Linux behaviour. The 27 "would need X" files (25 FreeBSD-fixture errno checks, posix_fallocate/00, rename/22) stay in and count as SKIP until their Linux fixture or gate change exists | TAP `ok` for every assertion. A whole-file skip is a SKIP, never a pass | **None** (metadata and errno only) |
| MinIO warp | v1.8.2: put, get, mixed, multipart, list, delete and stat (+ versioned), **plus `atomic`**. Parameters under D6 | Zero errors and zero `atomic` violations, parsed from the output. Throughput recorded, not gated | **`atomic`**: block-by-block read-after-write content check and ETag-is-MD5 check |

**Cross-suite oracle.** No suite-independent check for data corruption exists today. Only warp `atomic` and the partial read-back cases above observe data.
- **Proposal:** an E1 child for a backend consistency check plus a read-back of a known dataset after each suite run.
- The earlier proposal of a separate E1 child for a warp oracle is dropped, because `atomic` already covers it.

## Findings

These are candidates for E1 children (D8). None is fixed by this document. Evidence for each is in the section named.

### Security-relevant

1. **The pynfs harness enables an unauthenticated REST debug endpoint, and NFSv4 delegations, in every combined run.**
   - `scripts/pynfs_test_wrapper.sh:28` sets `FLAG_ARGS="$*"`.
   - `:85-89` sets `DELEG_ENABLE="true"` when `FLAG_ARGS` matches `*"delegations "*` or `*"deleg "*`. Those patterns also match the deselect tokens `nodelegations ` and `nodeleg `.
   - The combined runs append those tokens for every isolated flag (`pynfs/CMakeLists.txt:48-53`, `:300-307`), so every combined run carries one.
   - With `DELEG_ENABLE=true`, the wrapper writes `"rest_debug_fsops": true` and `"rest_auth_enabled": false` (`scripts/pynfs_test_wrapper.sh:102-105`) and passes `--serverhelper` (`:256-258`). The shipped defaults are `rest_debug_fsops = 0` and `rest_auth_enabled = 1` (`src/server/server.c:250-251`).
   - This contradicts the comment that the combined run "has delegations off" (`pynfs/CMakeLists.txt:295`).
   - The finding is derived from the code and was not observed in CI, which does not log the generated config.
2. **S3 accepts security configuration it does not enforce.** `?policy`, `?encryption` and other unknown subresources get an empty 200 (`src/server/s3/s3.c:1063-1081`). This is an ACL-4/SEC-4 gap; task `s3-unknown-subresources`.
3. **The shipped defaults conflict with SEC-7.** The NFS Kerberos acceptor is off and SMB encryption is off (`src/server/server.c:283-285`). Task `shipped-defaults-safe`.
4. **The Ceph harness aliases the main, alt and tenant keys to one user.** Every cross-user ACL result is therefore meaningless. See "Ceph s3-tests".

### Results that look green but are not

5. **GitLab retries failed test jobs once.** In pipeline 205316, job 3488415 failed on an smbtorture timeout and its retry, job 3488440, passed. See "Pass semantics today".
6. **pynfs counts WARN and NOTSUP as passes, and GitLab keeps no per-code JUnit.**
7. **smbtorture exits 0 on a skip.** 96 `bind_negative` cells and 103 `copy_chunk` cells off memfs pass by skipping. The `copy_chunk` skips follow from `resume_key_support=no` off memfs.
8. **pjdfstest upstream reports a whole-file skip as `ok 1`** (71 files on Linux). 27 of those test behaviour Linux provides but are gated on FreeBSD fixtures or capability lists, so a wired upstream run needs Linux fixtures for them. This only matters once upstream pjdfstest is wired; the in-tree port is unaffected.
9. **cthon04 in KVM runs the `chimera-nas` fork @ `13f4336`.** In that fork, a failing `./stat` no longer fails the `general` category. It also runs in `-f` mode, has no `-l` (lock) run, and mounts NFSv3 with `nolock`.

### Not the shipped configuration

10. **diskfs `unsafe_async: true` is set by 22 harness files**, including the posix, pynfs, smbtorture, S3 and KVM harnesses. No diskfs row proves durability. See "Harness deviations from shipped defaults".
11. **The smbtorture driver turns on persistent handles, leases, oplocks, directory leases and two multichannel NICs for every run**, and sets further per-subtest overrides. It also runs with `samba3=yes`.
12. **pynfs runs with short lease and grace times, 4 core threads (16 shipped) and 8 sync delegation threads (1 shipped).** The combined runs use a 10 s grace with a 90 s lease, so grace is shorter than lease.

### Coverage gaps

13. **pjdfstest never runs over FUSE** (see "No FUSE path"). No test exercises NFSv4 ACLs through FUSE. The port lacks 28 upstream files that run on Linux unchanged, and all 25 FreeBSD-fixture errno files (EROFS, ENOSPC, EMLINK, EXDEV, EBUSY, ETXTBSY).
14. **Ceph s3-tests is not registered in GitLab CI, and GitHub `extended` was not measured.** The system Python lacks boto3: job 3490972 logs "boto3 not found - S3 tests will be skipped". `builders/dev` also lacks `/opt/s3-tests`.
15. **MinIO warp is not in the tree.**
16. **Pool modes (MODE-1) are not implemented**, so ACL-4 refusal in multi-protocol pools cannot be tested yet.
17. **The pynfs fork is behind upstream**: it is missing 12 codes and the slot fixes. Bumping it is a prerequisite of the proposed pynfs full set.
18. **Samba is unpinned** (`builders/dev/Dockerfile:52`).
19. **The package-level `sntest` suites (`test_suites/`) are not wired into any CI in the tree.** They are the only runs against installed packages. The `acceptance` group sets `SKIP_FAIL=1`, which skips the smbtorture run entirely (`test_suites/validation/chimera-smbtorture.snt:140`).

### Stale or wrong in-tree text

20. **The pjdfstest backend-label regex** (`src/posix/tests/CMakeLists.txt:1710-1711`) leaves 727 direct tests without a backend label and labels 147 with the wrong one. The comment at `:1592` is stale, and `extended.yml:827` wrongly says pjd tests "carry both labels".
21. **Ceph netns gate.** The Ceph README and `src/server/s3/tests/ceph/CMakeLists.txt:5-7` claim netns gates registration; it does not.
22. **`docs/design/s3_ceph_compliance.md` is stale** at `:105` ("24%"), `:188` ("198 of the 201") and `:194-195`, which describe a harness that does not exist.
23. **`.gitlab-ci.yml` comments are stale.** They say "1376" pynfs tests (`:297`, `:314`; it is 1377), "1753" smbtorture tests (`:277`; CI runs 1741) and "Nightly" (`:25`; the schedule runs three times a week).
24. **`CMakeLists.txt:866-868` says pynfs combinations are "marked DISABLED"**; nothing does that.
25. **pynfs registers 24 tests twice.** LOCK1, LOCKHELD, LOCKRNG and LKU2 appear under two 4.0 flags on every backend.
26. **Unread config keys.** Nothing reads the `delegation_threads` key that the pynfs and KVM wrappers write.

### Licensing

27. **The `.reuse/dep5` glob `docs/**/*.md` (Unlicense) also matches `docs/product/`**, including this file, which carries a proprietary header. Task `product-docs-reuse-dep5-unlicense`.

## Open questions

All are for the operator. D2 (the backend a full run uses), D4 (pjdfstest through FUSE) and D6 (warp parameters and pass criterion) are presented above as proposals.

- **OQ-1. Advertised-feature matrix.**
  - It governs only the (O) rows and the profile deviations they justify. (P) and (M) rows are not the operator's to make optional.
  - Options: (a) compiled in; (b) on in the shipped default config; (c) announced on the wire; (d) an explicit product feature list.
  - **Recommendation: (d)**, seeded from "Requirements and features".
  - Sub-question: does SEC-9 imply any S3 SSE API?
- **OQ-2. Add WPTS and nfstest to SUC-2?**
  - **Recommendation: yes.** Both already gate the merge train.
  - pike, LTP, the KVM xfstests programs and the FUSE errno test stay supplementary.
  - The package-level `sntest` suites (see "Package-level validation") are a candidate home for runs against the shipped configuration once OQ-8 is settled.
- **OQ-3. Run cthon over SMB?**
  - **Recommendation: no** for SUC-2. Keep the KVM SMB runs as supplementary.
- **OQ-4. Retry policy.**
  - **Recommendation: no retry on test legs**, as in GitHub's `build-test.yml:37-40`.
  - Evidence: job 3488415 → job 3488440.
- **OQ-5. Interim exclusions of (M) and (P) rows.**
  - **Recommendation:** each is named, tied to a task and dated. None is permanent, and none is reclassified as (O).
- **OQ-6. Gate depth against time.**
  - **Recommendation:** the per-change gate uses cthon `-f` and the declared (T) tuning (pynfs timers, thread count). The release gate uses cthon `-t -N 1` and the shipped timers. smbtorture uses a rotating seed per scheduled run.
- **OQ-7. Does a canned public S3 ACL count as "an administrator turns it on" (SEC-1)?**
  - **Recommendation: no.** Enablement should be an administrative per-bucket setting. Allow-asserting anonymous cases then become declared-deviation rows.
- **OQ-8. Profile deviations for the full run.**
  - **Recommendation:** the shipped default plus only (P)-justified deviations, until OQ-1 is answered. These are HA-5 persistent handles on a CA share and SEC-6 encryption sessions.
- **OQ-9. How much of pynfs's delegation coverage HA-5 makes required.**
  - Today every pynfs delegation code is classed (P) under HA-5: 4.0 DELEG ×32, and 13 each at 4.1 and 4.2.
  - HA-5 requires only that delegation *state* survive the loss of a node.
  - Granting delegations is optional in the spec, and the proposed "advertised" matrix lists delegations as not advertised.
  - **Recommendation:** treat it as the smbtorture HA-5 rows are treated. The codes that test delegation state across a restart or reclaim are (P); the other delegation codes are (O), unless OQ-1 advertises delegations, and then they are (M).
