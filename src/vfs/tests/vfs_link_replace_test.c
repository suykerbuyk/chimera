// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * End-to-end check that chimera_vfs_link_at() with replace=1 authorizes
 * destroying the clobbered name like a delete, matching what rename_at already
 * does (SEC-4 / D11).  Driven over memfs (engine-authoritative) with explicit
 * AUTH_UNIX creds.
 *
 * Replacing a name in a sticky (01777) directory destroys whatever the name
 * pointed at, so a non-owner who is not the directory's owner is refused with
 * EACCES -- the POSIX sticky-bit owner rule, applied to the replace path.
 * Before the fix the directory add check (WRITE|EXECUTE) alone let it through.
 * Negative controls pin that parity did not over-tighten: a replace that
 * destroys nothing, a replace of the caller's own object, replace=0 of an
 * existing name, and a same-inode self-overwrite all still behave as before.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#undef NDEBUG
#include <assert.h>

#include "evpl/evpl.h"
#include "vfs/vfs.h"
#include "vfs/vfs_procs.h"
#include "vfs/vfs_release.h"
#include "vfs/sdk/vfs_attrs.h"
#include "vfs/sdk/vfs_cred.h"
#include "vfs/sdk/vfs_error.h"
#include "common/logging.h"
#include "prometheus-c.h"

#define TEST_PASS(name) fprintf(stderr, "  PASS: %s\n", name)

struct test_ctx {
    int                             done;
    enum chimera_vfs_error          status;
    struct chimera_vfs             *vfs;
    struct chimera_vfs_thread      *vfs_thread;
    struct evpl                    *evpl;
    uint8_t                         fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                        fh_len;
    struct chimera_vfs_open_handle *handle;
};

static void
wait_done(struct test_ctx *ctx)
{
    while (!ctx->done) {
        evpl_continue(ctx->evpl);
    }
    ctx->done = 0;
} /* wait_done */

static void
mount_cb(
    struct chimera_vfs_thread *thread,
    enum chimera_vfs_error     status,
    void                      *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = status;
    ctx->done   = 1;
} /* mount_cb */

static void
lookup_cb(
    enum chimera_vfs_error    error_code,
    struct chimera_vfs_attrs *attr,
    void                     *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = error_code;
    if (error_code == CHIMERA_VFS_OK) {
        memcpy(ctx->fh, attr->va_fh, attr->va_fh_len);
        ctx->fh_len = attr->va_fh_len;
    }
    ctx->done = 1;
} /* lookup_cb */

static void
openfh_cb(
    enum chimera_vfs_error          error_code,
    struct chimera_vfs_open_handle *oh,
    void                           *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = error_code;
    ctx->handle = oh;
    ctx->done   = 1;
} /* openfh_cb */

static void
openat_cb(
    enum chimera_vfs_error          error_code,
    struct chimera_vfs_open_handle *oh,
    struct chimera_vfs_attrs       *set_attr,
    struct chimera_vfs_attrs       *attr,
    struct chimera_vfs_attrs       *dir_pre,
    struct chimera_vfs_attrs       *dir_post,
    void                           *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = error_code;
    ctx->handle = oh;
    if (error_code == CHIMERA_VFS_OK) {
        memcpy(ctx->fh, oh->fh, oh->fh_len);
        ctx->fh_len = oh->fh_len;
    }
    ctx->done = 1;
} /* openat_cb */

static void
setattr_cb(
    enum chimera_vfs_error    error_code,
    struct chimera_vfs_attrs *pre_attr,
    struct chimera_vfs_attrs *set_attr,
    struct chimera_vfs_attrs *post_attr,
    void                     *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = error_code;
    ctx->done   = 1;
} /* setattr_cb */

static void
mkdir_cb(
    enum chimera_vfs_error    error_code,
    struct chimera_vfs_attrs *set_attr,
    struct chimera_vfs_attrs *attr,
    struct chimera_vfs_attrs *dir_pre,
    struct chimera_vfs_attrs *dir_post,
    void                     *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = error_code;
    if (error_code == CHIMERA_VFS_OK && attr &&
        (attr->va_set_mask & CHIMERA_VFS_ATTR_FH)) {
        memcpy(ctx->fh, attr->va_fh, attr->va_fh_len);
        ctx->fh_len = attr->va_fh_len;
    }
    ctx->done = 1;
} /* mkdir_cb */

static void
link_cb(
    enum chimera_vfs_error    error_code,
    struct chimera_vfs_attrs *r_attr,
    struct chimera_vfs_attrs *r_dir_pre_attr,
    struct chimera_vfs_attrs *r_dir_post_attr,
    void                     *private_data)
{
    struct test_ctx *ctx = private_data;

    ctx->status = error_code;
    ctx->done   = 1;
} /* link_cb */

