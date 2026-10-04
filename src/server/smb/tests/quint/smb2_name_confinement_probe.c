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
 * Symlink-component cases (SL*) follow, covering the parent-walk and
 * stop-on-symlink paths.
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
#ifndef ST_STOPPED_ON_SYMLINK
#define ST_STOPPED_ON_SYMLINK     0x8000002Du
#endif /* ifndef ST_STOPPED_ON_SYMLINK */
#ifndef SMB2_SYMLINK_ERROR_TAG
#define SMB2_SYMLINK_ERROR_TAG    0x4C4D5953u  /* 'SYML' */
#endif /* ifndef SMB2_SYMLINK_ERROR_TAG */

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

/* The confined share is rooted at a SUBDIRECTORY of the memfs filesystem, so
 * "outside" is a sibling the client must not reach. */
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

/* Turn an existing file into a symlink via FSCTL_SET_REPARSE_POINT.  `relative`
 * picks SYMLINK_FLAG_RELATIVE vs ABSOLUTE.  Returns the IOCTL status. */
static uint32_t
plant_symlink(
    struct smb2_conn *c,
    const char       *name,
    const char       *target,
    int               relative)
{
    struct smb2_create_out o;
    uint8_t                in[512];
    int                    nlen = (int) strlen(target) * 2;
    uint32_t               st;

    st = smb2_create(c, name, MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    if (st != ST_SUCCESS) {
        return st;
    }

    p32(in, 0, SMB2_IO_REPARSE_TAG_SYMLINK);
    p16(in, 4, (uint16_t) (12 + 2 * nlen));   /* ReparseDataLength */
    p16(in, 6, 0);
    p16(in, 8, 0);                            /* SubstituteNameOffset */
    p16(in, 10, (uint16_t) nlen);             /* SubstituteNameLength */
    p16(in, 12, (uint16_t) nlen);             /* PrintNameOffset */
    p16(in, 14, (uint16_t) nlen);             /* PrintNameLength */
    p32(in, 16, relative ? 1 : 0);            /* SYMLINK_FLAG_RELATIVE */
    utf16le(target, in + 20);
    utf16le(target, in + 20 + nlen);

    st = smb2_ioctl(c, SMB2_FSCTL_SET_REPARSE_POINT, o.file_id, in,
                    (uint32_t) (20 + 2 * nlen));
    smb2_close(c, o.file_id);
    return st;
} /* plant_symlink */

/* Parse the SymbolicLinkErrorResponse left in the connection's receive buffer by
 * the most recent STOPPED_ON_SYMLINK reply; returns its UnparsedPathLength (in
 * UTF-16 bytes), or -1 if the body is not a well-formed symlink error. */
static int
last_symlink_unparsed(struct smb2_conn *c)
{
    const uint8_t *body = c->rbuf + 4 + SMB2_HDR_SIZE;

    if (c->rlen < 4 + SMB2_HDR_SIZE + 24) {
        return -1;
    }
    if (g16(body, 0) != 9) {            /* ERROR Response StructureSize */
        return -1;
    }
    if (g32(body, 12) != SMB2_SYMLINK_ERROR_TAG) {
        return -1;
    }
    return (int) g16(body, 22);         /* UnparsedPathLength */
} /* last_symlink_unparsed */

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

    /* ST2: a 1200-byte stream name.  The stream-name bound must refuse an
     * over-long name cleanly. */
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
    CHECK(st == ST_OBJECT_NAME_INVALID, "ST4-stream 256-byte stream -> 0x%08x", st);

    /* ST4 as planned: a 255-byte BASE with a stream suffix succeeds; a 256-byte
     * base with a stream suffix is refused (the base-name bound, with a
     * stream present). */
    nlen = 0;
    for (i = 0; i < 255; i++) {
        n16[nlen++] = 'B';
        n16[nlen++] = 0;
    }
    nlen += utf16le(":s", n16 + nlen);
    st    = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                              MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                              MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_SUCCESS, "ST4 255-byte base + ':s' -> 0x%08x", st);
    if (st == ST_SUCCESS) {
        smb2_close(inner_c, o.file_id);
    }

    nlen = 0;
    for (i = 0; i < 256; i++) {
        n16[nlen++] = 'C';
        n16[nlen++] = 0;
    }
    nlen += utf16le(":s", n16 + nlen);
    st    = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN_IF,
                              MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                              MBT_FILE_NON_DIRECTORY_FILE, &o);
    CHECK(st == ST_OBJECT_NAME_INVALID, "ST4 256-byte base + ':s' -> 0x%08x", st);

    /* P1 (stream positive): a legitimate named stream still opens. */
    st = smb2_create(inner_c, "f:s", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_SUCCESS, "P1 'f:s' stream opens -> 0x%08x", st);
    if (st == ST_SUCCESS) {
        smb2_close(inner_c, o.file_id);
    }

    check_alive(inner_c, "stream rows");
} /* run_stream_rows */

