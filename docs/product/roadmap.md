<!--
SPDX-FileCopyrightText: 2026 Quantum Corporation

SPDX-License-Identifier: LicenseRef-Quantum-Proprietary
-->

# Chimera Roadmap: Epic Map and Traceability

| | |
|---|---|
| Status | Outline 0.4. Not yet reviewed. |
| Date | 2026-10-03 |
| Owner | John Suykerbuyk |
| Answers to | [Product requirements](prd.md) (draft 0.3), [backend storage contract](storage-contract.md) (draft 0.2) |
| Read against | `develop` @ `22f6b54f`, with the PRD and contract changes of 2026-10-03 not yet committed |

This document turns the PRD and the backend storage contract into a
delivery plan. It is an outline: the epic map, the dependencies between
epics, the waves those dependencies produce, and where every PRD
requirement and contract decision is delivered. Scope and exit criteria
per epic come in the next pass.

Planning is rolling-wave. Only wave 1 is broken down into tasks. A later
wave is broken down when it is next.

Epic IDs (`E1`, `E2`, ...) are stable. Change an epic's text in review; do
not renumber. A dropped epic is marked withdrawn, and its ID is not reused.

## 1. Epic map

| ID | Epic | Wave | Depends on | Mostly serves |
|---|---|---|---|---|
| E1 | Protocol compliance gate: the SUC-2 suites run in full on every change, with zero corruption. Absorbs CHIM-230 and CHIM-231. | 1 | none | SUC-1, SUC-2, P7 |
| E2 | Security baseline on today's code | 1 | none | SEC-1 to SEC-7, D11 |
| E3 | Backend contract interface and conformance suite | 1 | none | P1, D5, D12 |
| E14 | Backend facade prototype on linear 4K block media (loopback file of at least 1 GB) | 1 | E3 (contract interface only) | LAY-4, D9 data verification, `DATA_UNRECOVERABLE` |
| E4 | Namespace module over the contract | 2 | E3, E14 | D1, D4, LAY-1 to LAY-3, NS-1 to NS-3 |
| E10 | NFS over RDMA qualification | 2 | E1 | PROTO-1, O4 |
| E5 | Cross-protocol visibility, mutability and compatibility modes | 3 | E2, E4 | D2, D3, MODE, MUT, NS-4 |
| E6 | Recall and stubs | 3 | E4 | D6, RCL, TIER-3, TIER-5, TIER-9 |
| E7 | Policy and quota | 3 | E4 | D7, D13, POL-1 |
| E8 | Cluster: fencing, cluster manager, persisted protocol state | 3 | E3, E4 | D10, HA-1 to HA-5 |
| E13 | Canonical ACL and identity | 3 | E2, E4 | P4, ACL, SEC-10 |
| E9 | Snapshots, retention, protection and encryption at rest | 4 | E4, E7 | D8, D9, SEC-9, TIER-8 |
| E11 | Tier-facing spokes: tape, HDD, cloud | 4 | E3, E6 | P5, P8, TIER-1, TIER-6, TIER-7 |
| E12 | Single administrative pane | 5 | E7, E9, E11 | P6, POL-2, SEC-8 |

**E14 is our own backend facade (LAY-4), in its first, prototype form.**

- **What it is.** It implements the contract above a media interface of
  linear 4K block addresses. The media is a loopback file of at least 1 GB.
- **What it owns:**
  - block allocation;
  - object identity;
  - durability (D5 levels and epochs);
  - namespace-record storage and transactions;
  - reference counting and garbage collection;
  - data verification (D9), which raises `DATA_UNRECOVERABLE`.
- **What it is not: the hot hub.** The real hub's backend design and
  hardware are not yet defined (contract O15). E14 must not be described
  as the hub, and it does not fix the hub's internals.

**E14 replaces outline 0.1's "reference backend" in E3.** Both had the
same two purposes: run the conformance suite, and let E4 be built and
tested before anything else implements the contract. E14 serves both,
and it is real product code rather than a throwaway.

The one need it does not cover by default is power-loss testing, which
must drop unflushed writes on demand. That need is met inside E14, by a
crash-simulating layer at the media interface. A separate in-memory double
is not planned. Add one to E3 only if the suite needs fault injection that
the media layer cannot provide.

## 2. Dependency graph

