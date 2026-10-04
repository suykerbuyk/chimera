/* SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * SMB2 name-confinement probe.  Drives the in-process chimera SMB server over
 * the hand-rolled SMB2 client in smb2_mbt_common.h to pin that a path
 * component is confined to its share: a CREATE, rename or hard-link name may
 * not carry a path separator, an embedded NUL, a ".." component, or a
 * component longer than 255 bytes, and a ".", a legal 255-byte name, and an
 * empty (reconnect / share-root) name still work.  The server runs on memfs,
 * with a nested share rooted at a subdirectory so confinement is observable.
 *
 * Symlink-component cases (SL*) live in phase 3 and are added there.
 */

#include "smb2_mbt_common.h"

/* Statuses the name policy returns that smb2_mbt_common.h does not yet name. */
#ifndef ST_OBJECT_PATH_SYNTAX_BAD
#define ST_OBJECT_PATH_SYNTAX_BAD 0xC000003Bu
#endif /* ifndef ST_OBJECT_PATH_SYNTAX_BAD */
#ifndef ST_OBJECT_NAME_INVALID
#define ST_OBJECT_NAME_INVALID    0xC0000033u
#endif /* ifndef ST_OBJECT_NAME_INVALID */
#ifndef ST_NOT_A_DIRECTORY
#define ST_NOT_A_DIRECTORY        0xC0000103u
#endif /* ifndef ST_NOT_A_DIRECTORY */

static int failures = 0;

#define CHECK(cond, ...)                      \
        do {                                  \
            if (cond) {                       \
                printf("ok   - " __VA_ARGS__); \
                printf("\n");                 \
            } else {                          \
                printf("FAIL - " __VA_ARGS__); \
                printf("\n");                 \
                failures++;                   \
            }                                 \
        } while (0)

/* The share under attack is rooted at a SUBDIRECTORY of the memfs filesystem,
 * so "outside" is a sibling the client must not reach. */
static struct smb2_conn *root_c;  /* tree on share "share" (the fs root) */
static struct smb2_conn *inner_c; /* tree on share "inner" (= share/inner) */

/* Create a directory or file through a tree, OPEN_IF, and close it. */
static uint32_t
mk(
    struct smb2_conn *c,
    const char       *name,
    int               is_dir,
    const void       *data,
    uint32_t          data_len)
{
    struct smb2_create_out o;
    uint32_t               opt = is_dir ? MBT_FILE_DIRECTORY_FILE
                               : MBT_FILE_NON_DIRECTORY_FILE;
    uint32_t               st = smb2_create_opts(c, name, MBT_FILE_OPEN_IF,
                                                 MBT_FILE_ALL_ACCESS,
                                                 MBT_FILE_SHARE_RWD, opt, NULL, &o);

    if (st == ST_SUCCESS) {
        uint32_t cnt = 0;
        if (data && data_len) {
            smb2_write(c, o.file_id, 0, data, data_len, &cnt);
        }
        smb2_close(c, o.file_id);
    }
    return st;
} /* mk */

/* Does `name` exist under the fs root, reached through the root tree? */
static int
exists_at_root(const char *name)
{
    struct smb2_create_out o;
    uint32_t               st = smb2_create(root_c, name, MBT_FILE_OPEN,
                                            MBT_FILE_ALL_ACCESS,
                                            MBT_FILE_SHARE_RWD, NULL, &o);

    if (st == ST_SUCCESS) {
        smb2_close(root_c, o.file_id);
        return 1;
    }
    return 0;
} /* exists_at_root */

/* A CREATE (not creating a dir) on the inner tree; returns the status. */
static uint32_t
inner_open(
    const char *name,
    uint32_t    disp,
    uint32_t   *out_status_only)
{
    struct smb2_create_out o;
    uint32_t               st = smb2_create(inner_c, name, disp,
                                            MBT_FILE_ALL_ACCESS,
                                            MBT_FILE_SHARE_RWD, NULL, &o);

    if (st == ST_SUCCESS) {
        smb2_close(inner_c, o.file_id);
    }
    (void) out_status_only;
    return st;
} /* inner_open */