static void
run_rename_link_rows(void)
{
    uint8_t  fid[16];
    uint32_t st;

    /* R2: rename target '..\outside\r2' (a backslash ".." form). */
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

    /* L2: hard-link target '..\outside\l2' (a backslash ".." form). */
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

    /* R3 / N4: a rename target carrying an embedded NUL (strrchr stops at it)
     * is refused; nothing escapes. */
    if (inner_source("rsrc4", fid)) {
        uint8_t n16[64];
        int     nlen = utf16le("x", n16);
        n16[nlen++] = 0; n16[nlen++] = 0;              /* embedded U+0000 */
        nlen       += utf16le("\\..\\..\\outside\\n4", n16 + nlen);
        st          = smb2_rename_raw16(inner_c, fid, n16, nlen, 0);
        CHECK(st == ST_OBJECT_NAME_INVALID, "R3/N4 rename 'x'+NUL+'\\..\\..\\outside\\n4' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\n4"), "R3/N4 left nothing in outside");
    }

    /* L-NUL: a hard-link target with an embedded NUL is refused. */
    if (inner_source("lsrc4", fid)) {
        uint8_t n16[64];
        int     nlen = utf16le("y", n16);
        n16[nlen++] = 0; n16[nlen++] = 0;
        nlen       += utf16le("\\..\\..\\outside\\lnul", n16 + nlen);
        st          = smb2_link_raw16(inner_c, fid, n16, nlen, 0);
        CHECK(st == ST_OBJECT_NAME_INVALID, "L-NUL link 'y'+NUL+'\\..\\..\\outside\\lnul' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\lnul"), "L-NUL left nothing in outside");
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

/* Symlink-component confinement: a symlink used as a NON-final parent component
 * must stop the walk (STOPPED_ON_SYMLINK for CREATE, NOT_A_DIRECTORY for
 * rename/link), never be followed server-side.  A symlink as the LAST parent
 * component already stopped; a legitimate in-share link now also stops, and the
 * client resolves it and retries. */