Each epic is shown with the epics it depends on, wave by wave:

```
wave 1   E1            E2            E3      E14 <- E3 (interface only)
           |             |             |      |
wave 2   E10 <- E1                   E4 <- E3, E14
                                       |
wave 3   E5  <- E2, E4    E6  <- E4    E7  <- E4
         E8  <- E3, E4    E13 <- E2, E4
                                       |
wave 4   E9  <- E4, E7    E11 <- E3, E6
                                       |
wave 5   E12 <- E7, E9, E11
```

Edges, as a list:

- E1 → E10.
- E2 → E5, E13.
- E3 → E4, E8, E11, E14 (E14 needs only E3's contract interface task, so
  both are in wave 1; see section 3).
- E14 → E4.
- E4 → E5, E6, E7, E8, E9, E13.
- E6 → E11.
- E7 → E9, E12.
- E9 → E12.
- E11 → E12.

## 3. Waves

A wave is a dependency level: every epic in a wave depends only on epics
in earlier waves. How many epics in one wave run at once is a staffing
decision, not set here.

- **Wave 1: make today's code trustworthy, fix the contract, and start
  our backend.**
  - **E1, E2 and E3** apply to the code as it stands and need nothing else
    first.
  - **E1 comes first** because SUC-1 is the PRD's first measure of success.
  - **E2** closes the security findings in today's defaults and code paths.
  - **E3** fixes the contract in code, so the namespace module has
    something to be built against.
  - **E14** builds the prototype backend facade behind that contract.
- **E14's place in wave 1 is an exception** to the wave rule. It depends
  on E3, but only on E3's first task, the contract interface. Its media,
  allocation, durability and verification work needs nothing from E3. The
  edge is held at task level (`backend-facade-prototype` children depend
  on `contract-interface-v1`).
- **The alternative is to put E14 in wave 2.** That pushes E4, and every
  wave after it, one wave later.
- **Wave 2: the new namespace layer, and the first-ship fabric.** E4
  builds the namespace module on E3's contract and runs it against E14. E10 qualifies the NFS over
  RDMA transport that already exists in the tree (`rdma` in
  `docs/configuration.md`) under E1's gate.
- **Wave 3: the PRD's namespace semantics.** E5, E6, E7, E8 and E13 each
  need the namespace module.
- **Wave 4: E9 and E11.** E9 applies retention and protection, which come
  from policy (E7). E11 adds the tier-facing spokes, which need stubs and
  recall (E6).
- **Wave 5: E12.** The single administrative pane covers policy,
  protection and spokes, so it comes last.

Changes from the starting map in the `roadmap` task:

- **RDMA moved from "later" to wave 2.** PROTO-1 settled Q-CUS-3, and NFS
  over RDMA is already in the code.
- **Tape and spokes moved from "later" to wave 4.** Q-CUS-2 is settled
  (hub and spokes). PRD section 2.1 still requires senior-management
  sign-off for defining the spokes from release 1. Until that is given, E11
  carries the risk.
- **E14 added (outline 0.2), and E3 narrowed to the contract interface and
  the conformance suite.** The backend is ours to build (LAY-4), so its
  prototype is an epic of its own.
- **Two epics added.**
  - **E13, canonical ACL and identity:** no epic in the starting map
    covered ACL-1 to ACL-5.
  - **E10:** the starting map's "RDMA fabric protocols", now scoped to
    PROTO-1.
- **Recall moved behind the namespace module.** The ticket model (D6) is a
  namespace-module behaviour, so recall depends on E4.
  - The head-of-line stall that RCL-3 forbids (as-is H27) exists today. It
    is assigned to E6, not to wave 1. Pull it forward if it shows up as an
    E1 failure.

## 4. Traceability

Each item is assigned to exactly one epic. "Also" names an epic that
depends on the item without owning it.

### 4.1 PRD requirements (draft 0.3)

