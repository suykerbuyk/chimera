<!--
SPDX-FileCopyrightText: 2026 Quantum Corporation

SPDX-License-Identifier: LicenseRef-Quantum-Proprietary
-->

# Chimera Backend Storage Contract

| | |
|---|---|
| Status | Draft 0.2. Not yet reviewed. |
| Date | 2026-10-03 |
| Owner | John Suykerbuyk |
| Companion | [Product requirements](prd.md) (draft 0.3) |
| Read against | `develop` @ `22f6b54f`, with the PRD and contract changes of 2026-10-03 not yet committed |

This document specifies the contract between Chimera and its backend: an
object-level storage facade that we design and build ourselves, not a
third-party object store. The ownership boundary it formalises is PRD
[section 4](prd.md#4-layer-responsibilities). It also settles the items PRD
[section 12](prd.md#12-open-questions) defers to this spec. Each decision
(`D1`…`D13`) names the PRD requirement or question it serves. Each open issue
(`O1`…`O15`) names the PRD question it depends on, where there is one.

Statements about current code cite `path:line` at the commit above. They come
from a read-only review: the code was read, not run.

Decision IDs and open-issue IDs are stable. Change their text in review; do
not renumber them.

## 1. Scope and status

In scope:

- the operations Chimera calls on the backend;
- the namespace events Chimera reports;
- what the backend returns;
- the durability, consistency and availability guarantees on both sides;
- recall;
- the policy inputs Chimera supplies;
- quota;
- the error obligations;
- how the contract is versioned and tested.

Out of scope:

- the backend's internal design: allocation, protection codes, spoke formats,
  the policy engine, media layout. We design it too, but outside this
  contract. Section 3 states only the media assumption it starts from;
- Chimera's protocol facades, except for the error mappings they owe.

Every decision here is a proposal until review accepts it. Five decisions are
recorded from the operator:
- D1 was decided on 2026-10-03 on the criterion of reaching the PRD
  objectives.
- O1 was decided on 2026-10-03: the seal point is NFS close or COMMIT, SMB
  close or flush, and S3 PUT or multipart completion, with close-to-open
  consistency.
- O2 was decided on 2026-10-03: S3 multipart uploads and SMB persistent
  handles on continuously available shares are stored as namespace records,
  alongside NFSv4 state.
- O6 was decided on 2026-10-03: pool mode and name validation stay in
  Chimera; verifying stored data is the backend's alone.
- O8 was decided on 2026-10-03: S3 follows Ceph RGW's error conventions.

## 2. Terms

- **Chimera**: the protocol facades, plus the VFS core that implements the
  namespace. It is the "Chimera" column of PRD section 4.
- **Backend**: our own storage facade behind this contract. We design and
  build it; it is not a third-party object store. It owns storage,
  durability, data verification, placement and policy. It is the "Backend"
  column of PRD section 4.
- **Media interface**: the backend's lower interface, below this contract:
  linear 4K block addresses, possibly one address space per namespace
  (section 3).
- **Object**: a byte array with a backend-assigned identity (section 4.1).
- **Namespace record**: a key/value record that Chimera writes and the backend
  stores without interpreting it (section 5.1).
- **Seal**: the point at which an object's current bytes become the version
  that every protocol sees (section 4.3).
- **Policy set**: an identifier that Chimera stamps on an object and the
  backend resolves to lifecycle policy (D7).
- **Quota domain**: an identifier that Chimera stamps on an object and the
  backend charges usage against (D13).
- Hub, spoke, stub, recall, origin protocol: as defined in PRD
  [section 14](prd.md#14-glossary).

## 3. Layering

```
 NFS  SMB  S3  FUSE  RDMA-fabric           protocol facades (src/server/<proto>/)
   \   |    |    |     /
    VFS core API (chimera_vfs_*)           namespace, authorisation, mutability,
           |                               claims, open cache        (Chimera)
    namespace module  <-- this contract -->  backend facade, built by us  (Backend)
           |                                   allocation, durability, verification,
           |                                   tiers, placement
           |                                       |  media interface: linear 4K block
           |                                       v  addresses, possibly per namespace
           |                                   media: prototype = loopback file >= 1 GB;
           |                                   real hot hub not yet defined (O15)
    [dev/test and gateway modules: memfs, linux, io_uring, nfs, smb, cairn, ...]
```

The contract stays at objects and metadata. The backend behind it is ours
to build. It assumes media addressed as linear 4K blocks, possibly one
address space per namespace. The first prototype facade runs on a loopback
file of at least 1 GB. That prototype is not the hub: the real hot hub,
both its backend design and its hardware, is not yet defined (O15).

PRD section 4 did not cover cluster membership. This spec added two rows,
explained in D10. They were proposed for PRD section 4 as well. **Applied
2026-10-03 (operator): both rows are now in PRD section 4.**

| Concern | Owner |
|---|---|
| Declaring a node dead; moving its clients to surviving nodes (address takeover, protocol grace and reclaim) | Chimera, through its cluster manager (O14) |
| Issuing, expiring and revoking node fencing epochs on the backend's store | Backend |

### D1. The VFS core API stays as Chimera's internal namespace layer

**Decided 2026-10-03, on the criterion of reaching the PRD objectives.** The
operator delegated this decision. The operator's concern is that the PRD
objectives are realised, not where the VFS ends up. Reopen D1 if a later
finding shows that the VFS core API blocks a PRD objective. Serves P1 and
LAY-1 to LAY-3; it answers
the operator direction recorded on the spec task.

What the decision says:

- **The API stays.** The `chimera_vfs_*` API that the facades call remains
  Chimera's namespace layer.
- **One new namespace module implements it** by storing namespace records and
  objects through this contract.
- **The existing modules are kept as development, test and gateway backends**
  under the VFS module SDK (`src/vfs/sdk/`). They are not product backends.
  The modules are memfs, diskfs, cairn, linux, io_uring, nfs, smb, snfs,
  memkv and sqlite.
- **The module SDK is not this contract.**

Evidence:

1. **The facades depend on the core API, not on modules.**

   | Facade | Distinct `chimera_vfs_*` functions called |
   |---|---|
   | NFS | 83 |
   | SMB | 127 |
   | S3 | 26 |
   | FUSE | 51 |
   | REST | 14 |

   Command: `grep -rhoE 'chimera_vfs_[a-z_]+\(' src/server/<p> --include=*.c | sort -u | wc -l`.

   Direct references to a module struct (`vfs_module->` or `->dispatch(`) total
   9: 3 in NFS and 6 in SMB. Replacing the API would mean rewriting roughly 116K
   lines of facade code.
2. **The module SDK mixes namespace with storage.** Under the SDK, the backend
   owns:
   - directory entries and lookup (`lookup_at`, `readdir`);
   - inode identity;
   - the file-handle fragment (`src/vfs/sdk/vfs_fh.h:13-45`).

   The contract in PRD section 4 assigns the namespace to Chimera instead. The
   SDK has 45 `CHIMERA_VFS_OP_*` definitions and 31 `CHIMERA_VFS_CAP_*` bits.
   Command: `grep -cE '^#define CHIMERA_VFS_(OP|CAP)_' src/vfs/sdk/vfs_request.h src/vfs/sdk/vfs_module.h`.
   These are POSIX-shaped throughout.
3. **Backend shape leaks into the facades through capability tests.** There are
   66 references to `capabilities` in `src/server/` (`grep -rn capabilities
   src/server --include=*.c | wc -l`). For example, S3 picks its write path from
   `CAP_CREATE_UNLINKED` and `CAP_COPY_RANGE`. With one product namespace
   module, the facades test a fixed namespace-level capability set instead.

   This is a migration item (section 13), not a contract term.

The rejected alternative was to replace the VFS core API with a new
namespace API that facades call directly. That would rewrite every facade and
gain nothing that the namespace module cannot provide behind the existing API.

## 4. Object model

### 4.1 Objects

- **Identity.** An object has an opaque, immutable 128-bit identifier. The
  backend assigns it at creation and never reuses it. This rules out the
  inode-reuse defect found in cairn today (`src/vfs/cairn/cairn.c:2060,2080-2083`).
- **Data.** An object's data is a byte array. While the object is on the hot
  tier, Chimera reads and writes it by extent (offset, length).
- **Movement.** The object moves between hub and spokes only as a whole (P5,
  TIER-7).
- **References.** An object is referenced by zero or more namespace bindings
  (section 5.2). An unreferenced object belongs to the backend: it is either
  garbage-collected, or retained because a snapshot or retention policy still
  pins it (MUT-8).

### 4.2 Object states

| State | Data location | Writable | Movable to a spoke |
|---|---|---|---|
| `open` | hub | yes, by extent | no |
| `sealed` | hub, spoke, or both (TIER-9) | no | yes |
| `stubbed` | spoke only; the hub keeps the marker (TIER-3) | no | already moved |

Transitions:

| From | Event | To |
|---|---|---|
| `open` | SEAL | `sealed` |
| `sealed` | the backend's placement releases the hub copy, only once the spoke copy is durable (TIER-5) | `stubbed` |
| `stubbed` | recall completes | `sealed` |

A write to a sealed or stubbed object never changes it in place. See D2 and D3.

### 4.3 Versions and visibility (D2, D3)

#### D2. Replace and partial write create a new object; readers never see a mix

Serves MUT-6, MUT-8 and P5.

- **Full overwrite is a REPLACE.** This covers `O_TRUNC`, SMB `OVERWRITE` and
  `SUPERSEDE` dispositions, S3 PUT, and truncate to zero. Chimera creates a new
  `open` object. When it seals, Chimera reports REPLACE(old, new). A full
  overwrite never recalls the old object's data, even when that object is
  stubbed. The old object becomes unreferenced, and the backend may
  garbage-collect it (MUT-8; operator answer to Q-TIER-2).
- **The first partial write to a sealed object creates a working version.**
  The backend clones the sealed object's extent map into a new `open` object.
  On the hot tier this is a metadata-only clone. All further writes through the
  origin protocol go to the working version.
- **The old version is never modified.** Readers see either the old version or
  the new one in full.

#### D3. Same-protocol and cross-protocol visibility

Serves MUT-1, MUT-2, MUT-5, MUT-6, P7 and RCL-1.

- **Same-protocol readers** resolve the binding to the working version, so the
  origin protocol keeps its own semantics. For example, NFS writes are visible
  to other NFS readers under NFS rules, as P7 requires.
- **Cross-protocol readers** resolve to the last sealed version. They cannot
  write it (MUT-2), so two views are enough.
- **At the seal point** (O1), Chimera reports REPLACE(sealed, working). The
  working version becomes the version every protocol sees. Before reporting
  completion to the origin client, Chimera recalls the read caches it has
  handed to other protocols (MUT-5).
- **A partial change to a stubbed object recalls first.** "Partial change"
  means a partial write, an append, a truncate to a non-zero size, an
  allocation, or a hole punch. The change uses the recall ticket semantics of
  D6, so the waiting request holds no worker thread. Only a full overwrite
  avoids recall (D2).
- **A binding whose object has never been sealed is invisible to other
  protocols until its first seal.** This covers, for example, a file still
  being created over NFS, or an S3 upload in progress.
  - Through the origin protocol, the binding behaves as that protocol
    requires.
  - Through every other protocol, lookup returns "not found", directory
    listings omit the binding, and creating the same name fails as the name
    already existing.
  - This matches S3 semantics, where an object does not exist until its PUT
    or multipart upload completes, and it follows from MUT-6.
  - The as-is review found the opposite today. S3 writes named temporary and
    part files into the destination directory on backends without
    create-unlinked support. NFS, SMB and FUSE can see those files, and only
    S3 listing hides them (`src/server/s3/s3_put.c:481-500`,
    `src/server/s3/s3_list.c:564-574`; as-is finding M36).
  - Under this rule, an S3 upload is an `open` object with a pending binding,
    so no temporary name is needed.
  - The first seal point per protocol is O1.

## 5. Metadata model

### 5.1 Two kinds of metadata

| Kind | Owner | Stored by | Interpreted by | Examples |
|---|---|---|---|---|
| Namespace records | Chimera | backend, hot tier | Chimera only (LAY-1, LAY-3) | directory entries, name→object bindings, NFSv4 ACLs (ACL-1), origin protocol (NS-3), protocol attributes, NFSv4 and SMB protocol state (HA-5) |
| Object metadata | Backend | backend, hot tier | both; Chimera reads it | size, allocation map, state (section 4.2), placement or stub marker, protection class, retention lock, checksum, policy set, quota domains |

Chimera reads the checksum for protocol use only, for example to return an
S3 ETag or to compare it with a Content-MD5 header the client sent. Chimera
does not verify stored data against it. Data verification is the backend's
(D9).

### 5.2 Namespace transactions (D4)

#### D4. Multi-key namespace transactions

Serves HA-1 to HA-5, MUT-6, MUT-8 and LAY-2.

- **Transaction model.** The backend provides compare-and-set transactions over
  namespace records. They are linearizable across all nodes and durable when
  the commit returns.
- **Event deltas ride in the same transaction.** A transaction may carry object
  reference deltas and object-metadata stamps (policy set, quota domains). The
  namespace change and its event deltas therefore commit or fail together.
  After a crash, no object is referenced by a binding that does not exist, and
  no live binding points at a collected object.
- **What this rules out.** Today's cairn writes data and metadata to separate
  RocksDB instances with no atomicity between them
  (`src/vfs/cairn/cairn.c:64-74`). The as-is review also found that cairn
  appears to commit extent deletes before the metadata that stops referencing
  them (finding H9). That finding is graded plausible: read from source, not
  reproduced.
- **Rename** is one transaction. It covers the source and target directory
  records, the binding, and any replaced target's reference decrement.

## 6. Operations

All operations carry the caller's node identity and fencing epoch (D10).

### 6.1 Object data

| Operation | Meaning | Returns |
|---|---|---|
| `obj_create(attrs)` | create an `open` object | object ID |
| `obj_clone_working(sealed_id)` | D2 working version | new object ID; a recall ticket if stubbed; `RETENTION_LOCKED` if the sealed object is locked (D8) |
| `obj_read(id, extent)` | read bytes | data, or `RECALL_PENDING`(ticket) (D6), or `DATA_UNRECOVERABLE` (D9) |
| `obj_write(id, extent, data, level)` | write into an `open` object; `level` ∈ {buffered, durable} | bytes written, achieved level, durability epoch (D5) |
| `obj_flush(id, extent?)` | make prior buffered writes durable | durability epoch |
| `obj_truncate`, `obj_allocate`, `obj_punch` | size and space changes on an `open` object | as `obj_write` |
| `obj_seal(id)` | `open` → `sealed` | — |
| `obj_stat(id)` | object metadata (section 5.1) | metadata |

### 6.2 Namespace records

| Operation | Meaning |
|---|---|
| `ns_get`, `ns_scan(prefix, cursor)` | read namespace records; a stable cursor for readdir |
| `ns_txn(compare[], put[], delete[], deltas[])` | D4 transaction |
| `ns_watch(prefix)` | change notifications for records written by other nodes (D10) |

### 6.3 Namespace events

Chimera reports events inside `ns_txn` as `deltas[]`. This is the sole
mechanism for LAY-2 and MUT-8.

| Event | Effect on the backend |
|---|---|
| CREATE(binding → obj) | reference +1; stamp origin protocol, policy set and quota domains |
| REPLACE(old, new) | old −1, new +1 (D2) |
| DELETE(obj) | reference −1 |
| SEAL(obj) | object eligible for placement |
| RETAG(obj, policy set, quota domains) | re-stamp (D7, D13) |
| RECALL_HINT(set or group tag) | related-set recall (D6) |

### 6.4 Recall

| Operation | Meaning |
|---|---|
| `recall(ids[] or group)` | start a recall; returns a ticket |
| `recall_wait(ticket, callback)` | asynchronous completion; never blocks the caller |

### 6.5 Attach

`attach(contract_version, node_id)` returns the negotiated version, the
capability set, and the node's fencing epoch, which is a lease (D10, D12).

| Operation | Meaning |
|---|---|
| `renew(epoch)` | heartbeat; extends the epoch lease |
| `fence(node_id)` | revoke that node's epoch now; returns only once no operation carrying it can still commit (D10) |

## 7. Durability, consistency and availability

### D5. Durability is proven, not asserted

Serves HA-3, SUC-1, MUT-5 and TIER-5.

- **Two write levels.** `obj_write` takes a level: `buffered` or `durable`. It
  returns the level actually achieved. A backend must never report `durable`
  for data that a power loss can lose.
- **Today's code shows why this rule is needed:**
  - the core neither presets nor checks the reported level (`src/vfs/vfs_proc_write.c:116-131`);
  - cairn reports FILE_SYNC after an unsynced write (`src/vfs/cairn/cairn.c:4709`; data sync only after COMMIT at `:5659`);
  - memfs reports FILE_SYNC for RAM (`src/vfs/memfs/memfs.c:4409`).
- **Durability epoch.** Every write and flush returns the backend's durability
  epoch. If the epoch changes, buffered data from an earlier epoch may have
  been lost.
  - Chimera derives NFS write verifiers from the epoch, not from boot time
    (`src/server/nfs/nfs.c:275` today).
  - This lets a client resend after a backend-side loss without a Chimera
    restart.
- **Namespace changes are always durable on commit.** This closes the gap
  where a published name is not synced (as-is: `src/vfs/linux/linux.c:1588`
  is the only fsync in that backend).
- **Conformance test.** The suite (D12) includes a power-loss test that fails
  any backend whose acknowledgement outruns persistence.

### D10. Availability: active/active with fencing

Serves HA-1 to HA-5.

- **Any node may issue any operation.**
- **Fencing epochs.** `attach` grants each node a fencing epoch, held as a
  lease that the node renews with `renew`. The backend rejects every operation
  carrying a revoked or expired epoch.
- **Who declares a node dead.** Two owners, matching the rows added to the
  layering in section 3:
  - **The backend owns the epochs.** It issues them, expires them when the
    lease runs out, and revokes them on `fence`. Expiry alone guarantees that a
    partitioned node can never commit after its lease ends, even if Chimera's
    view of membership is wrong.
  - **Chimera's cluster manager owns membership for clients.** It decides that
    a node is dead and moves that node's clients to surviving nodes: address
    takeover, plus protocol grace and reclaim.
- **Failover order.** Before a surviving node accepts any client or protocol
  state that belonged to the dead node, the cluster manager must call
  `fence(node_id)` and wait for it to return, or wait for the lease to expire.
  Otherwise the two nodes could serve the same NFSv4 state at once.
- **No cluster manager exists today.** The only multi-instance provision is an
  NFSv4 node identifier for instances sharing one KV store
  (`src/server/server.c:471-473`). Its design is O14.
- **NFSv4 state** (opens, locks, delegations, client records) is stored as
  namespace records, so it survives node loss (HA-5).
  - Today it lives in RAM claim tables and, by default, in the in-memory memkv
    (`src/vfs/vfs.c:775`).
  - Which other protocols' state is stored this way is O2.
- **Node caches.** `ns_watch` delivers changes made by other nodes. Chimera
  uses it to invalidate its attribute and name caches and to drive protocol
  cache recalls on the local node.

### D11. Authorisation and mutability are enforced in Chimera

Serves SEC-3, SEC-4, SEC-5, MUT-4 and ACL-1.

- **In the VFS core.** Chimera enforces the canonical ACL, origin-protocol
  mutability, pool mode (O6) and confinement in the VFS core, below the
  facades. Every mutating operation passes through one check.
- **The gap this closes.** Today the core skips the check for allocate,
  write-same and xattr changes. `vfs_proc_allocate.c`,
  `vfs_proc_write_same.c`, `vfs_proc_set_xattr.c` and
  `vfs_proc_remove_xattr.c` contain no gate, and WRITE_SAME is missing from
  the mutating-op list (`src/vfs/vfs_internal.h:654-694`).
- **What the backend enforces.** The backend does not evaluate ACLs (LAY-3).
  It enforces only retention and immutability locks (D8) and quota (D13).

## 8. Recall

### D6. Recall never blocks a thread and is never abandoned

Serves RCL-1 to RCL-4 and POL-3.

- **Tickets instead of blocking.** A read or clone of a `stubbed` object
  returns `RECALL_PENDING` with a ticket. Chimera parks the protocol request on
  the ticket, holding no worker thread.
- **What the client sees while it waits.** Chimera answers with the protocol's
  standard wait response where one exists:
  - NFSv3 `NFS3ERR_JUKEBOX`;
  - NFSv4 `NFS4ERR_DELAY`;
  - SMB interim `STATUS_PENDING`.

  When the protocol times out, the client gets that protocol's standard error
  (RCL-2).
- **S3 uses AWS's archived-object surface.**
  - **GET on a stubbed object** returns 403 `InvalidObjectState`, as AWS does
    for an archived object. It also starts the recall, as RCL-1 requires. A
    client that retries after the recall completes gets the data.
  - **RestoreObject** (`POST /{key}?restore`) is supported. It returns 202
    Accepted when it starts a recall, and 200 when the data is already
    hub-resident.
  - **HEAD** reports the object's storage class and an `x-amz-restore` header
    with the recall state, so S3 tools can poll.
  - Starting a recall on GET goes beyond AWS, which recalls only on
    RestoreObject. This is a deliberate choice for RCL-1, and it changes no
    response code that AWS clients depend on.
  - S3 does not parse `?restore` today (`src/server/s3/s3.c:899-986` is the
    set of recognised subresources), so this is new facade work.
- **The recall continues.** The backend keeps recalling after the client gives
  up, so a retry finds the data hub-resident.
- **Unrelated requests are not delayed (RCL-3).**
  - Today, blocking backends are pinned by `fh_hash % n` to FIFO threads
    (`src/vfs/vfs_internal.h:758-762`).
  - A slow operation therefore stalls unrelated files that hash to the same
    thread.
  - The namespace module must not use that dispatch for recall-capable data.
- **Related-set recall (RCL-4).** `recall` accepts a set of object IDs or a
  group tag. Chimera may send `RECALL_HINT` when it detects a related set, for
  example a directory scan or a sequential multi-object read.
- **Optimisation is a non-goal for this release** (PRD section 11). The
  interface is required.
- **Naming.** The VFS function named "recall" today breaks leases and
  delegations; it dispatches nothing (`src/vfs/vfs_proc_recall.c:49`). The
  namespace module must keep the two meanings apart in its names.

## 9. Policy inputs and granularity

### D7. Policy granularity: pool defaults, directory-subtree overrides, resolved by Chimera

Serves POL-1 to POL-3, P6 and LAY-3. It settles the "policy granularity" item
that PRD section 12 defers to this spec.

#### Stamping and resolution

- **Stamped at creation.** Each object carries a policy-set ID. Chimera stamps
  it at CREATE from the nearest enclosing directory override, or from the
  pool default.
- **The backend never parses a path.** It resolves the policy-set ID to policy.
- **Policy inputs supplied by Chimera:**
  - policy set;
  - quota domains (D13);
  - owner principal;
  - origin protocol (NS-3).
- **Policy inputs measured by the backend:** access recency and frequency
  (POL-1, most recently used and least frequently used), because data I/O
  passes through it.

#### Renames across a policy boundary

| Rename | Treatment |
|---|---|
| A file | RETAG inside the rename transaction. One object, synchronous. |
| A directory, where only placement or performance policy differs | Commits in O(1); only the moved directory's record changes. Chimera enqueues a retag job (below). |
| A directory, where the quota domain, retention or protection class differs | Refused with the cross-device error: NFS3ERR_XDEV, NFS4ERR_XDEV, SMB `STATUS_NOT_SAME_DEVICE`. S3 has no rename. |

The retag job is:
- stored as namespace records, so it is durable;
- resumable by any node after a node failure;
- batched and rate-limited.

Until the job reaches an object, that object stays under its old policy for
placement. The only visible effect is placement or recall latency (POL-3).
Progress is shown in the administrative pane (POL-2). The cost is O(n)
background work; the rename itself pays nothing per object.

Refusing the last kind of rename has a precedent: XFS project quotas. Clients
fall back to copy and delete. It guarantees that a pending retag never weakens
retention or protection, and never leaves usage charged to the wrong quota
domain.

## 10. Protection, snapshots, retention and encryption

### D8. Snapshots and retention

Serves MUT-8 and P2. It settles the "snapshot and retention semantics" item
that PRD section 12 defers to this spec.

- **Snapshots are pool-level and crash-consistent.**
  - A snapshot is a point-in-time copy of the pool's namespace records, plus a
    pin on every object they reference.
  - The backend can take it without understanding the namespace, because the
    namespace records are already in its store (D4).
  - Snapshots of a subtree are not offered in this release.
- **Retention and immutability (WORM) are object locks.**
  - They come from policy (D7).
  - The backend refuses REPLACE or DELETE of a locked object with
    `RETENTION_LOCKED`.
  - Chimera maps that to each protocol's read-only or access-denied error
    (MUT-2 style). It is never a generic I/O error.
- **A locked object is refused when the change starts, never at seal.**
  Refusing at seal would tell the client that its writes succeeded and then
  refuse them. Chimera checks the lock in the VFS core:
  - **A partial change** (a write, truncate to a non-zero size, allocate or
    punch) is refused at open where the protocol has one. The opens that
    count are an NFSv4 OPEN with write access, an SMB CREATE requesting write
    or delete access, and a FUSE open for write. At that open, Chimera reads
    the lock with `obj_stat` and refuses with `RETENTION_LOCKED`. No working
    version exists yet.
  - **At the first partial write**, as D2 states, Chimera calls
    `obj_clone_working` to create the working version. This is also the
    first point at which stateless NFSv3 can be checked. The backend's
    `RETENTION_LOCKED` return here is the atomic backstop. It catches the
    NFSv3 case, and a lock that arrives between the open and the first
    write (see O13).
  - **A full overwrite** is refused when the object is opened with truncate,
    or with the SMB OVERWRITE or SUPERSEDE disposition, and an S3 PUT is
    refused before any body is accepted. Chimera reads the lock with
    `obj_stat` before it creates the replacement object. The backend refuses
    the REPLACE delta again as a backstop.
  - **A delete or rename-over** is refused at the request.
  - **A lock that arrives after a working version exists** is O13.
- **Garbage collection is backend-only (MUT-8).** An object is collected when
  no binding, snapshot or retention lock references it.

### D9. Protection and encryption

Serves TIER-8 and SEC-9. It settles the "data-protection schemes" item that
PRD section 12 defers to this spec.

- **Protection classes.** The backend declares them as capabilities at attach,
  for example replication ×N or erasure code k+m. Policy selects one.
  Protection is independent of placement (TIER-8).
- **Encryption at rest** is transparent to the contract. The contract carries
  no keys.
- **Keys withdrawn.** If the customer's key is unavailable, operations on
  affected objects fail with `KEY_UNAVAILABLE`. Chimera maps that to a
  protocol access error, not to data corruption. Key supply and withdrawal are
  O3.
- **Data verification** belongs to the backend alone (O6, decided): checking
  the integrity of stored data across tiers, capacities and media, repairing
  it from protection where it can, and the tools that do this. Chimera
  consumes storage and does not verify it. The backend never returns data
  that failed verification. If the data cannot be recovered, the operation
  fails with `DATA_UNRECOVERABLE`, and Chimera maps that to the protocol's
  I/O error (section 11).

## 11. Quota (D13) and error obligations

### D13. Quota is enforced by the backend, against domains Chimera stamps

Serves POL-1 and P6.

#### What is enforced, and where

- **Domains.** Chimera stamps each object with its quota domains at CREATE and
  on RETAG: owner principal, group, and directory or project domain. The
  backend never parses a path.
- **The backend enforces quota.** It is the only layer that sees every
  allocation from every node on the shared hot tier (HA-2, HA-4).
- **Where it refuses:**
  - byte quota: at extent allocation or flush, with `QUOTA_EXCEEDED`;
  - object-count quota: at the CREATE transaction.
- **Pool-wide space exhaustion** is a separate error, `NO_SPACE`.

#### Keeping counters correct in active/active

The backend keeps counters in its shared transactional store and offers two
modes per domain:

| Mode | How it works | Overrun |
|---|---|---|
| strict | counters are updated in every transaction | none, at a throughput cost |
| escrow (default) | each node holds a reservation chunk and allocates locally against it | bounded by the sum of outstanding chunks |

In escrow mode, a dead node's chunks are reclaimed when its fencing epoch is
revoked (D10). The default mode and the chunk size are O9.

#### Error mapping Chimera owes each protocol

Today there is no quota enforcement anywhere in the tree. The existing
mappings are partly wrong:
- NFS maps `CHIMERA_VFS_EDQUOT` to `NFS3ERR_DQUOT` and `NFS4ERR_DQUOT`
  (`src/nfs_common/nfs3_status.h:47-48`, `src/server/nfs/nfs4_status.h:53-54`).
- SMB maps quota to disk-full (`src/server/smb/smb_proc_create.c:221-222`).
- S3 maps every error other than EACCES/EPERM to the handler's fallback
  (`src/server/s3/s3_status.h:55-62`), so quota surfaces as an internal error.

The required mappings:

| Backend error | NFSv3 | NFSv4 | SMB | S3 |
|---|---|---|---|---|
| `QUOTA_EXCEEDED` | `NFS3ERR_DQUOT` | `NFS4ERR_DQUOT` | `STATUS_QUOTA_EXCEEDED` | 403 `QuotaExceeded` (O8, decided) |
| `NO_SPACE` | `NFS3ERR_NOSPC` | `NFS4ERR_NOSPC` | `STATUS_DISK_FULL` | 507 `InsufficientCapacity` (O8, decided) |
| `RECALL_PENDING` | `NFS3ERR_JUKEBOX` | `NFS4ERR_DELAY` | interim `STATUS_PENDING`, then the protocol timeout error | GET: 403 `InvalidObjectState`, and the recall starts. RestoreObject: 202, or 200 if already resident (D6) |
| `RETENTION_LOCKED` | `NFS3ERR_ACCES` | `NFS4ERR_ACCESS` | `STATUS_ACCESS_DENIED` | 403 `AccessDenied` |
| `KEY_UNAVAILABLE` | `NFS3ERR_ACCES` | `NFS4ERR_ACCESS` | `STATUS_ACCESS_DENIED` | 403 `AccessDenied` |
| `DATA_UNRECOVERABLE` | `NFS3ERR_IO` | `NFS4ERR_IO` | `STATUS_DATA_ERROR` | 500 `InternalError` |
| cross-device rename (D7) | `NFS3ERR_XDEV` | `NFS4ERR_XDEV` | `STATUS_NOT_SAME_DEVICE` | n/a |

`DATA_UNRECOVERABLE` means the backend found data that failed verification
and could not repair it (D9). The mappings are each protocol's standard
hard I/O error. NFS has only `NFS3ERR_IO` and `NFS4ERR_IO` for this. For SMB,
`STATUS_DATA_ERROR` reports a failed read of data; `STATUS_FILE_CORRUPT_ERROR`
was not chosen, because it tells clients the file system structure is
damaged and points them at a disk check. S3 has no standard data-integrity
error, so it returns 500 `InternalError`, which clients treat as a server
fault. FUSE returns `EIO`.

## 12. Versioning, capabilities and conformance

### D12. Versioned contract with capability negotiation and a conformance suite

Serves P1, SUC-1 and Q-SUC-2.

- **Version negotiation.** The contract has a semantic version. `attach`
  negotiates it. A major mismatch refuses to attach.
- **Capabilities.** Optional behaviour is a capability declared at attach:
  protection classes, spokes, group recall, escrow quota. Mandatory behaviour
  is never a capability.
- **Today's SDK precedent.** The module SDK already carries a version checked
  at registration (`src/vfs/sdk/vfs_module.h:31,394`; currently 3). The
  contract adopts the same rule.
- **Conformance suite.** It ships with the contract and covers:
  - durability under power loss (D5);
  - transaction atomicity across a crash (D4);
  - fencing (D10);
  - non-blocking recall (D6);
  - quota bounds in both modes (D13);
  - the error table (section 11).

  A backend must pass it to be supported. This is the contract-level
  counterpart of the protocol suites in SUC-1.

## 13. Migration from today

| Item | Today | Under this contract |
|---|---|---|
| Namespace storage | each module owns directories and handles (`src/vfs/sdk/vfs_fh.h:13-45`) | namespace records in the backend hot tier (D4) |
| Claims and leases | RAM, keyed by file-handle bytes (`src/vfs/vfs_claim.c:137-147`) | NFSv4 state persisted as namespace records (D10); other protocols per O2 |
| Default protocol-state KV | memkv, in memory (`src/vfs/vfs.c:775`) | namespace records |
| Stability reporting | trusted, never checked (D5 evidence) | achieved level plus durability epoch, enforced by the conformance suite |
| Recall | none for data; "recall" means a lease break (`src/vfs/vfs_proc_recall.c:49`) | D6 |
| Policy, quota, placement | absent | D7, D13, backend |
| Facade capability tests | 66 sites in `src/server/` | a fixed namespace-level set (D1) |

## 14. Open issues

| ID | Issue | PRD link |
|---|---|---|
| O1 | **Decided 2026-10-03 (operator): the seal point (D3) is NFS close or COMMIT, SMB close or flush, and S3 PUT or multipart completion, with close-to-open consistency.** A reader that already has the object open is not guaranteed to see the change. | MUT-5 |
| O2 | **Decided 2026-10-03 (operator): besides NFSv4 state, S3 multipart uploads and SMB persistent handles on continuously available shares are stored as namespace records.** No other protocol state is required to survive failover in the first release. | HA-5 |
| O3 | Key supply and withdrawal; crypto-erase; fit with tape-drive encryption. | Q-SEC-2 |
| O4 | Whether RDMA fabric clients do data I/O directly against backend objects, bypassing Chimera's data path. | PROTO-1 |
| O5 | Whether stub, recall and spoke operations are mandatory in contract version 1. This draft recommends yes. | section 2.1, TIER-1 |
| O6 | **Decided 2026-10-03 (operator): pool mode and name validation stay in Chimera; verifying stored data is the backend's alone.** Chimera requests what the pool mode needs from the backend, and the backend obliges; it does not decide. Data verification, meaning the integrity of stored data across tiers, capacities and media and the tools that check it, belongs to the backend: Chimera only consumes storage and knows none of those. Pools default to multi-protocol, and a native pool cannot be converted to multi-protocol in the first release. | MODE-1, MODE-3, MODE-4, section 4 |
| O7 | How the conformance suite (D12) is gated alongside the protocol suites. | Q-SUC-2 |
| O8 | **Decided 2026-10-03 (operator): follow Ceph RGW's S3 conventions.** S3 has no AWS-standard error for quota or for out-of-space, so the product uses the codes Ceph RGW returns. Source: the S3 table `rgw_http_s3_errors` in `src/rgw/rgw_common.cc` of ceph/ceph at commit `4b394111370385015fb957c91fcbdca88dddbfce` (main, read 2026-10-03), lines 46-146. `QUOTA_EXCEEDED` maps to 403 `QuotaExceeded` (line 91, `ERR_QUOTA_EXCEEDED`). `NO_SPACE` maps to 507 `InsufficientCapacity` (lines 140-141, `EDQUOT` and `ENOSPC`). Ceph's Swift table maps quota to 413 (line 157), and that does not apply to S3. Recall is not part of O8: it uses AWS's standard archived-object surface (D6). | P7 |
| O13 | A retention lock that policy applies while a working version is outstanding (D8). Refusing the later seal would break the rule that a client is never told its writes succeeded and then refused. Recommended default: the seal is allowed, the locked sealed version is kept, because the lock pins it from garbage collection, and the new version inherits the lock. To be confirmed against the WORM semantics customers expect. | none yet |
| O14 | Chimera cluster manager: membership protocol, failure detection, client address takeover, and its interaction with protocol grace periods (D10). No such component exists today (`src/server/server.c:471-473` is the only multi-instance provision). Recommended default: the cluster manager declares nodes dead, but never moves clients until `fence` returns or the epoch lease expires. | HA-1 to HA-5; Q-HA-2 |
| O15 | The real hot hub is not yet defined: its backend design and its hardware. The first prototype facade runs on a loopback file of at least 1 GB of linear 4K blocks, and it is not the hub. Also open: whether the media interface is one address space per namespace. | HA-6, TIER-1 |
| O9 | Quota default mode (strict or escrow) and escrow chunk size. | none yet |
| O10 | Cost of the first partial write to a large sealed object: a metadata-only clone on the hub, but a full recall first if stubbed (D2, D3). | none yet |
| O11 | Hard links: one object referenced by several bindings, against whole-object reference counting and D2 working versions. Which bindings see a working version? | none yet |
| O12 | Sparse files: how holes are represented in an object, and in a stub. | none yet |
