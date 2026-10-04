<!--
SPDX-FileCopyrightText: 2026 Quantum Corporation

SPDX-License-Identifier: LicenseRef-Quantum-Proprietary
-->

# Storage Contract Version 1: C Interface

| | |
|---|---|
| Status | Companion to the contract draft. Positions marked PROVISIONAL await operator review. |
| Date | 2026-10-04 |
| Interface | `src/vfs/contract/` (`chimera_sc.h` and the headers it includes) |
| Read against | [Backend storage contract](storage-contract.md) draft 0.2; [product requirements](prd.md) draft 0.3 |

This document explains how the C interface in `src/vfs/contract/` maps the
[backend storage contract](storage-contract.md) onto code. It also records
where the interface had to take a position the contract does not yet state.
It does not change the contract. The provisional items S1 to S7 and S10 to
S13 are listed for the operator to fold into a later contract draft, or to
reject. S8 and S9 are deferred to a minor version (section 5).

## 1. What the interface is

- **The in-process binding of contract version 1, one backend instance per
  Chimera node.**
  - The namespace module (E4) loads a backend descriptor in-process. So
    does the conformance harness.
  - Requests are dispatched to it on that node's event-loop threads.
  - A wire protocol to a remote backend is out of scope (S10).
- **Not the VFS module SDK.** `src/vfs/sdk/` is POSIX-shaped and serves
  the development, test and gateway modules (contract D1). The contract
  headers include nothing from the SDK or from Chimera internals, only libc.
  The `sc_selfcontained` build target proves this: it compiles each header
  with its directory as the only include path.
- **Licence.** The headers, tests and build files are MIT, so that a backend
  under any licence can implement them.

The interface has three parts:
- **A request structure** with an opcode and a tagged argument structure
  per operation.
- **A backend descriptor**: `init`, `thread_init`, `dispatch` and
  `set_upcalls`. Its shape follows the module SDK.
- **Two version concepts:**
  - an ABI version for the C layout, refused on mismatch at registration;
  - the contract's semantic version, negotiated at `ATTACH` (D12).

The rules for threading:
- `dispatch` never blocks.
- A request completes on the thread that dispatched it.
- Many requests may be in flight per thread.
- The backend does not touch a request after completing it.

The ABI rules:
- Enumerated values travel in `uint32_t` fields.
- No structure has implicit padding. Every gap is an explicit `reserved`
  field that must be zero.
- The test suite pins every value and the LP64 layout, so any change fails
  to build until `CHIMERA_SC_ABI_VERSION` is bumped.

**Lease time base.** The contract makes the fence epoch a lease (6.5, D10)
without naming a clock.
- `ATTACH` and `RENEW` return `r_lease_ns`: a duration, counted from the
  moment the caller dispatched the request.
- The caller measures it on its own `CLOCK_MONOTONIC` and must renew before
  it runs out.
- The backend never expires the epoch sooner than that duration after it
  completed the request. The caller's deadline is therefore never later
  than the backend's, and no clock is shared between them.

## 2. Contract-to-interface map

