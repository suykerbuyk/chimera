<!--
SPDX-FileCopyrightText: 2026 Quantum Corporation

SPDX-License-Identifier: LicenseRef-Quantum-Proprietary
-->

# Chimera Test Infrastructure Roadmap

| | |
|---|---|
| Status | Finalized 2026-10-05 (0.2; passed two adversarial-review rounds). |
| Date | 2026-10-05 |
| Owner | John Suykerbuyk |
| Companions | [Compliance suites](compliance-suites.md) (suite inventory), [Storage contract](storage-contract.md) (D12 backend conformance), [PRD](prd.md) (SUC-1, SUC-2), [Roadmap](roadmap.md) |
| Read against | `develop` @ `6e9a6b8b`; the `qb`, `pveforge` and `qa-metabuild-system` repos surveyed 2026-10-05 |

This document is the reference for **how** Chimera proves protocol compliance:
the substrate a client runs on, the fixtures that build and drive it, the
provisioning that stands them up, the lifecycle contract they obey, and the
result model that records compliance over time.

It is the infrastructure layer **beneath** [`compliance-suites.md`](compliance-suites.md).
That document owns *which* suites run and *what counts* as a full run; this one
owns *where and how* a real client is built, launched, exercised, and torn
down. The two are read together.

## 1. Scope

In scope:

- the substrates test clients run on (containers, KVM guests, Proxmox VMs);
- the client-fixture lifecycle contract (`up` / `exercise` / `down`);
- reproducibility of every fixture image (containers and VM guests);
- provisioning and orchestration, as shell and make facades;
- secrets handling for the suites that need credentials;
- the compliance result model (matrix plus expected-failure baselines).

Out of scope:

- the suite inventory and "full run" definition — [`compliance-suites.md`](compliance-suites.md);
- backend internals — [`storage-contract.md`](storage-contract.md);
- the backend conformance **suite content** (D12); this doc covers only how its
  harness shares this substrate (section 7, best effort).

## 2. Guiding principles

1. **Shell and make facades only.** Provisioning and orchestration are make
   targets and shell scripts, matching the existing Chimera Makefile facade
   (`mk/vp-facade.mk`) and the `qa-metabuild-system` `mk/proxmox.mk` lifecycle.
   Pulumi, `pveforge` and Salt are **not adopted as dependencies**; no new
   orchestration framework is taken on. (If the Proxmox tier ever outgrows
   `qm`-over-SSH, `pveforge` — not Pulumi — is the nearer-ready candidate;
   section 7. It remains a candidate, not a dependency.)
2. **Evolve, don't replace.** A working real-client harness already exists (the
   KVM tier, section 3). We unblock what is blocked and make it reproducible;
   we do not rip and replace a working mechanism until a concrete need forces
   it.
3. **Reproducible recipes, no un-reconstructable upstream.** Every fixture
   image — container or VM guest — is built from a source-controlled recipe
   that pins its inputs (base image **by digest**, packages from a dated
   snapshot) and fails the build if an installed package is not in the pinned
   set. The `builders/thin` recipe is the exemplar for **container** images
   (section 5). Reproducibility is a property of the **recipe plus its lock
   gate**, independent of whatever runs the build. (The exemplar covers apt
   packages, not the construction of a bootable VM guest — see section 5.)
4. **Idempotent lifecycle.** Every client fixture exposes `up` (provision and
   configure, idempotent), `exercise` (run a suite, emit artifacts), and
   `down` (reclaim). Re-running `up` converges; `down` leaves nothing behind.
   `qa-metabuild-system/mk/proxmox.mk` already implements this shape end to end
   for a Proxmox host VM and is the reference (section 4).
5. **Real clients prove compliance.** A protocol claim is proven by a real
   client exercising the running server. Pick the lightest substrate that still
   proves the claim: a protocol-wire assertion can run in a container; a
   kernel-mount or OS-behavior assertion needs a VM.
6. **NFS v3 and v4 first.** They are the top priority. Note the split this
   forces: NFSv4 (4.0/4.1/4.2) has a userspace conformance client (pynfs) that
   can be containerized; **NFSv3 has no such suite**, so v3 is proven through
   the kernel-mount KVM tier, not the container tier (section 7, F-note). SMB
   follows without a Windows client for now; S3 follows NFS. Windows, a real
   multi-host network, and NVMeoF block are later waves.