| Requirement | Epic | Notes |
|---|---|---|
| P1 | E3 | |
| P2 | E4 | |
| P3 | E5 | |
| P4 | E13 | |
| P5 | E11 | also E5 (D2 whole-object replace) |
| P6 | E12 | also E7 |
| P7 | E1 | |
| P8 | E11 | |
| PROTO-1 | E10 | no compliance suite yet; see section 5.2 |
| PROTO-2 | none | a non-goal for release 1 (operator, 2026-10-03), and a design constraint: nothing may preclude flex-files pNFS later. Checked in E4's and E14's design reviews, like HA-6. See section 5.4 |
| LAY-1, LAY-2, LAY-3 | E4 | |
| LAY-4 | E14 | for the prototype facade; the hot hub's design is O15 |
| NS-1, NS-2, NS-3 | E4 | |
| NS-4 | E5 | |
| MODE-1 to MODE-4 | E5 | |
| MUT-1 to MUT-7 | E5 | MUT-4 extends E2's single check (D11) |
| MUT-8 | E4 | the namespace events that carry it |
| ACL-1 to ACL-5 | E13 | |
| SEC-1, SEC-2, SEC-3, SEC-5, SEC-6, SEC-7 | E2 | |
| SEC-4 | E2 | |
| SEC-8 | E12 | auditing administrative actions |
| SEC-9 | E9 | blocked on Q-SEC-2 |
| SEC-10 | E13 | blocked on Q-SEC-1 |
| SEC-11 | E2 | the REST control plane today (H18); also E12 (the administrative pane enforces it too) |
| TIER-1 | E11 | |
| TIER-2 | E4 | also E14 (stores every object's metadata on its media) |
| TIER-3 | E6 | |
| TIER-4 | none | withdrawn in draft 0.2 |
| TIER-5 | E6 | |
| TIER-6, TIER-7 | E11 | |
| TIER-8 | E9 | |
| TIER-9 | E6 | |
| RCL-1 to RCL-4 | E6 | |
| POL-1 | E7 | |
| POL-2 | E12 | |
| POL-3 | E6 | recall latency is the only effect users see |
| HA-1 to HA-5 | E8 | failover time blocked on Q-HA-2 |
| HA-6 | none | a design constraint owned outside this roadmap (operator, 2026-10-03); see section 6 |
| SUC-1, SUC-2 | E1 | "full" blocked on Q-SUC-2 |

### 4.2 Contract decisions

| Decision | Epic | Notes |
|---|---|---|
| D1 | E4 | |
| D2, D3 | E5 | D3's stubbed-object clause is exercised once E6 lands |
| D4 | E4 | also E14 (provides the transactions over namespace records) |
| D5 | E3 | the rule and its power-loss test; also E14 (implements the levels and the epoch), and E1 (zero corruption) |
| D6 | E6 | |
| D7 | E7 | |
| D8 | E9 | |
| D9: protection classes and encryption at rest | E9 | |
| D9: data verification | E14 | for the prototype; added in contract draft 0.2 |
| `DATA_UNRECOVERABLE` (section 11) | E14 | raised when verification fails and the data cannot be repaired; also E4 (maps it to the VFS I/O error, which the facades map per protocol) |
| D10 | E8 | also E3 (`attach`, `renew` and `fence` are contract operations) |
| D11 | E2 | the single check in the VFS core; E5 adds mutability and pool mode to it |
| D12 | E3 | |
| D13 | E7 | |

### 4.3 Contract open issues

| Issue | Epic | State |
|---|---|---|
| O1 | E5 | decided (MUT-5) |
| O2 | E8 | decided (HA-5) |
| O3 | E9 | open; Q-SEC-2 |
| O4 | E10 | open; PROTO-2 excludes block and SCSI pNFS layouts for this reason |
| O5 | E3 | open; decides whether E11's contract operations are in version 1 |
| O6 | E5 | decided (MODE-3, MODE-4) |
| O7 | E3 | open; with E1 and Q-SUC-2 |
| O8 | E7 | decided |
| O9 | E7 | open |
| O10 | E5 | open |
| O11 | E4 | open |
| O12 | E3 | open |
| O13 | E9 | open |
| O14 | E8 | open; Q-HA-2 |
| O15 | none | open; the real hot hub is not defined (see section 6). E14 owns only the sub-question of one address space per namespace, for the prototype |

### 4.4 As-is findings

These are from the internal review (vault `doc/as-is/ISSUES.md`, H1 to
L42). "Moot" means the defect is only in a module that D1 keeps for
development, test or gateway use, so it does not carry into the product.
Its lesson is still an input to the named epic.

| Epic | Findings |
|---|---|
| E1 | H20, H25, H29, L42; CHIM-230, CHIM-231 |
| E2 | H7 (shipped default config), H11, H12, H13, H14, H15, H16, H18, H19, H26, M37 |
| E3 | H3 and H6 (the contract gaps they expose: no write verifier; no durability rule for namespace operations and copies) |
| E4 | M35 |
| E5 | H21, H22, H23, H24, M36 |
| E6 | H27 |
| E8 | M34 |
| E13 | M38 |
| Moot under D1 | H1, H2, H4, H5, H8, H9, H10, H17, H28, H30, M31, M32, M33, L41 |
| No epic | L40 (section 6) |
| Iceboxed | M39, with its task `pnfs-ds-file-lifecycle`; pNFS is out of release 1 (section 5.4) |

Notes:

- **The starting map named H11 to H17 for E2.** H17 is in the linux and
  snfs passthrough handle encoders, so it is moot unless a passthrough
  gateway ships. If one does, H17 moves to E2.
- **E2's actual list runs past H17.** It includes H18 and H19 (REST and S3
  authorisation), H7 (the installed default config), H26 and M37.