static void
run_symlink_rows(void)
{
    struct smb2_create_out o;
    uint8_t                fid[16];
    uint32_t               st;
    int                    unparsed;

    /* Plant, through the root tree: inner/esc -> ../outside (climbs out),
     * inner/abs -> / (absolute), inner/al -> sub (legit in-share), and the
     * directories a legitimate retry needs. */
    (void) mk(root_c, "inner\\sub\\deeper", 1, NULL, 0);
    (void) mk(root_c, "inner\\sub\\deeper\\f", 0, "deep", 4);
    (void) mk(root_c, "outside\\d", 1, NULL, 0);
    (void) mk(root_c, "outside\\d\\target", 0, "OUT", 3);
    CHECK(plant_symlink(root_c, "inner\\esc", "../outside", 1) == ST_SUCCESS, "plant esc -> ../outside");
    CHECK(plant_symlink(root_c, "inner\\abs", "/", 0) == ST_SUCCESS, "plant abs -> /");
    CHECK(plant_symlink(root_c, "inner\\al", "sub", 1) == ST_SUCCESS, "plant al -> sub");

    /* SL1: read esc\d\target -- the escaping link as a non-final component. */
    st = smb2_create(inner_c, "esc\\d\\target", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL1 'esc\\d\\target' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\d\\target") * 2,
          "SL1 UnparsedPathLength=%d (expect %d)", unparsed, (int) strlen("\\d\\target") * 2);

    /* SL2: create esc\d\new -- nothing lands outside. */
    st = smb2_create(inner_c, "esc\\d\\new", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL2 create 'esc\\d\\new' -> 0x%08x", st);
    CHECK(!exists_at_root("outside\\d\\new"), "SL2 left nothing in outside/d");

    /* SL3/SL4: rename/link target through the link -- NOT_A_DIRECTORY. */
    if (inner_source("slsrc1", fid)) {
        st = smb2_rename(inner_c, fid, "esc\\d\\x", 0);
        CHECK(st == ST_NOT_A_DIRECTORY, "SL3 rename 'esc\\d\\x' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\d\\x"), "SL3 left nothing in outside/d");
    }
    if (inner_source("slsrc2", fid)) {
        st = smb2_link(inner_c, fid, "esc\\d\\y", 0);
        CHECK(st == ST_NOT_A_DIRECTORY, "SL4 link 'esc\\d\\y' -> 0x%08x", st);
        smb2_close(inner_c, fid);
        CHECK(!exists_at_root("outside\\d\\y"), "SL4 left nothing in outside/d");
    }

    /* SL5: absolute-target link as a non-final component -> STOPPED. */
    st = smb2_create(inner_c, "abs\\x\\y", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL5 'abs\\x\\y' -> 0x%08x", st);

    /* SL6: a legitimate in-share link now stops too (decision 9); the client
     * re-issues the resolved path, which opens. */
    st = smb2_create(inner_c, "al\\deeper\\f", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL6 'al\\deeper\\f' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\deeper\\f") * 2,
          "SL6 UnparsedPathLength=%d (expect %d)", unparsed, (int) strlen("\\deeper\\f") * 2);
    st = smb2_create(inner_c, "sub\\deeper\\f", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_SUCCESS, "SL6 client retry 'sub\\deeper\\f' opens -> 0x%08x", st);
    if (st == ST_SUCCESS) {
        smb2_close(inner_c, o.file_id);
    }

    /* SL7: a link as the LAST parent component of a single-component parent ->
     * STOPPED (unchanged), unparsed is just the final name. */
    st = smb2_create(inner_c, "al\\inner.txt", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL7 'al\\inner.txt' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\inner.txt") * 2,
          "SL7 UnparsedPathLength=%d (expect %d)", unparsed, (int) strlen("\\inner.txt") * 2);

    /* CR1: a link that is the LAST component of a parent with two or more
     * components must still carry the symlink error body (it regressed to a
     * bare STOPPED).  Plant links one and two directories deep. */
    (void) mk(root_c, "inner\\sub\\mid", 1, NULL, 0);
    CHECK(plant_symlink(root_c, "inner\\sub\\escd", "../../outside/d", 1) == ST_SUCCESS,
          "plant sub\\escd -> ../../outside/d");
    CHECK(plant_symlink(root_c, "inner\\sub\\mid\\escd3", "../../../outside/d", 1) == ST_SUCCESS,
          "plant sub\\mid\\escd3 -> ../../../outside/d");
    CHECK(plant_symlink(root_c, "inner\\sub\\lgood", "deeper", 1) == ST_SUCCESS,
          "plant sub\\lgood -> deeper (in-share)");

    /* SL8 (depth 2, escaping, last component): STOPPED WITH body, unparsed
     * '\target'; nothing reached outside. */
    st = smb2_create(inner_c, "sub\\escd\\target", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL8 'sub\\escd\\target' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\target") * 2,
          "SL8 UnparsedPathLength=%d (expect %d, body present)", unparsed,
          (int) strlen("\\target") * 2);

    /* SL9 (depth 2 with a '.' component): same stop, same unparsed. */
    st = smb2_create(inner_c, "sub\\.\\escd\\target", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL9 'sub\\.\\escd\\target' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\target") * 2,
          "SL9 UnparsedPathLength=%d (expect %d)", unparsed, (int) strlen("\\target") * 2);

    /* SL10 (depth 3, escaping, last component): STOPPED WITH body. */
    st = smb2_create(inner_c, "sub\\mid\\escd3\\target", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL10 'sub\\mid\\escd3\\target' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\target") * 2,
          "SL10 UnparsedPathLength=%d (expect %d, body present)", unparsed,
          (int) strlen("\\target") * 2);

    /* SL11 (depth 2, legitimate in-share, last component): STOPPED WITH body so
     * the client can resolve the link; the retry opens. */
    st = smb2_create(inner_c, "sub\\lgood\\f", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL11 'sub\\lgood\\f' -> 0x%08x", st);
    unparsed = last_symlink_unparsed(inner_c);
    CHECK(unparsed == (int) strlen("\\f") * 2,
          "SL11 UnparsedPathLength=%d (expect %d, body present)", unparsed,
          (int) strlen("\\f") * 2);
    st = smb2_create(inner_c, "sub\\deeper\\f", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_SUCCESS, "SL11 client retry 'sub\\deeper\\f' opens -> 0x%08x", st);
    if (st == ST_SUCCESS) {
        smb2_close(inner_c, o.file_id);
    }

    /* SL12/SL13: a CREATE-if THROUGH the escaping last-component link (depth 2
     * and depth 3) is stopped at the link, so nothing is created outside. */
    st = smb2_create(inner_c, "sub\\escd\\newx", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL12 create 'sub\\escd\\newx' -> 0x%08x", st);
    CHECK(!exists_at_root("outside\\d\\newx"), "SL12 created nothing in outside/d");

    st = smb2_create(inner_c, "sub\\mid\\escd3\\newy", MBT_FILE_OPEN_IF, MBT_FILE_ALL_ACCESS,
                     MBT_FILE_SHARE_RWD, NULL, &o);
    CHECK(st == ST_STOPPED_ON_SYMLINK, "SL13 create 'sub\\mid\\escd3\\newy' -> 0x%08x", st);
    CHECK(!exists_at_root("outside\\d\\newy"), "SL13 created nothing in outside/d");

    /* SL14 (M2): a non-ASCII unresolved tail after the stopped link.
     * UnparsedPathLength counts the tail in UTF-16 bytes regardless of its UTF-8
     * encoding.  'sub\escd' stops at the link; the tail '\t'+U+00E9+U+20AC is
     * four UTF-16 code units = 8 bytes, as is '\'+U+00E9+'\x' (a non-ASCII
     * component in the middle of the remainder). */
    {
        uint8_t n16[64];
        int     nlen;

        nlen        = utf16le("sub\\escd\\t", n16);
        n16[nlen++] = 0xE9; n16[nlen++] = 0x00;   /* U+00E9 e-acute */
        n16[nlen++] = 0xAC; n16[nlen++] = 0x20;   /* U+20AC euro */
        st          = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN,
                                        MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                                        MBT_FILE_NON_DIRECTORY_FILE, &o);
        CHECK(st == ST_STOPPED_ON_SYMLINK, "SL14 non-ASCII tail -> 0x%08x", st);
        unparsed = last_symlink_unparsed(inner_c);
        CHECK(unparsed == 8, "SL14 UnparsedPathLength=%d (expect 8, non-ASCII tail)", unparsed);

        nlen        = utf16le("sub\\escd\\", n16);
        n16[nlen++] = 0xE9; n16[nlen++] = 0x00;   /* U+00E9 */
        nlen       += utf16le("\\x", n16 + nlen);
        st          = smb2_create_raw16(inner_c, n16, nlen, MBT_FILE_OPEN,
                                        MBT_FILE_ALL_ACCESS, MBT_FILE_SHARE_RWD,
                                        MBT_FILE_NON_DIRECTORY_FILE, &o);
        CHECK(st == ST_STOPPED_ON_SYMLINK, "SL14b non-ASCII mid-path tail -> 0x%08x", st);
        unparsed = last_symlink_unparsed(inner_c);
        CHECK(unparsed == 8, "SL14b UnparsedPathLength=%d (expect 8, non-ASCII mid-path)", unparsed);
    }

    check_alive(inner_c, "symlink rows");
} /* run_symlink_rows */

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

    /* T1-T3: a trailing backslash (an empty final component) is NOT one of the
     * new refusals -- each keeps exactly today's status (never the component
     * checks' OBJECT_NAME_INVALID). */
    st = inner_open("sub\\", MBT_FILE_OPEN, NULL);
    CHECK(st == ST_OBJECT_NAME_NOT_FOUND, "T1 'sub\\' -> NAME_NOT_FOUND (0x%08x)", st);
    {
        struct smb2_create_out o;
        st = smb2_create_opts(inner_c, "newdir\\", MBT_FILE_CREATE, MBT_FILE_ALL_ACCESS,
                              MBT_FILE_SHARE_RWD, MBT_FILE_DIRECTORY_FILE, NULL, &o);
        CHECK(st == ST_OBJECT_PATH_NOT_FOUND, "T2 'newdir\\' -> PATH_NOT_FOUND (0x%08x)", st);
        if (st == ST_SUCCESS) {
            smb2_close(inner_c, o.file_id);
        }
    }
    st = inner_open("sub\\inner.txt\\", MBT_FILE_OPEN, NULL);
    CHECK(st == ST_NOT_A_DIRECTORY, "T3 'sub\\inner.txt\\' -> NOT_A_DIRECTORY (0x%08x)", st);

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

    /* A v1 DHnC reconnect with a bad name is refused the same way (the name
     * check runs before the reclaim dispatch regardless of the context). */
    {
        struct smb2_durable_req dnc;
        memset(&dnc, 0, sizeof(dnc));
        dnc.dhnc = 1;
        memcpy(dnc.file_id, o.file_id, 16);
        st = smb2_create_dur(b, "a/b", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                             MBT_FILE_SHARE_RWD, NULL, &dnc, &r);
        CHECK(st == ST_OBJECT_NAME_INVALID, "reconnect DHnC 'a/b' -> 0x%08x", st);
    }

    /* The legitimate reclaim by the ORIGINAL name works: a persistent handle
     * reclaims by FileId, and sending the original name (the normal client
     * form) must not be mistaken for a crafted name.  Do not close it, so it
     * parks again for the empty-name form below. */
    memset(&dur, 0, sizeof(dur));
    dur.dh2c = 1;
    memcpy(dur.file_id, o.file_id, 16);
    memset(dur.create_guid, 0x5A, 16);
    st = smb2_create_dur(b, "recon", MBT_FILE_OPEN, MBT_FILE_ALL_ACCESS,
                         MBT_FILE_SHARE_RWD, NULL, &dur, &r);
    CHECK(st == ST_SUCCESS, "reconnect original name 'recon' reclaims -> 0x%08x", st);

    /* Park the handle again (disconnect without close) and confirm the other
     * normal form, the empty-name reclaim, still works. */
    smb2_conn_disconnect(b);
    smb2_quiesce(&env);
    b = smb2_conn_reopen(&env, a);

    memset(&dur, 0, sizeof(dur));
    dur.dh2c = 1;
    memcpy(dur.file_id, o.file_id, 16);
    memset(dur.create_guid, 0x5A, 16);
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
    run_symlink_rows();
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
