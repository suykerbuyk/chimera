/*
 * Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
 */

#pragma once

#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include <extapi.h>
#include <extapi_internal.h>

#include "vfs/vfs_attrs.h"

/**
 * @file snfs_cvapi.h
 * @brief Helpers wrapping StorNext's CvApi for the SNFS VFS module.
 *
 * The CvApi @c *_CreateTime calls dispatch ioctls on the file descriptor, so
 * they require a regular fd (ioctl is not supported on @c O_PATH fds). SNFS
 * handles can be opened with @c O_PATH (the module advertises
 * @c CHIMERA_VFS_CAP_OPEN_PATH_REQUIRED), so these helpers materialize a
 * non-@c O_PATH fd before calling into CvApi.
 */

/**
 * @brief Acquire a non-O_PATH fd for @p (dirfd, path) suitable for
 *        ioctl-based CvApi calls.
 *
 * Behavior by case:
 *  - @p path non-empty: openat() the child with @c O_RDONLY; caller owns
 *    the returned fd.
 *  - @p path empty, @p dirfd is already regular: @p dirfd is returned
 *    borrowed (no syscall, caller must NOT close).
 *  - @p path empty, @p dirfd is @c O_PATH: reopened via the
 *    @c /proc/self/fd magic link so the returned fd is regular; caller
 *    owns the new fd.
 *
 * @param dirfd      Open file descriptor used as the base for path resolution.
 *                   May be @c O_PATH.
 * @param path       Relative path beneath @p dirfd, or an empty string ("")
 *                   to operate on @p dirfd itself.
 * @param out_owned  Set to 1 if the caller must @c close() the returned fd,
 *                   or 0 if the fd is borrowed from @p dirfd. Only valid on
 *                   successful return.
 *
 * @return >= 0 fd on success, or -1 with @c errno set on failure.
 */
static inline int
snfs_cvapi_open_fd(
    int         dirfd,
    const char *path,
    int        *out_owned)
{
    char procpath[64];
    int  fd;
    int  flags;

    *out_owned = 0;

    if (path[0] != '\0') {
        fd = openat(dirfd, path, O_RDONLY | O_NOFOLLOW);
        if (fd >= 0) {
            *out_owned = 1;
        }
        return fd;
    }

    flags = fcntl(dirfd, F_GETFL);
    if (flags < 0) {
        return -1;
    }

    if (!(flags & O_PATH)) {
        /* dirfd is already usable for ioctl; borrow it */
        return dirfd;
    }

    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", dirfd);
    fd = open(procpath, O_RDONLY);
    if (fd >= 0) {
        *out_owned = 1;
    }
    return fd;
} /* snfs_cvapi_open_fd */

/**
 * @brief Read the StorNext birth/creation time for @p (dirfd, path).
 *
 * Wraps @c CvApi_GetCreateTime, transparently handling @c O_PATH @p dirfd
 * via snfs_cvapi_open_fd().
 *
 * @param dirfd  Open file descriptor used as the base for path resolution.
 *               May be @c O_PATH.
 * @param path   Relative path beneath @p dirfd, or an empty string ("") to
 *               refer to @p dirfd itself.
 * @param btime  Out parameter; on success receives the file's birth time.
 *
 * @return 0 on success, or a negative @c errno value on failure.
 */
static inline int
snfs_cvapi_get_btime(
    int              dirfd,
    const char      *path,
    struct timespec *btime)
{
    int fd;
    int owned;
    int rc;
    int saved_errno;

    fd = snfs_cvapi_open_fd(dirfd, path, &owned);

    if (fd < 0) {
        return -errno;
    }

    rc          = CvApi_GetCreateTime(fd, btime);
    saved_errno = errno;

    if (owned) {
        close(fd);
    }

    return rc ? -saved_errno : 0;
} /* snfs_cvapi_get_btime */

/**
 * @brief Set the StorNext birth/creation time on @p (dirfd, path).
 *
 * Wraps @c CvApi_SetCreateTime, transparently handling @c O_PATH @p dirfd
 * via snfs_cvapi_open_fd(). If @p btime->tv_nsec equals
 * @c CHIMERA_VFS_TIME_NOW, the current wall-clock time is used instead of
 * the value in @p btime.
 *
 * @param dirfd  Open file descriptor used as the base for path resolution.
 *               May be @c O_PATH.
 * @param path   Relative path beneath @p dirfd, or an empty string ("") to
 *               refer to @p dirfd itself.
 * @param btime  Desired birth time. Not modified; @c CHIMERA_VFS_TIME_NOW is
 *               expanded into a local copy.
 *
 * @return 0 on success, or a negative @c errno value on failure.
 */
static inline int
snfs_cvapi_set_btime(
    int                    dirfd,
    const char            *path,
    const struct timespec *btime)
{
    struct timespec t = *btime;
    int             fd;
    int             owned;
    int             rc;
    int             saved_errno;

    fd = snfs_cvapi_open_fd(dirfd, path, &owned);

    if (fd < 0) {
        return -errno;
    }

    if (t.tv_nsec == CHIMERA_VFS_TIME_NOW) {
        clock_gettime(CLOCK_REALTIME, &t);
    }

    rc          = CvApi_SetCreateTime(fd, &t);
    saved_errno = errno;

    if (owned) {
        close(fd);
    }

    return rc ? -saved_errno : 0;
} /* snfs_cvapi_set_btime */
