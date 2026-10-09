// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Exercise the VFS core whole-path branches unprivileged.  The branches in
 * vfs_proc_{remove,open,rename,link,mknod,symlink}.c that hand a whole
 * relative path to a backend only run for a module advertising
 * CHIMERA_VFS_CAP_FS_PATH_OP -- linux/io_uring/snfs, none of which mount
 * without CAP_DAC_READ_SEARCH.  "pathshim" is memfs plus that capability: it
 * resolves the directory prefix of a multi-component name itself and dispatches
 * the final component to memfs, so a quick-tier test reaches those branches and
 * the _path twins they call.
 *
 * The shim reuses memfs's fh_magic (CHIMERA_VFS_FH_MAGIC_MEMFS); modules are
 * stored by fh_magic with last-writer-wins, so this binary must NOT also
 * register or mount memfs, or memfs-encoded handles would bypass the shim.  The
 * rebase counter asserted at the end catches a silent bypass.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memrchr */
#endif /* ifndef _GNU_SOURCE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#undef NDEBUG
#include <assert.h>

#include "evpl/evpl.h"
#include "vfs/vfs.h"
#include "vfs/vfs_procs.h"
#include "vfs/vfs_release.h"
#include "vfs/sdk/vfs_attrs.h"
#include "vfs/sdk/vfs_cred.h"
#include "vfs/sdk/vfs_error.h"
#include "vfs/sdk/vfs_fh_magic.h"
#include "vfs/sdk/vfs_module.h"
#include "vfs/sdk/vfs_request.h"
#include "vfs/sdk/vfs_utils.h"
#include "common/logging.h"
#include "common/macros.h"
#include "prometheus-c.h"

extern struct chimera_vfs_module vfs_memfs;

/* ---- pathshim ----------------------------------------------------------- */

/* One (dir fh, name) pair the shim may have to rebase. */
struct slot {
    uint8_t     *fh;        /* points INTO the request (request->fh or a copy) */
    uint32_t    *fh_len_u32;
    int         *fh_len_int;
    const void **fh_ptr;    /* rename new_fh / link dir_fh are pointers */
    uint64_t    *fh_hash;
    const char **name;
    int         *namelen_int;
    uint32_t    *namelen_u32;
    uint64_t    *name_hash;

    /* saved originals */
    uint8_t      save_fh[CHIMERA_VFS_FH_SIZE];
    int          save_fh_len;
    const void  *save_fh_ptr;
    uint64_t     save_fh_hash;
    const char  *save_name;
    int          save_namelen;
    uint64_t     save_name_hash;

    /* resolved */
    uint8_t      new_fh[CHIMERA_VFS_FH_SIZE];
    int          new_fh_len;
    int          active;
};

struct shim_ctx {
    struct chimera_vfs_request     *request;
    chimera_vfs_complete_callback_t complete;
    void                           *private_data;
    struct slot                     s[2];
    int                             nslots;
    int                             cur;
};

#define SHIM_SLOT(request) \
        ((uint8_t *) (request)->plugin_data + CHIMERA_VFS_PLUGIN_DATA_SIZE - sizeof(void *))

static int stat_rebased;

static int
slot_namelen(struct slot *s)
{
    return s->namelen_int ? *s->namelen_int : (int) *s->namelen_u32;
} /* slot_namelen */

static void
slot_set_namelen(
    struct slot *s,
    int          len)
{
    if (s->namelen_int) {
        *s->namelen_int = len;
    } else {
        *s->namelen_u32 = len;
    }
} /* slot_set_namelen */

static const void *
slot_fh(struct slot *s)
{
    return s->fh_ptr ? *s->fh_ptr : s->fh;
} /* slot_fh */

static int
slot_fh_len(struct slot *s)
{
    return s->fh_len_int ? *s->fh_len_int : (int) *s->fh_len_u32;
} /* slot_fh_len */

static void shim_next(
    struct shim_ctx *c);