## 3. Current state (honest baseline)

### What already works

- **The KVM tier (`kvm/`) is a real-client harness.** It provisions a guest,
  boots QEMU in a network namespace on a TAP device with the Chimera server in
  the same namespace, mounts with **real kernel NFS** (`vers=<v>,tcp,nconnect=16`)
  and **cifs** clients, runs **cthon04, nfstest (including a two-client
  delegation test), LTP, xfstests (fsstress/fsx), Kerberos NFS, pNFS and reboot
  wrappers**, and tears down via an `EXIT` trap plus a `timeout` backstop. It is
  the pattern to extend, not to discard. It is also where **NFSv3** is actually
  exercised (a kernel mount), and it is privileged and single-host (below).
- **A reproducible container recipe exists.** `builders/thin` pins its base
  image by digest, pins a `snapshot.ubuntu.com` timestamp as apt's only source,
  and runs `lock-gate.py`, which fails the build unless every installed package
  is in the snapshot (the build even self-tests the gate by injecting a leak
  row and asserting the failure). A rebuild touches only public,
  reconstructable inputs.
- **A suite inventory exists.** [`compliance-suites.md`](compliance-suites.md)
  catalogs the SUC-2 suites and the adjacent ones, and defines a proposed
  "full run".
- **`qb` orchestrates container builds.** `qb` is a Go orchestrator that runs
  the `build_container` / `run_container` / `run_ssh` tasks a project declares
  in its `qb.yaml`, building from plain Dockerfiles via Kaniko (on Kubernetes)
  or local Docker, and publishing to the internal Nexus registry by tag.
- **A full Proxmox lifecycle facade exists.** `qa-metabuild-system`'s
  `mk/proxmox.mk` is a complete, idempotent `pve-up` / `pve-down` /
  `pve-destroy` / `pve-snapshot` / `pve-rollback` lifecycle over `pvesh` and
  `qm` via SSH — today scoped to a single host VM, but it is the working
  shell+make expression of the lifecycle contract (section 4), not merely
  "provisioning".

### The gaps this roadmap drains

- **KVM guests are golden images.** They are pulled **by tag** (`v1.9.0`, and
  the fetch even falls back to `latest`) — not by digest — from
  `ghcr.io/chimera-nas/kvm-test-base`, and their recipe lives in a separate
  repo. `qb` does **not** build them — it has no VM/qcow2 path. This is the
  clearest violation of principle 3, and it has two very different fixes of very
  different size (section 5, section 7).
- **`qb` is a build runner, not a source of reproducibility.** Its own builder
  image and the `chimera-dev` image build from a floating base tag and a
  rolling apt mirror with no lock gate. Running a build through `qb` does not
  make the result reproducible; the recipe must.
- **The real-client tier is privileged and single-host.** The KVM tier needs
  uid 0, `/dev/kvm`, `CAP_NET_ADMIN` and oras, so it runs only under qb/CI; and
  everything is loopback or a TAP namespace on one host — no second machine, no
  real network. Making its guest reproducible does **not** change this
  (section 6, portability).
- **No containerized userspace-client tier.** pynfs, pike and smbtorture run
  host-side in a namespace (`ip netns add`, which needs `CAP_NET_ADMIN` and has
  no fallback); Ceph **s3-tests exist but never run in CI** (the image lacks
  boto3) and MinIO warp is not in the tree. There is no idempotent,
  unprivileged container fixture for the cheap, run-anywhere protocol clients.
- **No Windows client** anywhere (WPTS is a protocol suite over a namespace,
  not a Windows box).
- **The provisioning tools are early.** `pveforge` is a Go CLI that drives VMs,
  networks, storage, nodes, users/ACLs and `exec` — but `vm create` is
  QEMU-only (**no LXC**), and VM **destroy exists only in its library, not as a
  CLI command**. `qa-metabuild-system` has no API or manifest — only the
  `mk/proxmox.mk` make facade, scoped to its own single host VM. Hence
  principle 1: we use the shell+make facade and grow it, not a framework.