/* Open a fresh source file inside the inner share, returning its file_id for a
 * rename/link.  Leaves the handle OPEN; the caller closes via the op or
 * explicitly. */
static int
inner_source(
    const char *name,
    uint8_t     file_id[16])
{
    struct smb2_create_out o;
    uint32_t               st = smb2_create(inner_c, name, MBT_FILE_OPEN_IF,
                                            MBT_FILE_ALL_ACCESS,
                                            MBT_FILE_SHARE_RWD, NULL, &o);

    if (st != ST_SUCCESS) {
        return 0;
    }
    memcpy(file_id, o.file_id, 16);
    return 1;
} /* inner_source */

/* Still-alive check: an ECHO round-trips iff the connection was not torn down
 * by the preceding reject. */
static void
check_alive(
    struct smb2_conn *c,
    const char       *label)
{
    CHECK(smb2_echo_barrier(c) >= 0, "%s: connection still alive after reject", label);
} /* check_alive */

/* ---- the matrix --------------------------------------------------------- */

static void
run_create_rows(void)
{
    struct smb2_create_out o;
    uint32_t               st;

    /* Baseline: a legal in-share name opens. */
    CHECK(mk(inner_c, "f", 0, "data", 4) == ST_SUCCESS, "baseline create f");

    /* '/'-bearing and ".."-bearing CREATE names are refused, nothing escapes. */
    st = smb2_create(inner_c, "../outside/c1", MBT_FILE_OPEN_IF,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "F1/C1 create '../outside/c1' -> 0x%08x", st);
    CHECK(!exists_at_root("outside\\c1"), "C1 left nothing in outside");
    CHECK(!exists_at_root("inner\\../outside/c1"), "C1 left no literal dirent");

    st = smb2_create(inner_c, "sub\\../../outside/c2", MBT_FILE_OPEN_IF,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "F2/C2 create 'sub\\../../outside/c2' -> 0x%08x", st);
    CHECK(!exists_at_root("outside\\c2"), "C2 left nothing in outside");

    st = smb2_create(inner_c, "sub/c3", MBT_FILE_OPEN_IF,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "C3 create 'sub/c3' -> 0x%08x", st);

    st = smb2_create(inner_c, "sub/inner.txt", MBT_FILE_OPEN,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "F7 read 'sub/inner.txt' -> 0x%08x", st);

    /* The existing ".." statuses are unchanged (no '/' in the name). */
    st = smb2_create(inner_c, "..\\outside\\sentinel", MBT_FILE_OPEN,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_OBJECT_PATH_SYNTAX_BAD, "F3 '..\\outside\\sentinel' -> 0x%08x", st);

    st = smb2_create(inner_c, "sub\\..\\..\\outside", MBT_FILE_OPEN,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_INVALID_PARAMETER, "F4 'sub\\..\\..\\outside' -> 0x%08x", st);

    st = smb2_create(inner_c, "sub\\..", MBT_FILE_OPEN,
                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_INVALID_PARAMETER, "F6 'sub\\..' -> 0x%08x", st);

    check_alive(inner_c, "create rows");
} /* run_create_rows */

static void
run_nul_rows(void)
{
    struct smb2_create_out o;
    uint8_t                n16[64];
    int                    nlen;
    uint32_t               st;

    /* ".." + U+0000: passes the exact-".." test today; must be refused. */
    n16[0] = '.'; n16[1] = 0; n16[2] = '.'; n16[3] = 0; n16[4] = 0; n16[5] = 0;
    st     = smb2_create_raw16(inner_c, n16, 6, MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                               MBT_FILE_SHARE_RWD, MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "N1 '..'+NUL -> 0x%08x", st);

    /* 'x' NUL '\..\..\outside\sentinel': strrchr stops at the NUL today. */
    {
        const char *tail = "x";
        nlen        = utf16le(tail, n16);
        n16[nlen++] = 0; n16[nlen++] = 0;               /* embedded U+0000 */
        nlen       += utf16le("\\..\\..\\outside", n16 + nlen);
        st          = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN,
                                        MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                                        MBT_FILE_NON_DIRECTORY_FILE, &o);
        CHECK(st == ST_OBJECT_NAME_INVALID, "N2 'x'+NUL+'\\..\\..\\outside' -> 0x%08x", st);
    }

    /* 'nul' NUL 'tail' create: refused, nothing with a NUL dirent created. */
    {
        nlen        = utf16le("nul", n16);
        n16[nlen++] = 0; n16[nlen++] = 0;
        nlen       += utf16le("tail", n16 + nlen);
        st          = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                                        MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                                        MBT_FILE_NON_DIRECTORY_FILE, &o);
        CHECK(st == ST_OBJECT_NAME_INVALID, "N3 'nul'+NUL+'tail' -> 0x%08x", st);
    }

    check_alive(inner_c, "nul rows");
} /* run_nul_rows */