static void
shim_complete(struct chimera_vfs_request *request)
{
    struct shim_ctx *c;
    int              i;

    memcpy(&c, SHIM_SLOT(request), sizeof(c));

    /* Restore what the core handed us, so it sees its own (whole-path) view. */
    for (i = 0; i < c->nslots; i++) {
        struct slot *s = &c->s[i];

        if (!s->active) {
            continue;
        }
        if (s->fh_ptr) {
            *s->fh_ptr = s->save_fh_ptr;
        } else {
            memcpy(s->fh, s->save_fh, s->save_fh_len);
        }
        if (s->fh_len_int) {
            *s->fh_len_int = s->save_fh_len;
        } else {
            *s->fh_len_u32 = s->save_fh_len;
        }
        if (s->fh_hash) {
            *s->fh_hash = s->save_fh_hash;
        }
        *s->name = s->save_name;
        slot_set_namelen(s, s->save_namelen);
        if (s->name_hash) {
            *s->name_hash = s->save_name_hash;
        }
    }

    request->complete = c->complete;
    free(c);
    request->complete(request);
} /* shim_complete */

static void
shim_lookup_cb(
    enum chimera_vfs_error    error_code,
    struct chimera_vfs_attrs *attr,
    void                     *private_data)
{
    struct shim_ctx            *c       = private_data;
    struct chimera_vfs_request *request = c->request;
    struct slot                *s       = &c->s[c->cur];
    const char                 *slash;

    if (error_code != CHIMERA_VFS_OK) {
        request->status = error_code;
        memcpy(&c, SHIM_SLOT(request), sizeof(c));
        request->complete = c->complete;
        free(c);
        request->complete(request);
        return;
    }

    memcpy(s->new_fh, attr->va_fh, attr->va_fh_len);
    s->new_fh_len = attr->va_fh_len;

    slash = memrchr(s->save_name, '/', s->save_namelen);

    if (s->fh_ptr) {
        *s->fh_ptr = s->new_fh;
    } else {
        memcpy(s->fh, s->new_fh, s->new_fh_len);
    }
    if (s->fh_len_int) {
        *s->fh_len_int = s->new_fh_len;
    } else {
        *s->fh_len_u32 = s->new_fh_len;
    }
    if (s->fh_hash) {
        *s->fh_hash = chimera_vfs_hash(s->new_fh, s->new_fh_len);
    }
    *s->name = slash + 1;
    slot_set_namelen(s, s->save_namelen - (int) (slash + 1 - s->save_name));
    if (s->name_hash) {
        *s->name_hash = chimera_vfs_hash(*s->name, slot_namelen(s));
    }
    s->active = 1;
    stat_rebased++;

    c->cur++;
    shim_next(c);
} /* shim_lookup_cb */

static void
shim_next(struct shim_ctx *c)
{
    struct chimera_vfs_request *request = c->request;

    while (c->cur < c->nslots) {
        struct slot *s     = &c->s[c->cur];
        const char  *name  = *s->name;
        int          len   = slot_namelen(s);
        const char  *slash = memrchr(name, '/', len);

        if (!slash) {
            c->cur++;
            continue;
        }

        s->save_fh_len = slot_fh_len(s);
        memcpy(s->save_fh, slot_fh(s), s->save_fh_len);
        s->save_fh_ptr    = s->fh_ptr ? *s->fh_ptr : NULL;
        s->save_fh_hash   = s->fh_hash ? *s->fh_hash : 0;
        s->save_name      = name;
        s->save_namelen   = len;
        s->save_name_hash = s->name_hash ? *s->name_hash : 0;

        /* Resolve the directory prefix component-by-component through the
         * core (which, for a non-path-only module, walks with single-name
         * lookup_at that this shim passes straight to memfs). */
        chimera_vfs_lookup(request->thread, request->cred,
                           s->save_fh, s->save_fh_len,
                           name, (int) (slash - name),
                           CHIMERA_VFS_ATTR_FH, CHIMERA_VFS_LOOKUP_FOLLOW,
                           shim_lookup_cb, c);
        return;
    }

    vfs_memfs.dispatch(request, c->private_data);
} /* shim_next */