- **Shipped-config and pass-semantics caveats** (every diskfs row uses
  `unsafe_async`; pjdfstest does not run through FUSE; some "passes" count
  WARN/NOTSUP/skip; CI retries can mask a failure) are tracked in
  [`compliance-suites.md`](compliance-suites.md); the result model (section 4)
  is designed not to inherit them.

## 4. The model: substrate × fixture lifecycle

### Substrates

| Substrate | For | Privilege / host | Provisioned by |
|---|---|---|---|
| Containers in this VM | userspace protocol clients: pynfs (v4), pike, smbtorture, boto3/s3-tests | unprivileged, local, **no metabuild dependency** | `qb` build + make/shell run |
| KVM guests on this host | kernel clients: kernel NFS (incl. **v3**) / cifs mount, multi-client, Kerberos, pNFS | privileged (uid 0, `/dev/kvm`, `CAP_NET_ADMIN`), single host | the `kvm/` wrapper (shell) |
| Proxmox VMs / LXC | multi-host, real network, OS clients (later) | a Proxmox cluster | the `mk/proxmox.mk` shell facade |

The rule from principle 5 decides which: the lightest substrate that still
proves the claim.

### The fixture lifecycle contract

Every fixture, on any substrate, exposes three idempotent make targets:

- **`up`** — build the pinned image if needed, provision and configure the
  client, converge on re-run.
- **`exercise`** — run one suite against a named Chimera endpoint, write results
  and artifacts to a declared output directory.
- **`down`** — reclaim the client (container, guest, or VM) and leave nothing
  behind.

`qa-metabuild-system/mk/proxmox.mk` already realizes this contract for a
Proxmox host VM (`pve-up`/`pve-down`/`pve-destroy`/`pve-snapshot`/`pve-rollback`,
idempotent, over `pvesh`/`qm`/SSH). It is the reference implementation to
generalize, not a shape to re-derive. The target naming and the
output-directory convention are an open item (section 9).

### Secrets

Several legs need credentials, and a harness meant to run in more than one place
needs a secrets story from the start, not bolted on:

- **Kerberos NFS** needs a keytab and `krb5.conf` (handed into the KVM guest
  over 9p today).
- **S3** needs boto3 access keys for the Ceph s3-tests leg.
- **The later Proxmox tier** needs SSH keys / API tokens (`pveforge` keeps an
  age-encrypted roster; `mk/proxmox.mk` takes `PVE_ACCOUNT`/token inputs).

The contract: no secret is baked into a fixture image or committed; each fixture
reads its secrets from an injected path or environment at `up` time, and the
unprivileged container tier must run its no-secret legs (pynfs v4, smbtorture)
without any. How secrets are stored and injected is an open item (OQ-TI-6).

### Results

A compliance **matrix** records, per run: protocol × dialect/version × suite ×
client → an outcome, against a **versioned expected-failure baseline**.

- **The outcome is not binary.** It captures `pass`, `fail`, `xfail`, and the
  distinct soft states the suites actually emit — `warn`, `notsup`, `skip` —
  **separately**. These are *not* folded into `pass`. Today's harnesses do fold
  them (pynfs WARN/NOTSUP, smbtorture exit-0-on-skip), which is exactly how a
  shrink-only baseline would entrench false greens; the matrix must record the
  real state.
- **The baseline may only shrink.** A known gap is an explicit baseline entry
  with an owner; a new genuine `fail` that is not in the baseline fails the run.
- **The baseline is net-new machinery and must be designed before fixtures
  stand on it** (OQ-TI-2). Its nearest precedent is **quint's expected-failure
  rules for strict twins** (`docs/quint-testing.md`), *not* the prerequisite
  `SKIP_RETURN_CODE 77` skip contract, which governs gated skips rather than
  known failures. The design must also account for CI retry-on-failure
  (`.gitlab-ci.yml`), which can otherwise turn a real regression green.

## 5. Reproducibility mandate

Principle 3, made concrete, with the two cases kept apart because they are not
the same size.