| Contract item | Interface construct |
|---|---|
| D1 | a sibling of the module SDK with libc-only includes; proved by `sc_selfcontained` (own directory as the only include path, `-pedantic`, `-Wpadded`) and a configure-time check of every `#include` |
| 4.1 identity, extents | `struct chimera_sc_obj_id` (16 bytes, never reused), `struct chimera_sc_extent` |
| 4.1 references | `CREATE`, `REPLACE` and `DELETE` deltas; `obj_meta.refs`; holds on never-referenced objects only (S13) |
| 4.2 states | `enum chimera_sc_obj_state`; `OBJ_SEAL`; `CHIMERA_SC_E_STATE` |
| 4.3 / D2 | `OBJ_CLONE_WORKING` (epoch-held, inherits stamps and retention); `HOLD` before any durability acknowledgement ahead of the seal; `REPLACE` with `UNHOLD` at the seal point; `REPLACE` never recalls |
| 4.3 / D3 | a pending binding is a `CREATE` with a record other protocols cannot see |
| 5.1 | opaque record keys and values (`struct chimera_sc_bytes`); `struct chimera_sc_obj_meta` |
| 5.2 / D4 | `NS_TXN`: compares, puts, deletes and deltas commit or fail together; per-delta preconditions are checked in order, the never-both-referenced-and-held invariant and quota net; `r_failed_part` and `r_failed_index` name the failure |
| 6 | `struct chimera_sc_caller` (node identity and fence epoch) in every request |
| 6.1 | `OBJ_CREATE`, `OBJ_CLONE_WORKING`, `OBJ_READ`, `OBJ_WRITE`, `OBJ_FLUSH`, `OBJ_TRUNCATE`, `OBJ_ALLOCATE`, `OBJ_PUNCH`, `OBJ_SEAL`, `OBJ_STAT`; plus `OBJ_MAP` (S3) and `OBJ_DISCARD` (S13) |
| 6.2 | `NS_GET`, `NS_SCAN` (stable cursor), `NS_TXN`, `NS_WATCH`; plus `NS_UNWATCH` (S2) |
| 6.3 | `CREATE`, `REPLACE`, `DELETE`, `SEAL`, `RETAG`, `RECALL_HINT`; plus `HOLD` and `UNHOLD` (S13) |
| 6.4 | `RECALL`, `RECALL_WAIT` |
| 6.5 | `ATTACH`, `RENEW`, `FENCE`; plus `DETACH` (S2) |
| 11 | the six domain errors, with the protocol mapping table in `sc_error.h` |
| D5 | write level, achieved level, durability epoch on every write, flush and size change |
| D6 | `CHIMERA_SC_E_RECALL_PENDING` with a ticket; `RECALL_WAIT` parks without a thread; non-blocking dispatch; completion on the dispatching thread |
| D7 | policy-set stamp; no path anywhere in the interface |
| D8 | `CHIMERA_SC_E_RETENTION_LOCKED`; `obj_meta.retention`; no snapshot operation (S5); the collection rule clarified for never-referenced objects (S13) |
| D9 | protection classes declared at `ATTACH`; `CHIMERA_SC_E_DATA_UNRECOVERABLE`, `CHIMERA_SC_E_KEY_UNAVAILABLE` |
| D10 | fence epoch per node; `CHIMERA_SC_E_FENCED` |
| D12 | ABI version, semantic version, `chimera_sc_version_negotiate()`, four capability bits |
| D13 | quota domains; S12 charging, checked net per transaction; `CHIMERA_SC_E_QUOTA_EXCEEDED`, `CHIMERA_SC_E_NO_SPACE`; `CHIMERA_SC_CAP_ESCROW_QUOTA` |

## 3. Positions on open issues

- **O5. Stub, recall and spoke operations are mandatory in version 1.**
  - `RECALL`, `RECALL_WAIT`, the `stubbed` state and
    `CHIMERA_SC_E_RECALL_PENDING` are required of every backend.
  - The `CHIMERA_SC_CAP_SPOKES` capability declares only whether objects
    can become stubbed.
  - A backend without spokes answers `RECALL` with a ticket that completes
    at once.
- **O12. Holes.**
  - A hole is an unallocated extent, and it reads as zeros.
  - `OBJ_PUNCH` creates a hole.
  - `OBJ_ALLOCATE` reserves space, which `OBJ_MAP` reports as unwritten.
  - A stub keeps the object's hole map.
- **O11 (hard links) is not addressed in version 1.0.** An object has at
  most one reference (section 5, S8).
- **No interface construct for O4, O9, O13 or O14.**
  - O4: there is no direct-I/O operation.
  - O9: there is no quota-mode field.
  - O13: `OBJ_SEAL` never returns `CHIMERA_SC_E_RETENTION_LOCKED`.
  - O14: the `FENCE` comment restates D10's failover order.

## 4. Provisional additions

