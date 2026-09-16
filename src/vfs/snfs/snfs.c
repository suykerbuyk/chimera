/*
 * Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <uthash.h>
#include <utlist.h>
#include <jansson.h>
#include <linux/version.h>
#include "vfs/sdk/vfs_error.h"

// fchmodat support for AT_SYMLINK_NOFOLLOW was added in Linux 6.6
#if defined(LINUX_VERSION_CODE) && defined(KERNEL_VERSION)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define HAVE_FCHMODAT_AT_SYMLINK_NOFOLLOW 1
#endif /* if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) */
#endif /* if defined(LINUX_VERSION_CODE) && defined(KERNEL_VERSION) */

#include "evpl/evpl.h"

#include "vfs/sdk/chimera_vfs_sdk.h"
#include "snfs_common.h"
#include "common/logging.h"
#include "common/format.h"
#include "common/misc.h"
#include "common/macros.h"

extern struct chimera_vfs_module vfs_snfs;

/*
 * CHIMERA_VFS_CAP_CLAIM_RANGE registry.
 *
 * A byte-range claim reaches this backend only after the claim core has
 * arbitrated it against every other claim on this node; the real fcntl() adds
 * exactly one thing, visibility to holders OUTSIDE this process (a local
 * application, or another chimera node sharing the same filesystem).
 *
 * The claim wire is file-handle based and carries no open handle, so the
 * projection owns its own descriptors: one per (file handle, claim owner),
 * refcounted by the records standing on it and closed once the last one goes
 * away.  Keying the descriptor on the owner is what makes same-owner upgrades
 * coalesce the way POSIX expects while still letting two distinct owners
 * conflict with each other in the kernel.
 *
 * Open file description locks (F_OFD_*) are used where the host provides them:
 * they belong to the descriptor we hold rather than to the process, so an
 * unrelated close() of the same file elsewhere in the server cannot silently
 * drop them, and each owner's descriptor is a distinct lock owner.  A kernel
 * without them falls back to process-wide record locks, where neither property
 * holds -- the projection is then only an approximation of cross-process
 * visibility, which is all the old CHIMERA_VFS_OP_LOCK wire ever was.
 */

#ifdef F_OFD_SETLK
#define SNFS_LOCK_GET  F_OFD_GETLK
#define SNFS_LOCK_SET  F_OFD_SETLK
#define SNFS_LOCK_SETW F_OFD_SETLKW
#else /* ifdef F_OFD_SETLK */
#define SNFS_LOCK_GET  F_GETLK
#define SNFS_LOCK_SET  F_SETLK
#define SNFS_LOCK_SETW F_SETLKW
#endif /* ifdef F_OFD_SETLK */

struct snfs_range_file {
    uint8_t                    fh[CHIMERA_VFS_FH_SIZE];
    uint32_t                   fh_len;
    uint64_t                   fh_hash;
    struct chimera_claim_owner owner;
    int                        fd;
    uint32_t                   refcnt;
    struct snfs_range_file    *next;
};

/* One granted range record, named by the token the core hands back to us on
 * CHIMERA_VFS_OP_CLAIM_RELEASE.  offset/length are absolute (SEEK_SET) so the
 * unlock reproduces exactly the bytes that were locked; length 0 means to-EOF
 * in the fcntl spelling.  projected == 0 marks a record the host cannot
 * express, which the release must not try to undo. */
struct snfs_range {
    uint64_t                token;
    struct snfs_range_file *file;
    uint64_t                offset;
    uint64_t                length;
    uint8_t                 projected;
    struct snfs_range      *next;
};

struct snfs_shared {
    int                     readdir_verifier;

    pthread_mutex_t         range_lock;
    struct snfs_range_file *range_files;
    struct snfs_range      *ranges;
    uint64_t                range_next_token;
};

struct snfs_thread {
    struct evpl            *evpl;
    struct snfs_shared     *shared;
    struct snfs_mount_table mount_table;
    int                     readdir_verifier;
};

static void *
snfs_init(
    const char                *cfgdata,
    struct prometheus_metrics *metrics)
{
    (void) metrics;
    struct snfs_shared *shared;

    shared = calloc(1, sizeof(*shared));

    pthread_mutex_init(&shared->range_lock, NULL);

    if (cfgdata && cfgdata[0] != '\0') {
        json_error_t json_error;
        json_t      *cfg = json_loads(cfgdata, 0, &json_error);

        if (cfg) {
            json_t *verf = json_object_get(cfg, "readdir_verifier");

            if (json_is_boolean(verf)) {
                shared->readdir_verifier = json_boolean_value(verf);
            }

            json_decref(cfg);
        }
    }

    return shared;
} /* snfs_init */ /* snfs_init */

static void
snfs_destroy(void *private_data)
{
    struct snfs_shared     *shared = private_data;
    struct snfs_range_file *file;
    struct snfs_range      *range;

    while ((range = shared->ranges)) {
        LL_DELETE(shared->ranges, range);
        free(range);
    }

    while ((file = shared->range_files)) {
        LL_DELETE(shared->range_files, file);
        close(file->fd);
        free(file);
    }

    pthread_mutex_destroy(&shared->range_lock);

    free(shared);
} /* snfs_destroy */ /* snfs_destroy */

static void *
snfs_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    struct snfs_shared *shared = private_data;
    struct snfs_thread *thread =
        (struct snfs_thread *) calloc(1, sizeof(*thread));

    thread->evpl             = evpl;
    thread->shared           = shared;
    thread->readdir_verifier = shared->readdir_verifier;

    return thread;
} /* snfs_thread_init */ /* snfs_thread_init */

static void
snfs_thread_destroy(void *private_data)
{
    struct snfs_thread *thread = private_data;

    snfs_mount_table_destroy(&thread->mount_table);

    free(thread);
} /* snfs_thread_destroy */

/**
 * @brief Apply VFS attribute changes to a StorNext filesystem object.
 *
 * Sets one or more attributes on the file or directory identified by
 * @p dirfd and @p path according to the mask in @p attr->va_set_mask.
 * Supported attributes:
 *  - @c CHIMERA_VFS_ATTR_MODE  - file permission bits (fchmodat)
 *  - @c CHIMERA_VFS_ATTR_UID   - owner user ID (fchownat)
 *  - @c CHIMERA_VFS_ATTR_GID   - owner group ID (fchownat)
 *  - @c CHIMERA_VFS_ATTR_SIZE  - file size / truncation (truncate via /proc)
 *  - @c CHIMERA_VFS_ATTR_ATIME - access time (utimensat; CHIMERA_VFS_TIME_NOW sets to current time)
 *  - @c CHIMERA_VFS_ATTR_MTIME - modification time (utimensat; CHIMERA_VFS_TIME_NOW sets to current time)
 *  - @c CHIMERA_VFS_ATTR_BTIME - birth/creation time via StorNext CvApi_SetCreateTime
 *                                (CHIMERA_VFS_TIME_NOW sets to current time)
 *
 * On success the corresponding bits in @p attr->va_set_mask are set to
 * confirm which attributes were applied.
 *
 * @param dirfd  Open directory file descriptor used as the base for relative
 *               path operations (AT_EMPTY_PATH semantics).  May be an O_PATH
 *               descriptor; size changes are handled via /proc/self/fd, and
 *               btime changes via snfs_cvapi_set_btime() which upgrades the
 *               fd as needed.
 * @param path   Relative path beneath @p dirfd, or an empty string ("") to
 *               operate directly on @p dirfd itself.
 * @param attr   Pointer to a chimera_vfs_attrs structure.  @c va_set_mask
 *               selects which fields to apply; individual @c va_* fields
 *               carry the desired values.
 *
 * @return 0 on success, or a negative @c errno value on failure.
 */
