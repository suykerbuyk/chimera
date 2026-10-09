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
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#undef NDEBUG
#include <assert.h>
#include <sys/types.h>

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

static void
link_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *a,
    struct chimera_vfs_attrs *dp,
    struct chimera_vfs_attrs *dq,
    void                     *p)
{
    G.status = st;
    G.done   = 1;
} /* link_cb */

static void
rename_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *a,
    struct chimera_vfs_attrs *b,
    struct chimera_vfs_attrs *c,
    struct chimera_vfs_attrs *d,
    void                     *p)
{
    G.status = st;
    G.done   = 1;
} /* rename_cb */

static void
remove_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *pre,
    struct chimera_vfs_attrs *post,
    void                     *p)
{
    G.status = st;
    G.done   = 1;
} /* remove_cb */

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

static enum chimera_vfs_error
op_mknod_at(
    const char *name,
    int         namelen)
{
    struct chimera_vfs_attrs sattr;

    memset(&sattr, 0, sizeof(sattr));
    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE;
    sattr.va_mode     = S_IFIFO | 0644;
    chimera_vfs_mknod_at(G.t, &root, dir, name, namelen, &sattr,
                         CHIMERA_VFS_ATTR_FH, 0, 0, mkdir_cb, NULL);
    wait_done();
    return G.status;
} /* op_mknod_at */

static uint8_t  src_fh[CHIMERA_VFS_FH_SIZE];   /* "okfile" fh, for link/rename */
static uint32_t src_fh_len;

static enum chimera_vfs_error
op_link_at(
    const char *name,
    int         namelen)
{
    chimera_vfs_link_at(G.t, &root, src_fh, src_fh_len, dir->fh, dir->fh_len,
                        name, namelen, 0, CHIMERA_VFS_ATTR_FH, 0, 0, NULL, NULL,
                        link_cb, NULL);
    wait_done();
    return G.status;
} /* op_link_at */

static enum chimera_vfs_error
op_rename_at(
    const char *to_name,
    int         to_namelen)
{
    /* Rename a dedicated throwaway source so a successful oracle rename does
     * not consume "okfile" (which link/rename rows still need). */
    chimera_vfs_rename_at(G.t, &root, dir->fh, dir->fh_len, "renme", 5,
                          dir->fh, dir->fh_len, to_name, to_namelen, NULL, 0, 0,
                          0, 0, NULL, NULL, rename_cb, NULL);
    wait_done();
    return G.status;
} /* op_rename_at */

static enum chimera_vfs_error
op_remove_at(
    const char  *name,
    int          namelen,
    unsigned int flags)
{
    chimera_vfs_remove_at(G.t, &root, dir, name, namelen, NULL, 0, flags, 0, 0,
                          NULL, remove_cb, NULL);
    wait_done();
    return G.status;
} /* op_remove_at */

static enum chimera_vfs_error
op_remove_at_match_fh(
    const char *name,
    int         namelen)
{
    /* The component check runs in the public entry ahead of the fh match, so a
     * crafted name is refused whatever child_fh is passed. */
    chimera_vfs_remove_at_match_fh(G.t, &root, dir, name, namelen, src_fh,
                                   src_fh_len, 0, 0, NULL, remove_cb, NULL);
    wait_done();
    return G.status;
} /* op_remove_at_match_fh */

/* Behaviour-unchanged oracle: for a name the backend still owns (".", "..",
 * ""), the checked _at entry must return exactly what the unchecked _path twin
 * does -- the component check must not alter today's handling. */
static void
oracle_lookup(
    const char *name,
    int         namelen)
{
    enum chimera_vfs_error a, b;

    chimera_vfs_lookup_at(G.t, &root, dir, name, namelen, CHIMERA_VFS_ATTR_FH, 0,
                          lookup_cb, NULL);
    wait_done();
    a = G.status;
    chimera_vfs_lookup_at_path(G.t, &root, dir, name, namelen,
                               CHIMERA_VFS_ATTR_FH, 0, lookup_cb, NULL);
    wait_done();
    b = G.status;
    CHECK(a == b, "lookup_at '%s' == lookup_at_path twin (0x%x vs 0x%x)",
          namelen ? name : "", a, b);
} /* oracle_lookup */