/* setattr the directory/file at `fh` to `mode` as `cred` (owner implicitly
 * holds WRITE_ACL); return the status. */
static enum chimera_vfs_error
setmode_as(
    struct test_ctx               *ctx,
    const struct chimera_vfs_cred *cred,
    const uint8_t                 *fh,
    uint32_t                       fh_len,
    uint32_t                       mode)
{
    struct chimera_vfs_attrs sattr;
    enum chimera_vfs_error   st;

    chimera_vfs_open_fh(ctx->vfs_thread, cred, fh, fh_len,
                        CHIMERA_VFS_OPEN_INFERRED, openfh_cb, ctx);
    wait_done(ctx);
    assert(ctx->status == CHIMERA_VFS_OK);

    memset(&sattr, 0, sizeof(sattr));
    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE;
    sattr.va_mode     = mode;

    chimera_vfs_setattr(ctx->vfs_thread, cred, ctx->handle, &sattr, 0, 0,
                        setattr_cb, ctx);
    wait_done(ctx);
    st = ctx->status;

    chimera_vfs_release(ctx->vfs_thread, ctx->handle);
    return st;
} /* setmode_as */

/* Create regular file `name` under directory handle `dir` as `cred`, owned
 * uid:gid with `mode`; copy its FH into out_fh/out_len.  Returns the status. */
static enum chimera_vfs_error
create_owned(
    struct test_ctx                *ctx,
    const struct chimera_vfs_cred  *cred,
    struct chimera_vfs_open_handle *dir,
    const char                     *name,
    uint64_t                        uid,
    uint64_t                        gid,
    uint32_t                        mode,
    uint8_t                        *out_fh,
    uint32_t                       *out_len)
{
    struct chimera_vfs_attrs sattr;

    memset(&sattr, 0, sizeof(sattr));
    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE | CHIMERA_VFS_ATTR_UID |
        CHIMERA_VFS_ATTR_GID;
    sattr.va_mode = mode;
    sattr.va_uid  = uid;
    sattr.va_gid  = gid;

    chimera_vfs_open_at(ctx->vfs_thread, cred, dir, name, strlen(name),
                        CHIMERA_VFS_OPEN_CREATE, &sattr, CHIMERA_VFS_ATTR_FH,
                        0, 0, openat_cb, ctx);
    wait_done(ctx);
    if (ctx->status == CHIMERA_VFS_OK) {
        if (out_fh) {
            memcpy(out_fh, ctx->fh, ctx->fh_len);
            *out_len = ctx->fh_len;
        }
        if (ctx->handle) {
            chimera_vfs_release(ctx->vfs_thread, ctx->handle);
        }
    }
    return ctx->status;
} /* create_owned */

/* Hard-link source handle `src_fh` into directory `dir_fh` as `name`, with
 * `replace`, acting as `cred`.  By FH with no op_handle -- the NFS/S3 shape. */
static enum chimera_vfs_error
link_as(
    struct test_ctx               *ctx,
    const struct chimera_vfs_cred *cred,
    const uint8_t                 *src_fh,
    uint32_t                       src_fh_len,
    const uint8_t                 *dir_fh,
    uint32_t                       dir_fh_len,
    const char                    *name,
    unsigned int                   replace)
{
    chimera_vfs_link_at(ctx->vfs_thread, cred, src_fh, src_fh_len,
                        dir_fh, dir_fh_len, name, strlen(name), replace, 0,
                        CHIMERA_VFS_ATTR_FH, 0, 0, NULL, NULL, link_cb, ctx);
    wait_done(ctx);
    return ctx->status;
} /* link_as */

/* As link_as, but requests CHIMERA_VFS_RENAME_DURABLE -- the Option A name-
 * binding durability flag S3 passes on publish.  memfs is durable by
 * construction, so it must accept the flag and still succeed (never ENOTSUP),
 * and the flag must survive the gate path (chimera_vfs_gate_needed_dac is true
 * for an AUTH_UNIX cred) down to the backend. */
static enum chimera_vfs_error
link_durable_as(
    struct test_ctx               *ctx,
    const struct chimera_vfs_cred *cred,
    const uint8_t                 *src_fh,
    uint32_t                       src_fh_len,
    const uint8_t                 *dir_fh,
    uint32_t                       dir_fh_len,
    const char                    *name,
    unsigned int                   replace)
{
    chimera_vfs_link_at(ctx->vfs_thread, cred, src_fh, src_fh_len,
                        dir_fh, dir_fh_len, name, strlen(name), replace,
                        CHIMERA_VFS_RENAME_DURABLE,
                        CHIMERA_VFS_ATTR_FH, 0, 0, NULL, NULL, link_cb, ctx);
    wait_done(ctx);
    return ctx->status;
} /* link_durable_as */