static inline int
snfs_set_attrs(
    int                       dirfd,
    char                     *path,
    struct chimera_vfs_attrs *attr)
{
    int      rc;
    uint64_t set_mask = attr->va_set_mask;

    if (set_mask & CHIMERA_VFS_ATTR_MODE) {
#ifdef HAVE_FCHMODAT_AT_SYMLINK_NOFOLLOW
        // Use fchmodat with AT_SYMLINK_NOFOLLOW on kernels >= 6.6
        rc = fchmodat(dirfd, path, attr->va_mode, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
#else  /* ifdef HAVE_FCHMODAT_AT_SYMLINK_NOFOLLOW */
        if (strlen(path) != 0) {
            rc = fchmodat(dirfd, path, attr->va_mode, 0);
        } else {
            // dirfd may be O_PATH; chmod via /proc symlink avoids needing
            // read/write permission on the file (chmod only requires ownership)
            char procpath[64];
            snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", dirfd);
            rc = chmod(procpath, attr->va_mode);
        }
#endif /* ifdef HAVE_FCHMODAT_AT_SYMLINK_NOFOLLOW */
        if (rc) {
            snfs_error("snfs_setattr: fchmod(%o) failed: %s",
                       attr->va_mode, strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_MODE;
    }

    if ((set_mask & (CHIMERA_VFS_ATTR_UID | CHIMERA_VFS_ATTR_GID)) ==
        (CHIMERA_VFS_ATTR_UID | CHIMERA_VFS_ATTR_GID)) {

        rc = fchownat(dirfd, path, attr->va_uid, attr->va_gid,
                      AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: fchown(%u,%u) failed: %s",
                       attr->va_uid, attr->va_gid, strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_UID | CHIMERA_VFS_ATTR_GID;
    } else if (set_mask & CHIMERA_VFS_ATTR_UID) {

        rc = fchownat(dirfd, path, attr->va_uid, -1,
                      AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: fchown(%u,-1) failed: %s",
                       attr->va_uid,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_UID;
    } else if (set_mask & CHIMERA_VFS_ATTR_GID) {

        rc = fchownat(dirfd, path, -1, attr->va_gid,
                      AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: fchown(%u,-1) failed: %s",
                       attr->va_gid,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_GID;
    }

    if (set_mask & CHIMERA_VFS_ATTR_SIZE) {
        // dirfd might be O_PATH which doesn't support ftruncate directly,
        // so use truncate() on /proc/self/fd/N path which follows the symlink
        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", dirfd);
        rc = truncate(procpath, attr->va_size);

        if (rc) {
            snfs_error("snfs_setattr: truncate(%ld) failed: %s",
                       attr->va_size,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_SIZE;
    }


    if ((set_mask & CHIMERA_VFS_ATTR_BTIME) &&
        attr->va_btime.tv_nsec != CHIMERA_VFS_TIME_OMIT) {
        rc = snfs_cvapi_set_btime(dirfd, path, &attr->va_btime);

        if (rc) {
            snfs_error("snfs_setattr: set btime failed: %s",
                       strerror(-rc));

            return rc;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_BTIME;
    }


    if (set_mask & (CHIMERA_VFS_ATTR_ATIME | CHIMERA_VFS_ATTR_MTIME)) {
        struct timespec times[2];
        int             have_any = 0;

        if (set_mask & CHIMERA_VFS_ATTR_ATIME) {
            if (attr->va_atime.tv_nsec == CHIMERA_VFS_TIME_NOW) {
                times[0].tv_nsec = UTIME_NOW;
                have_any         = 1;
            } else if (attr->va_atime.tv_nsec == CHIMERA_VFS_TIME_OMIT) {
                times[0].tv_nsec = UTIME_OMIT;
            } else {
                times[0] = attr->va_atime;
                have_any = 1;
            }

            attr->va_set_mask |= CHIMERA_VFS_ATTR_ATIME;
        } else {
            times[0].tv_nsec = UTIME_OMIT;
        }

        if (set_mask & CHIMERA_VFS_ATTR_MTIME) {
            if (attr->va_mtime.tv_nsec == CHIMERA_VFS_TIME_NOW) {
                times[1].tv_nsec = UTIME_NOW;
                have_any         = 1;
            } else if (attr->va_mtime.tv_nsec == CHIMERA_VFS_TIME_OMIT) {
                times[1].tv_nsec = UTIME_OMIT;
            } else {
                times[1] = attr->va_mtime;
                have_any = 1;
            }

            attr->va_set_mask |= CHIMERA_VFS_ATTR_MTIME;
        } else {
            times[1].tv_nsec = UTIME_OMIT;
        }

        if (have_any) {
            rc = utimensat(dirfd, path, times, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

            if (rc) {
                snfs_error("snfs_setattr: utimensat() failed: %s",
                           strerror(errno));

                return -errno;
            }
        }
    }

    return 0;
} /* snfs_set_attrs */

static void
snfs_getattr(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int fd;

    fd = (int) request->getattr.handle->vfs_private;

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->getattr.r_attr,
                   fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_getattr */

static void
snfs_setattr(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int fd, rc;

    fd = request->setattr.handle->vfs_private;

    rc = chimera_setup_credential(request->cred, request->setattr.set_attr);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    rc = snfs_set_attrs(fd, "", request->setattr.set_attr);
    chimera_restore_privilege(request->cred);

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->setattr.r_post_attr,
                   fd);

    request->status = snfs_errno_to_status(-rc);
    request->complete(request);
} /* snfs_setattr */

static void
snfs_mount(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int                       mount_fd, rc;
    struct chimera_vfs_attrs *r_attr;
    char                     *scratch = (char *) request->plugin_data;

    r_attr = &request->mount.r_attr;

    TERM_STR(fullpath,
             request->mount.path,
             request->mount.pathlen,
             scratch);

    mount_fd = open(fullpath, O_DIRECTORY | O_RDONLY);

    if (mount_fd < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }
    rc = snfs_get_fh(NULL, /* mount context - compute fsid */
                     mount_fd,
                     fullpath,
                     r_attr->va_fh,
                     &r_attr->va_fh_len);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        close(mount_fd);
        request->complete(request);
        return;
    }

    r_attr->va_set_mask |= CHIMERA_VFS_ATTR_FH;

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   r_attr,
                   mount_fd);

    request->status = CHIMERA_VFS_OK;

    close(mount_fd);

    request->complete(request);
} /* snfs_lookup_path */

static void
snfs_umount(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    /* No action required */
    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_umount */

static void
snfs_lookup_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int   parent_fd, rc;
    char *scratch = (char *) request->plugin_data;

    parent_fd = (int) request->lookup_at.handle->vfs_private;

    TERM_STR(fullname, request->lookup_at.component, request->lookup_at.component_len, scratch);

    rc = snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                              request,
                              &request->lookup_at.r_attr,
                              parent_fd,
                              fullname);

    if (rc) {
        request->status = rc;
        request->complete(request);
        return;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->lookup_at.r_dir_attr,
                   parent_fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_lookup_at */

static void
snfs_readdir(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread      *thread = private_data;
    int                      fd, dup_fd, rc;
    DIR                     *dir;
    struct dirent           *dirent;
    struct chimera_vfs_attrs vattr;
    int                      eof = 1;

    fd = request->readdir.handle->vfs_private;

    snfs_debug("snfs_readdir: opening %d", fd);

    rc = chimera_setup_credential(request->cred, NULL);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    if (thread->readdir_verifier) {
        struct stat st;

        rc = fstat(fd, &st);

        if (rc == 0) {
            uint64_t mtime_verf = snfs_mtime_to_verifier(&st);

            if (request->readdir.verifier &&
                request->readdir.verifier != mtime_verf) {
                chimera_restore_privilege(request->cred);
                request->status = CHIMERA_VFS_EBADCOOKIE;
                request->complete(request);
                return;
            }

            request->readdir.r_verifier = mtime_verf;
        }
    }

    dup_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY);

    if (dup_fd < 0) {
        snfs_error("snfs_readdir: openat() failed: %s",
                   strerror(errno));
        chimera_restore_privilege(request->cred);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    dir = fdopendir(dup_fd);

    if (!dir) {
        snfs_error("snfs_readdir: fdopendir() failed: %s",
                   strerror(errno));
        close(dup_fd);
        chimera_restore_privilege(request->cred);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    if (request->readdir.cookie) {
        seekdir(dir, request->readdir.cookie);
    }

    vattr.va_req_mask = request->readdir.attr_mask;

    while ((dirent = readdir(dir))) {

        /* Skip . and .. unless explicitly requested */
        if (!(request->readdir.flags & CHIMERA_VFS_READDIR_EMIT_DOT)) {
            if ((dirent->d_name[0] == '.' && dirent->d_name[1] == '\0') ||
                (dirent->d_name[0] == '.' && dirent->d_name[1] == '.' &&
                 dirent->d_name[2] == '\0')) {
                continue;
            }
        }

        snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                             request,
                             &vattr,
                             fd,
                             dirent->d_name);

        rc = request->readdir.callback(
            dirent->d_ino,
            dirent->d_off,
            dirent->d_name,
            strlen(dirent->d_name),
            &vattr,
            request->proto_private_data);

        if (rc) {
            eof = 0;
            break;
        }

    } /* snfs_readdir */

    request->readdir.r_cookie = telldir(dir);
    request->readdir.r_eof    = eof;

    closedir(dir);
    chimera_restore_privilege(request->cred);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_readdir */ /* snfs_readdir */

static int
snfs_set_open_flags(uint32_t in_flags)
{
    int flags = 0;

    if (in_flags & CHIMERA_VFS_OPEN_PATH) {
        flags |= O_PATH;
    } else {
        if (in_flags & (CHIMERA_VFS_OPEN_DIRECTORY | CHIMERA_VFS_OPEN_READ_ONLY)) {
            flags |= O_RDONLY;
        } else {
            flags |= O_RDWR;
        }
        if (in_flags & CHIMERA_VFS_OPEN_CREATE) {
            flags |= O_CREAT;
        }
        if (in_flags & CHIMERA_VFS_OPEN_EXCLUSIVE) {
            flags |= O_EXCL;
        }
    }
    if (in_flags & CHIMERA_VFS_OPEN_DIRECTORY) {
        flags |= O_DIRECTORY;
    }
    if (in_flags & CHIMERA_VFS_OPEN_NOFOLLOW) {
        flags |= O_NOFOLLOW;
    }
    return flags;
} /* snfs_set_open_flags */

static void
snfs_open_fh(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    int                 flags;
    int                 fd;
    int                 probe_fd;
    struct stat         st;

    flags = snfs_set_open_flags(request->open_fh.flags);

    /* CHIMERA_VFS_OPEN_REGULAR_ONLY: establish the type BEFORE opening for
     * data, not after.  Opening a FIFO for data blocks until a peer appears,
     * and a device can have side effects of its own, so the open that would
     * tell us what this is must not be the open we are trying to gate.  O_PATH
     * resolves without any of that.
     *
     * Two opens on this backend, then -- the same two the caller used to make
     * for itself.  What changes is that they are one VFS operation and the
     * answer is the module's, rather than a stat the caller took separately and
     * then acted on. */
    if (request->open_fh.flags & CHIMERA_VFS_OPEN_REGULAR_ONLY) {
        probe_fd = snfs_open_by_handle(&thread->mount_table,
                                       request->fh,
                                       request->fh_len,
                                       O_PATH | O_NOFOLLOW);

        if (probe_fd < 0) {
            request->status = snfs_errno_to_status(errno);
            request->complete(request);
            return;
        }

        if (fstat(probe_fd, &st) != 0) {
            request->status = snfs_errno_to_status(errno);
            close(probe_fd);
            request->complete(request);
            return;
        }

        close(probe_fd);

        if (!S_ISREG(st.st_mode)) {
            request->status = chimera_vfs_nonreg_error(st.st_mode);
            request->complete(request);
            return;
        }
    }

    fd = snfs_open_by_handle(&thread->mount_table,
                             request->fh,
                             request->fh_len,
                             flags);

    if (fd < 0) {
        if (errno == ENOTDIR && (request->open_fh.flags & CHIMERA_VFS_OPEN_DIRECTORY)) {
            probe_fd = snfs_open_by_handle(&thread->mount_table,
                                           request->fh,
                                           request->fh_len,
                                           O_PATH | O_NOFOLLOW);

            if (probe_fd >= 0) {
                if (fstat(probe_fd, &st) == 0 && S_ISLNK(st.st_mode)) {
                    request->status = CHIMERA_VFS_ESYMLINK;
                } else {
                    request->status = CHIMERA_VFS_ENOTDIR;
                }
                close(probe_fd);
            } else {
                request->status = CHIMERA_VFS_ENOTDIR;
            }
        } else {
            request->status = snfs_errno_to_status(errno);
        }
        request->complete(request);
        return;
    }

    request->open_fh.r_vfs_private = fd;

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_open */

static void
snfs_open_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int      parent_fd, fd, flags, rc;
    uint32_t mode;
    char    *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->open_at.name, request->open_at.namelen, scratch);

    parent_fd = request->open_at.handle->vfs_private;

    if (request->open_at.set_attr->va_set_mask & CHIMERA_VFS_ATTR_MODE) {
        mode                                    = request->open_at.set_attr->va_mode;
        request->open_at.set_attr->va_set_mask &= ~CHIMERA_VFS_ATTR_MODE;
    } else {
        mode = 0600;
    }

    flags = snfs_set_open_flags(request->open_at.flags);

    rc = chimera_setup_credential(request->cred, request->open_at.set_attr);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    fd = openat(parent_fd, fullname, flags, mode);

    if (fd < 0 && errno == ELOOP &&
        (request->open_at.flags & CHIMERA_VFS_OPEN_NOFOLLOW) &&
        !(flags & (O_CREAT | O_EXCL))) {
        /* Symlink with O_NOFOLLOW: retry with O_PATH to get a handle */
        fd = openat(parent_fd, fullname, O_PATH | O_NOFOLLOW, 0);
    }

    if (fd < 0) {
        snfs_debug("snfs_open_at: openat(%d,%s,%d, 0%o) failed: %s",
                   parent_fd, fullname, flags, mode, strerror(errno));
        chimera_restore_privilege(request->cred);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    rc = snfs_set_attrs(fd, "", request->open_at.set_attr);
    chimera_restore_privilege(request->cred);

    if (rc < 0) {
        request->status = snfs_errno_to_status(-rc);
        request->complete(request);
        return;
    }

    request->open_at.r_vfs_private = fd;

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->open_at.r_attr,
                         parent_fd,
                         fullname);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_open_at */

static void
snfs_close(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int fd = request->close.vfs_private;

    close(fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_close */

static void
snfs_mkdir_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int                       fd, rc;
    char                     *scratch  = (char *) request->plugin_data;
    struct chimera_vfs_attrs *set_attr = request->mkdir_at.set_attr;
    uint32_t                  mode;

    TERM_STR(fullname, request->mkdir_at.name, request->mkdir_at.name_len, scratch);

    fd = request->mkdir_at.handle->vfs_private;

    if (set_attr->va_set_mask & CHIMERA_VFS_ATTR_MODE) {
        mode = set_attr->va_mode;
    } else {
        mode = S_IRWXU;
    }

    rc = chimera_setup_credential(request->cred, set_attr);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    rc = mkdirat(fd, fullname, mode);

    int mkdirat_errno = errno;

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->mkdir_at.r_dir_post_attr,
                   fd);

    if (rc < 0) {
        chimera_restore_privilege(request->cred);
        if (mkdirat_errno == EEXIST) {
            snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                                 request,
                                 &request->mkdir_at.r_attr,
                                 fd,
                                 fullname);
        }

        request->status = snfs_errno_to_status(mkdirat_errno);
        request->complete(request);
        return;
    }

    rc = snfs_set_attrs(fd, fullname, request->mkdir_at.set_attr);
    chimera_restore_privilege(request->cred);
    if (rc < 0) {
        request->status = snfs_errno_to_status(-rc);
        request->complete(request);
        return;
    }

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->mkdir_at.r_attr,
                         fd,
                         fullname);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_mkdir_at */

static void
snfs_mknod_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int      fd, rc;
    char    *scratch = (char *) request->plugin_data;
    uint32_t mode;
    dev_t    dev = 0;

    TERM_STR(fullname, request->mknod_at.name, request->mknod_at.name_len, scratch);

    fd = request->mknod_at.handle->vfs_private;

    if (request->mknod_at.set_attr->va_set_mask & CHIMERA_VFS_ATTR_MODE) {
        mode = request->mknod_at.set_attr->va_mode;
    } else {
        mode = S_IFREG | 0644;
    }

    if (request->mknod_at.set_attr->va_set_mask & CHIMERA_VFS_ATTR_RDEV) {
        dev = makedev(request->mknod_at.set_attr->va_rdev >> 32,
                      request->mknod_at.set_attr->va_rdev & 0xFFFFFFFF);
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->mknod_at.r_dir_pre_attr,
                   fd);

    rc = chimera_setup_credential(request->cred, request->mknod_at.set_attr);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    rc = mknodat(fd, fullname, mode, dev);

    int mknodat_errno = errno;

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->mknod_at.r_dir_post_attr,
                   fd);

    if (rc < 0) {
        chimera_restore_privilege(request->cred);

        if (mknodat_errno == EEXIST) {
            snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                                 request,
                                 &request->mknod_at.r_attr,
                                 fd,
                                 fullname);
        }

        request->status = snfs_errno_to_status(mknodat_errno);
        request->complete(request);
        return;
    }

    rc = snfs_set_attrs(fd, fullname, request->mknod_at.set_attr);
    chimera_restore_privilege(request->cred);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->mknod_at.r_attr,
                         fd,
                         fullname);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_mknod_at */

static void
snfs_remove_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int   fd, rc;
    char *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->remove_at.name, request->remove_at.namelen, scratch);

    fd = request->remove_at.handle->vfs_private;

    request->remove_at.r_removed_attr.va_req_mask = CHIMERA_VFS_ATTR_FH;

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->remove_at.r_removed_attr,
                         fd,
                         fullname);

    rc = chimera_setup_credential(request->cred, NULL);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    rc = unlinkat(fd, fullname, 0);

    if (rc == -1 && errno == EISDIR) {
        rc = unlinkat(fd, fullname, AT_REMOVEDIR);
    }

    int unlinkat_errno = errno;
    chimera_restore_privilege(request->cred);

    if (rc) {
        request->status = snfs_errno_to_status(unlinkat_errno);
    } else {
        request->status = CHIMERA_VFS_OK;
    }

    request->complete(request);
} /* snfs_remove_at */

static void
snfs_read(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    struct evpl        *evpl   = thread->evpl;
    int                 fd, i;
    ssize_t             len, left = request->read.length;
    struct iovec       *iov;

    /* Handle 0-byte reads specially - preadv with uninitialized iov causes EFAULT */
    if (request->read.length == 0) {
        fd = (int) request->read.handle->vfs_private;
        snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->read.r_attr, fd);
        request->read.r_niov   = 0;
        request->read.r_length = 0;
        request->read.r_eof    = 1;
        request->status        = CHIMERA_VFS_OK;
        request->complete(request);
        return;
    }

    request->read.r_niov = evpl_iovec_alloc(evpl,
                                            request->read.length,
                                            4096,
                                            8,
                                            EVPL_IOVEC_FLAG_SHARED, request->read.iov);

    iov = request->plugin_data;

    for (i = 0; left && i < request->read.r_niov; i++) {

        iov[i].iov_base = request->read.iov[i].data;
        iov[i].iov_len  = request->read.iov[i].length;

        if (iov[i].iov_len > left) {
            iov[i].iov_len = left;
        }

        left -= iov[i].iov_len;
    }

    fd = (int) request->read.handle->vfs_private;

    len = preadv(fd,
                 iov,
                 request->read.r_niov,
                 request->read.offset);

    if (len < 0) {
        request->status = snfs_errno_to_status(errno);

        evpl_iovecs_release(evpl, request->read.iov, request->read.r_niov);

        request->read.r_niov   = 0;
        request->read.r_length = 0;
        request->read.r_eof    = 0;
        request->complete(request);
        return;
    }

    if (len == 0) {
        evpl_iovecs_release(evpl, request->read.iov, request->read.r_niov);
        request->read.r_niov = 0;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->read.r_attr, fd);

    request->read.r_length = len;
    request->read.r_eof    = (len < request->read.length);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);

} /* snfs_read */

static void
snfs_write(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int           fd, i, niov = 0, flags = 0;
    uint32_t      left, chunk;
    ssize_t       len;
    struct iovec *iov;

    request->write.r_sync = request->write.sync;

    iov = request->plugin_data;

    left = request->write.length;
    for (i = 0; left && i < request->write.niov; i++) {
        if (request->write.iov[i].length <= left) {
            chunk = request->write.iov[i].length;
        } else {
            chunk = left;
        }
        iov[i].iov_base = request->write.iov[i].data;
        iov[i].iov_len  = chunk;
        left           -= chunk;
        niov++;
    }

    fd = (int) request->write.handle->vfs_private;

    if (request->write.sync) {
        flags = RWF_SYNC;
    }

    len = pwritev2(fd,
                   iov,
                   niov,
                   request->write.offset,
                   flags);

    /* Note: Write iovecs are NOT released here. They were allocated on the
     * server thread and must be released there. The server's write completion
     * callback handles the release after this request completes via doorbell.
     */

    if (len < 0) {
        request->status         = snfs_errno_to_status(errno);
        request->write.r_length = 0;
        request->complete(request);
        return;
    }

    request->write.r_length = len;

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->write.r_post_attr, fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);

} /* snfs_write */

