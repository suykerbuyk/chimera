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
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <uthash.h>
#include "vfs/vfs_error.h"
#include "vfs/vfs.h"

#include "evpl/evpl.h"

#include "snfs_common.h"
#include "common/logging.h"
#include "common/format.h"
#include "common/misc.h"
#include "common/macros.h"

extern struct chimera_vfs_module vfs_snfs;

struct snfs_thread {
    struct evpl            *evpl;
    struct snfs_mount_table mount_table;
};

static void *
snfs_init(const char *cfgfile)
{
    /* We don't need any private data, but we're expected to return something */
    return (void *) 42;
} /* snfs_init */

static void
snfs_destroy(void *private_data)
{

} /* snfs_destroy */

static void *
snfs_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    struct snfs_thread *thread =
        (struct snfs_thread *) calloc(1, sizeof(*thread));

    thread->evpl = evpl;

    return thread;
} /* snfs_thread_init */

static void
snfs_thread_destroy(void *private_data)
{
    struct snfs_thread *thread = private_data;

    snfs_mount_table_destroy(&thread->mount_table);

    free(thread);
} /* snfs_thread_destroy */

static inline int
snfs_set_attrs(
    int                       fd,
    struct chimera_vfs_attrs *attr,
    uint64_t                  preset_attr)
{
    int rc;

    attr->va_set_mask = preset_attr;

    if (attr->va_req_mask & CHIMERA_VFS_ATTR_MODE) {

        rc = fchmodat(fd, "", attr->va_mode, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH
                      );

        if (rc) {
            snfs_error("snfs_setattr: fchmod(%o) failed: %s",
                       attr->va_mode,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_MODE;
    } /* snfs_set_attrs */

    if ((attr->va_req_mask & (CHIMERA_VFS_ATTR_UID | CHIMERA_VFS_ATTR_GID)) ==
        (CHIMERA_VFS_ATTR_UID | CHIMERA_VFS_ATTR_GID)) {

        rc = fchownat(fd, "", attr->va_uid, attr->va_gid,
                      AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: fchown(%u,%u) failed: %s",
                       attr->va_uid,
                       attr->va_gid,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_UID | CHIMERA_VFS_ATTR_GID;

    } else if (attr->va_req_mask & CHIMERA_VFS_ATTR_UID) {

        rc = fchownat(fd, "", attr->va_uid, -1,
                      AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: fchown(%u,-1) failed: %s",
                       attr->va_uid,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_UID;

    } else if (attr->va_req_mask & CHIMERA_VFS_ATTR_GID) {

        rc = fchownat(fd, "", -1, attr->va_gid,
                      AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

        if (rc) {
            snfs_error("snfs_setattr: fchown(%u,-1) failed: %s",
                       attr->va_gid,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_GID;
    }

    if (attr->va_req_mask & CHIMERA_VFS_ATTR_SIZE) {
        rc = ftruncate(fd, attr->va_size);

        if (rc) {
            snfs_error("snfs_setattr: ftruncate(%ld) failed: %s",
                       attr->va_size,
                       strerror(errno));

            return -errno;
        }

        attr->va_set_mask |= CHIMERA_VFS_ATTR_SIZE;
    }

    if (attr->va_req_mask & (CHIMERA_VFS_ATTR_ATIME | CHIMERA_VFS_ATTR_MTIME)) {
        struct timespec times[2];

        if (attr->va_req_mask & CHIMERA_VFS_ATTR_ATIME) {
            if (attr->va_atime.tv_nsec == CHIMERA_VFS_TIME_NOW) {
                times[0].tv_nsec = UTIME_NOW;
            } else {
                times[0] = attr->va_atime;
            }

            attr->va_set_mask |= CHIMERA_VFS_ATTR_ATIME;
        } else {
            times[0].tv_nsec = UTIME_OMIT;
        }

        if (attr->va_req_mask & CHIMERA_VFS_ATTR_MTIME) {
            if (attr->va_mtime.tv_nsec == CHIMERA_VFS_TIME_NOW) {
                times[1].tv_nsec = UTIME_NOW;
            } else {
                times[1] = attr->va_mtime;
            }

            attr->va_set_mask |= CHIMERA_VFS_ATTR_MTIME;

        } else {
            times[1].tv_nsec = UTIME_OMIT;
        }

        rc = utimensat(fd, "", times, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);

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

    rc = snfs_set_attrs(fd, request->setattr.set_attr, 0);

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->setattr.r_post_attr,
                   fd);

    request->status = snfs_errno_to_status(rc);
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
    rc = snfs_get_fh(CHIMERA_VFS_FH_MAGIC_SNFS,
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
} /* snfs_mount */

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
snfs_lookup(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int   parent_fd, rc;
    char *scratch = (char *) request->plugin_data;

    parent_fd = (int) request->lookup.handle->vfs_private;

    TERM_STR(fullname, request->lookup.component, request->lookup.component_len, scratch);

    snfs_debug("snfs_lookup: parent_fd=%d name=%s",
               parent_fd,
               fullname);
    rc = snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                              request,
                              &request->lookup.r_attr,
                              parent_fd,
                              fullname);

    if (rc) {
        request->status = rc;
        request->complete(request);
        return;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->lookup.r_dir_attr,
                   parent_fd);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_lookup */

static void
snfs_readdir(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int                      fd, dup_fd, rc;
    DIR                     *dir;
    struct dirent           *dirent;
    struct chimera_vfs_attrs vattr;
    int                      eof = 1;

    fd = request->readdir.handle->vfs_private;

    snfs_debug("snfs_readdir: opening %d", fd);

    dup_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY);

    if (dup_fd < 0) {
        snfs_error("snfs_readdir: openat() failed: %s",
                   strerror(errno));
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    dir = fdopendir(dup_fd);

    if (!dir) {
        snfs_error("snfs_readdir: fdopendir() failed: %s",
                   strerror(errno));
        close(dup_fd);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    if (request->readdir.cookie) {
        seekdir(dir, request->readdir.cookie);
    }

    vattr.va_req_mask = request->readdir.attr_mask;

    while ((dirent = readdir(dir))) {

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

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_readdir */

static void
snfs_open(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    int                 flags  = 0;
    int                 fd;

    if (request->open.flags & CHIMERA_VFS_OPEN_PATH) {
        flags |= O_PATH;
    }

    if (request->open.flags & CHIMERA_VFS_OPEN_DIRECTORY) {
        flags |= O_DIRECTORY | O_RDONLY;
    } else {
        flags |= O_RDWR;
    }

    fd = snfs_open_by_handle(&thread->mount_table,
                             request->fh,
                             request->fh_len,
                             flags);

    if (fd < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    request->open.r_vfs_private = fd;

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
    snfs_debug("snfs_open_at: parent_fd=%d name=%s with flags=0x%x",
               parent_fd, fullname, request->open_at.flags);
    flags = 0;

    if (request->open_at.flags & (CHIMERA_VFS_OPEN_PATH | CHIMERA_VFS_OPEN_DIRECTORY | CHIMERA_VFS_OPEN_READ_ONLY)) {
        flags |= O_RDONLY;
    } else {
        flags |= O_RDWR;
    }

    if (request->open_at.flags & CHIMERA_VFS_OPEN_DIRECTORY) {
        flags |= O_DIRECTORY;
    }

    if (request->open_at.flags & CHIMERA_VFS_OPEN_CREATE) {
        flags |= O_CREAT;
    }

    if (request->open_at.flags & CHIMERA_VFS_OPEN_PATH) {
        flags |= O_PATH;
    }

    if (request->open_at.set_attr->va_req_mask & CHIMERA_VFS_ATTR_MODE) {
        mode                                    = request->open_at.set_attr->va_mode;
        request->open_at.set_attr->va_set_mask |= CHIMERA_VFS_ATTR_MODE;
    } else {
        mode = 0600;
    }

    fd = openat(parent_fd,
                fullname,
                flags,
                mode);

    if (fd < 0) {
        snfs_error("snfs_open_at: openat(%d,%s,%d) failed: %s",
                   parent_fd,
                   fullname,
                   flags,
                   strerror(errno));
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    rc = snfs_set_attrs(fd, request->open_at.set_attr, CHIMERA_VFS_ATTR_MODE);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
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
snfs_mkdir(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int      fd, rc;
    char    *scratch = (char *) request->plugin_data;
    uint32_t mode;

    TERM_STR(fullname, request->mkdir.name, request->mkdir.name_len, scratch);

    fd = request->mkdir.handle->vfs_private;

    if (request->mkdir.set_attr->va_req_mask & CHIMERA_VFS_ATTR_MODE) {
        mode = request->mkdir.set_attr->va_mode;
    } else {
        mode = S_IRWXU;
    }

    rc = mkdirat(fd, fullname, mode);

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                   &request->mkdir.r_dir_post_attr,
                   fd);

    if (rc < 0) {

        if (errno == EEXIST) {
            snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                                 request,
                                 &request->mkdir.r_attr,
                                 fd,
                                 fullname);
        }

        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    rc = snfs_set_attrs(fd, request->mkdir.set_attr, CHIMERA_VFS_ATTR_MODE);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->mkdir.r_attr,
                         fd,
                         fullname);


    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_mkdir */

static void
snfs_remove(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int   fd, rc;
    char *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->remove.name, request->remove.namelen, scratch);

    fd = request->remove.handle->vfs_private;

    request->remove.r_removed_attr.va_req_mask = CHIMERA_VFS_ATTR_FH;

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->remove.r_removed_attr,
                         fd,
                         fullname);

    rc = unlinkat(fd, fullname, 0);

    if (rc == -1 && errno == EISDIR) {
        rc = unlinkat(fd, fullname, AT_REMOVEDIR);
    }

    if (rc) {
        request->status = snfs_errno_to_status(errno);
    } else {
        request->status = CHIMERA_VFS_OK;
    }

    request->complete(request);
} /* snfs_remove */

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

    request->read.r_niov = evpl_iovec_alloc(evpl,
                                            request->read.length,
                                            4096,
                                            8,
                                            request->read.iov);

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

        for (i = 0; i < request->read.r_niov; i++) {
            evpl_iovec_release(&request->read.iov[i]);
        }

        request->read.r_niov   = 0;
        request->read.r_length = 0;
        request->read.r_eof    = 0;
        request->complete(request);
        return;
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
snfs_symlink(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    int   fd, rc;
    char *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->symlink.name, request->symlink.namelen, scratch);
    TERM_STR(target, request->symlink.target, request->symlink.targetlen, scratch);

    fd = request->symlink.handle->vfs_private;

    rc = symlinkat(target, fd, fullname);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    snfs_map_attrs(CHIMERA_VFS_FH_MAGIC_SNFS, &request->symlink.r_dir_post_attr, fd);

    snfs_get_fh(CHIMERA_VFS_FH_MAGIC_SNFS,
                fd,
                fullname,
                request->symlink.r_attr.va_fh,
                &request->symlink.r_attr.va_fh_len);

    snfs_map_child_attrs(CHIMERA_VFS_FH_MAGIC_SNFS,
                         request,
                         &request->symlink.r_attr,
                         fd,
                         fullname);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_symlink */

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

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* snfs_readlink */

static void
snfs_rename(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    int                 old_fd, new_fd, rc;
    char               *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->rename.name, request->rename.namelen, scratch);
    TERM_STR(full_newname, request->rename.new_name, request->rename.new_namelen, scratch);

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
                                 request->rename.new_fh,
                                 request->rename.new_fhlen,
                                 O_PATH | O_RDONLY | O_NOFOLLOW);

    if (new_fd < 0) {
        close(old_fd);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    rc = renameat(old_fd, fullname, new_fd, full_newname);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
    } else {
        request->status = CHIMERA_VFS_OK;
    }

    close(old_fd);
    close(new_fd);

    request->complete(request);
} /* snfs_rename */

static void
snfs_link(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct snfs_thread *thread = private_data;
    int                 fd, dir_fd, rc;
    char               *scratch = (char *) request->plugin_data;

    TERM_STR(fullname, request->link.name, request->link.namelen, scratch);

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
                                 request->link.dir_fh,
                                 request->link.dir_fhlen,
                                 O_PATH | O_RDONLY | O_NOFOLLOW);

    if (dir_fd < 0) {
        close(fd);
        request->status = snfs_errno_to_status(errno);
        request->complete(request);
        return;
    }

    rc = linkat(fd, "", dir_fd, fullname, AT_EMPTY_PATH);

    if (rc < 0) {
        request->status = snfs_errno_to_status(errno);
    } else {
        request->status = CHIMERA_VFS_OK;
    }

    close(fd);
    close(dir_fd);

    request->complete(request);

} /* snfs_link */
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
        case CHIMERA_VFS_OP_LOOKUP:
            snfs_lookup(request, private_data);
            break;
        case CHIMERA_VFS_OP_GETATTR:
            snfs_getattr(request, private_data);
            break;
        case CHIMERA_VFS_OP_OPEN:
            snfs_open(request, private_data);
            break;
        case CHIMERA_VFS_OP_OPEN_AT:
            snfs_open_at(request, private_data);
            break;
        case CHIMERA_VFS_OP_CLOSE:
            snfs_close(request, private_data);
            break;
        case CHIMERA_VFS_OP_MKDIR:
            snfs_mkdir(request, private_data);
            break;
        case CHIMERA_VFS_OP_READDIR:
            snfs_readdir(request, private_data);
            break;
        case CHIMERA_VFS_OP_REMOVE:
            snfs_remove(request, private_data);
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
        case CHIMERA_VFS_OP_SYMLINK:
            snfs_symlink(request, private_data);
            break;
        case CHIMERA_VFS_OP_READLINK:
            snfs_readlink(request, private_data);
            break;
        case CHIMERA_VFS_OP_RENAME:
            snfs_rename(request, private_data);
            break;
        case CHIMERA_VFS_OP_LINK:
            snfs_link(request, private_data);
            break;
        case CHIMERA_VFS_OP_SETATTR:
            snfs_setattr(request, private_data);
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
    .capabilities = CHIMERA_VFS_CAP_BLOCKING | CHIMERA_VFS_CAP_OPEN_PATH_REQUIRED | CHIMERA_VFS_CAP_OPEN_FILE_REQUIRED
    ,
    .init           = snfs_init,
    .destroy        = snfs_destroy,
    .thread_init    = snfs_thread_init,
    .thread_destroy = snfs_thread_destroy,
    .dispatch       = snfs_dispatch,
};