static void
pathshim_dispatch(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct shim_ctx *c = calloc(1, sizeof(*c));
    struct slot     *s = &c->s[0];

    c->request      = request;
    c->complete     = request->complete;
    c->private_data = private_data;

    /* default slot 0 parent = request->fh */
    s->fh         = request->fh;
    s->fh_len_u32 = &request->fh_len;
    s->fh_hash    = &request->fh_hash;

    switch (request->opcode) {
        case CHIMERA_VFS_OP_LOOKUP_AT:
            s->name        = &request->lookup_at.component;
            s->namelen_u32 = &request->lookup_at.component_len;
            s->name_hash   = &request->lookup_at.component_hash;
            c->nslots      = 1;
            break;
        case CHIMERA_VFS_OP_REMOVE_AT:
            s->name        = &request->remove_at.name;
            s->namelen_int = &request->remove_at.namelen;
            s->name_hash   = &request->remove_at.name_hash;
            c->nslots      = 1;
            break;
        case CHIMERA_VFS_OP_OPEN_AT:
            s->name        = &request->open_at.name;
            s->namelen_int = &request->open_at.namelen;
            s->name_hash   = &request->open_at.name_hash;
            c->nslots      = 1;
            break;
        case CHIMERA_VFS_OP_MKDIR_AT:
            s->name        = &request->mkdir_at.name;
            s->namelen_u32 = &request->mkdir_at.name_len;
            s->name_hash   = &request->mkdir_at.name_hash;
            c->nslots      = 1;
            break;
        case CHIMERA_VFS_OP_MKNOD_AT:
            s->name        = &request->mknod_at.name;
            s->namelen_u32 = &request->mknod_at.name_len;
            s->name_hash   = &request->mknod_at.name_hash;
            c->nslots      = 1;
            break;
        case CHIMERA_VFS_OP_SYMLINK_AT:
            s->name        = &request->symlink_at.name;
            s->namelen_int = &request->symlink_at.namelen;
            s->name_hash   = &request->symlink_at.name_hash;
            c->nslots      = 1;
            break;
        case CHIMERA_VFS_OP_RENAME_AT:
            s->name        = &request->rename_at.name;
            s->namelen_int = &request->rename_at.namelen;
            s->name_hash   = &request->rename_at.name_hash;
            s              = &c->s[1];
            s->fh_ptr      = &request->rename_at.new_fh;
            s->fh_len_int  = &request->rename_at.new_fhlen;
            s->fh_hash     = &request->rename_at.new_fh_hash;
            s->name        = &request->rename_at.new_name;
            s->namelen_int = &request->rename_at.new_namelen;
            s->name_hash   = &request->rename_at.new_name_hash;
            c->nslots      = 2;
            break;
        case CHIMERA_VFS_OP_LINK_AT:
            /* request->fh is the SOURCE file; the dir is dir_fh. */
            memset(s, 0, sizeof(*s));
            s->fh_ptr      = &request->link_at.dir_fh;
            s->fh_len_int  = &request->link_at.dir_fhlen;
            s->fh_hash     = &request->link_at.dir_fh_hash;
            s->name        = &request->link_at.name;
            s->namelen_int = &request->link_at.namelen;
            s->name_hash   = &request->link_at.name_hash;
            c->nslots      = 1;
            break;
        default:
            free(c);
            vfs_memfs.dispatch(request, private_data);
            return;
    } /* switch */

    memcpy(SHIM_SLOT(request), &c, sizeof(c));
    request->complete = shim_complete;
    shim_next(c);
} /* pathshim_dispatch */

SYMBOL_EXPORT struct chimera_vfs_module vfs_pathshim = {
    .sdk_version = CHIMERA_VFS_SDK_VERSION,
    .name        = "pathshim",
    .fh_magic    = CHIMERA_VFS_FH_MAGIC_MEMFS,
    .dispatch    = pathshim_dispatch,
};

/* ---- harness ------------------------------------------------------------ */

struct test_ctx {
    int                             done;
    enum chimera_vfs_error          status;
    struct chimera_vfs_thread      *vfs_thread;
    struct evpl                    *evpl;
    uint8_t                         fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                        fh_len;
    struct chimera_vfs_open_handle *handle;
};

static struct test_ctx G;

static void
wait_done(void)
{
    while (!G.done) {
        evpl_continue(G.evpl);
    }
    G.done = 0;
} /* wait_done */