int
main(
    int    argc,
    char **argv)
{
    struct test_ctx                 ctx = { 0 };
    struct chimera_vfs_module_cfg   module_cfgs[2];
    struct prometheus_metrics      *metrics;
    struct chimera_vfs_cred         a_cred, b_cred;
    uint8_t                         root_fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                        root_fh_len;
    uint8_t                         dir_fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                        dir_fh_len;
    uint8_t                         src_fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                        src_fh_len;
    struct chimera_vfs_open_handle *dir_handle;

    chimera_log_init();

    /* A (1000) owns the directory and the victim object; B (2000) is the
     * non-owner attacker.  Both AUTH_UNIX, both non-root. */
    chimera_vfs_cred_init_unix(&a_cred, 1000, 1000, 0, NULL);
    chimera_vfs_cred_init_unix(&b_cred, 2000, 2000, 0, NULL);

    metrics = prometheus_metrics_create(NULL, NULL, 0);
    assert(metrics != NULL);

    memset(module_cfgs, 0, sizeof(module_cfgs));
    strncpy(module_cfgs[0].module_name, "memfs",
            sizeof(module_cfgs[0].module_name) - 1);
    strncpy(module_cfgs[1].module_name, "memkv",
            sizeof(module_cfgs[1].module_name) - 1);

    ctx.evpl = evpl_create(NULL);
    assert(ctx.evpl != NULL);

    ctx.vfs = chimera_vfs_init(0, 0, module_cfgs, 2, "memkv", 60, 1, 1, 0,
                               metrics);
    assert(ctx.vfs != NULL);

    ctx.vfs_thread = chimera_vfs_thread_init(ctx.evpl, ctx.vfs);
    assert(ctx.vfs_thread != NULL);

    chimera_vfs_mkfs(ctx.vfs_thread, NULL, "memfs", "fs0", NULL, mount_cb, &ctx);
    wait_done(&ctx);
    assert(ctx.status == CHIMERA_VFS_OK);

    chimera_vfs_mount(ctx.vfs_thread, NULL, "/test", "memfs", "fs0", NULL,
                      mount_cb, &ctx);
    wait_done(&ctx);
    assert(ctx.status == CHIMERA_VFS_OK);

    chimera_vfs_get_root_fh(ctx.vfs_thread->vfs, root_fh, &root_fh_len);

    chimera_vfs_lookup(ctx.vfs_thread, &a_cred, root_fh, root_fh_len, "test", 4,
                       CHIMERA_VFS_ATTR_FH | CHIMERA_VFS_ATTR_MASK_STAT, 0,
                       lookup_cb, &ctx);
    wait_done(&ctx);
    assert(ctx.status == CHIMERA_VFS_OK);
    memcpy(root_fh, ctx.fh, ctx.fh_len);
    root_fh_len = ctx.fh_len;

    /* Create a directory owned by A, then make it sticky + world-writable
     * (S_IFDIR | 01777 -- type bits included for backend portability). */
    chimera_vfs_open_fh(ctx.vfs_thread, &a_cred, root_fh, root_fh_len,
                        CHIMERA_VFS_OPEN_INFERRED, openfh_cb, &ctx);
    wait_done(&ctx);
    assert(ctx.status == CHIMERA_VFS_OK);
    {
        struct chimera_vfs_open_handle *root_handle = ctx.handle;
        struct chimera_vfs_attrs        dattr;

        memset(&dattr, 0, sizeof(dattr));
        dattr.va_set_mask = CHIMERA_VFS_ATTR_MODE | CHIMERA_VFS_ATTR_UID |
            CHIMERA_VFS_ATTR_GID;
        dattr.va_mode = 0755;
        dattr.va_uid  = 1000;
        dattr.va_gid  = 1000;
        chimera_vfs_mkdir_at(ctx.vfs_thread, &a_cred, root_handle, "d", 1,
                             &dattr, CHIMERA_VFS_ATTR_FH, 0, 0, mkdir_cb, &ctx);
        wait_done(&ctx);
        assert(ctx.status == CHIMERA_VFS_OK);
        memcpy(dir_fh, ctx.fh, ctx.fh_len);
        dir_fh_len = ctx.fh_len;
        chimera_vfs_release(ctx.vfs_thread, root_handle);
    }

    assert(setmode_as(&ctx, &a_cred, dir_fh, dir_fh_len,
                      S_IFDIR | 01777) == CHIMERA_VFS_OK);

    /* Open the directory (as A) to seed its children. */
    chimera_vfs_open_fh(ctx.vfs_thread, &a_cred, dir_fh, dir_fh_len,
                        CHIMERA_VFS_OPEN_INFERRED, openfh_cb, &ctx);
    wait_done(&ctx);
    assert(ctx.status == CHIMERA_VFS_OK);
    dir_handle = ctx.handle;

    /* The link source (owned by A) -- link_at does not gate the source. */
    assert(create_owned(&ctx, &a_cred, dir_handle, "src", 1000, 1000, 0644,
                        src_fh, &src_fh_len) == CHIMERA_VFS_OK);
    /* The victim object A owns, that B will try to clobber. */
    assert(create_owned(&ctx, &a_cred, dir_handle, "victim", 1000, 1000, 0644,
                        NULL, NULL) == CHIMERA_VFS_OK);
    /* An object B owns in the same sticky directory. */
    assert(create_owned(&ctx, &b_cred, dir_handle, "bvictim", 2000, 2000, 0644,
                        NULL, NULL) == CHIMERA_VFS_OK);

    /* The fix: B replacing A's object in a sticky dir is a delete B may not
     * perform -> EACCES, even though B holds write+execute (add) on the dir. */
    assert(link_as(&ctx, &b_cred, src_fh, src_fh_len, dir_fh, dir_fh_len,
                   "victim", 1) == CHIMERA_VFS_EACCES);
    TEST_PASS("link replace: non-owner clobber in sticky dir denied (EACCES)");

    /* Control 1: replace=1 of a name that does not exist destroys nothing
     * (ENOENT arm) -> the add succeeds. */
    assert(link_as(&ctx, &b_cred, src_fh, src_fh_len, dir_fh, dir_fh_len,
                   "newname", 1) == CHIMERA_VFS_OK);
    TEST_PASS("link replace: replace of a missing name succeeds (no delete)");

    /* Control 2: B replacing its OWN object in the sticky dir is authorized. */
    assert(link_as(&ctx, &b_cred, src_fh, src_fh_len, dir_fh, dir_fh_len,
                   "bvictim", 1) == CHIMERA_VFS_OK);
    TEST_PASS("link replace: owner may replace its own object in sticky dir");

    /* Control 3: replace=0 over an existing name is still EEXIST (victim was
     * not clobbered by the denied attempt above). */
    assert(link_as(&ctx, &b_cred, src_fh, src_fh_len, dir_fh, dir_fh_len,
                   "victim", 0) == CHIMERA_VFS_EEXIST);
    TEST_PASS("link no-replace: existing name is EEXIST");

    /* Control 4: same-inode self-overwrite ("src" already names src_fh), by
     * the object's owner A, succeeds with no delete denial. */
    assert(link_as(&ctx, &a_cred, src_fh, src_fh_len, dir_fh, dir_fh_len,
                   "src", 1) == CHIMERA_VFS_OK);
    TEST_PASS("link replace: same-inode self-overwrite succeeds");

    /* Option A (S3 publish durability): a DURABLE link of a fresh name is
     * accepted and succeeds on a durable-by-construction backend (memfs) -- the
     * flag threads through the signature + gate path to the backend and is not
     * rejected.  (The linux parent-fsync / write-only-bucket no-regression +
     * no-attr-leak assertions need a privileged linux-backed harness; see the
     * plan's S4 -- those run under the Chair's gate, not this in-process test.) */
    assert(link_durable_as(&ctx, &a_cred, src_fh, src_fh_len, dir_fh, dir_fh_len,
                           "durname", 0) == CHIMERA_VFS_OK);
    TEST_PASS("link durable: DURABLE accepted on durable-by-construction backend");

    chimera_vfs_release(ctx.vfs_thread, dir_handle);

    chimera_vfs_umount(ctx.vfs_thread, NULL, "/test", mount_cb, &ctx);
    wait_done(&ctx);
    assert(ctx.status == CHIMERA_VFS_OK);

    for (int i = 0; i < 50; i++) {
        chimera_vfs_rmfs(ctx.vfs_thread, NULL, "memfs", "fs0", mount_cb, &ctx);
        wait_done(&ctx);
        if (ctx.status != CHIMERA_VFS_EBUSY) {
            break;
        }
        usleep(100000);
    }
    assert(ctx.status == CHIMERA_VFS_OK);

    chimera_vfs_thread_destroy(ctx.vfs_thread);
    chimera_vfs_destroy(ctx.vfs);
    evpl_destroy(ctx.evpl);
    prometheus_metrics_destroy(metrics);

    fprintf(stderr, "All VFS link-replace tests passed!\n");
    return 0;
} /* main */
