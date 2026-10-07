// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * SMB3 persistent-handle COLD reclaim probe.
 *
 * A "cold" reclaim is a durable-handle reconnect (DH2C/DHnC) whose matching
 * handle was recovered from the persistent KV AFTER the server restarted and
 * the original connection/session is gone.  This probe drives a genuine
 * in-process restart over the persistent sqlite KV and a persistent (linux
 * passthrough) file backend, so both the handle record and the file survive,
 * then exercises the full enforcement matrix with NEGATIVE CONTROLS -- each
 * gate is shown to be capable of FAILING a reclaim, not merely passing one:
 *
 *   - same owner + empty name  -> SUCCESS, re-opening the RECORDED path
 *     (data written before the restart reads back through the reclaim);
 *   - same owner + differing non-empty name -> INVALID_PARAMETER (and the
 *     entry is NOT consumed);
 *   - a different authenticated uid (same ClientGuid) -> ACCESS_DENIED;
 *   - a wrong / missing CreateGuid (incl. a DHnC v1 reconnect) -> OBJECT_NAME_NOT_FOUND;
 *   - a reclaim through the wrong share -> OBJECT_NAME_NOT_FOUND;
 *   - a conflicting fresh open of an unreclaimed cold file -> FILE_NOT_AVAILABLE,
 *     then SUCCESS once the reclaim window lapses.
 *
 * The real-client cross-restart proof (smbtorture smb2.durable-v2-open) is out
 * of scope here and is deferred to the Wave-2 real-client SMB harness; this is
 * the quick-tier, unprivileged gate.
 */

#include <sys/stat.h>

#include "smb2_mbt_common.h"

static int nfail = 0;

#define EXPECT(ok, ...)                              \
        do {                                         \
            if (ok) { printf("ok   - "); }           \
            else { printf("FAIL - "); nfail++; }     \
            printf(__VA_ARGS__);                     \
            printf("\n");                            \
        } while (0)

