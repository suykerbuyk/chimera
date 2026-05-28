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
#include <errno.h>
#include <uthash.h>
#include <jansson.h>
#include <linux/version.h>
#include "vfs/vfs_error.h"
#include "vfs/vfs_internal.h"

// fchmodat support for AT_SYMLINK_NOFOLLOW was added in Linux 6.6
#if defined(LINUX_VERSION_CODE) && defined(KERNEL_VERSION)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define HAVE_FCHMODAT_AT_SYMLINK_NOFOLLOW 1
#endif /* if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) */
#endif /* if defined(LINUX_VERSION_CODE) && defined(KERNEL_VERSION) */

#include "evpl/evpl.h"

#include "vfs/vfs.h"
#include "snfs_common.h"
#include "common/logging.h"
#include "common/format.h"
#include "common/misc.h"
#include "common/macros.h"

extern struct chimera_vfs_module vfs_snfs;

struct snfs_shared {
    int readdir_verifier;
};

struct snfs_thread {
    struct evpl            *evpl;
    struct snfs_mount_table mount_table;
    int                     readdir_verifier;
};

static void *
snfs_init(const char *cfgdata)
{
    struct snfs_shared *shared;

    shared = calloc(1, sizeof(*shared));

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
    free(private_data);
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


    if (set_mask & CHIMERA_VFS_ATTR_BTIME) {
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

        if (set_mask & CHIMERA_VFS_ATTR_ATIME) {
            if (attr->va_atime.tv_nsec == CHIMERA_VFS_TIME_NOW) {
                times[0].tv_nsec = UTIME_NOW;
            } else {
                times[0] = attr->va_atime;
            }

            attr->va_set_mask |= CHIMERA_VFS_ATTR_ATIME;
        } else {
            times[0].tv_nsec = UTIME_OMIT;
        }

        if (set_mask & CHIMERA_VFS_ATTR_MTIME) {
            if (attr->va_mtime.tv_nsec == CHIMERA_VFS_TIME_NOW) {
                times[1].tv_nsec = UTIME_NOW;
            } else {
                times[1] = attr->va_mtime;
            }

            attr->va_set_mask |= CHIMERA_VFS_ATTR_MTIME;
        } else {
            times[1].tv_nsec = UTIME_OMIT;
        }

        rc = utimensat(dirfd, path, times, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: utimensat() failed: %s",
                       strerror(errno));

            return -errno;
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
static void
snfs_lock(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int          fd = (int) request->lock.handle->vfs_private;
    int          cmd;
    struct flock fl = { 0 };
    int          rc;

    switch (request->lock.lock_type) {
        case CHIMERA_VFS_LOCK_READ:
            fl.l_type = F_RDLCK;
            break;
        case CHIMERA_VFS_LOCK_WRITE:
            fl.l_type = F_WRLCK;
            break;
        case CHIMERA_VFS_LOCK_UNLOCK:
            fl.l_type = F_UNLCK;
            break;
        default:
            request->status = snfs_errno_to_status(EINVAL);
            request->complete(request);
            return;
    } /* switch */

    fl.l_whence = request->lock.whence;
    if (request->lock.whence == SEEK_END) {
        fl.l_start = (off_t) (int64_t) request->lock.offset;
        fl.l_len   = (off_t) (int64_t) request->lock.length;
    } else {
        fl.l_start = (off_t) request->lock.offset;
        fl.l_len   = (off_t) request->lock.length; /* 0 = to EOF */
    }
    fl.l_pid = 0;

    if (request->lock.flags & CHIMERA_VFS_LOCK_TEST) {
        cmd = F_GETLK;
    } else if (request->lock.flags & CHIMERA_VFS_LOCK_WAIT) {
        cmd = F_SETLKW;
    } else {
        cmd = F_SETLK;
    }

    rc = fcntl(fd, cmd, &fl);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    if (request->lock.flags & CHIMERA_VFS_LOCK_TEST) {
        if (fl.l_type == F_UNLCK) {
            request->lock.r_conflict_type   = CHIMERA_VFS_LOCK_UNLOCK;
            request->lock.r_conflict_offset = 0;
            request->lock.r_conflict_length = 0;
            request->lock.r_conflict_pid    = 0;
        } else {
            request->lock.r_conflict_type = (fl.l_type == F_RDLCK)
                ? CHIMERA_VFS_LOCK_READ
                : CHIMERA_VFS_LOCK_WRITE;
            request->lock.r_conflict_offset = fl.l_start;
            request->lock.r_conflict_length = fl.l_len;
            request->lock.r_conflict_pid    = fl.l_pid;
        }
    }

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_lock */

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
        case CHIMERA_VFS_OP_SEEK:
            snfs_seek(request, private_data);
            break;
        case CHIMERA_VFS_OP_LOCK:
            snfs_lock(request, private_data);
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
    .name         = "snfs",
    .fh_magic     = CHIMERA_VFS_FH_MAGIC_SNFS,
    .capabilities = CHIMERA_VFS_CAP_BLOCKING | CHIMERA_VFS_CAP_OPEN_PATH_REQUIRED | CHIMERA_VFS_CAP_OPEN_FILE_REQUIRED |
        CHIMERA_VFS_CAP_FS | CHIMERA_VFS_CAP_FS_RELATIVE_OP | CHIMERA_VFS_CAP_FS_PATH_OP |
        CHIMERA_VFS_CAP_FS_LOCK
    ,
    .init           = snfs_init,
    .destroy        = snfs_destroy,
    .thread_init    = snfs_thread_init,
    .thread_destroy = snfs_thread_destroy,
    .dispatch       = snfs_dispatch,
};