- **Lessons from moot findings:**
  - H1 and H10 feed E3's power-loss test.
  - H2 (inode reuse) feeds E14's object identity. The contract makes
    object IDs the backend's.
  - H4, H5, M31 and L41 feed E14's log, allocation and on-media format.
    They are diskfs's redo-log, ordering, sync-reporting and upgrade
    defects, and E14 builds the same kind of layer.
  - M32 feeds E4's name matching.

## 5. Placements the review asked for

### 5.1 CHIM-230 and CHIM-231

Both are in **E1**. They have been on hold since 2026-10-03 until the PRD
exists.

- **CHIM-231 (FUSE opendir ESTALE).** This is the known intermittent
  failure in `fuse/mbt/strict_memfs` and `batch_memfs`. A full run is not
  clean while it is open, so E1 cannot reach its exit criterion without
  it.
- **CHIM-230 (NFSv4 READ count not clamped).** This is a protocol
  correctness fix. By code reading, an oversized READ can abort the server,
  so it is also a remote crash. Its owner stays E1; E2 depends on it.

### 5.2 PROTO-1 has no compliance test

None of the SUC-2 suites tests NFS over RDMA:

- pynfs runs over TCP only.
- The tree does register cthon, fstest and POSIX tests named `nfs3rdma`
  (`src/posix/tests/CMakeLists.txt:559-580, 923-940`). Read from the CMake,
  not run: those tests set `CHIMERA_TEST_TRANSPORT` to `tcp` or `inproc`.
  They exercise the NFSv3 client path, not an RDMA transport.
- There is no NFSv4.x over RDMA test.

E10 owns closing this gap. **Decided 2026-10-03 (operator):** the real test
for PROTO-1 is a real NFS over RDMA mount. The options for running it
(for example soft-RoCE or hardware, and which suites run over the mount)
are re-evaluated when E10 starts.

### 5.3 Open questions that still block

| Question | Blocks | Earliest wave affected |
|---|---|---|
| Q-SUC-2 (which tests make up a full run; the provisional suite list) | E1's exit criterion; O7 | **1** |
| Q-SEC-1 (identity sources) | SEC-10 in E13 | 3 |
| Q-HA-2 (acceptable failover time) | E8's exit criterion; O14 | 3 |
| Q-SEC-2 (key supply and withdrawal) | SEC-9 in E9; O3 | 4 |

Q-SUC-2 is the only open question that blocks a wave 1 epic. E1 can start
running the suites without it, but cannot be declared done.

### 5.4 pNFS (PROTO-2)

**Decided 2026-10-03 (operator): pNFS is not in release 1.** PROTO-2 is a
non-goal and a design constraint.

- **Nothing may preclude flex-files pNFS later.** Its data servers would be
  Chimera nodes, so client I/O stays behind the backend facade.
- **Block and SCSI layouts are excluded outright.**
- **Q-PNFS-1 is deleted.**

**Where the constraint is checked.** Like HA-6, PROTO-2 has no delivery
epic. It is checked in E4's and E14's design reviews: layout state must be
storable as namespace records, and the data path must allow Chimera nodes
to act as flex-files data servers.

**Epic ID E15 is reserved** for the vault epic `pnfs-under-contract`,
which is iceboxed. M39 is iceboxed with its task `pnfs-ds-file-lifecycle`.

