// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * The VFS core's second line of name defence: the public chimera_vfs_*_at
 * functions refuse a name that carries a path separator or an embedded NUL, and
 * the create-type ones refuse an exact "..", before any dispatch -- so a bug or
 * a bypass at a protocol facade cannot reach a backend with a name that escapes
 * the share.  A "." and a lookup of "."/".." stay legal, and POSIX mkdir/link of
 * "d/.." keep their EEXIST.  The share-confined lookup flags NO_DOTDOT and
 * NO_SYMLINK are exercised directly, with no facade in front of them.
 *
 * Backend from argv[1] (default "memfs").  memfs runs unprivileged in the quick
 * tier; the passthrough backends are registered with SKIP_RETURN_CODE 77 and
 * run only where they can mount.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* ifndef _GNU_SOURCE */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "evpl/evpl.h"
#include "vfs/vfs.h"
#include "vfs/vfs_procs.h"
#include "vfs/vfs_release.h"
#include "vfs/sdk/vfs_attrs.h"
#include "vfs/sdk/vfs_cred.h"
#include "vfs/sdk/vfs_error.h"
#include "vfs/sdk/vfs_request.h"
#include "common/logging.h"
#include "prometheus-c.h"

#define SKIP_RC 77

struct ctx {
    int                             done;
    enum chimera_vfs_error          status;
    struct chimera_vfs_thread      *t;
    struct evpl                    *evpl;
    uint8_t                         fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                        fh_len;
    int                             is_lnk;
    struct chimera_vfs_open_handle *h;
};

static struct ctx                      G;
static int                             failures;
static struct chimera_vfs_cred         root;
static struct chimera_vfs_open_handle *dir;      /* parent dir handle for *_at */

static void
wait_done(void)
{
    while (!G.done) {
        evpl_continue(G.evpl);
    }
    G.done = 0;
} /* wait_done */

static void
mnt_cb(
    struct chimera_vfs_thread *t,
    enum chimera_vfs_error     st,
    void                      *p)
{
    G.status = st;
    G.done   = 1;
} /* mnt_cb */

static void
attr_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *a,
    void                     *p)
{
    G.status = st;
    G.is_lnk = 0;
    if (st == CHIMERA_VFS_OK && a) {
        if (a->va_set_mask & CHIMERA_VFS_ATTR_FH) {
            memcpy(G.fh, a->va_fh, a->va_fh_len);
            G.fh_len = a->va_fh_len;
        }
        if ((a->va_set_mask & CHIMERA_VFS_ATTR_MODE) && S_ISLNK(a->va_mode)) {
            G.is_lnk = 1;
        }
    }
    G.done = 1;
} /* attr_cb */

static void
lookup_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *a,
    struct chimera_vfs_attrs *da,
    void                     *p)
{
    attr_cb(st, a, p);
} /* lookup_cb */

static void
openfh_cb(
    enum chimera_vfs_error          st,
    struct chimera_vfs_open_handle *oh,
    void                           *p)
{
    G.status = st;
    G.h      = oh;
    G.done   = 1;
} /* openfh_cb */

static void
openat_cb(
    enum chimera_vfs_error          st,
    struct chimera_vfs_open_handle *oh,
    struct chimera_vfs_attrs       *sa,
    struct chimera_vfs_attrs       *a,
    struct chimera_vfs_attrs       *dp,
    struct chimera_vfs_attrs       *dq,
    void                           *p)
{
    G.status = st;
    G.h      = oh;
    if (st == CHIMERA_VFS_OK && oh) {
        memcpy(G.fh, oh->fh, oh->fh_len);
        G.fh_len = oh->fh_len;
    }
    G.done = 1;
} /* openat_cb */

static void
mkdir_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *sa,
    struct chimera_vfs_attrs *a,
    struct chimera_vfs_attrs *dp,
    struct chimera_vfs_attrs *dq,
    void                     *p)
{
    attr_cb(st, a, p);
} /* mkdir_cb */