static void
run_length_rows(void)
{
    struct smb2_create_out o;
    uint8_t                n16[2 * 300];
    char                   nm[300];
    int                    i;
    uint32_t               st;

    /* LN: a 255-byte base name is legal; 256 is refused. */
    for (i = 0; i < 255; i++) {
        nm[i] = 'a';
    }
    nm[255] = 0;
    st      = mk(inner_c, nm, 0, NULL, 0);
    CHECK(st == ST_SUCCESS, "LN 255-byte name create -> 0x%08x", st);

    for (i = 0; i < 256; i++) {
        n16[2 * i]     = 'b';
        n16[2 * i + 1] = 0;
    }
    st = smb2_create_raw16(inner_c, n16, 2 * 256, MBT_FILE_OPEN_IF,
                           MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                           MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "LN 256-byte name create -> 0x%08x", st);

    check_alive(inner_c, "length rows");
} /* run_length_rows */

static void
run_stream_rows(void)
{
    struct smb2_create_out o;
    uint8_t                n16[2 * 1400];
    int                    i, nlen;
    uint32_t               st;

    /* Ensure the base file exists for the stream opens. */
    (void) mk(inner_c, "f", 0, NULL, 0);

    /* ST1: 'f:s' + NUL + 'x' -> refused. */
    nlen        = utf16le("f:s", n16);
    n16[nlen++] = 0; n16[nlen++] = 0;
    nlen       += utf16le("x", n16 + nlen);
    st          = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                                    MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                                    MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "ST1 'f:s'+NUL+'x' -> 0x%08x", st);

    /* ST2: a 1200-byte stream name.  On unpatched code this overran a fixed
     * buffer and crashed the server; the bound must refuse it cleanly. */
    nlen = utf16le("f:", n16);
    for (i = 0; i < 1200; i++) {
        n16[nlen++] = 'A';
        n16[nlen++] = 0;
    }
    st = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                           MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                           MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "ST2 1200-byte stream -> 0x%08x", st);

    /* ST3: 255-byte stream succeeds, 256 refused. */
    nlen = utf16le("f:", n16);
    for (i = 0; i < 255; i++) {
        n16[nlen++] = 'A';
        n16[nlen++] = 0;
    }
    st = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                           MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                           MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_SUCCESS, "ST3 255-byte stream -> 0x%08x", st);
    if (st == ST_SUCCESS) {
        smb2_close(inner_c, o.file_id);
    }

    nlen = utf16le("f:", n16);
    for (i = 0; i < 256; i++) {
        n16[nlen++] = 'A';
        n16[nlen++] = 0;
    }
    st = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                           MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                           MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "ST4 256-byte stream -> 0x%08x", st);

    check_alive(inner_c, "stream rows");
} /* run_stream_rows */