Each item is marked `PROVISIONAL` in the headers. None of them contradicts
draft 0.2.

| ID | Addition | Why the interface needs it |
|---|---|---|
| S1 | Generic errors: `FENCED`, `VERSION`, `NOT_FOUND`, `STATE`, `COMPARE_FAILED`, `INVALID`, `NOT_SUPPORTED`, `IO` | The operations of section 6 fail for reasons section 11 does not list |
| S2 | `DETACH` and `NS_UNWATCH` | A node needs a clean way to end its epoch, and a watch needs a way to end |
| S3 | `OBJ_MAP` (data, hole, unwritten segments) | SEEK_DATA, NFS READ_PLUS and SMB allocated-range queries need the allocation map, and `OBJ_STAT` cannot return an unbounded map |
| S4 | `NS_TXN` compares by record revision; revision 0 means "does not exist" | Makes compare-and-set exact and cheap |
| S5 | No snapshot operation | Snapshots (D8) are taken through the administrative plane, not by Chimera |
| S6 | Checksum algorithm and width in object metadata: `CHECKSUM_MD5` (1) defined, values from 256 up backend-private | S3 ETags and Content-MD5 need to know them; the contract does not name one |
| S7 | `OBJ_CREATE` carries the stamps | Byte quota is charged at extent allocation (D13), before the binding's `CREATE` delta commits |
| S10 | Deployment shape: an in-process binding per node, with inter-node coordination behind the interface | The contract does not say whether the backend is a linked library or a service. That choice is deferred, because it shapes E14 and E8. |
| S11 | Bytes the backend returns are valid only until the request completes | An asynchronous interface that returns borrowed memory needs a lifetime rule. This is the module SDK's own rule for returned values. |
| S12 | Object count is charged at the first reference and released at the last, checked net per transaction. A working version is charged for bytes on copy-on-write. | Makes a full overwrite (`REPLACE`) never fail at an exact limit, and stops a clone being charged twice |
| S13 | The object lifecycle: epoch hold, hold flag (`HOLD`/`UNHOLD`), `OBJ_DISCARD`, and released objects returning `NOT_FOUND` | Below |

**S13 in detail.** S13 has four parts.
- Items 1 and 4 resolve an ambiguity and a silence in draft 0.2.
- Items 2 and 3 are additions:
  - item 2: the `HOLD` and `UNHOLD` delta kinds and `OBJ_DISCARD`;
  - item 3: Chimera's obligation to hold an object before a pre-seal
    durability acknowledgement.

Contract D2 has Chimera create a new `open` object and
`REPLACE` it in at the seal point. Between those two moments the object has
no reference.

- **Read literally, 4.1 and D8 would let a backend collect it then.**
  - 4.1: "An unreferenced object belongs to the backend: it is either
    garbage-collected, or retained because a snapshot or retention policy
    still pins it".
  - D8: "An object is collected when no binding, snapshot or retention lock
    references it".
- **Every text that speaks to collection is framed around an object that
  *became* unreferenced.**
  - D2: "The old object becomes unreferenced, and the backend may
    garbage-collect it".
  - PRD MUT-8: "When an object is deleted or replaced … Whether its storage
    is reclaimed … is the backend's decision".
- **S13 therefore clarifies** that 4.1 and D8 govern objects that were
  referenced. An object that has never been referenced is kept:
  - while its creator's fence epoch is live (the epoch hold);
  - or while Chimera has set its hold flag.
- **Chimera holds only two kinds of object:**
  - D2 working versions;
  - full-overwrite replacements: `O_TRUNC`, SMB OVERWRITE or SUPERSEDE,
    truncate to zero, and S3 PUT or multipart over an existing key.
- **When it must hold.** Chimera holds such an object before giving any
  durability acknowledgement for it ahead of its seal point, so that the
  data survives the loss of the node that wrote it (D5, HA-3). Holding
  `DELETE`d or `REPLACE`d objects is not allowed: once referenced, an
  object's fate after its last reference is the backend's, as 4.1, D8 and
  MUT-8 say.
