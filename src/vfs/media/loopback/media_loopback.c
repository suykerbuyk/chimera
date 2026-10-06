// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * The loopback-file media provider.  See media_loopback.h for the contract.
 *
 * One struct chimera_media_space maps to one libevpl block device opened with
 * EVPL_BLOCK_PROTOCOL_PREAD over a regular file, plus a per-space queue on the
 * opening evpl thread.  v1 is a single space covering the whole file
 * (base_block 0); the base_block field is where a later multi-space carve
 * (contract O15) would record each space's offset without changing callers.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evpl/evpl.h"

#include "common/macros.h"
#include "media_loopback.h"

/* Smallest backing file the prototype creates (contract/E14: >= 1 GiB). */
#define LOOPBACK_MIN_BYTES (1ULL << 30)

struct chimera_media_space {
    struct evpl              *evpl;
    struct evpl_block_device *bdev;
    struct evpl_block_queue  *queue;
    uint64_t                  base_block;    /* space start within the device */
    uint64_t                  nblocks;       /* space size, in blocks */
    uint32_t                  max_io_blocks; /* largest single read/write */
};

/* Per-operation trampoline state: carries the media callback across one
 * libevpl block completion. */
struct loopback_io {
    chimera_media_io_cb_t cb;
    void                 *private_data;
};

struct loopback_open_ctx {
    chimera_media_open_cb_t     cb;
    void                       *private_data;
    struct chimera_media_space *space;
};

struct loopback_close_ctx {
    chimera_media_io_cb_t       cb;
    void                       *private_data;
    struct chimera_media_space *space;
};

/* A libevpl block status is 0 or a positive errno. */
static enum chimera_media_error
loopback_map_status(int status)
{
    if (status == 0) {
        return CHIMERA_MEDIA_OK;
    }
    if (status == ENOSPC) {
        return CHIMERA_MEDIA_E_NO_SPACE;
    }
    return CHIMERA_MEDIA_E_IO;
} /* loopback_map_status */

/* ---- read / write / flush completion ---- */

static void
loopback_io_complete(
    struct evpl *evpl,
    int          status,
    void        *private_data)
{
    struct loopback_io   *io   = private_data;
    chimera_media_io_cb_t cb   = io->cb;
    void                 *priv = io->private_data;

    (void) evpl;
    free(io);
    cb(loopback_map_status(status), priv);
} /* loopback_io_complete */

/* Validate a read/write's arguments against the space.  Returns
 * CHIMERA_MEDIA_OK, or the rejection to report (synchronously, a caller bug). */
static enum chimera_media_error
loopback_check_io(
    const struct chimera_media_space *space,
    int                               niov,
    const struct evpl_iovec          *iov,
    chimera_media_block_t             lba,
    uint32_t                          nblocks)
{
    uint64_t total = 0;
    int i;

    if (!iov || niov <= 0 || nblocks == 0 || nblocks > space->max_io_blocks) {
        return CHIMERA_MEDIA_E_INVAL;
    }
    /* lba + nblocks must fit in the space; written so it cannot overflow.
     * Checked before the iovec total so a range error is reported as E_RANGE
     * rather than being masked by a length mismatch. */
    if (nblocks > space->nblocks || lba > space->nblocks - nblocks) {
        return CHIMERA_MEDIA_E_RANGE;
    }
    /* The block ops carry no byte length: evpl_block_read/write move exactly
     * the sum of the iovec lengths.  Require that to equal nblocks * the block
     * size, so a short or long iovec cannot read/write past the addressed
     * blocks (and a write cannot extend the file past the space's last block). */
    for (i = 0; i < niov; i++) {
        total += evpl_iovec_length(&iov[i]);
    }
    if (total != ((uint64_t) nblocks << CHIMERA_MEDIA_BLOCK_SHIFT)) {
        return CHIMERA_MEDIA_E_INVAL;
    }
    return CHIMERA_MEDIA_OK;
} /* loopback_check_io */

static uint64_t
loopback_byte_offset(
    const struct chimera_media_space *space,
    chimera_media_block_t             lba)
{
    return (space->base_block + lba) << CHIMERA_MEDIA_BLOCK_SHIFT;
} /* loopback_byte_offset */