**Container fixtures** are built by `qb` (a `build_container` task in Chimera's
`qb.yaml`) from a Dockerfile that follows the thin discipline: base pinned by
`@sha256`, packages from a dated snapshot, a `lock-gate.py` that fails on any
unpinned package, and checksums on downloaded tools. `qb` runs the build; the
recipe carries the reproducibility. `chimera-dev` is a candidate to bring under
the same discipline over time, off the critical path.

**VM guest images** are the hard case, and the thin exemplar does **not** cover
it. Lock-gating apt packages is not the same as building a bootable qcow2 with a
chosen kernel, virtio built in, 9p shares, sshd plus a key, and a Kerberos
bring-up — the capabilities the KVM guests carry today. There is no build
orchestrator for this: `qb` has no VM path, and we are not adopting Packer or
Pulumi (principle 1). So the mandate splits into two deliverables of very
different cost:

- **Immediate (the 80/20): digest-pin the existing guest image in place.** The
  actual reproducibility defect is the *mutable tag* (`v1.9.0`, falling back to
  `latest`). The fetch computes its reference as `${VARIANT}-${VERSION}-${ARCH}`
  across four variants and two arches, so pinning it means a per-variant,
  per-arch `@sha256` digest map in place of the computed tag — a small, bounded
  change (not a single line), with no new image built. The result is immutable
  and reconstructable-to. This is a pre-wave action (section 7).
- **Larger (its own epic): a from-scratch, source-controlled reproducible guest
  recipe.** Bringing the guest build in-repo (or vendoring its recipe) with
  digest/snapshot pins and a lock gate, built by a shell+make mechanism, is a
  net-new image-build sub-project sized like an epic. It is tracked separately
  from the Wave-1 items (section 7), not bundled with them.

## 6. Tiering and where things run

| Tier | Contents | Where |
|---|---|---|
| quick | model replay (quint), strict twins, probes — unchanged | local, unprivileged |
| extended — unprivileged (new) | containerized userspace client fixtures: pynfs (v4), pike, smbtorture, s3-tests | local or CI, no privilege |
| extended — privileged | the KVM tier, with a digest-pinned guest | qb/CI (uid 0, `/dev/kvm`, `CAP_NET_ADMIN`) |
| proxmox (later) | multi-host, real network, OS/Windows clients | Proxmox via the `mk/proxmox.mk` facade, on demand |

The tier variable `CHIMERA_TEST_TIER` is two-valued (`quick` vs `extended`,
`CMakeLists.txt`). The unprivileged-vs-privileged split *within* extended is
realized by the existing **runtime probes** (netns, raised limits, `/dev/kvm`)
and ctest labels, not by the tier variable. The new unprivileged container tier
is what lets basic NFSv4 and S3 compliance run anywhere, including off the
privileged CI hosts.

**Reproducibility is not portability.** Digest-pinning the KVM guest makes it
reconstructable; it does **not** make the KVM tier portable — that tier still
needs uid 0, `/dev/kvm`, `CAP_NET_ADMIN` and single-host loopback. "Run
anywhere" is a property of the container tier only.

## 7. Roadmap

Priority follows principle 6 (NFS v3/v4 first) and principle 2 (evolve, don't
replace). The ordering below is deliberately re-cut so each item is
independently deliverable and the one epic-sized item is not hidden inside a
wave.

> **F-note (pynfs is NFSv4-only).** pynfs has no NFSv3 path; v4.2 is the v4.1
> tree at `--minorversion=2`. So the containerized pynfs fixture proves **v4
> only**. NFSv3 conformance is proven through the kernel-mount KVM tier
> (cthon04, xfstests over a v3 mount). Principle 6 is met across the two tiers,
> not by the container alone.

### Pre-wave (do now — small, unblocks everything)

- **Digest-pin the KVM guest image** — replace the computed
  `${VARIANT}-${VERSION}-${ARCH}` tag (`kvm/fetch_vm_image.sh`) with a
  per-variant, per-arch `@sha256` digest map. A small, bounded change (the tag
  encodes variant and arch, so not literally one line); closes the principle-3
  mutable-image violation immediately (section 5).
- **Add boto3 to the S3 test image** so the Ceph s3-tests leg that exists today
  actually runs in CI.

### Wave 1 — the result model, then NFSv4 (NFS-first, honestly)