static void
snfs_commit(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int fd = (int) request->commit.handle->vfs_private;

    fsync(fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);

} /* snfs_commit */

static void
snfs_allocate(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int fd   = (int) request->allocate.handle->vfs_private;
    int mode = 0;
    int rc;

    if (request->allocate.flags & CHIMERA_VFS_ALLOCATE_DEALLOCATE) {
        mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
    }

    rc = fallocate(fd, mode, request->allocate.offset, request->allocate.length);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->allocate.r_post_attr, fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);

} /* snfs_allocate */

#define SNFS_COPY_RANGE_BUFSZ (1024 * 1024)

/* Copy one buffered chunk: fill the buffer from the source range, then
 * write it out.  Reading the whole chunk before writing makes a single
 * chunk safe even when source and destination ranges overlap in the
 * same file.  Returns bytes copied, or -1 with errno set.  A return
 * shorter than @p chunk means the source hit EOF. */
static ssize_t
snfs_copy_range_chunk(
    int      src_fd,
    int      dst_fd,
    void    *buf,
    size_t   chunk,
    uint64_t src_off,
    uint64_t dst_off)
{
    ssize_t nread, nwritten;
    size_t  fill = 0, woff = 0;

    while (fill < chunk) {
        nread = pread(src_fd, (char *) buf + fill, chunk - fill, src_off + fill);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (nread == 0) {
            /* EOF on source */
            break;
        }

        fill += (size_t) nread;
    }

    while (woff < fill) {
        nwritten = pwrite(dst_fd, (char *) buf + woff, fill - woff,
                          dst_off + woff);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        woff += (size_t) nwritten;
    }

    return (ssize_t) fill;
} /* snfs_copy_range_chunk */