static void
mount_cb(
    struct chimera_vfs_thread *t,
    enum chimera_vfs_error     st,
    void                      *p)
{
    G.status = st; G.done = 1;
} /* mount_cb */
static void
attr_cb(
    enum chimera_vfs_error    st,
    struct chimera_vfs_attrs *a,
    void                     *p)
{
    G.status = st;
    if (st == CHIMERA_VFS_OK && a && (a->va_set_mask & CHIMERA_VFS_ATTR_FH)) {
        memcpy(G.fh, a->va_fh, a->va_fh_len); G.fh_len = a->va_fh_len;
    }
    G.done = 1;
} /* attr_cb */
static void
st_cb(
    enum chimera_vfs_error st,
    void                  *p)
{
    G.status = st; G.done = 1;
} /* st_cb */
static void
open_cb(
    enum chimera_vfs_error          st,
    struct chimera_vfs_open_handle *oh,
    struct chimera_vfs_attrs       *a,
    void                           *p)
{
    G.status = st; G.handle = oh; G.done = 1;
} /* open_cb */

#define CHECK(what, expect) do { \
            fprintf(stderr, "  %-44s -> %d (expect %d) %s\n", what, G.status, expect, \
                    G.status == (expect) ? "ok" : "MISMATCH"); \
            if (G.status != (expect)) failures++; \
} while (0)