- **Stand up the result model and the expected-failure baseline first**
  (section 4), so every new fixture reports into it from day one, with distinct
  WARN/NOTSUP/SKIP states and a defined baseline store/review (OQ-TI-2).
- **Containerized pynfs v4 client fixture** (v4.0/4.1/4.2), unprivileged and
  local, with the `up`/`exercise`/`down` contract and no metabuild dependency.
  This requires a **network bring-up rewrite**: today's pynfs wrapper uses
  `ip netns add` (needs `CAP_NET_ADMIN`); the unprivileged fixture must reach a
  named Chimera endpoint over plain loopback/socket instead. Lift, rewrite the
  network setup, then containerize — it is not a lift-and-shift.
- NFSv3 stays proven in the KVM tier (F-note); no v3 container work is implied.

### Wave 2 — SMB and shipped-config honesty

- **Containerized SMB client fixtures** (smbtorture, pike, and a kernel-cifs
  client — no Windows).
- **Shipped-config / durable-write runs.** Address the `unsafe_async` and
  timer-tuning caveats that [`compliance-suites.md`](compliance-suites.md)
  records, so a gate run reflects the shipped product.

### Wave 3 — backend conformance (best effort)

- Fold the storage-contract **D12** backend-conformance harness onto the shared
  substrate and result model. Best effort: lower priority than unblocking the
  protocol tests; the existing mechanisms are not replaced until a need
  requires it.

### Separate epic — reproducible KVM guest recipe

- A from-scratch, source-controlled, reproducible guest-image build (section 5,
  second bullet). Epic-sized, privileged, net-new; it follows the pre-wave
  digest-pin and does not block Waves 1–3.

### Later waves (context, unscheduled)

- A **real Windows client** leg for authoritative SMB2/3 behavior.
- A **multi-host, real-network** tier via the `mk/proxmox.mk` facade, toward the
  eventual 2-node cluster.
- **NVMeoF block** compliance, when that protocol facade materializes.
- If the Proxmox tier outgrows `qm`-over-SSH, **`pveforge`** is the nearer-ready
  candidate to grow (it already has VM destroy/clone in its library and was
  built because Terraform/Ansible/Salt were found wanting for raw QEMU) — but it
  is adopted only if a concrete need forces it, and Pulumi is not the path.

## 8. Relationship to other documents

| Document | Owns |
|---|---|
| [`compliance-suites.md`](compliance-suites.md) | which suites run; the full-run definition; per-suite deviations and open questions |
| this document | substrate, fixtures, provisioning, lifecycle, reproducibility, secrets, result model |
| [`storage-contract.md`](storage-contract.md) | the backend contract; the D12 conformance suite content (Wave 3 shares this substrate) |
| [`roadmap.md`](roadmap.md) | where the waves above sit among the delivery epics |
| [`prd.md`](prd.md) | SUC-1 (protocol suites gate) and SUC-2 (which suites) as product requirements |

## 9. Open questions

- **OQ-TI-1.** Fixture make-target naming and the artifact/output-directory
  convention for `up`/`exercise`/`down`.
- **OQ-TI-2.** The expected-failure baseline: store, format, and review/change
  process — designed **before** fixtures stand on it (section 4), accounting for
  CI retry-on-failure.
- **OQ-TI-3.** The from-scratch reproducible KVM guest recipe (the separate
  epic): vendor the `kvm-test-base` recipe into this repo, or rebuild it in-repo
  from pinned inputs. (The immediate digest-pin is already a pre-wave action,
  not an open question.)
- **OQ-TI-4.** Whether the unprivileged container tier gates on merge, or runs
  scheduled, given host and time budgets (coordinate with
  [`compliance-suites.md`](compliance-suites.md) OQ-6).
- **OQ-TI-5.** Whether `chimera-dev` is brought under the thin-image discipline,
  and on what timeline.
- **OQ-TI-6.** Secrets storage and injection (section 4, Secrets): Kerberos
  keytabs, S3 access keys, and the later Proxmox SSH/API tokens.
- **OQ-TI-7.** The unprivileged network model for the pynfs v4 fixture (loopback
  vs. a user-namespace network) and how a containerized client addresses the
  Chimera endpoint without the single-host `RESOURCE_LOCK` serialization.