static void
symlink_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *a,
    struct chimera_vfs_attrs *dp,
    struct chimera_vfs_attrs *dq,
    void                     *p)
{
    G.status = st;
    G.done   = 1;
} /* symlink_cb */

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

/* ---- helpers that drive one op and return its status -------------------- */

static enum chimera_vfs_error
op_openat(
    const char  *name,
    int          namelen,
    unsigned int flags)
{
    struct chimera_vfs_attrs sattr;

    memset(&sattr, 0, sizeof(sattr));
    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE;
    sattr.va_mode     = 0644;
    chimera_vfs_open_at(G.t, &root, dir, name, namelen, flags, &sattr,
                        CHIMERA_VFS_ATTR_FH, 0, 0, openat_cb, NULL);
    wait_done();
    if (G.status == CHIMERA_VFS_OK && G.h) {
        chimera_vfs_release(G.t, G.h);
    }
    return G.status;
} /* op_openat */

static enum chimera_vfs_error
op_mkdir_at(
    const char *name,
    int         namelen)
{
    struct chimera_vfs_attrs sattr;

    memset(&sattr, 0, sizeof(sattr));
    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE;
    sattr.va_mode     = 0755;
    chimera_vfs_mkdir_at(G.t, &root, dir, name, namelen, &sattr,
                         CHIMERA_VFS_ATTR_FH, 0, 0, mkdir_cb, NULL);
    wait_done();
    return G.status;
} /* op_mkdir_at */

static enum chimera_vfs_error
op_symlink_at(
    const char *name,
    int         namelen)
{
    struct chimera_vfs_attrs sattr;

    memset(&sattr, 0, sizeof(sattr));
    chimera_vfs_symlink_at(G.t, &root, dir, name, namelen, "t", 1, &sattr,
                           CHIMERA_VFS_ATTR_FH, 0, 0, symlink_cb, NULL);
    wait_done();
    return G.status;
} /* op_symlink_at */

static enum chimera_vfs_error
op_lookup_at(
    const char *name,
    int         namelen)
{
    chimera_vfs_lookup_at(G.t, &root, dir, name, namelen, CHIMERA_VFS_ATTR_FH,
                          0, lookup_cb, NULL);
    wait_done();
    return G.status;
} /* op_lookup_at */