/* The StorNext kernel module implements neither copy_file_range(2) nor
 * the splice file ops the kernel's generic fallback path requires, so
 * the syscall fails with EINVAL on cvfs.  Emulate the copy with a
 * buffered pread/pwrite loop, short-copying at source EOF.
 *
 * Unlike copy_file_range(2), overlapping ranges within the same file
 * are copied correctly (memmove-style) rather than rejected: SMB
 * COPYCHUNK requires same-file overlapping copies to succeed
 * (smbtorture smb2.ioctl.copy_chunk_src_is_dest_overlap).  When the
 * destination range starts above the source the chunks are processed
 * back to front so no chunk reads bytes an earlier chunk wrote. */
static void
snfs_copy_range_emulate(
    struct chimera_vfs_request *request,
    int                         src_fd,
    int                         dst_fd)
{
    struct stat src_st, dst_st;
    uint64_t    src_off   = request->copy_range.src_offset;
    uint64_t    dst_off   = request->copy_range.dst_offset;
    uint64_t    remaining = request->copy_range.length;
    uint64_t    copied    = 0;
    void       *buf;
    size_t      chunk;
    ssize_t     rc;

    if (fstat(src_fd, &src_st) < 0 || fstat(dst_fd, &dst_st) < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    buf = malloc(SNFS_COPY_RANGE_BUFSZ);

    if (!buf) {
        request->status = snfs_errno_to_status(ENOMEM);
        request->complete(request);
        return;
    }

    if (src_st.st_dev == dst_st.st_dev &&
        src_st.st_ino == dst_st.st_ino &&
        src_off < dst_off + remaining &&
        dst_off < src_off + remaining &&
        dst_off > src_off) {
        /* Overlap with the destination above the source: copy chunks
         * back to front.  Clamp to the source EOF first since the EOF
         * cannot be discovered as we go in reverse order. */
        uint64_t avail = 0;

        if ((uint64_t) src_st.st_size > src_off) {
            avail = (uint64_t) src_st.st_size - src_off;
            if (avail > remaining) {
                avail = remaining;
            }
        }

        while (copied < avail) {
            uint64_t left = avail - copied;

            chunk = left < SNFS_COPY_RANGE_BUFSZ ?
                left : SNFS_COPY_RANGE_BUFSZ;

            rc = snfs_copy_range_chunk(src_fd, dst_fd, buf, chunk,
                                       src_off + left - chunk,
                                       dst_off + left - chunk);

            if (rc < 0) {
                free(buf);
                request->status = snfs_errno_to_status(errno);
                request->complete(request);
                return;
            }

            if ((size_t) rc != chunk) {
                /* Concurrent truncation below the pre-computed EOF */
                free(buf);
                request->status = CHIMERA_VFS_EIO;
                request->complete(request);
                return;
            }

            copied += chunk;
        }
    } else {
        while (remaining > 0) {
            chunk = remaining < SNFS_COPY_RANGE_BUFSZ ?
                remaining : SNFS_COPY_RANGE_BUFSZ;

            rc = snfs_copy_range_chunk(src_fd, dst_fd, buf, chunk,
                                       src_off + copied,
                                       dst_off + copied);

            if (rc < 0) {
                free(buf);
                request->status = snfs_errno_to_status(errno);
                request->complete(request);
                return;
            }

            copied    += (uint64_t) rc;
            remaining -= (uint64_t) rc;

            if ((size_t) rc < chunk) {
                /* EOF on source */
                break;
            }
        }
    }

    free(buf);

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->copy_range.r_post_attr,
                   dst_fd);

    request->copy_range.r_length = copied;
    request->status              = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_copy_range_emulate */

static void
snfs_copy_range(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int      src_fd, dst_fd;
    loff_t   src_off, dst_off;
    uint64_t remaining;
    uint64_t copied = 0;
    ssize_t  rc;

    if (request->copy_range.src_handle->vfs_module !=
        request->copy_range.dst_handle->vfs_module) {
        request->status = CHIMERA_VFS_ENOTSUP;
        request->complete(request);
        return;
    }

    src_fd    = (int) request->copy_range.src_handle->vfs_private;
    dst_fd    = (int) request->copy_range.dst_handle->vfs_private;
    src_off   = (loff_t) request->copy_range.src_offset;
    dst_off   = (loff_t) request->copy_range.dst_offset;
    remaining = request->copy_range.length;

    while (remaining > 0) {
        rc = copy_file_range(src_fd, &src_off, dst_fd, &dst_off, remaining, 0);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (copied == 0 &&
                (errno == EINVAL || errno == EXDEV ||
                 errno == ENOSYS || errno == EOPNOTSUPP)) {
                snfs_copy_range_emulate(request, src_fd, dst_fd);
                return;
            }
            request->status = snfs_errno_to_status(errno);
            request->complete(request);
            return;
        }

        if (rc == 0) {
            /* EOF on source */
            break;
        }

        copied    += (uint64_t) rc;
        remaining -= (uint64_t) rc;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->copy_range.r_post_attr,
                   dst_fd);

    request->copy_range.r_length = copied;
    request->status              = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_copy_range */

static void
snfs_clone_range(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int                     src_fd, dst_fd;
    struct file_clone_range args;
    int                     rc;

    if (request->clone_range.src_handle->vfs_module !=
        request->clone_range.dst_handle->vfs_module) {
        request->status = CHIMERA_VFS_ENOTSUP;
        request->complete(request);
        return;
    }

    src_fd = (int) request->clone_range.src_handle->vfs_private;
    dst_fd = (int) request->clone_range.dst_handle->vfs_private;

    args.src_fd      = src_fd;
    args.src_offset  = request->clone_range.src_offset;
    args.src_length  = request->clone_range.length;
    args.dest_offset = request->clone_range.dst_offset;

    rc = ioctl(dst_fd, FICLONERANGE, &args);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->clone_range.r_post_attr,
                   dst_fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_clone_range */

static void
snfs_seek(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int   fd = (int) request->seek.handle->vfs_private;
    int   whence;
    off_t result;

    if (request->seek.what == 0) {
        whence = SEEK_DATA;
    } else {
        whence = SEEK_HOLE;
    }

    result = lseek(fd, request->seek.offset, whence);

    if (result < 0) {
        if (errno == ENXIO) {
            request->seek.r_eof    = 1;
            request->seek.r_offset = 0;
            request->status        = CHIMERA_VFS_OK;
        } else {
            request->status = snfs_errno_to_status(errno);
        }
        request->complete(request);
        return;
    }

    request->seek.r_eof    = 0;
    request->seek.r_offset = result;
    request->status        = CHIMERA_VFS_OK;
    request->complete(request);

} /* snfs_seek */

static void
snfs_symlink_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int                       fd, rc;
    char                     *scratch  = (char *) request->plugin_data;
    struct chimera_vfs_attrs *set_attr = request->symlink_at.set_attr;

    if (request->symlink_at.namelen + request->symlink_at.targetlen + 2 >
        CHIMERA_VFS_PLUGIN_DATA_SIZE) {
        request->status = CHIMERA_VFS_ENAMETOOLONG;
        request->complete(request);
        return;
    }

    TERM_STR(fullname, request->symlink_at.name, request->symlink_at.namelen, scratch);
    TERM_STR(target, request->symlink_at.target, request->symlink_at.targetlen, scratch);

    fd = request->symlink_at.handle->vfs_private;

    /* symlinks do not support chmod, remove mode from attr set mask */
    set_attr->va_set_mask &= ~CHIMERA_VFS_ATTR_MODE;

    rc = chimera_setup_credential(request->cred, set_attr);
    if (rc != 0) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    rc = symlinkat(target, fd, fullname);

    if (rc < 0) {
        chimera_restore_privilege(request->cred);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }
    // Set attributes on the symlink itself if requested.
    rc = snfs_set_attrs(fd, fullname, request->symlink_at.set_attr);
    chimera_restore_privilege(request->cred);
    if (rc < 0) {
        request->status = snfs_errno_to_status(-rc);
        request->complete(request);
        return;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->symlink_at.r_dir_post_attr, fd);

    snfs_get_fh(request->fh, /* use parent's mount_id */
                fd,
                fullname,
                request->symlink_at.r_attr.va_fh,
                &request->symlink_at.r_attr.va_fh_len);

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->symlink_at.r_attr,
                         fd,
                         fullname);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_symlink_at */

static void
snfs_readlink(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int fd, rc;

    fd = request->readlink.handle->vfs_private;

    rc = readlinkat(fd, "", request->readlink.r_target,
                    request->readlink.target_maxlength);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    request->readlink.r_target_length = rc;

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->readlink.r_attr, fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_readlink */

static void
snfs_rename_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    int                 old_fd, new_fd, rc;
    char               *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->rename_at.name, request->rename_at.namelen, scratch);
    TERM_STR(full_newname, request->rename_at.new_name, request->rename_at.new_namelen, scratch);

    old_fd = snfs_open_by_handle(&thread->mount_table,
                                 request->fh,
                                 request->fh_len,
                                 O_PATH | O_RDONLY | O_NOFOLLOW);

    if (old_fd < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    new_fd = snfs_open_by_handle(&thread->mount_table,
                                 request->rename_at.new_fh,
                                 request->rename_at.new_fhlen,
                                 O_PATH | O_RDONLY | O_NOFOLLOW);

    if (new_fd < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        close(old_fd);
        return;
    }

    rc = chimera_setup_credential(request->cred, NULL);
    if (rc != 0) {
        close(old_fd);
        close(new_fd);
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    rc = renameat(old_fd, fullname, new_fd, full_newname);

    int renameat_errno = errno;
    chimera_restore_privilege(request->cred);

    if (rc < 0) {
        request->status = snfs_errno_to_status(renameat_errno);
    } else {
        request->status = CHIMERA_VFS_OK;
    }

    close(old_fd);
    close(new_fd);

    request->complete(request);
} /* snfs_rename_at */

static void
snfs_link_at(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    int                 fd, dir_fd, rc;
    char               *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->link_at.name, request->link_at.namelen, scratch);

    fd = snfs_open_by_handle(&thread->mount_table,
                             request->fh,
                             request->fh_len,
                             O_PATH | O_RDONLY | O_NOFOLLOW);

    if (fd < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    dir_fd = snfs_open_by_handle(&thread->mount_table,
                                 request->link_at.dir_fh,
                                 request->link_at.dir_fhlen,
                                 O_PATH | O_RDONLY | O_NOFOLLOW);

    if (dir_fd < 0) {
        close(fd);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    rc = linkat(fd, "", dir_fd, fullname, AT_EMPTY_PATH);

    if (rc < 0) {
        if (errno == EPERM) {
            struct stat st;

            if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
                request->status = CHIMERA_VFS_EISDIR;
            } else {
                request->status = CHIMERA_VFS_EPERM;
            }
        } else {
            request->status = snfs_errno_to_status(errno);
        }
    } else {
        request->status = CHIMERA_VFS_OK;
    }

    close(fd);
    close(dir_fd);

    request->complete(request);

} /* snfs_link_at */

/* Look up the descriptor this (file handle, owner) pair locks through, without
 * opening one.  Called with range_lock held; takes no reference. */
static struct snfs_range_file *
snfs_range_file_find(
    struct snfs_shared               *shared,
    const uint8_t                    *fh,
    uint32_t                          fh_len,
    uint64_t                          fh_hash,
    const struct chimera_claim_owner *owner)
{
    struct snfs_range_file *file;

    for (file = shared->range_files; file; file = file->next) {
        if (file->fh_hash == fh_hash && file->fh_len == fh_len &&
            memcmp(file->fh, fh, fh_len) == 0 &&
            chimera_claim_owner_equal(&file->owner, owner)) {
            return file;
        }
    }

    return NULL;
} /* snfs_range_file_find */

/* Find (or open) the descriptor this (file handle, owner) pair locks through
 * and take a reference on it.  Called with range_lock held. */
static struct snfs_range_file *
snfs_range_file_get(
    struct snfs_thread               *thread,
    const uint8_t                    *fh,
    uint32_t                          fh_len,
    uint64_t                          fh_hash,
    const struct chimera_claim_owner *owner)
{
    struct snfs_shared     *shared = thread->shared;
    struct snfs_range_file *file;
    int                     fd;

    file = snfs_range_file_find(shared, fh, fh_len, fh_hash, owner);

    if (file) {
        file->refcnt++;
        return file;
    }

    /* A read lock needs a readable descriptor and a write lock a writable one,
     * so ask for both and settle for read-only on a read-only file. */
    fd = snfs_open_by_handle(&thread->mount_table, fh, fh_len, O_RDWR);

    if (fd < 0) {
        fd = snfs_open_by_handle(&thread->mount_table, fh, fh_len, O_RDONLY);
    }

    if (fd < 0) {
        return NULL;
    }

    file = calloc(1, sizeof(*file));

    memcpy(file->fh, fh, fh_len);
    file->fh_len  = fh_len;
    file->fh_hash = fh_hash;
    file->owner   = *owner;
    file->fd      = fd;
    file->refcnt  = 1;

    LL_PREPEND(shared->range_files, file);

    return file;
} /* snfs_range_file_get */

/* Drop a reference; the last one closes the descriptor.  No record of ours can
 * still be standing on it at that point, so nothing is unlocked by surprise.
 * Called with range_lock held. */
static void
snfs_range_file_put(
    struct snfs_shared     *shared,
    struct snfs_range_file *file)
{
    if (--file->refcnt) {
        return;
    }

    LL_DELETE(shared->range_files, file);
    close(file->fd);
    free(file);
} /* snfs_range_file_put */

/* Translate a RANGE claim into the flock the host understands.  Returns 0 if
 * the range is projectable, or -1 if it is one the kernel cannot express (a
 * genuine zero-byte range, or one starting past the largest representable
 * offset) and the caller should grant it unprojected. */
static int
snfs_range_to_flock(
    const struct chimera_vfs_request *request,
    struct flock                     *fl)
{
    uint64_t offset = request->claim_acquire.offset;
    uint64_t length = request->claim_acquire.length;

    fl->l_type = request->claim_acquire.exclusive ? F_WRLCK : F_RDLCK;
    fl->l_pid  = 0;

    if (request->claim_acquire.whence == SEEK_END) {
        /* Handed to the kernel untouched so EOF is resolved atomically with
         * the lock.  offset and length are bit-casts of signed values and keep
         * the POSIX flock conventions intact -- l_len 0 is to-EOF here, and a
         * negative l_len runs backwards from l_start. */
        fl->l_whence = SEEK_END;
        fl->l_start  = (off_t) (int64_t) offset;
        fl->l_len    = (off_t) (int64_t) length;
        return 0;
    }

    fl->l_whence = SEEK_SET;
    fl->l_start  = (off_t) offset;

    if (length == UINT64_MAX) {
        fl->l_len = 0;                 /* to-EOF, which fcntl spells as 0 */
    } else if (length == 0) {
        /* A genuine zero-byte range, which fcntl cannot express at all since
         * l_len 0 already means to-EOF.  The core has arbitrated it locally;
         * granting it unprojected beats refusing an SMB zero-byte lock. */
        return -1;
    } else if (offset > (uint64_t) INT64_MAX) {
        return -1;
    } else if (length > (uint64_t) INT64_MAX - offset) {
        fl->l_len = 0;                 /* runs past the last byte an off_t has */
    } else {
        fl->l_len = (off_t) length;
    }

    return 0;
} /* snfs_range_to_flock */

/* Record the absolute bytes a granted lock covers, so the release can undo
 * exactly them.  A SEEK_END lock was resolved by the kernel against the size
 * it saw; re-resolving it at release time -- arbitrarily much later -- would
 * unlock the wrong bytes, so resolve it here instead, while the size is still
 * the one the lock was placed against. */
static void
snfs_range_resolve(
    const struct flock *fl,
    int                 fd,
    uint64_t           *r_offset,
    uint64_t           *r_length)
{
    struct stat st;
    int64_t     base;

    if (fl->l_whence != SEEK_END || fstat(fd, &st) < 0) {
        *r_offset = (uint64_t) fl->l_start;
        *r_length = (uint64_t) fl->l_len;
        return;
    }

    base = (int64_t) st.st_size + (int64_t) fl->l_start;

    if (fl->l_len < 0) {
        base += (int64_t) fl->l_len;
    }

    if (base < 0) {
        base = 0;
    }

    *r_offset = (uint64_t) base;
    *r_length = (fl->l_len < 0)
        ? 0 - (uint64_t) fl->l_len
        : (uint64_t) fl->l_len;
} /* snfs_range_resolve */

/* Describe the holder an F_GETLK found back to the caller.  F_GETLK returns
 * the conflict in SEEK_SET terms whatever whence was asked about. */
static void
snfs_range_report_conflict(
    struct chimera_vfs_request *request,
    const struct flock         *fl)
{
    if (fl->l_type == F_UNLCK) {
        return;
    }

    request->claim_acquire.r_conflict_type = (fl->l_type == F_RDLCK)
        ? CHIMERA_VFS_LOCK_READ
        : CHIMERA_VFS_LOCK_WRITE;
    request->claim_acquire.r_conflict_offset = (uint64_t) fl->l_start;
    /* fcntl reports to-EOF as 0; the claim wire spells it UINT64_MAX. */
    request->claim_acquire.r_conflict_length = (fl->l_len == 0)
        ? UINT64_MAX
        : (uint64_t) fl->l_len;
    /* An OFD holder reports l_pid -1, meaning "no process owns this". */
    request->claim_acquire.r_conflict_pid = (fl->l_pid > 0)
        ? (uint32_t) fl->l_pid
        : 0;
} /* snfs_range_report_conflict */

static void
snfs_claim_acquire(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread     *thread = private_data;
    struct snfs_shared     *shared = thread->shared;
    struct snfs_range_file *file;
    struct snfs_range      *range     = NULL;
    struct flock            fl        = { 0 };
    int                     projected = 1;
    int                     cmd, rc;

    if (request->claim_acquire.klass != CHIMERA_VFS_CLAIM_KLASS_RANGE) {
        /* This module arbitrates ranges only; it does not declare
         * CHIMERA_VFS_CAP_CLAIM_AGGREGATE. */
        request->status = CHIMERA_VFS_ENOTSUP;
        request->complete(request);
        return;
    }

    if (snfs_range_to_flock(request, &fl) < 0) {
        projected = 0;
    }

    if (request->claim_acquire.flags & CHIMERA_VFS_CLAIM_TEST) {
        cmd = SNFS_LOCK_GET;
    } else if (request->claim_acquire.flags & CHIMERA_VFS_CLAIM_WAIT) {
        cmd = SNFS_LOCK_SETW;
    } else {
        cmd = SNFS_LOCK_SET;
    }

    if (!projected) {
        /* Nothing to ask the host about.  A probe sees no conflict; an acquire
         * gets a record that releases as a no-op. */
        if (!(request->claim_acquire.flags & CHIMERA_VFS_CLAIM_TEST)) {
            range = calloc(1, sizeof(*range));

            pthread_mutex_lock(&shared->range_lock);
            range->token = ++shared->range_next_token;
            LL_PREPEND(shared->ranges, range);
            pthread_mutex_unlock(&shared->range_lock);

            request->claim_acquire.r_token   = range->token;
            request->claim_acquire.r_granted = 1;
        }

        request->status = CHIMERA_VFS_OK;
        request->complete(request);
        return;
    }

    pthread_mutex_lock(&shared->range_lock);

    file = snfs_range_file_get(thread,
                               request->fh,
                               request->fh_len,
                               request->fh_hash,
                               &request->claim_acquire.owner);

    rc = file ? 0 : errno;

    pthread_mutex_unlock(&shared->range_lock);

    if (!file) {
        request->status = snfs_errno_to_status(rc);
        request->complete(request);
        return;
    }

    /* Outside the registry lock: F_SETLKW blocks until the contending lock
     * goes away, and every other claim on this module would be stuck behind it
     * -- including the release that would let it through.  The reference taken
     * above keeps file->fd alive meanwhile. */
    rc = fcntl(file->fd, cmd, &fl);

    if (rc < 0) {
        int err = errno;

        if (err == EACCES || err == EAGAIN) {
            /* Held by somebody else: a refusal, not a failure.  Ask who, so
             * the caller can describe the denial. */
            if (snfs_range_to_flock(request, &fl) == 0 &&
                fcntl(file->fd, SNFS_LOCK_GET, &fl) == 0) {
                snfs_range_report_conflict(request, &fl);
            }
            request->status = CHIMERA_VFS_OK;
        } else {
            request->status = snfs_errno_to_status(err);
        }
    } else if (request->claim_acquire.flags & CHIMERA_VFS_CLAIM_TEST) {
        /* A probe acquires nothing: the answer is the conflict block. */
        snfs_range_report_conflict(request, &fl);
        request->status = CHIMERA_VFS_OK;
    } else {
        range            = calloc(1, sizeof(*range));
        range->file      = file;
        range->projected = 1;

        snfs_range_resolve(&fl, file->fd, &range->offset, &range->length);
    }

    pthread_mutex_lock(&shared->range_lock);

    if (range) {
        range->token = ++shared->range_next_token;
        LL_PREPEND(shared->ranges, range);

        request->claim_acquire.r_token   = range->token;
        request->claim_acquire.r_granted = 1;
        request->status                  = CHIMERA_VFS_OK;
    } else {
        /* Nothing standing on the descriptor from this request. */
        snfs_range_file_put(shared, file);
    }

    pthread_mutex_unlock(&shared->range_lock);

    request->complete(request);
} /* snfs_claim_acquire */

/* Release by GEOMETRY (claim_release.token == 0): the caller never learned the
 * absolute bytes it holds -- a SEEK_END lock is resolved down here and the
 * resolution is never reported back -- so it names the range to drop in exactly
 * the spelling it named the lock, and this side resolves EOF again.  Every
 * record of this owner's that overlaps the resolved range goes, each unlocked
 * over its own bytes so the kernel is left holding precisely what the registry
 * still describes.  Matching nothing is success. */
static void
snfs_claim_release_ranged(
    struct chimera_vfs_request *request,
    struct snfs_thread         *thread)
{
    struct snfs_shared     *shared = thread->shared;
    struct snfs_range_file *file;
    struct snfs_range      *range, *tmp, *matched = NULL;
    struct flock            fl     = { 0 };
    uint64_t                offset = request->claim_release.offset;
    uint64_t                length = request->claim_release.length;
    int                     err    = 0;

    pthread_mutex_lock(&shared->range_lock);

    file = snfs_range_file_find(shared,
                                request->fh,
                                request->fh_len,
                                request->fh_hash,
                                &request->claim_release.owner);

    if (file) {
        /* Pin it across the syscalls below, which run unlocked. */
        file->refcnt++;
    }

    pthread_mutex_unlock(&shared->range_lock);

    if (!file) {
        /* This owner locks nothing on this file, so there is nothing of ours
         * to drop and no size worth resolving against. */
        request->status = CHIMERA_VFS_OK;
        request->complete(request);
        return;
    }

    if (request->claim_release.whence == SEEK_END) {
        /* offset and length are bit-casts of the caller's signed l_start and
         * l_len and keep POSIX's conventions: l_len 0 is to-EOF and a negative
         * l_len runs backwards from l_start.  The descriptor is the one the
         * locks were taken through, so its size is the one the kernel would
         * resolve an F_UNLCK against. */
        struct stat st;
        int64_t     start = (int64_t) offset;
        int64_t     len   = (int64_t) length;

        if (fstat(file->fd, &st) < 0) {
            err = errno;
        } else {
            start += (int64_t) st.st_size;

            if (len < 0) {
                start += len;
                len    = -len;
            }

            if (start < 0) {
                err = EINVAL;
            } else {
                offset = (uint64_t) start;
                length = (len == 0) ? UINT64_MAX : (uint64_t) len;
            }
        }
    }

    if (err) {
        pthread_mutex_lock(&shared->range_lock);
        snfs_range_file_put(shared, file);
        pthread_mutex_unlock(&shared->range_lock);

        request->status = snfs_errno_to_status(err);
        request->complete(request);
        return;
    }

    pthread_mutex_lock(&shared->range_lock);

    LL_FOREACH_SAFE(shared->ranges, range, tmp)
    {
        /* One descriptor per (file handle, owner), so having been taken through
         * this one is the fh and chimera_claim_owner_equal() test already. */
        if (range->file != file) {
            continue;
        }

        /* The record keeps fcntl's spelling, where a length of 0 is to-EOF;
         * the overlap test speaks the claim wire's, where UINT64_MAX is. */
        if (!chimera_vfs_claim_range_overlap_i(range->offset,
                                               range->length ? range->length : UINT64_MAX,
                                               offset, length)) {
            continue;
        }

        LL_DELETE(shared->ranges, range);
        LL_PREPEND(matched, range);
    }

    pthread_mutex_unlock(&shared->range_lock);

    /* Outside the registry lock, as every other lock syscall on this module is.
     * F_UNLCK does not block, but the descriptor put below wants the lock and
     * there is no reason to hold it across a syscall at all. */
    LL_FOREACH(matched, range)
    {
        fl.l_type   = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = (off_t) range->offset;
        fl.l_len    = (off_t) range->length;
        fl.l_pid    = 0;

        fcntl(file->fd, SNFS_LOCK_SET, &fl);
    }

    pthread_mutex_lock(&shared->range_lock);

    while (matched) {
        range = matched;
        LL_DELETE(matched, range);
        snfs_range_file_put(shared, range->file);
        free(range);
    }

    snfs_range_file_put(shared, file);

    pthread_mutex_unlock(&shared->range_lock);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_claim_release_ranged */

static void
snfs_claim_release(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    struct snfs_shared *shared = thread->shared;
    struct snfs_range  *range;
    struct flock        fl = { 0 };

    /* claim_release.retained is an AGGREGATE downgrade mask; a RANGE record is
     * binding and all-or-nothing, so the release simply drops it. */

    if (request->claim_release.token == 0 &&
        request->claim_release.klass == CHIMERA_VFS_CLAIM_KLASS_RANGE) {
        snfs_claim_release_ranged(request, thread);
        return;
    }

    pthread_mutex_lock(&shared->range_lock);

    for (range = shared->ranges; range; range = range->next) {
        if (range->token == request->claim_release.token) {
            LL_DELETE(shared->ranges, range);
            break;
        }
    }

    pthread_mutex_unlock(&shared->range_lock);

    if (range && range->projected) {
        fl.l_type   = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = (off_t) range->offset;
        fl.l_len    = (off_t) range->length;
        fl.l_pid    = 0;

        fcntl(range->file->fd, SNFS_LOCK_SET, &fl);

        pthread_mutex_lock(&shared->range_lock);
        snfs_range_file_put(shared, range->file);
        pthread_mutex_unlock(&shared->range_lock);
    }

    free(range);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_claim_release */

static void
snfs_dispatch(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    switch (request->opcode) {
        case CHIMERA_VFS_OP_MOUNT:
            snfs_mount(request, private_data);
            break;
        case CHIMERA_VFS_OP_UMOUNT:
            snfs_umount(request, private_data);
            break;
        case CHIMERA_VFS_OP_LOOKUP_AT:
            snfs_lookup_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_GETATTR:
            snfs_getattr(request, private_data);
            break;
        case CHIMERA_VFS_OP_OPEN_FH:
            snfs_open_fh(request, private_data);
            break;
        case CHIMERA_VFS_OP_OPEN_AT:
            snfs_open_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_CLOSE:
            snfs_close(request, private_data);
            break;
        case CHIMERA_VFS_OP_MKDIR_AT:
            snfs_mkdir_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_MKNOD_AT:
            snfs_mknod_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_READDIR:
            snfs_readdir(request, private_data);
            break;
        case CHIMERA_VFS_OP_REMOVE_AT:
            snfs_remove_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_READ:
            snfs_read(request, private_data);
            break;
        case CHIMERA_VFS_OP_WRITE:
            snfs_write(request, private_data);
            break;
        case CHIMERA_VFS_OP_COMMIT:
            snfs_commit(request, private_data);
            break;
        case CHIMERA_VFS_OP_SYMLINK_AT:
            snfs_symlink_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_READLINK:
            snfs_readlink(request, private_data);
            break;
        case CHIMERA_VFS_OP_RENAME_AT:
            snfs_rename_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_LINK_AT:
            snfs_link_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_SETATTR:
            snfs_setattr(request, private_data);
            break;
        case CHIMERA_VFS_OP_ALLOCATE:
            snfs_allocate(request, private_data);
            break;
        case CHIMERA_VFS_OP_COPY_RANGE:
            snfs_copy_range(request, private_data);
            break;
        case CHIMERA_VFS_OP_CLONE_RANGE:
            snfs_clone_range(request, private_data);
            break;
        case CHIMERA_VFS_OP_SEEK:
            snfs_seek(request, private_data);
            break;
        case CHIMERA_VFS_OP_CLAIM_ACQUIRE:
            snfs_claim_acquire(request, private_data);
            break;
        case CHIMERA_VFS_OP_CLAIM_RELEASE:
            snfs_claim_release(request, private_data);
            break;
        default:
            snfs_error("snfs_dispatch: unknown operation %d",
                       request->opcode);
            request->status = CHIMERA_VFS_ENOTSUP;
            request->complete(request);
            break;
    } /* switch */
} /* snfs_dispatch */

SYMBOL_EXPORT struct chimera_vfs_module vfs_snfs = {
    .sdk_version  = CHIMERA_VFS_SDK_VERSION,
    .name         = "snfs",
    .fh_magic     = CHIMERA_VFS_FH_MAGIC_SNFS,
    .capabilities = CHIMERA_VFS_CAP_BLOCKING | CHIMERA_VFS_CAP_OPEN_PATH_REQUIRED |
        CHIMERA_VFS_CAP_FS | CHIMERA_VFS_CAP_FS_RELATIVE_OP | CHIMERA_VFS_CAP_FS_PATH_OP |
        CHIMERA_VFS_CAP_CLAIM_RANGE |
        CHIMERA_VFS_CAP_COPY_RANGE | CHIMERA_VFS_CAP_CLONE_RANGE
    ,
    .init           = snfs_init,
    .destroy        = snfs_destroy,
    .thread_init    = snfs_thread_init,
    .thread_destroy = snfs_thread_destroy,
    .dispatch       = snfs_dispatch,
};