/* Per-backend wiring (module cfgs + mount module/path + any on-disk scratch),
 * mirroring vfs_zerorange_test so the engine-managed diskfs/cairn legs run
 * unprivileged from argv.  cfgs[] always ends with the memkv KV backend. */
struct backend_spec {
    int         ncfg;
    const char *mount_module;
    char        mount_path[300];
    int         needs_mkfs;
    int         is_passthrough;   /* linux/io_uring: host-dir mount, may SKIP */
};

#define NC_DEV_SIZE_BYTES (1024ULL * 1024ULL * 1024ULL)   /* 1 GiB, sparse */

static void
backend_configure(
    const char                    *backend,
    const char                    *session_dir,
    struct chimera_vfs_module_cfg *cfgs,
    struct backend_spec           *spec)
{
    char cfg[512];

    memset(spec, 0, sizeof(*spec));

    if (strcmp(backend, "memfs") == 0) {
        strncpy(cfgs[0].module_name, "memfs", sizeof(cfgs[0].module_name) - 1);
        spec->mount_module = "memfs";
        spec->needs_mkfs   = 1;
        snprintf(spec->mount_path, sizeof(spec->mount_path), "fs0");
    } else if (strcmp(backend, "linux") == 0 ||
               strcmp(backend, "io_uring") == 0) {
        strncpy(cfgs[0].module_name, backend, sizeof(cfgs[0].module_name) - 1);
        spec->mount_module   = (strcmp(backend, "linux") == 0) ? "linux" : "io_uring";
        spec->is_passthrough = 1;
        snprintf(spec->mount_path, sizeof(spec->mount_path), "%s", session_dir);
    } else if (strcmp(backend, "cairn") == 0) {
        strncpy(cfgs[0].module_name, "cairn", sizeof(cfgs[0].module_name) - 1);
        snprintf(cfg, sizeof(cfg),
                 "{\"initialize\":true,\"path\":\"%s\"}", session_dir);
        strncpy(cfgs[0].config_data, cfg, sizeof(cfgs[0].config_data) - 1);
        spec->mount_module = "cairn";
        spec->needs_mkfs   = 1;
        snprintf(spec->mount_path, sizeof(spec->mount_path), "fs0");
    } else if (strcmp(backend, "diskfs_io_uring") == 0 ||
               strcmp(backend, "diskfs_aio") == 0) {
        const char *iotype = (strcmp(backend, "diskfs_aio") == 0) ? "libaio"
                                                                  : "io_uring";
        char        dev_path[300];
        int         fd, rc;

        snprintf(dev_path, sizeof(dev_path), "%s/device-0.img", session_dir);
        fd = open(dev_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        assert(fd >= 0);
        rc = ftruncate(fd, (off_t) NC_DEV_SIZE_BYTES);
        assert(rc == 0);
        close(fd);

        snprintf(cfg, sizeof(cfg),
                 "{\"initialize\":true,\"unsafe_async\":true,"
                 "\"intent_log_size\":67108864,"
                 "\"devices\":[{\"type\":\"%s\",\"size\":1,\"path\":\"%s\"}]}",
                 iotype, dev_path);
        strncpy(cfgs[0].module_name, "diskfs", sizeof(cfgs[0].module_name) - 1);
        strncpy(cfgs[0].config_data, cfg, sizeof(cfgs[0].config_data) - 1);
        spec->mount_module = "diskfs";
        spec->needs_mkfs   = 1;
        snprintf(spec->mount_path, sizeof(spec->mount_path), "fs0");
    } else {
        fprintf(stderr, "unknown backend: %s\n", backend);
        _exit(2);
    }

    strncpy(cfgs[1].module_name, "memkv", sizeof(cfgs[1].module_name) - 1);
    spec->ncfg = 2;
} /* backend_configure */

int
main(
    int    argc,
    char **argv)
{
    struct chimera_vfs_module_cfg cfgs[2];
    struct backend_spec           spec;
    struct prometheus_metrics    *metrics;
    struct chimera_vfs           *vfs;
    const char                   *backend = argc > 1 ? argv[1] : "memfs";
    char                          tmpl[]  = "/tmp/vfs_name_component.XXXXXX";
    char                         *session_dir;
    struct chimera_vfs_attrs      sattr;
    uint8_t                       vroot[CHIMERA_VFS_FH_SIZE], mnt[CHIMERA_VFS_FH_SIZE];
    uint32_t                      vroot_len, mnt_len;
    static const char             nul3[3] = { 'a', '\0', 'b' };

    chimera_log_init();
    chimera_vfs_cred_init_unix(&root, 0, 0, 0, NULL);

    session_dir = mkdtemp(tmpl);
    if (!session_dir) {
        fprintf(stderr, "SKIP: backend %s: mkdtemp failed\n", backend);
        _exit(SKIP_RC);
    }

    metrics = prometheus_metrics_create(NULL, NULL, 0);
    memset(cfgs, 0, sizeof(cfgs));
    backend_configure(backend, session_dir, cfgs, &spec);

    G.evpl = evpl_create(NULL);
    /* Name and attr caches off: the component checks sit in the public *_at
     * entry ahead of any cache, so cache state cannot change what they test,
     * and a disabled cache keeps teardown clean (no lazily-released handles). */
    vfs = chimera_vfs_init(0, 0, cfgs, spec.ncfg, "memkv", 0, 0, 0, 0, metrics);
    G.t = chimera_vfs_thread_init(G.evpl, vfs);

    /* memfs/diskfs/cairn are named filesystems (mkfs + mount by name); the
     * linux/io_uring passthrough backends mount a host directory and resolve
     * file handles only with CAP_DAC_READ_SEARCH, so unprivileged they SKIP
     * (77) -- exactly the skip contract gate 7 runs them under privileged.  A
     * memfs setup failure, by contrast, is a real failure, not a skip. */
    if (spec.needs_mkfs) {
        chimera_vfs_mkfs(G.t, NULL, spec.mount_module, "fs0", NULL, mnt_cb, NULL);
        wait_done();
        if (G.status != CHIMERA_VFS_OK) {
            if (strcmp(backend, "memfs") == 0) {
                fprintf(stderr, "FAIL: memfs mkfs failed (0x%x)\n", G.status);
                return 1;
            }
            fprintf(stderr, "SKIP: backend %s cannot mkfs here (0x%x)\n", backend, G.status);
            _exit(SKIP_RC);
        }
    }
    chimera_vfs_mount(G.t, NULL, "/test", spec.mount_module, spec.mount_path,
                      NULL, mnt_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK) {
        if (strcmp(backend, "memfs") == 0) {
            fprintf(stderr, "FAIL: memfs mount failed (0x%x)\n", G.status);
            return 1;
        }
        fprintf(stderr, "SKIP: backend %s cannot mount here (0x%x)%s\n", backend,
                G.status, spec.is_passthrough ? " -- needs CAP_DAC_READ_SEARCH" : "");
        _exit(SKIP_RC);
    }

    chimera_vfs_get_root_fh(G.t->vfs, vroot, &vroot_len);
    chimera_vfs_lookup(G.t, &root, vroot, vroot_len, "test", 4,
                       CHIMERA_VFS_ATTR_FH, 0, attr_cb, NULL);
    wait_done();
    if (G.status != CHIMERA_VFS_OK) {
        fprintf(stderr, "SKIP: backend %s mount not reachable (0x%x)\n", backend, G.status);
        _exit(SKIP_RC);
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
        _exit(SKIP_RC);
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
            _exit(SKIP_RC);
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
        _exit(SKIP_RC);
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
    CHECK(op_lookup_at(nul3, 3) == CHIMERA_VFS_EINVAL, "lookup_at 'a\\0b'");

    /* A plain create first, so link/rename have a source. */
    CHECK(op_openat("okfile", 6, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_OK, "open_at CREATE 'okfile'");
    memcpy(src_fh, G.fh, G.fh_len);
    src_fh_len = G.fh_len;
    CHECK(op_openat("renme", 5, CHIMERA_VFS_OPEN_CREATE) == CHIMERA_VFS_OK, "open_at CREATE 'renme'");
    CHECK(op_mkdir_at("okdir", 5) == CHIMERA_VFS_OK, "mkdir_at 'okdir'");

    /* Refusals on every other name-taking op: '/', NUL, 'x/..', and an exact
     * '..' on the create-type ones (rename/remove already reject dots). */
    CHECK(op_mknod_at("a/b", 3) == CHIMERA_VFS_EINVAL, "mknod_at 'a/b'");
    CHECK(op_mknod_at(nul3, 3) == CHIMERA_VFS_EINVAL, "mknod_at 'a\\0b'");
    CHECK(op_mknod_at("..", 2) == CHIMERA_VFS_EINVAL, "mknod_at '..'");
    CHECK(op_mknod_at("x/..", 4) == CHIMERA_VFS_EINVAL, "mknod_at 'x/..'");
    CHECK(op_link_at("a/b", 3) == CHIMERA_VFS_EINVAL, "link_at 'a/b'");
    CHECK(op_link_at(nul3, 3) == CHIMERA_VFS_EINVAL, "link_at 'a\\0b'");
    CHECK(op_link_at("..", 2) == CHIMERA_VFS_EINVAL, "link_at '..'");
    CHECK(op_symlink_at("x/..", 4) == CHIMERA_VFS_EINVAL, "symlink_at 'x/..'");
    CHECK(op_mkdir_at(nul3, 3) == CHIMERA_VFS_EINVAL, "mkdir_at 'a\\0b'");
    CHECK(op_symlink_at(nul3, 3) == CHIMERA_VFS_EINVAL, "symlink_at 'a\\0b'");
    CHECK(op_rename_at("a/b", 3) == CHIMERA_VFS_EINVAL, "rename_at to 'a/b'");
    CHECK(op_rename_at(nul3, 3) == CHIMERA_VFS_EINVAL, "rename_at to 'a\\0b'");
    CHECK(op_remove_at("a/b", 3, 0) == CHIMERA_VFS_EINVAL, "remove_at 'a/b'");
    CHECK(op_remove_at(nul3, 3, 0) == CHIMERA_VFS_EINVAL, "remove_at 'a\\0b'");
    CHECK(op_remove_at_match_fh("a/b", 3) == CHIMERA_VFS_EINVAL, "remove_at_match_fh 'a/b'");
    CHECK(op_remove_at_match_fh(nul3, 3) == CHIMERA_VFS_EINVAL, "remove_at_match_fh 'a\\0b'");

    /* The rename SOURCE name is checked too, not only the target. */
    chimera_vfs_rename_at(G.t, &root, dir->fh, dir->fh_len, "a/b", 3,
                          dir->fh, dir->fh_len, "okt", 3, NULL, 0, 0, 0, 0,
                          NULL, NULL, rename_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_EINVAL, "rename_at FROM 'a/b' -> EINVAL (0x%x)", G.status);
    chimera_vfs_rename_at(G.t, &root, dir->fh, dir->fh_len, nul3, 3,
                          dir->fh, dir->fh_len, "okt", 3, NULL, 0, 0, 0, 0,
                          NULL, NULL, rename_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_EINVAL, "rename_at FROM 'a\\0b' -> EINVAL (0x%x)", G.status);

    /* Keep-today oracle: dots and the empty name are the backend's.  The
     * checked _at entry must return exactly what the unchecked _path twin does
     * (today's result, whatever it is on this backend), not be turned into
     * EINVAL by the component check. */
    oracle_lookup(".", 1);
    oracle_lookup("..", 2);
    oracle_lookup("", 0);
    {
        /* open and create of '.' / '' through the checked entry versus the
         * _path twin, each into its own fresh directory so the two calls do not
         * interfere; equal status proves the check is transparent. */
        struct chimera_vfs_attrs        md = { .va_set_mask = CHIMERA_VFS_ATTR_MODE,
                                               .va_mode     = 0755 };
        struct chimera_vfs_open_handle *dchk, *dpth;
        struct chimera_vfs_attrs        os;
        enum chimera_vfs_error          a, b;
        const char                     *dotnames[2] = { ".", "" };
        int                             dotlens[2]  = { 1, 0 };
        int                             k;

        CHECK(op_mkdir_at("ochk", 4) == CHIMERA_VFS_OK, "mkdir_at 'ochk' (oracle)");
        CHECK(op_mkdir_at("opth", 4) == CHIMERA_VFS_OK, "mkdir_at 'opth' (oracle)");
        (void) md;

        chimera_vfs_lookup_at(G.t, &root, dir, "ochk", 4, CHIMERA_VFS_ATTR_FH, 0,
                              lookup_cb, NULL);
        wait_done();
        chimera_vfs_open_fh(G.t, &root, G.fh, G.fh_len,
                            CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                            CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
        wait_done();
        CHECK(G.status == CHIMERA_VFS_OK && G.h, "open 'ochk' handle (oracle) -> 0x%x", G.status);
        dchk = G.h;
        chimera_vfs_lookup_at(G.t, &root, dir, "opth", 4, CHIMERA_VFS_ATTR_FH, 0,
                              lookup_cb, NULL);
        wait_done();
        chimera_vfs_open_fh(G.t, &root, G.fh, G.fh_len,
                            CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                            CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
        wait_done();
        CHECK(G.status == CHIMERA_VFS_OK && G.h, "open 'opth' handle (oracle) -> 0x%x", G.status);
        dpth = G.h;

        for (k = 0; k < 2; k++) {
            memset(&os, 0, sizeof(os));
            os.va_set_mask = CHIMERA_VFS_ATTR_MODE;
            os.va_mode     = 0644;
            chimera_vfs_open_at(G.t, &root, dchk, dotnames[k], dotlens[k],
                                CHIMERA_VFS_OPEN_CREATE, &os, CHIMERA_VFS_ATTR_FH,
                                0, 0, openat_cb, NULL);
            wait_done();
            a = G.status;
            if (a == CHIMERA_VFS_OK && G.h) {
                chimera_vfs_release(G.t, G.h);
            }
            memset(&os, 0, sizeof(os));
            os.va_set_mask = CHIMERA_VFS_ATTR_MODE;
            os.va_mode     = 0644;
            chimera_vfs_open_at_path(G.t, &root, dpth, dotnames[k], dotlens[k],
                                     CHIMERA_VFS_OPEN_CREATE, &os,
                                     CHIMERA_VFS_ATTR_FH, 0, 0, openat_cb, NULL);
            wait_done();
            b = G.status;
            if (b == CHIMERA_VFS_OK && G.h) {
                chimera_vfs_release(G.t, G.h);
            }
            CHECK(a == b, "open_at CREATE '%s' == open_at_path twin (0x%x vs 0x%x)",
                  dotlens[k] ? dotnames[k] : "", a, b);
        }
        chimera_vfs_release(G.t, dchk);
        chimera_vfs_release(G.t, dpth);
    }
    CHECK(op_remove_at(".", 1, 0) == CHIMERA_VFS_EINVAL, "remove_at '.' -> EINVAL (unchanged)");
    CHECK(op_remove_at("..", 2, CHIMERA_VFS_REMOVE_ISDIR) == CHIMERA_VFS_ENOTEMPTY,
          "remove_at '..' -> ENOTEMPTY (unchanged)");
    CHECK(op_rename_at("", 0) != CHIMERA_VFS_EINVAL, "rename_at to '' not newly refused");

    /* Twin rows: the unchecked _path variants accept a whole path (what the
     * core's own path procs and the dirfd layer pass). */
    chimera_vfs_mkdir_at_path(G.t, &root, dir, "d", 1,
                              &(struct chimera_vfs_attrs) { .va_set_mask = CHIMERA_VFS_ATTR_MODE, .va_mode = 0755 },
                              CHIMERA_VFS_ATTR_FH, 0, 0, mkdir_cb, NULL);
    wait_done();
    CHECK(G.status == CHIMERA_VFS_OK, "mkdir_at 'd' (for twin rows) -> 0x%x", G.status);
    {
        struct chimera_vfs_attrs ts;
        memset(&ts, 0, sizeof(ts));
        ts.va_set_mask = CHIMERA_VFS_ATTR_MODE;
        ts.va_mode     = 0644;
        chimera_vfs_open_at_path(G.t, &root, dir, "d/f", 3, CHIMERA_VFS_OPEN_CREATE,
                                 &ts, CHIMERA_VFS_ATTR_FH, 0, 0, openat_cb, NULL);
        wait_done();
        CHECK(G.status == CHIMERA_VFS_OK, "open_at_path 'd/f' (whole path) -> 0x%x", G.status);
        if (G.status == CHIMERA_VFS_OK && G.h) {
            chimera_vfs_release(G.t, G.h);
        }
        chimera_vfs_remove_at_path(G.t, &root, dir, "d/f", 3, NULL, 0, 0, 0, 0,
                                   NULL, remove_cb, NULL);
        wait_done();
        CHECK(G.status == CHIMERA_VFS_OK, "remove_at_path 'd/f' (whole path) -> 0x%x", G.status);

        /* A whole-path mkdir_at_path: the twin accepts the separator that the
         * checked mkdir_at refuses ('d' exists, so 'd/sub2' creates under it). */
        chimera_vfs_mkdir_at_path(G.t, &root, dir, "d/sub2", 6, &ts,
                                  CHIMERA_VFS_ATTR_FH, 0, 0, mkdir_cb, NULL);
        wait_done();
        CHECK(G.status == CHIMERA_VFS_OK, "mkdir_at_path 'd/sub2' (whole path) -> 0x%x", G.status);
    }

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
        CHECK(G.status == CHIMERA_VFS_OK, "okdir lookup (NO_SYMLINK setup) -> 0x%x", G.status);
        if (G.status == CHIMERA_VFS_OK) {
            uint8_t  odfh[CHIMERA_VFS_FH_SIZE];
            uint32_t odfh_len = G.fh_len;

            memcpy(odfh, G.fh, odfh_len);
            chimera_vfs_open_fh(G.t, &root, odfh, odfh_len,
                                CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                                CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
            wait_done();
            CHECK(G.status == CHIMERA_VFS_OK && G.h, "open okdir handle (NO_SYMLINK) -> 0x%x", G.status);
            odir = G.h;
            /* okdir/x is a real file; okdir/lnk -> "." so lnk/x resolves to
             * okdir/x when followed. */
            memset(&s2, 0, sizeof(s2));
            s2.va_set_mask = CHIMERA_VFS_ATTR_MODE;
            s2.va_mode     = 0644;
            chimera_vfs_open_at(G.t, &root, odir, "x", 1, CHIMERA_VFS_OPEN_CREATE,
                                &s2, CHIMERA_VFS_ATTR_FH, 0, 0, openat_cb, NULL);
            wait_done();
            if (G.status == CHIMERA_VFS_OK && G.h) {
                chimera_vfs_release(G.t, G.h);
            }
            memset(&s2, 0, sizeof(s2));
            chimera_vfs_symlink_at(G.t, &root, odir, "lnk", 3, ".", 1, &s2,
                                   CHIMERA_VFS_ATTR_FH, 0, 0, symlink_cb, NULL);
            wait_done();
            chimera_vfs_release(G.t, odir);

            if (G.status != CHIMERA_VFS_OK) {
                /* A backend with no symlink support cannot exercise these rows;
                 * that is a SKIP, not a silent pass. */
                fprintf(stderr, "SKIP: backend %s has no symlink (0x%x)\n", backend, G.status);
                _exit(SKIP_RC);
            }

            /* NO_SYMLINK: the non-final link is returned unfollowed (S_ISLNK). */
            chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/lnk/x", 11,
                               CHIMERA_VFS_ATTR_FH | CHIMERA_VFS_ATTR_MODE,
                               CHIMERA_VFS_LOOKUP_NO_SYMLINK, attr_cb, NULL);
            wait_done();
            CHECK(G.status == CHIMERA_VFS_OK && G.is_lnk,
                  "lookup 'okdir/lnk/x' NO_SYMLINK stops at the link (0x%x lnk=%d)",
                  G.status, G.is_lnk);

            /* Same link, no flag: it is followed (lnk -> ".", so the path
             * resolves to okdir/x, a regular file). */
            chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/lnk/x", 11,
                               CHIMERA_VFS_ATTR_FH | CHIMERA_VFS_ATTR_MODE,
                               CHIMERA_VFS_LOOKUP_FOLLOW, attr_cb, NULL);
            wait_done();
            CHECK(G.status == CHIMERA_VFS_OK && !G.is_lnk,
                  "lookup 'okdir/lnk/x' (FOLLOW) resolves through the link (0x%x lnk=%d)",
                  G.status, G.is_lnk);

            /* A spliced ".." target under NO_DOTDOT is refused, both when the
             * link is a NON-final component (okdir/dd -> ..; okdir/dd/x expands
             * to a ".." component) and when it is the FINAL component (okdir/fl
             * -> ..; following it under NO_DOTDOT yields a ".."). */
            {
                struct chimera_vfs_open_handle *od2;
                struct chimera_vfs_attrs        s3;
                uint8_t                         o2[CHIMERA_VFS_FH_SIZE];
                uint32_t                        o2len;

                chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir", 5,
                                   CHIMERA_VFS_ATTR_FH, 0, attr_cb, NULL);
                wait_done();
                memcpy(o2, G.fh, G.fh_len);
                o2len = G.fh_len;
                chimera_vfs_open_fh(G.t, &root, o2, o2len,
                                    CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_INFERRED |
                                    CHIMERA_VFS_OPEN_DIRECTORY, openfh_cb, NULL);
                wait_done();
                CHECK(G.status == CHIMERA_VFS_OK && G.h, "open okdir handle (splice) -> 0x%x", G.status);
                od2 = G.h;
                memset(&s3, 0, sizeof(s3));
                chimera_vfs_symlink_at(G.t, &root, od2, "dd", 2, "..", 2, &s3,
                                       CHIMERA_VFS_ATTR_FH, 0, 0, symlink_cb, NULL);
                wait_done();
                CHECK(G.status == CHIMERA_VFS_OK, "plant okdir/dd -> '..' -> 0x%x", G.status);
                memset(&s3, 0, sizeof(s3));
                chimera_vfs_symlink_at(G.t, &root, od2, "fl", 2, "..", 2, &s3,
                                       CHIMERA_VFS_ATTR_FH, 0, 0, symlink_cb, NULL);
                wait_done();
                CHECK(G.status == CHIMERA_VFS_OK, "plant okdir/fl -> '..' -> 0x%x", G.status);
                chimera_vfs_release(G.t, od2);

                chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/dd/x", 10,
                                   CHIMERA_VFS_ATTR_FH | CHIMERA_VFS_ATTR_MODE,
                                   CHIMERA_VFS_LOOKUP_FOLLOW | CHIMERA_VFS_LOOKUP_NO_DOTDOT,
                                   attr_cb, NULL);
                wait_done();
                CHECK(G.status == CHIMERA_VFS_EINVAL,
                      "lookup 'okdir/dd/x' (FOLLOW|NO_DOTDOT), spliced '..' -> EINVAL (0x%x)",
                      G.status);

                /* Final-component link that resolves to "..": following it under
                 * NO_DOTDOT must also refuse. */
                chimera_vfs_lookup(G.t, &root, mnt, mnt_len, "okdir/fl", 8,
                                   CHIMERA_VFS_ATTR_FH | CHIMERA_VFS_ATTR_MODE,
                                   CHIMERA_VFS_LOOKUP_FOLLOW | CHIMERA_VFS_LOOKUP_NO_DOTDOT,
                                   attr_cb, NULL);
                wait_done();
                CHECK(G.status == CHIMERA_VFS_EINVAL,
                      "lookup 'okdir/fl' (FOLLOW|NO_DOTDOT), final link -> '..' -> EINVAL (0x%x)",
                      G.status);
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