int
main(void)
{
    struct chimera_vfs_module_cfg cfgs[2];
    struct prometheus_metrics    *metrics;
    struct chimera_vfs           *vfs;
    struct chimera_vfs_cred       root;
    struct chimera_vfs_attrs      sattr;
    uint8_t                       vroot[CHIMERA_VFS_FH_SIZE], mfh[CHIMERA_VFS_FH_SIZE];
    uint32_t                      vroot_len, mfh_len;
    int                           failures = 0;

    chimera_log_init();
    chimera_vfs_cred_init_unix(&root, 0, 0, 0, NULL);

    vfs_pathshim.capabilities   = vfs_memfs.capabilities | CHIMERA_VFS_CAP_FS_PATH_OP;
    vfs_pathshim.init           = vfs_memfs.init;
    vfs_pathshim.destroy        = vfs_memfs.destroy;
    vfs_pathshim.thread_init    = vfs_memfs.thread_init;
    vfs_pathshim.thread_destroy = vfs_memfs.thread_destroy;

    metrics = prometheus_metrics_create(NULL, NULL, 0);
    memset(cfgs, 0, sizeof(cfgs));
    strcpy(cfgs[0].module_name, "pathshim");
    strcpy(cfgs[1].module_name, "memkv");
    G.evpl = evpl_create(NULL);
    /* name + attr caches ON: the whole-path lookup_at keys the name cache on
     * (parent, "a/b/f"), which is exactly what such a test should watch. */
    vfs          = chimera_vfs_init(0, 0, cfgs, 2, "memkv", 60, 1, 1, 0, metrics);
    G.vfs_thread = chimera_vfs_thread_init(G.evpl, vfs);

    chimera_vfs_mkfs(G.vfs_thread, NULL, "pathshim", "fs0", NULL, mount_cb, NULL);
    wait_done(); assert(G.status == 0);
    chimera_vfs_mount(G.vfs_thread, NULL, "/test", "pathshim", "fs0", NULL, mount_cb, NULL);
    wait_done(); assert(G.status == 0);

    chimera_vfs_get_root_fh(G.vfs_thread->vfs, vroot, &vroot_len);
    chimera_vfs_lookup(G.vfs_thread, &root, vroot, vroot_len, "test", 4,
                       CHIMERA_VFS_ATTR_FH, 0, attr_cb, NULL);
    wait_done(); assert(G.status == 0);
    memcpy(mfh, G.fh, G.fh_len); mfh_len = G.fh_len;

    memset(&sattr, 0, sizeof(sattr));
    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE;
    sattr.va_mode     = 0755;

    fprintf(stderr, "Path ops issued against the PATH_OP mount's root fh:\n");
    chimera_vfs_mkdir(G.vfs_thread, &root, mfh, mfh_len, "a", 1, &sattr, CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done(); CHECK("mkdir a", 0);
    chimera_vfs_mkdir(G.vfs_thread, &root, mfh, mfh_len, "a/b", 3, &sattr, CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done(); CHECK("mkdir a/b", 0);

    sattr.va_mode = 0644;
    chimera_vfs_open(G.vfs_thread, &root, mfh, mfh_len, "a/b/f", 5,
                     CHIMERA_VFS_OPEN_CREATE | CHIMERA_VFS_OPEN_CREATE_REGULAR,
                     &sattr, CHIMERA_VFS_ATTR_FH, open_cb, NULL);
    wait_done(); CHECK("open(CREATE) a/b/f", 0);
    if (G.status == 0) {
        chimera_vfs_release(G.vfs_thread, G.handle);
    }
    chimera_vfs_open(G.vfs_thread, &root, mfh, mfh_len, "a/b/f", 5, 0, NULL,
                     CHIMERA_VFS_ATTR_FH, open_cb, NULL);
    wait_done(); CHECK("open a/b/f", 0);
    if (G.status == 0) {
        chimera_vfs_release(G.vfs_thread, G.handle);
    }

    chimera_vfs_symlink(G.vfs_thread, &root, mfh, mfh_len, "a/b/s", 5, "f", 1, &sattr,
                        CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done(); CHECK("symlink a/b/s -> f", 0);

    sattr.va_set_mask = CHIMERA_VFS_ATTR_MODE;
    sattr.va_mode     = S_IFIFO | 0644;
    chimera_vfs_mknod(G.vfs_thread, &root, mfh, mfh_len, "a/b/p", 5, &sattr,
                      CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done(); CHECK("mknod a/b/p (fifo)", 0);

    chimera_vfs_link(G.vfs_thread, &root, mfh, mfh_len, "a/b/f", 5, 0, "a/b/h", 5, 0,
                     CHIMERA_VFS_ATTR_FH, attr_cb, NULL);
    wait_done(); CHECK("link a/b/f a/b/h", 0);

    chimera_vfs_rename(G.vfs_thread, &root, mfh, mfh_len, "a/b/h", 5, "a/h2", 4, st_cb, NULL);
    wait_done(); CHECK("rename a/b/h a/h2", 0);

    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/h2", 4, 0, st_cb, NULL);
    wait_done(); CHECK("remove a/h2", 0);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/b/f", 5, CHIMERA_VFS_REMOVE_ISNOTDIR, st_cb, NULL);
    wait_done(); CHECK("unlink a/b/f", 0);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/b/f", 5, 0, st_cb, NULL);
    wait_done(); CHECK("unlink a/b/f again (ENOENT)", CHIMERA_VFS_ENOENT);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/b", 3, CHIMERA_VFS_REMOVE_ISDIR, st_cb, NULL);
    wait_done(); CHECK("rmdir a/b (non-empty: ENOTEMPTY)", CHIMERA_VFS_ENOTEMPTY);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/b/s", 5, 0, st_cb, NULL);
    wait_done(); CHECK("unlink a/b/s", 0);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/b/p", 5, 0, st_cb, NULL);
    wait_done(); CHECK("unlink a/b/p", 0);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "a/b", 3, CHIMERA_VFS_REMOVE_ISDIR, st_cb, NULL);
    wait_done(); CHECK("rmdir a/b", 0);
    chimera_vfs_remove(G.vfs_thread, &root, mfh, mfh_len, "x/y", 3, 0, st_cb, NULL);
    wait_done(); CHECK("unlink x/y (missing parent: ENOENT)", CHIMERA_VFS_ENOENT);

    fprintf(stderr, "shim rebased %d multi-component names\n", stat_rebased);
    if (stat_rebased == 0) {
        fprintf(stderr, "FAIL: shim rebased nothing -- memfs handles bypassed it\n");
        failures++;
    }

    chimera_vfs_umount(G.vfs_thread, NULL, "/test", mount_cb, NULL);
    wait_done();
    chimera_vfs_thread_destroy(G.vfs_thread);
    chimera_vfs_destroy(vfs);
    evpl_destroy(G.evpl);
    prometheus_metrics_destroy(metrics);
    fprintf(stderr, failures ? "FAILURES: %d\n" : "all ok\n", failures);
    return failures ? 1 : 0;
} /* main */
