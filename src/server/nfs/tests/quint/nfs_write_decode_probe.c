// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Liveness probe for the XDR decode-length bound, run against an IN-PROCESS
 * chimera NFS server over libevpl's inproc transport (see nfs3_mbt_common.h).
 *
 * The tier-independent accept/reject regression coverage for the bound lives in
 * the unit fixture chimera/nfs_common/xdr_bounds_test, which drives the
 * generated decoders directly and asserts, by return value, that a declared
 * length is bounded against the bytes present across the iovec chain before
 * anything is read, aliased or copied.  This probe adds the end-to-end leg: a
 * real server, decoding a real framed WRITE off the wire across a multi-iovec
 * receive chain, stays alive and keeps serving under that bound.
 *
 * For NFSv3 WRITE and for an NFSv4 COMPOUND carrying OP_WRITE, it hand-builds a
 * single ONC-RPC call whose WRITE `data` opaque declares more bytes than the
 * record delivers, and ships it as two RPC record-mark fragments.  The server
 * reassembles the two fragments into a MULTI-IOVEC chain, so the generated WRITE
 * decoder exercises the remaining-bytes bound that reasons across every iovec in
 * the chain.  Both WRITE `data` fields are `zcopaque`, decoded via
 * __unmarshall_opaque_zerocopy_vector -> __unmarshall_opaque_fixed_vector.
 *
 * The reply does not discriminate at this altitude.  Both generated dispatchers
 * gate the WRITE argument decode on the same emitted
 * `if (unlikely(len != length)) return 2;`, so the record is answered
 * MSG_ACCEPTED / GARBAGE_ARGS and the connection is kept up either way
 * (ext/libevpl src/rpc2/rpc2.c: a failed recv_call_dispatch sends GARBAGE_ARGS;
 * it does not disconnect).  Reply-level accept/reject discrimination therefore
 * belongs to the unit fixture, not here.  This reply also carries our bespoke
 * xid, matches no outstanding call, and is dropped as benign, so the raw
 * injection cannot wedge the client.
 *
 * The probe does not parse the reply.  It asserts the end-to-end liveness
 * property: the server is still ALIVE and the connection still USABLE
 * afterwards, proven by a subsequent NFS NULL that completes normally.  The
 * watchdog below turns a wedged connection into a prompt, loud failure.
 */

#include "nfs3_mbt_common.h"
#include "common/mbt_watchdog.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ONC RPC (RFC 5531) and NFS program constants, spelled out locally so the
 * wire bytes this probe emits are self-evident rather than pulled from a
 * generated header. */
#define RPC_MSG_CALL      0u
#define RPC_VERS2         2u
#define RPC_AUTH_NONE     0u
#define NFS_PROGRAM       100003u
#define NFS_V3            3u
#define NFS_V4            4u
#define NFSPROC3_WRITE    7u
#define NFSPROC4_COMPOUND 1u
#define NFS4_OP_WRITE     38u

/* The `data` length the WRITE declares, and the payload bytes the record
 * actually delivers.  The declared length exceeds the delivered bytes, and the
 * delivered bytes straddle the fragment boundary, so the decoder's
 * remaining-bytes sum crosses an iovec seam. */
#define DECLARED_LEN      4096u
#define DELIVERED_LEN     8u

static int failures;

/* ---- flat big-endian XDR writers ---------------------------------------- */

static void
put_u32(
    uint8_t *b,
    size_t  *o,
    uint32_t v)
{
    b[(*o)++] = (uint8_t) (v >> 24);
    b[(*o)++] = (uint8_t) (v >> 16);
    b[(*o)++] = (uint8_t) (v >> 8);
    b[(*o)++] = (uint8_t) v;
} /* put_u32 */

static void
put_u64(
    uint8_t *b,
    size_t  *o,
    uint64_t v)
{
    put_u32(b, o, (uint32_t) (v >> 32));
    put_u32(b, o, (uint32_t) (v & 0xffffffffu));
} /* put_u64 */