#define NOTE(...)                                    \
        do { printf("note - "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* A second local user, distinct uid, so the owner gate can be shown to deny a
 * different authenticated principal reclaiming someone else's handle. */
#define USER2       "smbtest2"
#define PASS2       "Chimera!MBT2"
#define UID2        2000u
#define GID2        2000u

/* The share the handle is granted on, and a second CA share used to prove the
 * share binding refuses a cross-share reclaim. */
#define SHARE_PATH  "\\\\server\\share"
#define SHARE2_PATH "\\\\server\\share2"

static void
fill_guid(
    uint8_t guid[16],
    int     tag)
{
    memset(guid, 0, 16);
    guid[0]  = (uint8_t) tag;
    guid[1]  = 0xD0;
    guid[2]  = 0x0D;
    guid[15] = (uint8_t) ~tag;
} /* fill_guid */

static void
mk_oplock(
    struct smb2_oplock_req *r,
    uint8_t                 level)
{
    memset(r, 0, sizeof(*r));
    r->level = level;
} /* mk_oplock */

/* Mount the linux passthrough backend for a cold-reclaim share, honoring the
 * skip contract.  The linux backend requires a name_to_handle_at-capable
 * (non-tmpfs) filesystem and CAP_DAC_READ_SEARCH; when either is absent
 * chimera_server_mount returns the unavailability errno synchronously (it drives
 * the mount on a private evpl and returns the backend's status).  Mirror the
 * NFS/S3 cold siblings exactly (nfs3_mbt_common.h:1478-1514,
 * s3_mbt_common.h:749-782): {ENOTSUP,EPERM,ENXIO} -> SKIP, any other non-zero ->
 * hard fail.  _exit (not exit) is load-bearing: atexit(evpl_cleanup) aborts if a
 * server/evpl context is still live, which would SIGABRT over the skip. */
static void
cold_mount_or_skip(
    struct smb2_env *env,
    const char      *share,
    const char      *dir)
{
    int mrc = chimera_server_mount(env->server, share, "linux", dir, NULL);

    if (mrc == CHIMERA_VFS_ENOTSUP) {
        fprintf(stderr, "SKIP: linux backend needs a name_to_handle_at-capable "
                "scratch fs; %s is not one (set CHIMERA_MBT_SCRATCH)\n", dir);
        _exit(77);
    }
    if (mrc == CHIMERA_VFS_EPERM) {
        fprintf(stderr, "SKIP: linux backend needs CAP_DAC_READ_SEARCH "
                "(open_by_handle_at); this process is unprivileged\n");
        _exit(77);
    }
    if (mrc == CHIMERA_VFS_ENXIO) {
        fprintf(stderr, "SKIP: linux backend unavailable in this process "
                "(module init failed, e.g. io_uring_setup ENOSYS)\n");
        _exit(77);
    }
    if (mrc != 0) {
        fprintf(stderr, "mount linux at %s failed: status=%d\n", dir, mrc);
        _exit(1);
    }
} /* cold_mount_or_skip */

/* Register the second user, and (re-)establish the two persistent, continuously
 * available linux-backed shares.  Called after the initial bring-up and again
 * after each restart (chimera_server_destroy drops users, mounts and shares;
 * the backing directories and the sqlite KV under session_dir persist). */
static void
cold_env_services(struct smb2_env *env)
{
    const uint32_t gids[1] = { GID2 };
    char           d1[320], d2[320];

    if (chimera_server_add_user(env->server, USER2, PASS2, PASS2, NULL,
                                UID2, GID2, 1, gids, 1) != 0) {
        fprintf(stderr, "failed to register second SMB user\n");
        exit(1);
    }

    snprintf(d1, sizeof(d1), "%s/data", env->session_dir);
    snprintf(d2, sizeof(d2), "%s/data2", env->session_dir);
    mkdir(d1, 0700);   /* idempotent: survives the restart */
    mkdir(d2, 0700);

    cold_mount_or_skip(env, "share", d1);
    chimera_server_create_share(env->server, "share", "share", 1 /* CA */);
    cold_mount_or_skip(env, "share2", d2);
    chimera_server_create_share(env->server, "share2", "share2", 1 /* CA */);
} /* cold_env_services */

/* Open a connection with an explicit ClientGuid tag and authenticate as `user`,
 * tree-connecting to `share`.  guid_tag fixes the ClientGuid so several
 * connections can present as the SAME client (the owner gate, not the
 * ClientGuid, is then what distinguishes principals). */
static struct smb2_conn *
cold_conn(
    struct smb2_env *env,
    int              guid_tag,
    const char      *user,
    const char      *pass,
    const char      *share)
{
    struct smb2_conn *c = smb2_conn_open(env);
    uint32_t          st;

    c->guid_tag = guid_tag;

    st = smb2_negotiate(c);
    if (st != ST_SUCCESS) {
        fprintf(stderr, "NEGOTIATE failed: 0x%08x\n", st);
        exit(1);
    }
    st = smb2_session_setup_as(c, user, pass);
    if (st != ST_SUCCESS) {
        fprintf(stderr, "SESSION_SETUP as %s failed: 0x%08x\n", user, st);
        exit(1);
    }
    st = smb2_tree_connect(c, share);
    if (st != ST_SUCCESS) {
        fprintf(stderr, "TREE_CONNECT %s failed: 0x%08x\n", share, st);
        exit(1);
    }
    return c;
} /* cold_conn */

int
main(
    int   argc,
    char *argv[])
{
    struct smb2_env                 env;
    const struct smb2_wire_profile *wire = smb2_wire_profile_find("ntlmv2");
    struct smb2_env_opts            opts = { .oplocks                 = 1,
                                             .leases                  = 1,
                                             .directory_leases        = 1,
                                             .persistent_handles      = 1,
                                             .continuous_availability = 1,
                                             .kv_module               = "sqlite" };
    struct smb2_oplock_req          breq;
    struct smb2_durable_req         dur;
    struct smb2_create_out          o, ow, r;
    struct smb2_conn               *a, *b1, *b2, *bs2;
    uint32_t                        st, n = 0;
    uint8_t                         rdbuf[32];
    uint64_t                        deadline;

    (void) argc;
    (void) argv;

    printf("# SMB3 persistent-handle COLD reclaim probe"
           " (sqlite KV + persistent linux backend)\n");

    /* --- bring up, grant two persistent handles, write data, then RESTART. */
    smb2_env_open_wire(&env, &opts, wire);
    cold_env_services(&env);

    a = cold_conn(&env, 0, SMB2W_USER, SMB2W_PASSWORD, SHARE_PATH);
    mk_oplock(&breq, SMB2_OPLOCK_LEVEL_BATCH);

    /* cold.txt: the handle the reclaim matrix targets. */
    memset(&dur, 0, sizeof(dur));
    dur.dh2q  = 1;
    dur.flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    fill_guid(dur.create_guid, 0xC0);
    st = smb2_create_dur(a, "cold.txt", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, &breq, &dur, &o);
    EXPECT(st == ST_SUCCESS && o.has_dh2q &&
           (o.dh2q_flags & SMB2_DHANDLE_FLAG_PERSISTENT),
           "grant cold.txt persistent handle (st 0x%08x dh2q %d flags 0x%08x)",
           st, o.has_dh2q, o.dh2q_flags);
    if (st != ST_SUCCESS || !o.has_dh2q) {
        smb2_env_stop(&env);
        return 1;
    }
    st = smb2_write(a, o.file_id, 0, "COLDDATA01", 10, &n);
    EXPECT(st == ST_SUCCESS && n == 10, "wrote 10 bytes to cold.txt (st 0x%08x n %u)",
           st, n);

    /* win.txt: a short reclaim timeout so the conflict-window expiry is testable
     * in-process (the window is min(recorded durable timeout, the operator cap);
     * 800 ms keeps the test quick). */
    memset(&dur, 0, sizeof(dur));
    dur.dh2q       = 1;
    dur.flags      = SMB2_DHANDLE_FLAG_PERSISTENT;
    dur.timeout_ms = 800;
    fill_guid(dur.create_guid, 0xC1);
    st = smb2_create_dur(a, "win.txt", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, &breq, &dur, &ow);
    EXPECT(st == ST_SUCCESS && ow.has_dh2q,
           "grant win.txt persistent handle (st 0x%08x)", st);

    /* Let the server flush the persisted records to the sqlite KV before the
     * restart drops the in-memory state. */
    smb2_quiesce(&env);

    printf("# --- server restart (records recovered from sqlite KV) ---\n");
    smb2_env_restart(&env);
    cold_env_services(&env);

    /* NC1 (also the first CREATE on the CA share -> triggers lazy cold recovery):
     * a wrong CreateGuid DH2C reclaim is refused and leaves the entry intact. */
    b1 = cold_conn(&env, 0, SMB2W_USER, SMB2W_PASSWORD, SHARE_PATH);
    memset(&dur, 0, sizeof(dur));
    dur.dh2c            = 1;
    dur.reconnect_flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    memcpy(dur.file_id, o.file_id, 16);
    fill_guid(dur.create_guid, 0xFF);   /* wrong */
    st = smb2_create_dur(b1, "", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    EXPECT(st == ST_OBJECT_NAME_NOT_FOUND,
           "NC wrong CreateGuid -> OBJECT_NAME_NOT_FOUND (0x%08x)", st);

    /* NC2: a DHnC (v1) reconnect cannot reclaim a persistent cold handle (it
     * carries no CreateGuid to verify). */
    memset(&dur, 0, sizeof(dur));
    dur.dhnc = 1;
    memcpy(dur.file_id, o.file_id, 16);
    st = smb2_create_dur(b1, "", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    EXPECT(st == ST_OBJECT_NAME_NOT_FOUND,
           "NC DHnC v1 reclaim of a persistent cold handle -> OBJECT_NAME_NOT_FOUND"
           " (0x%08x)", st);

    /* NC3: wrong share.  Same owner, correct CreateGuid, but reclaimed through a
     * tree connected to a DIFFERENT CA share. */
    bs2 = cold_conn(&env, 0, SMB2W_USER, SMB2W_PASSWORD, SHARE2_PATH);
    memset(&dur, 0, sizeof(dur));
    dur.dh2c            = 1;
    dur.reconnect_flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    memcpy(dur.file_id, o.file_id, 16);
    fill_guid(dur.create_guid, 0xC0);
    st = smb2_create_dur(bs2, "", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    EXPECT(st == ST_OBJECT_NAME_NOT_FOUND,
           "NC wrong share -> OBJECT_NAME_NOT_FOUND (0x%08x)", st);

    /* NC4: a different authenticated uid, SAME ClientGuid, correct CreateGuid --
     * the owner gate denies it. */
    b2 = cold_conn(&env, 0, USER2, PASS2, SHARE_PATH);
    memset(&dur, 0, sizeof(dur));
    dur.dh2c            = 1;
    dur.reconnect_flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    memcpy(dur.file_id, o.file_id, 16);
    fill_guid(dur.create_guid, 0xC0);
    st = smb2_create_dur(b2, "", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    EXPECT(st == ST_ACCESS_DENIED,
           "NC different owner uid -> ACCESS_DENIED (0x%08x)", st);

    /* NC5: the owner with a NON-EMPTY name that is not the recorded path --
     * the reclaim must not be redirected.  Entry is kept (proven by the
     * positive reclaim that follows). */
    memset(&dur, 0, sizeof(dur));
    dur.dh2c            = 1;
    dur.reconnect_flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    memcpy(dur.file_id, o.file_id, 16);
    fill_guid(dur.create_guid, 0xC0);
    st = smb2_create_dur(b1, "decoy.txt", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    EXPECT(st == ST_INVALID_PARAMETER,
           "NC differing reconnect name -> INVALID_PARAMETER (0x%08x)", st);

    /* Conflict: a fresh open of the still-unreclaimed cold win.txt is blocked. */
    st = smb2_create(b1, "win.txt", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &r);
    EXPECT(st == ST_FILE_NOT_AVAILABLE,
           "conflicting open of an unreclaimed cold file -> FILE_NOT_AVAILABLE"
           " (0x%08x)", st);
    if (st == ST_SUCCESS) {
        smb2_close(b1, r.file_id);
    }

    /* Positive: the owner reclaims cold.txt with an empty name.  It re-opens the
     * RECORDED path and the pre-restart data reads back, proving the reopen
     * targets the right file and the earlier gates left the entry intact. */
    memset(&dur, 0, sizeof(dur));
    dur.dh2c            = 1;
    dur.reconnect_flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    memcpy(dur.file_id, o.file_id, 16);
    fill_guid(dur.create_guid, 0xC0);
    st = smb2_create_dur(b1, "", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    EXPECT(st == ST_SUCCESS, "positive cold reclaim (same owner, empty name)"
           " -> SUCCESS (0x%08x)", st);
    if (st == ST_SUCCESS) {
        EXPECT(memcmp(r.file_id, o.file_id, 16) == 0,
               "cold reclaim carries the ORIGINAL FileId");
        memset(rdbuf, 0, sizeof(rdbuf));
        n  = 0;
        st = smb2_read(b1, r.file_id, 0, 10, rdbuf, &n);
        EXPECT(st == ST_SUCCESS && n == 10 && memcmp(rdbuf, "COLDDATA01", 10) == 0,
               "pre-restart data reads back through the cold reclaim"
               " (st 0x%08x n %u)", st, n);
        smb2_close(b1, r.file_id);
    }

    /* Window expiry: once win.txt's cold reclaim window lapses, the reservation
     * is released and a conflicting open succeeds.  The window was ~800 ms from
     * recovery (triggered at NC1); wait past it (plus the 1 s sweeper tick). */
    deadline = smb2c_now_ms() + 2200;
    while (smb2c_now_ms() < deadline) {
        smb2_pump(&env);
    }
    st = smb2_create(b1, "win.txt", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &r);
    EXPECT(st == ST_SUCCESS,
           "after the reclaim window lapses, a conflicting open succeeds"
           " (0x%08x)", st);
    if (st == ST_SUCCESS) {
        smb2_close(b1, r.file_id);
    }

    smb2_quiesce(&env);
    smb2_env_stop(&env);

    printf("# cold reclaim probe: %s (%d failure(s))\n",
           nfail ? "FAIL" : "PASS", nfail);
    return nfail ? 1 : 0;
} /* main */