**Not in E1's gate.** pNFS is not advertised, so the in-tree KVM pNFS
suites (`kvm/kvm_pnfs_*_test_wrapper.sh`) and pynfs's flex-files tests are
not part of E1's gate.

**Analysis for a later release.** If flex-files pNFS is later brought in,
E1-style compliance alone will not be enough, because pNFS in the product
is a design problem:

- **Today's pNFS breaks LAY-2.**
  - Today, pNFS redirects a file's data operations, at the VFS level, to
    data-server backing files that Chimera chooses
    (`src/vfs/vfs_pnfs.h`, `chimera_vfs_pnfs_dispatch`).
  - Choosing where bytes live is a placement decision, and LAY-2 assigns
    placement to the backend.
- **Under the contract, a layout must describe where the backend facade
  holds an object's data** (E14). That raises four questions:
  - **Layout state.** Where it is kept, as namespace records (E4).
  - **Recall.** Layouts are client caches of where data lives. They must be
    recalled when an object is replaced or sealed (D2, D3), when another
    protocol's change completes (MUT-5), and when data moves to a spoke
    (E6).
  - **The data path.** Flex-files data servers are Chimera nodes reading
    objects through the contract, so D9 verification holds.
  - **Failover.** How layouts are fenced and survive failover (D10, E8).
- **That work needs E4 and E14,** so it would be a wave 3 epic at the
  earliest.

Block and SCSI layouts are excluded because they hand clients the backend
facade's block addresses. Direct access bypasses Chimera's data path (the
O4 question), bypasses D9's verification, would force E14 to expose its
extent maps, and fences clients with SCSI persistent reservations
alongside D10.

## 6. Gaps

**Requirements with no epic:**

- **HA-6** (each node carries at least 16 NVMe drives of 15 TB).
  **Decided 2026-10-03 (operator):** HA-6 is a design constraint, owned
  outside this roadmap, and has no epic. There will be no hardware until
  long after the software stack is proven. HA-6 gives only a basic concept
  of the hardware, so that the design does not break at hardware
  integration time. Each epic's design review checks against it.

**Other gaps:**

- **The production backend: closed.** Decided 2026-10-03 (operator; PRD
  LAY-4): we design and build the backend ourselves, as our own facade
  behind the contract.
  - **E14** is its prototype, on a loopback file of linear 4K blocks.
  - **The real hot hub,** both its backend design and its hardware, is not
    yet defined (contract O15). It has no epic until it is defined. When it
    is, it gets an epic that builds on E14's facade.
- **One finding has no epic.** L40 is stale comments. Fix them alongside
  the work in each file.
- **H18 and H29 are placed (operator, 2026-10-03).**
  - H18 is in E2, under the new SEC-11.
  - H29 is in E1, as a crash a compliance suite would find.
- **Two findings have no ID.** SYNTHESIS section 8 describes two
  cross-protocol permission findings that are not in the ISSUES ledger:
  - an SMB owner opening a file that has no ACL (E2 or E13);
  - an open-cache grant serving another protocol (E5 or E2).

  They need IDs before they can be assigned.
- **Overlaps resolved by choice.** These findings could belong to either of
  two epics:

  | Finding | Owner | Alternative |
  |---|---|---|
  | H20 | E1 | E9 |
  | H25 | E1 | E8 |
  | H26 | E2 | E5 |
  | M35 | E4 | E5 |
  | M37 | E2 | E5 |

  The table above records the choice. A reviewer may move any of them.

## 7. PRD changes this roadmap needs

These are listed for the operator. They are not made here.

1. **Section 13 (current state) should note the NFS over RDMA test gap**
   in section 5.2, alongside the other current-state findings.

**Contract changes this roadmap needs**, also for the operator:

1. **Optional: the PROTO-2 constraint could be recorded in the
   contract.** Section 5.1 could note that the namespace records must be
   able to hold pNFS layout state later, beside NFSv4 state, so that
   flex-files pNFS stays possible. Contract draft 0.2 has no pNFS item.

Already in PRD draft 0.3:

- outline 0.1's SUC-2 item (the NFS over RDMA mount);
- outline 0.1's HA-6 item (a design constraint);
- outline 0.2's "Facade" glossary item (both senses).