- **Released objects return `NOT_FOUND` to every Chimera operation.** This
  covers an object whose last reference dropped, and a never-referenced
  object whose epoch ended without a hold. The contract is silent here. The
  rule is observationally the same as the immediate collection 4.1, D2 and
  MUT-8 already permit, made deterministic.

## 5. Deferred to a minor version

Operations, delta kinds and errors are enumerated, and capabilities are
negotiated, so a later minor version can add these without breaking 1.0.

- **S8, hard links.**
  - **What is deferred:** a `LINK` delta, the cross-device rules for links
    and for renames of multiply-linked files, and object-count rules for
    objects with more than one reference.
  - **The reasons:** the PRD has no hard-link requirement, and O11 is open.
    With at most one reference per object, `REPLACE` is always net zero for
    quota, so a seal point can never fail on quota.
  - **The 1.0 rule:** `CREATE` or `REPLACE` naming an already-referenced
    object fails with `CHIMERA_SC_E_STATE`. That rule stays true when
    `LINK` arrives.
  - **When S8 returns:** a RETAG belongs in the transaction that brings an
    object back to one reference, so its stamps follow the surviving
    binding.
- **S9, cancelling a recall wait.**
  - **What is deferred:** a `RECALL_WAIT_CANCEL` operation.
  - **Why it is not needed:**
    - NFS and S3 answer a recall-pending request at once and let the client
      retry, so they park nothing in the backend (D6).
    - SMB's interim `STATUS_PENDING` requests can share one Chimera-side
      waiter per ticket (section 6). A client that gives up leaves only
      Chimera's table. The backend wait ends when the recall does, which
      D6 guarantees.
  - **When to revisit:** add the operation if E14 measurements show a need.

## 6. Guidance for the namespace module

None of this is normative for backends.

- **One `RECALL_WAIT` per ticket.** Every protocol request waiting on the
  same recall hangs off one Chimera-side waiter, and that waiter issues a
  single `RECALL_WAIT`. Backend resources are then bounded by the recalls
  in flight, not by the number of clients.
- **Unlink while open.**
  - **The unlink.** Unlinking a file that is still open does not `DELETE`
    its object. The unlink transaction moves the binding to a
    Chimera-private orphan record that names the owning node. That is a
    record put and a record delete only: no delta, the reference count stays
    1, and there is no RETAG.
  - **The last close** commits the `DELETE`. Object count stays charged
    while the file is open but unlinked, which matches POSIX freeing space
    at last close.
  - **Cleanup:**
    - **Persisted opens** (NFSv4 per D10 and HA-5; SMB on continuously
      available shares per O2): after the owning node is fenced or its epoch
      expires, protocol grace and reclaim on the surviving node decide. A
      sweeper `DELETE`s the orphans whose opens were not reclaimed.
    - **Non-persisted opens** (FUSE; SMB on other shares) die with their
      node. Any node's sweeper `DELETE`s the orphans of a node whose epoch
      has ended. A restarting node sweeps its own previous orphans after
      its new `ATTACH`.
    - **NFSv3** keeps no open state on the server. Its clients rename an
      unlinked open file themselves, so no orphan arises.
- **Consequences of released objects returning `NOT_FOUND`.**
  - Reading a snapshot's version of an object would need a new operation, in
    a minor version.
  - D2's guarantee that "readers see either the old version or the new one
    in full" cannot be met by reading the old object after its `REPLACE`,
    because the backend may already have collected it. In-flight reads must
    be designed with that in mind. O1's close-to-open consistency helps.
- **Before any durability acknowledgement for a working version or
  replacement, `HOLD` it** in the transaction that writes the record naming
  it:
  - an NFS FILE_SYNC or DATA_SYNC write;
  - an SMB write-through, including on continuously available shares;
  - a FUSE fsync.

  At the seal point, commit `REPLACE` and `UNHOLD` together.