static void
put_bytes(
    uint8_t    *b,
    size_t     *o,
    const void *src,
    size_t      n)
{
    memcpy(b + *o, src, n);
    *o += n;
} /* put_bytes */

/* The 40-byte ONC-RPC call preamble shared by every call: xid, CALL, rpcvers,
 * {prog,vers,proc}, then AUTH_NONE cred and verf (flavor + zero-length body). */
static void
put_rpc_call_header(
    uint8_t *b,
    size_t  *o,
    uint32_t xid,
    uint32_t vers,
    uint32_t proc)
{
    put_u32(b, o, xid);
    put_u32(b, o, RPC_MSG_CALL);
    put_u32(b, o, RPC_VERS2);
    put_u32(b, o, NFS_PROGRAM);
    put_u32(b, o, vers);
    put_u32(b, o, proc);
    put_u32(b, o, RPC_AUTH_NONE);   /* cred flavor */
    put_u32(b, o, 0);               /* cred body length */
    put_u32(b, o, RPC_AUTH_NONE);   /* verf flavor */
    put_u32(b, o, 0);               /* verf body length */
} /* put_rpc_call_header */

/*
 * Ship one RPC message as two ONC-RPC record-mark fragments: a non-terminal
 * fragment carrying msg[0, split) and a terminal (high-bit) fragment carrying
 * msg[split, len).  The server's rpc2 record-mark reassembly clones each
 * fragment's payload iovec into its accumulator, so the decoder is handed a
 * chain of at least two iovecs -- which is what forces the bound to reason
 * about bytes present across the whole chain, not just the current iovec.
 */
static void
send_two_fragments(
    struct mbt_env *env,
    const uint8_t  *msg,
    size_t          len,
    size_t          split)
{
    uint8_t frag[512];
    size_t  o;

    /* Fragment 1: mark (length, high bit clear = more to come) + first bytes. */
    o = 0;
    put_u32(frag, &o, (uint32_t) split);
    put_bytes(frag, &o, msg, split);
    evpl_send(env->evpl, env->nfs_conn->bind, frag, (unsigned int) o);

    /* Fragment 2: mark (length | 0x80000000 = last fragment) + the remainder. */
    o = 0;
    put_u32(frag, &o, 0x80000000u | (uint32_t) (len - split));
    put_bytes(frag, &o, msg + split, len - split);
    evpl_send(env->evpl, env->nfs_conn->bind, frag, (unsigned int) o);
} /* send_two_fragments */

/* Assert the server survived the injected record and the connection is still
 * usable: a plain NFS NULL must complete.  mbt_null() pumps the client loop
 * until the reply lands and aborts the process on any transport-level failure,
 * so simply returning from it is the liveness proof. */
static void
expect_alive(
    struct mbt_env *env,
    const char     *label)
{
    struct mbt_result *res = mbt_null(env);

    if (res->rpc_err != 0) {
        printf("  FAIL %s: NULL after injected record failed, rpc_err=%d\n",
               label, res->rpc_err);
        failures++;
    } else {
        printf("  ok  %s: server alive, connection usable (NULL succeeded)\n",
               label);
    }
} /* expect_alive */

/* NFSv3 WRITE whose `data` opaque declares DECLARED_LEN bytes but delivers
 * only DELIVERED_LEN, split across the fragment boundary. */