static void
run_rename_link_rows(void)
{
    uint8_t  fid[16];
    uint32_t st;

    /* R2 (the confirmed escape): rename target '..\outside\r2'. */
    if (inner_source("rsrc1", fid)) {
        st = smb2_rename(inner_c, fid, "..\\outside\\r2", 0);
        CHECK(st == ST_OBJECT_NAME_INVALID, "R2 rename '..\\outside\\r2' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\r2"), "R2 left nothing in outside");
    }

    /* R1: rename target '../outside/r1' ('/'-form). */
    if (inner_source("rsrc2", fid)) {
        st = smb2_rename(inner_c, fid, "../outside/r1", 0);
        CHECK(st == ST_OBJECT_NAME_INVALID, "R1 rename '../outside/r1' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\r1"), "R1 left nothing in outside");
    }

    /* L2 (the confirmed escape): hard-link target '..\outside\l2'. */
    if (inner_source("lsrc1", fid)) {
        st = smb2_link(inner_c, fid, "..\\outside\\l2", 0);
        CHECK(st == ST_OBJECT_NAME_INVALID, "L2 link '..\\outside\\l2' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\l2"), "L2 left nothing in outside");
    }

    /* L1: hard-link target '../outside/l1' ('/'-form). */
    if (inner_source("lsrc2", fid)) {
        st = smb2_link(inner_c, fid, "../outside/l1", 0);
        CHECK(st == ST_OBJECT_NAME_INVALID, "L1 link '../outside/l1' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\l1"), "L1 left nothing in outside");
    }

    /* Positive: an in-share rename and hard link still work. */
    if (inner_source("rsrc3", fid)) {
        st = smb2_rename(inner_c, fid, "sub\\g", 1);
        CHECK(st == ST_SUCCESS, "positive rename to 'sub\\g' -> 0x%08x", st);
        smb2_close(inner_c, fid);
    }
    if (inner_source("lsrc3", fid)) {
        st = smb2_link(inner_c, fid, "sub\\h", 1);
        CHECK(st == ST_SUCCESS, "positive link to 'sub\\h' -> 0x%08x", st);
        smb2_close(inner_c, fid);
    }

    check_alive(inner_c, "rename/link rows");
} /* run_rename_link_rows */

static void
run_positive_rows(void)
{
    uint32_t st;

    /* D1: a '.' component in the parent path still resolves. */
    st = inner_open("sub\\.\\inner.txt", MBT_FILE_OPEN, NULL);
    CHECK(st == ST_SUCCESS, "D1 'sub\\.\\inner.txt' opens -> 0x%08x", st);

    /* D2: a bare '.' final name is NAME_NOT_FOUND, as before. */
    st = inner_open("sub\\.", MBT_FILE_OPEN, NULL);
    CHECK(st == ST_OBJECT_NAME_NOT_FOUND, "D2 'sub\\.' -> 0x%08x", st);

    check_alive(inner_c, "positive rows");
} /* run_positive_rows */

/* A durable reclaim (DH2C/DHnC) names its open by FileId; the name field is
 * normally empty.  A reclaim that carries a non-empty name must be validated
 * the same way a fresh CREATE is, so a crafted reconnect cannot reach the open
 * path with a "/", a NUL or a ".." in the name.  The empty-name reclaim, the
 * legitimate form, still succeeds. */