static void
loopback_read(
    struct chimera_media_space *space,
    struct evpl_iovec          *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    enum chimera_media_error err;
    struct loopback_io      *io;

    err = loopback_check_io(space, niov, iov, lba, nblocks);
    if (err != CHIMERA_MEDIA_OK) {
        cb(err, private_data);
        return;
    }

    io = malloc(sizeof(*io));
    if (!io) {
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    io->cb           = cb;
    io->private_data = private_data;

    evpl_block_read(space->evpl, space->queue, iov, niov,
                    loopback_byte_offset(space, lba),
                    loopback_io_complete, io);
} /* loopback_read */

static void
loopback_write(
    struct chimera_media_space *space,
    const struct evpl_iovec    *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    int                         fua,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    enum chimera_media_error err;
    struct loopback_io      *io;

    err = loopback_check_io(space, niov, iov, lba, nblocks);
    if (err != CHIMERA_MEDIA_OK) {
        cb(err, private_data);
        return;
    }

    io = malloc(sizeof(*io));
    if (!io) {
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    io->cb           = cb;
    io->private_data = private_data;

    /* fua != 0 -> libevpl's sync flag: the data is durable before the
     * completion fires (force unit access). */
    evpl_block_write(space->evpl, space->queue, iov, niov,
                     loopback_byte_offset(space, lba), fua ? 1 : 0,
                     loopback_io_complete, io);
} /* loopback_write */

static void
loopback_flush(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    struct loopback_io *io = malloc(sizeof(*io));

    if (!io) {
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    io->cb           = cb;
    io->private_data = private_data;

    evpl_block_flush(space->evpl, space->queue, loopback_io_complete, io);
} /* loopback_flush */

/* ---- open ---- */

static void
loopback_device_opened(
    struct evpl              *evpl,
    struct evpl_block_device *bdev,
    int                       status,
    void                     *private_data)
{
    struct loopback_open_ctx   *octx  = private_data;
    struct chimera_media_space *space = octx->space;
    uint64_t                    max_bytes;

    if (status != 0 || !bdev) {
        octx->cb(CHIMERA_MEDIA_E_NODEV, NULL, octx->private_data);
        free(space);
        free(octx);
        return;
    }

    space->bdev       = bdev;
    space->queue      = evpl_block_open_queue(evpl, bdev);
    space->base_block = 0;
    space->nblocks    = evpl_block_size(bdev) >> CHIMERA_MEDIA_BLOCK_SHIFT;

    max_bytes            = evpl_block_max_request_size(bdev);
    space->max_io_blocks = (uint32_t) (max_bytes >> CHIMERA_MEDIA_BLOCK_SHIFT);
    if (space->max_io_blocks == 0) {
        space->max_io_blocks = 1;
    }

    octx->cb(CHIMERA_MEDIA_OK, space, octx->private_data);
    free(octx);
} /* loopback_device_opened */

/* Grow an existing fd to `size` if it is shorter.  Returns 0, or -1 with
 * errno set. */
static int
loopback_grow_to(
    int      fd,
    uint64_t cur,
    uint64_t size)
{
    if (cur >= size) {
        return 0;
    }
    if (ftruncate(fd, (off_t) size) < 0) {   /* sparse; no data is written */
        return -1;
    }
    return 0;
} /* loopback_grow_to */

/* Ensure a backing file at `path` exists and is at least `min_size` (rounded
 * up to a whole block).  The PREAD protocol opens O_RDWR with no O_CREAT, so
 * this runs before evpl_block_open_device.  An existing regular file that is
 * too small is grown.  A new file is created atomically -- sized in a temp
 * file and rename()d into place -- so an interrupted first run cannot leave a
 * 0-byte file that would later open as a 0-block space.  Returns 0, or -1 with
 * errno set. */
static int
loopback_create_backing(
    const char *path,
    uint64_t    min_size)
{
    struct stat st;
    uint64_t    size;
    char        tmp[PATH_MAX];
    int         fd;
    int         n;
    int         saved;

    size = min_size < LOOPBACK_MIN_BYTES ? LOOPBACK_MIN_BYTES : min_size;
    /* Round up to a whole block so the block count is exact. */
    size = (size + CHIMERA_MEDIA_BLOCK_SIZE - 1) & ~((uint64_t) CHIMERA_MEDIA_BLOCK_SIZE - 1);

    if (stat(path, &st) == 0) {
        /* Exists.  A regular file that is undersized is grown; a non-regular
         * backing object (e.g. a block device) is taken as-is. */
        if (!S_ISREG(st.st_mode)) {
            return 0;
        }
        if ((uint64_t) st.st_size >= size) {
            return 0;
        }
        fd = open(path, O_RDWR);
        if (fd < 0) {
            return -1;
        }
        if (loopback_grow_to(fd, (uint64_t) st.st_size, size) < 0) {
            saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        close(fd);
        return 0;
    }
    if (errno != ENOENT) {
        return -1;
    }

    /* Create atomically: size a temp file in the same directory, then rename
     * it into place. */
    n = snprintf(tmp, sizeof(tmp), "%s.tmpXXXXXX", path);
    if (n < 0 || (size_t) n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = mkstemp(tmp);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t) size) < 0) {   /* sparse; no data is written */
        saved = errno;
        close(fd);
        unlink(tmp);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0) {
        saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    if (rename(tmp, path) < 0) {
        saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
} /* loopback_create_backing */

static void
loopback_open(
    struct evpl               *evpl,
    const char                *uri,
    uint64_t                   min_size,
    struct prometheus_metrics *metrics,
    chimera_media_open_cb_t    cb,
    void                      *private_data)
{
    struct loopback_open_ctx   *octx;
    struct chimera_media_space *space;

    (void) metrics;

    if (loopback_create_backing(uri, min_size) < 0) {
        cb(CHIMERA_MEDIA_E_NODEV, NULL, private_data);
        return;
    }

    space = calloc(1, sizeof(*space));
    octx  = malloc(sizeof(*octx));
    if (!space || !octx) {
        free(space);
        free(octx);
        cb(CHIMERA_MEDIA_E_NODEV, NULL, private_data);
        return;
    }

    space->evpl        = evpl;
    octx->cb           = cb;
    octx->private_data = private_data;
    octx->space        = space;

    evpl_block_open_device(evpl, EVPL_BLOCK_PROTOCOL_PREAD, uri,
                           loopback_device_opened, octx);
} /* loopback_open */

/* ---- close ---- */

static void
loopback_device_closed(
    struct evpl *evpl,
    int          status,
    void        *private_data)
{
    struct loopback_close_ctx *cctx = private_data;
    chimera_media_io_cb_t      cb   = cctx->cb;
    void                      *priv = cctx->private_data;

    (void) evpl;
    free(cctx->space);
    free(cctx);
    cb(loopback_map_status(status), priv);
} /* loopback_device_closed */

static void
loopback_close(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    struct evpl               *evpl = space->evpl;
    struct evpl_block_device  *bdev = space->bdev;
    struct loopback_close_ctx *cctx;

    evpl_block_close_queue(evpl, space->queue);

    cctx = malloc(sizeof(*cctx));
    if (!cctx) {
        /* Fall back to a fire-and-forget device close so the device still
         * drains; the space is freed now and the caller is told. */
        evpl_block_close_device(evpl, bdev, NULL, NULL);
        free(space);
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    cctx->cb           = cb;
    cctx->private_data = private_data;
    cctx->space        = space;

    evpl_block_close_device(evpl, bdev, loopback_device_closed, cctx);
} /* loopback_close */

static uint64_t
loopback_size_bytes(struct chimera_media_space *space)
{
    return space->nblocks << CHIMERA_MEDIA_BLOCK_SHIFT;
} /* loopback_size_bytes */

static void
loopback_info(
    struct chimera_media_space      *space,
    struct chimera_media_space_info *out)
{
    out->nblocks       = space->nblocks;
    out->block_size    = CHIMERA_MEDIA_BLOCK_SIZE;
    out->max_io_blocks = space->max_io_blocks;
} /* loopback_info */

static const struct chimera_media_provider loopback_provider = {
    .abi_version = CHIMERA_MEDIA_ABI_VERSION,
    .reserved    = 0,
    .name        = "loopback",
    .open        = loopback_open,
    .read        = loopback_read,
    .write       = loopback_write,
    .flush       = loopback_flush,
    .close       = loopback_close,
    .size_bytes  = loopback_size_bytes,
    .info        = loopback_info,
};

SYMBOL_EXPORT const struct chimera_media_provider *
chimera_media_loopback(void)
{
    return &loopback_provider;
} /* chimera_media_loopback */
