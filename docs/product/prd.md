<!--
SPDX-FileCopyrightText: 2026 Quantum Corporation

SPDX-License-Identifier: LicenseRef-Quantum-Proprietary
-->

# Chimera Product Requirements: Unified Data Lifecycle Storage

| | |
|---|---|
| Status | Draft 0.3. Not yet reviewed. |
| Date | 2026-10-03 |
| Owner | John Suykerbuyk |
| Companion spec | [Backend storage contract](storage-contract.md) (draft 0.2) |

This document states what the product must be to succeed. It is the
reference that implementation decisions answer to. Where a requirement is
still a proposal, it says so. Open questions are collected in
[section 12](#12-open-questions) and are part of the document, not an
afterthought.

Requirement IDs (`NS-1`, `TIER-3`, ...) are stable. Change a requirement's
text in review; do not renumber. A requirement that is dropped is marked
withdrawn, and its ID is not reused.

## 1. Audience and customer

The customer for this document is Quantum senior management.

Tape has continued to fund the company and enable sales. This product is
Quantum's next-generation storage product, a product in its own right. It
is built on the tape and archive business Quantum already has.

The target market for the hot tier is neo-cloud providers: AI service
providers serving open models, who need very fast storage reached over RDMA
fabrics. Frontier AI labs have built their own data management. Neo-cloud
providers have not, and they do not have the resources to build a data
lifecycle pipeline the way Quantum can. This product is that pipeline,
delivered as storage.

## 2. Product vision

One product, one administrative pane, covering the full data lifecycle.

The hottest tier (NVMe reached over RDMA, serving AI workloads) is a hub
with many spokes. Some spokes face clients: the network protocols and RDMA
fabrics through which applications reach the data. Other spokes face
lower-cost storage tiers: SSD, HDD, glacier-class tape archive and cloud
object stores. The hub works like a railroad switch yard: it is either
taking data on, from clients or from lower-cost tiers, or sending data on
its way to its destination. Data lifecycle is not a one-dimensional
timeline on which data slides from hot to cold. Data arrives at the hub and
departs from it, in whichever direction policy sends it.

- Data lands once, through whichever protocol the application uses: NFS,
  SMB, S3, FUSE, or an RDMA fabric protocol (PROTO-1).
- The same data is visible through the other protocols.
- Administrator policy decides where data lives. Users and applications see
  only the namespace.

The product is data lifecycle management, not HSM. Administrators state
policy about data: how long to keep it, how fast it must be, how it is
protected, which quota it counts against. The system decides placement from
that policy. Stubs, recall and data movement are how the product works
inside. They are not what it sells, and the administrator never has to
manage them directly.

### 2.1 Relationship to the official roadmap

The chartered roadmap is a simplified, two-node version of ActiveScale that
scales horizontally. It defers the tape tie-in to a later release.

This document takes a different position on one point. Tape need not ship
in the first release, but the architecture must define the hub's
tier-facing spokes, tape among them, from the start. Adding them later
would mean redesigning the hub around them. This deviation from the
chartered roadmap still needs senior-management sign-off.

### 2.2 Protocols at first ship

- **PROTO-1.** NFS over RDMA is the only RDMA fabric protocol at first
  ship. Other fabric protocols (for example, S3 over RDMA or NVMe-oF) may
  follow in later releases.
- **PROTO-2.** NFSv4.1 parallel NFS (pNFS) is not in the first release
  (see [section 11](#11-non-goals)). It is a design constraint: nothing in
  the design may preclude flex-files pNFS later. Its data servers would be
  Chimera nodes, so client I/O stays behind the backend facade. The block
  and SCSI layouts are excluded outright, because they give clients direct
  access to the storage and so bypass the backend facade that owns
  allocation and verification (LAY-4; storage contract O4).

## 3. Principles

These govern every requirement below and every design decision that
follows.

- **P1. Chimera and the backend are separate layers with an explicit,
  versioned contract between them.** Chimera implements the network
  protocols and the namespace. The backend stores objects and owns
  everything about where and how they are stored. Section 4 sets out which
  layer owns what.
- **P2. The hot tier is the system of record.** It holds metadata for every
  object in the system, wherever that object's data lives.
- **P3. The landing protocol owns mutability.** By default, an object can be
  modified only through the protocol it was written with. Every other
  protocol sees it read-only. Section 6 sets out the one exception.
- **P4. NFSv4 ACLs are the canonical permission model.** Other protocols'
  permission models map onto them.
- **P5. Data moves as whole objects.** Only the metadata describing an
  object is managed at a finer grain.
- **P6. Administrators manage lifecycle policy, not tiers.** The
  administrative interface is written in terms of the data's lifecycle
  (retention, performance, protection, cost, quota). Placement follows from
  policy and is not a separate administrative task.
- **P7. Full standards compliance comes first.** Every protocol the product
  advertises complies fully with that protocol's published standards and
  specifications. Compatibility between protocols is something the
  administrator chooses to add on top (section 6). It never weakens the
  compliance of a single protocol.
- **P8. Hub and spoke, not a chain.** The hot tier is the hub. Lower-cost
  storage (HDD, tape, cloud) is reached directly from the hub, not through
  a chain of intermediate tiers. The backend moves data out to the spokes
  and brings it back as policy dictates.

## 4. Layer responsibilities

This table is the boundary that the backend storage contract will define.
When a design decision is unclear, ask which layer owns the concern.

| Concern | Owner |
|---|---|
| Wire protocols (NFS, SMB, S3, FUSE, RDMA fabric protocols) and their standards compliance | Chimera |
| The namespace: names, directories, positional relationships between objects | Chimera (stored as metadata in the backend hot tier) |
| Mapping protocol permissions onto canonical NFSv4 ACLs; identity mapping | Chimera |
| Cross-protocol compatibility mode and its enforcement (section 6) | Chimera (it requests what the mode needs from the backend; the backend obliges and does not decide) |
| Telling the backend that an object was created, replaced or deleted | Chimera |
| Declaring a node dead; moving its clients to surviving nodes (address takeover, protocol grace and reclaim) | Chimera, through its cluster manager ([storage contract](storage-contract.md) O14) |
| Object storage, allocation mapping, durability and data protection | Backend |
| Verifying stored data: its integrity across tiers and media, and the tools that check it | Backend |
| Issuing, expiring and revoking node fencing epochs on the backend's store | Backend |
| Placement, migration between hub and spokes, recall, hot-copy retention | Backend |
| Snapshots, retention, immutability (WORM) | Backend |
| Garbage collection of unreferenced objects | Backend |
| Encryption at rest | Backend |
| Lifecycle policy engine (driven from the single administrative pane) | Backend |

- **LAY-1.** Directories are not stored objects. They are metadata that
  Chimera owns, describing how objects relate to each other in a POSIX-like
  tree. The backend stores that metadata in the hot tier but does not
  interpret it. (Proposed reading of the answer to Q-NS-2. It needs
  confirmation.)
- **LAY-2.** Chimera never makes a placement, durability, retention or
  garbage-collection decision. It reports namespace events (create,
  replace, delete) and the backend acts on them.
- **LAY-3.** The backend never interprets a protocol's semantics. It stores
  objects and metadata and enforces the policies set on them.
- **LAY-4.** The backend is our own facade, which we design and build. It
  is not a third-party object store. The boundary between Chimera and the
  backend stays at objects and metadata, and the table above is unchanged.
  Below that boundary, the backend assumes media addressed as linear 4K
  blocks, possibly one address space per namespace. The first prototype
  facade runs on a loopback file of at least 1 GB. The real hot hub, both
  its backend design and its hardware, is not yet defined. The prototype is
  not the hub.

## 5. Namespace and cross-protocol visibility

- **NS-1.** All protocols share one hot storage layer and one namespace.
- **NS-2.** An object written through any protocol is visible through every
  other protocol enabled for that pool, without copying the data.
- **NS-3.** Each object records its origin protocol (the protocol that
  created it). This attribute is part of the object's metadata and is
  visible to administrators.
- **NS-4.** In a multi-protocol pool (section 6), a write whose name cannot
  be represented in every enabled protocol is refused. In a native pool, any
  name the landing protocol allows is accepted, and other protocols may be
  unable to see or open it. The administrator chooses which by choosing the
  pool's mode.

## 6. Mutability and protocol compatibility

### 6.1 Pool compatibility mode

- **MODE-1.** Each storage pool has a compatibility mode, chosen when the
  pool is created:
  - **Native.** Each protocol accepts everything its standard allows. Data
    written through one protocol may not be fully usable through another.
    The administrator has accepted that.
  - **Multi-protocol.** The administrator declares that the pool will be
    used through several protocols. From then on, every mutation through
    any protocol is limited to what all the enabled protocols can represent
    (the least common denominator): names, permissions and attributes alike.
    A mutation outside that set is refused with the protocol's standard
    error.
- **MODE-2.** Within each protocol, both modes comply fully with that
  protocol's standards (P7). Multi-protocol mode only refuses requests
  that the standard allows a server to refuse.
- **MODE-3.** A pool created without an explicit mode is multi-protocol.
- **MODE-4.** A native pool cannot be converted to multi-protocol in the
  first release. Conversion would need every existing name, permission and
  attribute checked first.

### 6.2 Mutability

- **MUT-1.** Only the origin protocol may change an object's data, change
  its metadata, rename it, or delete it.
- **MUT-2.** Through any other protocol the object is read-only. Write,
  truncate, rename, delete and setattr attempts fail with that protocol's
  standard read-only or access-denied error. They do not fail with a generic
  I/O error.
- **MUT-3.** Changing data across protocols is out of scope for this
  release, except as MUT-7 allows. See [section 11](#11-non-goals).
- **MUT-4.** The mutability rule is enforced inside Chimera, below the
  individual protocol facades, so a new or changed facade cannot bypass it.
  (Proposed. It needs confirmation.)
- **MUT-5.** Read-only consumers see current data, with close-to-open
  consistency. A change through the origin protocol becomes visible to the
  other protocols on NFS close or COMMIT, on SMB close or flush, and on S3
  PUT or multipart upload completion. Every open through any protocol after
  that point sees the new data. A reader that already has the object open
  is not guaranteed to see the change. Read caches that other protocols
  hand to clients (NFS delegations, SMB leases and oplocks) are recalled
  before the change is reported complete to the origin client.
- **MUT-6.** A whole-object replace is atomic for readers. A reader on any
  protocol sees either the old object or the new one in full, never a mix
  or a partly written object. (Proposed.)
- **MUT-7.** In a multi-protocol pool, an administrator may transfer an
  object's origin protocol, for example to let NFS edit an object that was
  ingested over S3. In a native pool, origin cannot be transferred.
  (Proposed reading of the answer to Q-MUT-1. It needs confirmation.)
- **MUT-8.** When an object is deleted or replaced through any protocol,
  Chimera tells the backend that the old object is no longer referenced by
  the namespace. Whether its storage is reclaimed (garbage collection) or
  kept (a snapshot or retention policy still references it) is the
  backend's decision. This applies equally to stubbed objects.

## 7. Access control

- **ACL-1.** NFSv4 ACLs are the canonical, stored permission model.
- **ACL-2.** SMB security descriptors map to and from NFSv4 ACLs.
- **ACL-3.** POSIX mode bits are derived from the canonical ACL.
- **ACL-4.** S3 permissions (bucket policy, object ACL) map onto the
  canonical ACL. In a multi-protocol pool, an S3 permission construct with
  no canonical equivalent is refused. In a native pool, it is accepted and
  enforced for S3 only.
- **ACL-5.** One identity model spans all protocols: a user is the same
  principal whether they arrive via NFS, SMB, S3 or a fabric protocol.

## 8. Security

The product holds an organisation's whole data estate, on every tier, so
one weak protocol endpoint exposes all of it. These
requirements set the baseline for first ship.

- **SEC-1. Authenticated by default.** Every protocol authenticates its
  clients. Anonymous or guest access is off unless an administrator turns it
  on for a specific share, export or bucket.
- **SEC-2. Default deny.** A new share, export or bucket grants no access
  until an administrator configures it. NFS exports squash root by default
  and can be restricted to listed client addresses. An export whose client
  address list is empty allows no clients.
- **SEC-3. Fail closed.** A request naming a share, export, bucket or file
  handle that does not exist, or no longer exists, is rejected. It is never
  served with default permissions.
- **SEC-4. Authorisation in one place.** Every operation that changes data
  or metadata is checked against the canonical ACL inside Chimera, below
  the protocol facades. This includes the less common ones: space
  allocation, write-same, and extended attributes. No facade can skip the
  check, and no backend is the only line of defence.
- **SEC-5. Confinement.** No request through any protocol can reach outside
  its share, export or bucket, whether by `..`, by an embedded path
  separator, or by a symbolic link.
- **SEC-6. Encryption in transit.** Each protocol offers its standard
  encryption: Kerberos privacy for NFS, SMB 3 encryption, and TLS for S3 and
  for the administrative API. The administrative API always requires
  authentication.
- **SEC-7. Safe shipped defaults.** The default configuration shipped with
  the product meets SEC-1 to SEC-6 and SEC-11. Any example configuration
  that does not is marked as test-only and is not installed as the default.
- **SEC-8. Audit.** Administrative actions are logged with who, what and
  when. Data access is not audited in the first release. How long audit
  logs are kept is an administrator policy, in the manner of logrotate or
  systemd-journald.
- **SEC-9. Customer-held encryption keys.** Encryption at rest is done by
  the backend, transparently and efficiently, on every tier. The keys are
  held only by the customer or their administrator. Quantum never holds a
  key that can decrypt customer data, and it can decrypt data only when the
  customer supplies an unlock key. (How keys are supplied and managed is
  [Q-SEC-2](#12-open-questions).)
- **SEC-10. Identity sources.** Identity follows established industry
  practice for multi-protocol NAS: Active Directory and Kerberos for SMB and
  NFSv4, LDAP for POSIX identity mapping, and S3 access keys bound to those
  same directory identities. (Proposed. See [Q-SEC-1](#12-open-questions).)
- **SEC-11. Administrative authorisation.** Every administrative action,
  through the REST control plane or the administrative pane, is authorised
  per action against the identity that requests it. Authentication alone
  (SEC-6) is not enough to perform an action.

## 9. Data lifecycle, stubs and recall

Everything in this section is implemented by the backend (section 4). It is
stated here because it shapes what the protocols promise to clients.

### 9.1 Hub and spokes

- **TIER-1.** The hot tier (NVMe) is the hub. Client-facing spokes are the
  protocols and RDMA fabrics through which clients reach the data.
  Tier-facing spokes lead to lower-cost storage: SSD, HDD, glacier-class
  archive (tape first), and cloud object stores. Only tier-facing spokes
  hold data, so wherever this document says a spoke holds data, it means a
  tier-facing spoke.
- **TIER-2.** The hot tier records every allocated byte across every
  network transaction. It holds the metadata of every object regardless of
  where the object's data is stored (P2).
- **TIER-4.** *Withdrawn in draft 0.2.* Stubs no longer chain through
  intermediate tiers (replaced by TIER-7).
- **TIER-6.** The same stub-and-marker design applies whichever spoke holds
  the data and however the data moves: RDMA fabric, S3, HDD or tape.
- **TIER-7.** Each spoke is reached directly from the hub. The backend
  moves data out to a spoke, and brings it back, as policy dictates. Data
  is not routed through intermediate tiers.

### 9.2 Stubs and copies

- **TIER-3.** When an object's data is released from the hot tier, the hot
  tier keeps a sparse stub. The stub carries a marker saying which spoke
  holds the data and how to retrieve it.
- **TIER-5.** Data is released from the hot tier only after the spoke's copy
  is durable. A failure at any point leaves the object readable.
- **TIER-8.** Moving data to a spoke does not provide durability. Data
  protection is a separate policy with its own guarantees, independent of
  placement.
- **TIER-9.** The hot tier may keep its copy of an object after the object
  has been copied to a spoke, acting as a cache. Whether it does is decided
  by the backend's adaptive policy (for example, most recently used and
  least frequently used).

### 9.3 Recall

- **RCL-1.** Reading a stubbed object through any protocol triggers recall
  from the spoke that holds the data.
- **RCL-2.** The product does not hide the physics of recall. A client
  whose protocol times out before the data arrives (SMB's 30-second timeout,
  for example) receives that protocol's standard error. The recall continues
  regardless, so the data is ready when the client retries.
- **RCL-3.** A recall in progress never delays requests for other objects.
- **RCL-4.** Recall performance is measured by time to the first gigabyte of
  a related data set, not time to the first byte of one object. The
  architecture must allow related objects to be recalled together. The
  first release does not have to optimise this, but the design must not
  prevent it.

### 9.4 Policy

- **POL-1.** Administrator policy decides placement. The first policy inputs
  are quota, most recently used, and least frequently used.
- **POL-2.** Policy is set and observed in one administrative pane that
  covers every spoke and every protocol.
- **POL-3.** Data movement is invisible to end users. The only effect they
  see is recall latency.

## 10. Platform and availability

- **HA-1.** The product is a cluster with at least two nodes.
- **HA-2.** The nodes share one hot tier ("shared everything"), reached over
  dual data paths. No single path failure loses access to the hot tier.
- **HA-3.** Losing a single node does not lose acknowledged data or
  metadata. Clients fail over to a surviving node.
- **HA-4.** Every node serves every protocol against the same shared hot
  tier at all times (active/active).
- **HA-5.** NFSv4 client state (opens, locks, delegations) survives the
  loss of a node. This is the most critical protocol state for the first
  release. S3 multipart uploads in progress also survive, as do SMB
  persistent handles, on continuously available shares only. No other
  protocol state is required to survive in the first release. The
  acceptable failover time is [Q-HA-2](#12-open-questions).
- **HA-6.** A design constraint, not a delivered requirement. The hardware
  arrives long after the software stack is proven, so the software is
  built and tested without it. The hardware concept is a node carrying at
  least 16 NVMe drives of 15 TB in the hot tier. The software must not
  design itself out of that concept, so that hardware integration does not
  break it. (The hot-tier hardware is still being defined.)

### Success measure

The first measure of success is full, verified compliance testing of every
advertised protocol, with no data corruption. Everything else is
optimisation after that.

- **SUC-1.** Each advertised protocol passes its industry compliance test
  suites in full, run against the product's shipping configuration, with
  no data corruption.
- **SUC-2.** The suites for SUC-1 are: pynfs and cthon for NFS,
  smbtorture for SMB, the Ceph S3 tests for S3, and pjdfstest for POSIX
  behaviour through FUSE. S3 is also load and stress tested with MinIO
  warp, which is a benchmark tool, not a conformance suite. NFS over RDMA
  (PROTO-1) is verified over a real NFS-over-RDMA mount; the suites for it
  are chosen when that testing starts. Which tests in each suite make up a
  full run is still to be listed. (Provisional. To be
  revisited; see [Q-SUC-2](#12-open-questions).)

## 11. Non-goals

These are out of scope for this release. Each is a deliberate decision, not
an omission.

- Changing data across protocols, apart from an administrator transferring
  origin in a multi-protocol pool (MUT-3, MUT-7).
- Moving part of an object, apart from its metadata (P5).
- Auditing data access (SEC-8).
- Optimising recall time (RCL-4 requires only that the design allows it).
- Single-node production deployment. Single-node remains supported for
  development and test.
- Converting a native pool to multi-protocol (MODE-4).
- RDMA fabric protocols other than NFS over RDMA (PROTO-1).
- NFSv4.1 parallel NFS (pNFS). Flex-files pNFS must stay possible later;
  block and SCSI layouts are excluded (PROTO-2).

## 12. Open questions

Each question names the requirement it blocks. Resolved questions move into
the requirement text, and the question is deleted.

**Security**

- **Q-SEC-1.** Confirm the identity sources proposed in SEC-10 against what
  Quantum's customers use today (for example, existing StorNext and
  ActiveScale deployments).
- **Q-SEC-2.** How do customers supply and manage keys (an external key
  manager over KMIP, or an unlock key supplied at boot)? What happens when a
  customer withdraws a key: is the data deliberately unrecoverable
  (crypto-erase)? How does this fit the encryption that tape drives already
  do?

**Platform**

- **Q-HA-2.** What failover time is acceptable (HA-3, HA-5)?
- **Q-SUC-2.** The suite list in SUC-2 is provisional and will be
  revisited. Which tests in each suite make up a full run?

**Deferred to the backend storage contract**

These belong to the backend (section 4) and will be settled in its spec:
policy granularity (per object, directory, pool or filesystem), the full
set of policy inputs, data-protection schemes, snapshot and retention
semantics, and recall grouping for RCL-4.

## 13. Current state

This section summarises an internal read-only review of the code
(2026-10-03). The code was read, not run, so runtime behaviour is still to
be confirmed.

- Chimera has a versioned backend module API, the VFS module SDK
  (`src/vfs/sdk/`, documented in `docs/vfs-module-sdk.md`). It is shaped
  like a POSIX file system: file handles, lookup, readdir, setattr, read and
  write. All protocol facades are built on it. It is not the object-level
  contract that section 4 describes: Chimera's namespace and the backend's
  storage are not yet separated.
- Several backends implement that API. The closest to the backend this
  document describes is `cairn`, which keeps metadata and data in separate
  RocksDB stores. It is a single-node module behind the file-shaped API,
  not a shared, clustered hot tier.
- Nothing yet provides placement, data movement, stubs, data recall, quota
  enforcement, compatibility modes, or origin-protocol ownership. (The VFS
  operation named "recall" breaks client leases and delegations; it is not
  data recall.)
- Requests for backends that can block are spread across a fixed set of
  worker threads, each of which serves its requests strictly in order. A
  slow recall on one thread therefore delays unrelated requests that
  happen to be hashed to it. RCL-3 is written to prevent this.
- Coordination between protocols is incomplete. SMB byte-range locks and
  leases are honoured only by SMB, not by NFS or S3, and FUSE I/O does not
  check locks. MUT-5 needs this closed for readers.
- The internal review found gaps against the security requirements in the
  current defaults and code paths: SMB always accepts anonymous sessions
  (mapped to the nobody identity) and has no setting to refuse them, NFS
  exports do not squash root by default, and the core does not check
  permissions on space allocation, write-same or extended attributes.
  SEC-1 to SEC-7 are written to close these.
- `cairn` is not yet safe as a durable store. It reports writes as stable
  before their data is synced, and it reuses inode numbers after a restart.

## 14. Glossary

- **Hub.** The hot tier. It is the system of record for all metadata, and
  every spoke connects to it.
- **Spoke.** A connection from the hub. A client-facing spoke is a protocol
  or RDMA fabric through which clients reach the data. A tier-facing spoke
  leads to lower-cost storage reached directly from the hub: SSD, HDD, tape
  or cloud.
- **Origin protocol.** The protocol through which an object was created. By
  default it is the only protocol that may change the object.
- **Native pool.** A storage pool where each protocol accepts everything
  its standard allows, at the cost of cross-protocol compatibility.
- **Multi-protocol pool.** A storage pool where every mutation is limited to
  what all enabled protocols can represent.
- **Stub.** A sparse placeholder left in the hot tier when an object's data
  is released. It records which spoke holds the data and how to retrieve
  it.
- **Recall.** Bringing an object's data back from a spoke so it can be
  served.
- **Facade.** The word has two senses in this document:
  - **Protocol facade.** A network protocol front end (NFS, SMB, S3, FUSE,
    RDMA fabric protocols) over the shared namespace, inside Chimera.
  - **Backend facade.** The backend behind the storage contract, which we
    design and build ourselves (LAY-4). It owns storage, allocation,
    durability and verification on the media below it.
- **Neo-cloud provider.** An AI service provider serving open models, as
  distinct from the frontier labs that build their own models and data
  infrastructure.