static void
run_reconnect_rows(void)
{
    struct smb2_env         env;
    struct smb2_env_opts    opts = { .oplocks                 = 1,
                                     .leases                  = 1,
                                     .persistent_handles      = 1,
                                     .continuous_availability = 1 };
    struct smb2_oplock_req  breq;
    struct smb2_durable_req dur;
    struct smb2_create_out  o, r;
    struct smb2_conn       *a, *b;
    uint8_t                 n16[16];
    uint32_t                st;

    smb2_env_start_opts(&env, &opts);
    a = smb2_conn_open(&env);
    smb2_handshake(a);

    memset(&breq, 0, sizeof(breq));
    breq.level = SMB2_OPLOCK_LEVEL_BATCH;
    memset(&dur, 0, sizeof(dur));
    dur.dh2q  = 1;
    dur.flags = SMB2_DHANDLE_FLAG_PERSISTENT;
    memset(dur.create_guid, 0x5A, 16);
    st = smb2_create_dur(a, "recon", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, &breq, &dur, &o);
    CHECK(st == ST_SUCCESS && o.has_dh2q, "reconnect: persistent grant -> 0x%08x", st);
    if (st != ST_SUCCESS || !o.has_dh2q) {
        smb2_env_stop(&env);
        return;
    }

    smb2_conn_disconnect(a);
    smb2_quiesce(&env);
    b = smb2_conn_reopen(&env, a);

    /* A bad reconnect name is refused before the reclaim dispatch, and the
     * parked handle is untouched. */
    memset(&dur, 0, sizeof(dur));
    dur.dh2c = 1;
    memcpy(dur.file_id, o.file_id, 16);
    memset(dur.create_guid, 0x5A, 16);
    st = smb2_create_dur(b, "a/b", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    CHECK(st == ST_OBJECT_NAME_INVALID, "reconnect 'a/b' -> 0x%08x", st);

    st = smb2_create_dur(b, "x\\..\\y", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    CHECK(st == ST_INVALID_PARAMETER, "reconnect 'x\\..\\y' -> 0x%08x", st);

    n16[0] = '.'; n16[1] = 0; n16[2] = '.'; n16[3] = 0; n16[4] = 0; n16[5] = 0;
    st     = smb2_create_dur_raw16(b, n16, 6, MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                                   MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    CHECK(st == ST_OBJECT_NAME_INVALID, "reconnect '..'+NUL -> 0x%08x", st);

    /* The legitimate empty-name reclaim still works. */
    st = smb2_create_dur(b, "", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    CHECK(st == ST_SUCCESS, "reconnect empty name reclaims -> 0x%08x", st);
    if (st == ST_SUCCESS) {
        smb2_close(b, r.file_id);
    }

    smb2_quiesce(&env);
    smb2_env_stop(&env);
} /* run_reconnect_rows */

int
main(
    int   argc,
    char *argv[])
{
    struct smb2_env      env;
    struct smb2_env_opts opts = { .named_streams = 1 };

    (void) argc;
    (void) argv;

    /* Unbuffered: a per-row verdict must reach the log even if a later row
     * aborts the process, so a regression is diagnosable from the output. */
    setvbuf(stdout, NULL, _IONBF, 0);

    smb2_env_open_opts(&env, &opts);
    smb2_env_fs_setup(&env, "fs0");

    /* The nested share "inner" is rooted at the subdirectory share/inner; the
     * default share "share" is the fs root, used to plant and inspect. */
    root_c = smb2_conn_open(&env);
    smb2_handshake(root_c);

    CHECK(mk(root_c, "inner", 1, NULL, 0) == ST_SUCCESS, "mkdir inner");
    CHECK(mk(root_c, "inner\\sub", 1, NULL, 0) == ST_SUCCESS, "mkdir inner\\sub");
    CHECK(mk(root_c, "inner\\sub\\inner.txt", 0, "inner", 5) == ST_SUCCESS, "mk inner\\sub\\inner.txt");
    CHECK(mk(root_c, "outside", 1, NULL, 0) == ST_SUCCESS, "mkdir outside");
    CHECK(mk(root_c, "outside\\sentinel", 0, "SENTINEL", 8) == ST_SUCCESS, "mk outside\\sentinel");

    chimera_server_create_share(env.server, "inner", "share/inner", 0);

    inner_c = smb2_conn_open(&env);
    if (smb2_negotiate(inner_c) != ST_SUCCESS ||
        smb2_session_setup(inner_c) != ST_SUCCESS ||
        smb2_tree_connect(inner_c, "\\\\server\\inner") != ST_SUCCESS) {
        fprintf(stderr, "inner tree connect failed\n");
        return 1;
    }

    run_create_rows();
    run_nul_rows();
    run_length_rows();
    run_stream_rows();
    run_rename_link_rows();
    run_positive_rows();

    /* The share root (empty name) still opens. */
    {
        struct smb2_create_out o;
        uint32_t               st = smb2_create_opts(inner_c, "", MBT_FILE_OPEN,
                                                     MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                                                     MBT_FILE_DIRECTORY_FILE, NULL, &o);
        CHECK(st == ST_SUCCESS, "share-root open (empty name) -> 0x%08x", st);
        if (st == ST_SUCCESS) {
            smb2_close(inner_c, o.file_id);
        }
    }

    smb2_env_stop(&env);

    run_reconnect_rows();

    if (failures) {
        fprintf(stderr, "%d name-confinement check(s) FAILED\n", failures);
        return 1;
    }
    printf("all SMB2 name-confinement checks passed\n");
    return 0;
} /* main */