int
main(
    int    argc,
    char **argv)
{
    struct chimera_vfs_module_cfg cfgs[2];
    struct prometheus_metrics    *metrics;
    struct chimera_vfs           *vfs;
    const char                   *backend = argc > 1 ? argv[1] : "memfs";
    struct chimera_vfs_attrs      sattr;
    uint8_t                       vroot[CHIMERA_VFS_FH_SIZE], mnt[CHIMERA_VFS_FH_SIZE];
    uint32_t                      vroot_len, mnt_len;
    static const char             nul3[3] = { 'a', '\0', 'b' };

    chimera_log_init();
    chimera_vfs_cred_init_unix(&root, 0, 0, 0, NULL);

    metrics = prometheus_metrics_create(NULL, NULL, 0);
    memset(cfgs, 0, sizeof(cfgs));
    strncpy(cfgs[0].module_name, backend, sizeof(cfgs[0].module_name) - 1);
    strncpy(cfgs[1].module_name, "memkv", sizeof(cfgs[1].module_name) - 1);

    G.evpl = evpl_create(NULL);
    /* Name and attr caches off: the component checks sit in the public *_at
     * entry ahead of any cache, so cache state cannot change what they test,
     * and a disabled cache keeps teardown clean (no lazily-released handles). */
    vfs = chimera_vfs_init(0, 0, cfgs, 2, "memkv", 0, 0, 0, 0, metrics);
    G.t = chimera_vfs_thread_init(G.evpl, vfs);

    chimera_vfs_mkfs(G.t, NULL, backend, "fs0", NULL, mnt_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK) {
        fprintf(stderr, "SKIP: backend %s cannot mkfs here (0x%x)\n", backend, G.status);
        return SKIP_RC;
    }
    chimera_vfs_mount(G.t, NULL, "/test", backend, "fs0", NULL, mnt_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK) {
        fprintf(stderr, "SKIP: backend %s cannot mount here (0x%x)\n", backend, G.status);
        return SKIP_RC;
    }

    chimera_vfs_get_root_fh(vroot, &vroot_len);
    chimera_vfs_lookup(G.t, &root, vroot, vroot_len, "test", 4,
                       CHIMERA_VFS_ATTR_FH, 0, attr_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK) {
        fprintf(stderr, "SKIP: backend %s mount not reachable (0x%x)\n", backend, G.status);
        return SKIP_RC;
    }
    memcpy(mnt, G.fh, G.fh_len);
    mnt_len = G.fh_len;

    /* Work inside a subdirectory "wd" and hold only its handle, releasing the
    * mount-root handle before the ops -- the mount-root handle outstanding at
    * umount is what blocks teardown (umount unregisters the mount itself). */
    chimera_vfs_open_fh(G.t, &root, mnt, mnt_len,
                        CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                        CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK || !G.h) {
        fprintf(stderr, "SKIP: backend %s open mount handle failed (0x%x)\n", backend, G.status);
        return SKIP_RC;
    }
    {
        struct chimera_vfs_open_handle *mroot = G.h;
        struct chimera_vfs_attrs        wsa;

        memset(&wsa, 0, sizeof(wsa));
        wsa.va_set_mask = CHIMERA_VFS_ATTR_MODE;
        wsa.va_mode     = 0755;
        chimera_vfs_mkdir_at(G.t, &root, mroot, "wd", 2, &wsa,
                             CHIMERA_VFS_ATTR_FH, 0, 0, mkdir_cb, NULL);
        wait_done();
        chimera_vfs_release(G.t, mroot);
        if (G.status != CHIMERA_VFS_OK) {
            fprintf(stderr, "SKIP: backend %s mkdir wd failed (0x%x)\n", backend, G.status);
            return SKIP_RC;
        }
        memcpy(mnt, G.fh, G.fh_len);   /* reuse mnt/mnt_len as the "wd" base fh */
        mnt_len = G.fh_len;
    }

    chimera_vfs_open_fh(G.t, &root, mnt, mnt_len,
                        CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                        CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK || !G.h) {
        fprintf(stderr, "SKIP: backend %s open wd handle failed (0x%x)\n", backend, G.status);
        return SKIP_RC;
    }
    dir = G.h;

    printf("# backend %s: component checks on the public *_at entries\n", backend);

    /* Refusals: a separator, a NUL, or an exact ".." on a create-type op. */
    CHECK(op_openat("a/b", 3, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_EINVAL, "open_at CREATE 'a/b'");
    CHECK(op_openat("../x", 4, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_EINVAL, "open_at CREATE '../x'");
    CHECK(op_openat(nul3, 3, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_EINVAL, "open_at CREATE 'a\\0b'");
    CHECK(op_openat("..", 2, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_EINVAL, "open_at CREATE '..'");
    CHECK(op_openat("..", 2, 0) == CHIMERA_VFS_EINVAL, "open_at (no create) '..'");
    CHECK(op_mkdir_at("a/b", 3) == CHIMERA_VFS_EINVAL, "mkdir_at 'a/b'");
    CHECK(op_mkdir_at("..", 2) == CHIMERA_VFS_EINVAL, "mkdir_at '..'");
    CHECK(op_symlink_at("a/b", 3) == CHIMERA_VFS_EINVAL, "symlink_at 'a/b'");
    CHECK(op_symlink_at("..", 2) == CHIMERA_VFS_EINVAL, "symlink_at '..'");
    CHECK(op_lookup_at("a/b", 3) == CHIMERA_VFS_EINVAL, "lookup_at 'a/b'");

    /* Legal: a lookup of "." and "..", and a plain create. */
    CHECK(op_openat("okfile", 6, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_OK, "open_at CREATE 'okfile'");
    CHECK(op_mkdir_at("okdir", 5) == CHIMERA_VFS_OK, "mkdir_at 'okdir'");
    CHECK(op_lookup_at(".", 1) != CHIMERA_VFS_EINVAL, "lookup_at '.' is not refused");
    CHECK(op_lookup_at("..", 2) != CHIMERA_VFS_EINVAL, "lookup_at '..' is not refused");

    /* NO_DOTDOT, with no facade in front: a ".." anywhere in the whole path is
     * refused, while the same path without the flag resolves today's way. */
    chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/../okdir", 14,
                       CHIMERA_VFS_ATTR_FH, CHIMERA_VFS_LOOKUP_NO_DOTDOT,
                       attr_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_EINVAL, "lookup 'okdir/../okdir' NO_DOTDOT -> EINVAL (0x%x)", G.status);

    chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/../okdir", 14,
                       CHIMERA_VFS_ATTR_FH, 0, attr_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_OK, "lookup 'okdir/../okdir' (no flag) -> OK (0x%x)", G.status);

    /* NO_SYMLINK, non-final link: the walk stops at the link and returns it
     * (S_ISLNK), rather than following it.  Build okdir/lnk -> okdir, then walk
     * okdir/lnk/okfile.  (Only meaningful where the backend keeps a real
     * symlink; skip the assertion if the symlink create was refused.) */
    {
        struct chimera_vfs_open_handle *odir;
        struct chimera_vfs_attrs        s2;

        chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir", 5,
                           CHIMERA_VFS_ATTR_FH, 0, attr_cb, NULL);
        wait_done();
        if (G.status == CHIMERA_VFS_OK) {
            uint8_t  odfh[CHIMERA_VFS_FH_SIZE];
            uint32_t odfh_len = G.fh_len;

            memcpy(odfh, G.fh, odfh_len);
            chimera_vfs_open_fh(G.t, &root, odfh, odfh_len,
                                CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                                CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
            wait_done();
            odir = G.h;
            memset(&s2, 0, sizeof(s2));
            chimera_vfs_symlink_at(G.t, &root, odir, "lnk", 3, "okdir", 5, &s2,
                                   CHIMERA_VFS_ATTR_FH, 0, 0, symlink_cb, NULL);
            wait_done();
            chimera_vfs_release(G.t, odir);

            if (G.status == CHIMERA_VFS_OK) {
                chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/lnk/okfile", 16,
                                   CHIMERA_VFS_ATTR_FH | CHIMERA_VFS_ATTR_MODE,
                                   CHIMERA_VFS_LOOKUP_NO_SYMLINK, attr_cb, NULL);
                wait_done();
                CHECK(G.status == CHIMERA_VFS_OK && G.is_lnk,
                      "lookup 'okdir/lnk/okfile' NO_SYMLINK stops at the link (0x%x lnk=%d)",
                      G.status, G.is_lnk);
            } else {
                printf("ok   - NO_SYMLINK case skipped (backend has no symlink, 0x%x)\n", G.status);
            }
        }
    }

    /* POSIX parity: mkdir/link of a path whose final component is ".." answer
     * EEXIST (the parent exists), not EINVAL. */
    (void) sattr;
    chimera_vfs_mkdir(G.t, &root, mnt, mnt_len, "okdir/..", 8,
                      &(struct chimera_vfs_attrs) { .va_set_mask = CHIMERA_VFS_ATTR_MODE, .va_mode = 0755 },
                      CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_EEXIST, "mkdir 'okdir/..' -> EEXIST (0x%x)", G.status);

    chimera_vfs_link(G.t, &root, mnt, mnt_len, "okfile", 6, 0, "okdir/..", 8, 0,
                     CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_EEXIST, "link to 'okdir/..' -> EEXIST (0x%x)", G.status);

    chimera_vfs_release(G.t, dir);
    chimera_vfs_umount(G.t, NULL, "/test", mnt_cb, NULL);
    wait_done();
    chimera_vfs_thread_destroy(G.t);
    chimera_vfs_destroy(vfs);
    evpl_destroy(G.evpl);
    prometheus_metrics_destroy(metrics);

    fprintf(stderr, failures ? "FAILURES: %d\n" : "all ok\n", failures);
    return failures ? 1 : 0;
} /* main */