static void
probe_nfs3_write(struct mbt_env *env)
{
    uint8_t msg[256];
    uint8_t filler[DELIVERED_LEN];
    size_t  o = 0;
    size_t  data_off;

    memset(filler, 0xAA, sizeof(filler));

    put_rpc_call_header(msg, &o, 0x7F000001u, NFS_V3, NFSPROC3_WRITE);
    /* WRITE3args: file nfs_fh3<> (empty), offset, count, stable, data<>. */
    put_u32(msg, &o, 0);                  /* file handle: zero-length opaque */
    put_u64(msg, &o, 0);                  /* offset */
    put_u32(msg, &o, DECLARED_LEN);   /* count (advisory; unchecked here) */
    put_u32(msg, &o, 0);                  /* stable = UNSTABLE */
    put_u32(msg, &o, DECLARED_LEN);   /* data length (declared) */
    data_off = o;
    put_bytes(msg, &o, filler, sizeof(filler));

    printf("NFSv3 WRITE, data declares %u bytes, record delivers %u "
           "(across 2 iovecs):\n", DECLARED_LEN, DELIVERED_LEN);
    send_two_fragments(env, msg, o, data_off + DELIVERED_LEN / 2);
    expect_alive(env, "NFSv3 WRITE decode-length bound");
} /* probe_nfs3_write */

/* NFSv4 COMPOUND { OP_WRITE } whose `data` opaque declares the same way. */
static void
probe_nfs4_write(struct mbt_env *env)
{
    uint8_t msg[256];
    uint8_t filler[DELIVERED_LEN];
    uint8_t stateid[16];
    size_t  o = 0;
    size_t  data_off;

    memset(filler, 0xBB, sizeof(filler));
    memset(stateid, 0, sizeof(stateid));

    put_rpc_call_header(msg, &o, 0x7F000002u, NFS_V4, NFSPROC4_COMPOUND);
    /* COMPOUND4args: tag utf8str_cs<> (empty), minorversion, argarray<>. */
    put_u32(msg, &o, 0);                  /* tag length */
    put_u32(msg, &o, 0);                  /* minorversion = 0 */
    put_u32(msg, &o, 1);                  /* argarray count = 1 */
    put_u32(msg, &o, NFS4_OP_WRITE);      /* argop */
    /* WRITE4args: stateid (seqid + other[12]), offset, stable, data<>. */
    put_bytes(msg, &o, stateid, sizeof(stateid));
    put_u64(msg, &o, 0);                  /* offset */
    put_u32(msg, &o, 0);                  /* stable = UNSTABLE4 */
    put_u32(msg, &o, DECLARED_LEN);   /* data length (declared) */
    data_off = o;
    put_bytes(msg, &o, filler, sizeof(filler));

    printf("NFSv4 COMPOUND/OP_WRITE, data declares %u bytes, record delivers "
           "%u (across 2 iovecs):\n", DECLARED_LEN, DELIVERED_LEN);
    send_two_fragments(env, msg, o, data_off + DELIVERED_LEN / 2);
    expect_alive(env, "NFSv4 WRITE decode-length bound");
} /* probe_nfs4_write */

int
main(
    int    argc,
    char **argv)
{
    struct mbt_env     *env = malloc(sizeof(*env));
    struct mbt_env_opts opts;

    /* A crash or a wedged connection would otherwise hang a pump loop forever;
     * the watchdog turns that into a prompt, loud failure. */
    mbt_watchdog_arm(60);

    memset(&opts, 0, sizeof(opts));
    opts.sec    = mbt_sec_scan_argv(argc, argv);
    opts.module = "memfs";
    mbt_env_start_opts(env, &opts);

    /* Baseline: the connection is healthy before any injected traffic. */
    if (mbt_null(env)->rpc_err != 0) {
        fprintf(stderr, "baseline NULL failed; harness broken\n");
        return 1;
    }
    printf("baseline: connection healthy\n");

    probe_nfs3_write(env);
    probe_nfs4_write(env);

    mbt_env_fs_teardown(env, "fs0");
    mbt_env_stop(env);
    free(env);

    if (failures) {
        printf("\n%d decode-length reachability assertion(s) failed.\n",
               failures);
        return 1;
    }
    printf("\nBoth WRITE decode-length bounds reached end-to-end; server "
           "bounds decode lengths against received bytes and stays alive.\n");
    return 0;
} /* main */
