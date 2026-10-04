// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include <stddef.h>
#include <string.h>

/*
 * A protocol-supplied name is one path component.  The public chimera_vfs_*_at
 * functions reject a name that carries a path separator or an embedded NUL
 * before any dispatch, so a backend that resolves paths (linux/io_uring/snfs)
 * cannot be handed a separator it would walk outside the share.  The whole-path
 * callers -- the VFS's own path procs and the POSIX/client dirfd layer -- use
 * the chimera_vfs_*_at_path twins instead, which skip this check.
 *
 * "." and the empty name are deliberately not judged here: a lookup of "." or
 * ".." is legitimate, and each backend keeps its own handling of an empty name.
 * An exact ".." is refused only on create-type operations, which never have a
 * legitimate reason to name it; that is chimera_vfs_name_is_dotdot(), applied at
 * those call sites.
 */

static inline int
chimera_vfs_name_bad(
    const char *name,
    int         namelen)
{
    return namelen < 0 ||
           memchr(name, '/', (size_t) namelen) != NULL ||
           memchr(name, '\0', (size_t) namelen) != NULL;
} /* chimera_vfs_name_bad */

static inline int
chimera_vfs_name_is_dotdot(
    const char *name,
    int         namelen)
{
    return namelen == 2 && name[0] == '.' && name[1] == '.';
} /* chimera_vfs_name_is_dotdot */
