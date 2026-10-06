// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Functional proof of the loopback media provider over a real evpl loop.
 *
 * Unprivileged: the PREAD block protocol is buffered pread/pwrite on a regular
 * file, so this creates a temp >= 1 GiB sparse backing file (via the provider
 * itself, exercising the create-before-open path), opens it, and checks:
 *   - sizing: size_bytes() matches the on-disk file size, a whole number of
 *     4 KiB blocks;
 *   - bounds/alignment: a block range past the space is E_RANGE; a zero or
 *     oversized request, or a null/empty iovec, is E_INVAL;
 *   - round-trip: a pattern written at an interior and the last block reads
 *     back byte-for-byte;
 *   - durability: a force-unit-access write and a plain write + flush both
 *     complete OK.
 * The loop is driven with the while(!done) evpl_continue() idiom diskfs uses.
 */

#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evpl/evpl.h"

#include "media_loopback.h"

#define ONE_GIB (1ULL << 30)

struct open_wait {
    int                         done;
    enum chimera_media_error status;
    struct chimera_media_space *space;
};

struct io_wait {
    int done;
    enum chimera_media_error status;
};

static void
on_open(
    enum chimera_media_error    status,
    struct chimera_media_space *space,
    void                       *private_data)
{
    struct open_wait *w = private_data;

    w->status = status;
    w->space  = space;
    w->done   = 1;
} /* on_open */

static void
on_io(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct io_wait *w = private_data;

    w->status = status;
    w->done   = 1;
} /* on_io */

static void
pump(
    struct evpl *evpl,
    const int   *done)
{
    while (!*done) {
        evpl_continue(evpl);
    }
} /* pump */

/* Fill a block with a seed-dependent pattern. */
static void
fill_pattern(
    void    *buf,
    unsigned seed)
{
    uint8_t *p = buf;
    unsigned i;

    for (i = 0; i < CHIMERA_MEDIA_BLOCK_SIZE; i++) {
        p[i] = (uint8_t) (i * 31u + seed);
    }
} /* fill_pattern */

/* Write one block of pattern `seed` at `lba`, fua if requested, and wait. */
static void
write_block(
    const struct chimera_media_provider *p,
    struct evpl                         *evpl,
    struct chimera_media_space          *space,
    chimera_media_block_t                lba,
    unsigned                             seed,
    int                                  fua)
{
    struct evpl_iovec iov;
    struct io_wait    w = { 0, CHIMERA_MEDIA_E_IO };
    int               n;

    n = evpl_iovec_alloc(evpl, CHIMERA_MEDIA_BLOCK_SIZE, CHIMERA_MEDIA_BLOCK_SIZE,
                         1, 0, &iov);
    assert(n == 1);
    fill_pattern(evpl_iovec_data(&iov), seed);

    p->write(space, &iov, 1, lba, 1, fua, on_io, &w);
    pump(evpl, &w.done);
    assert(w.status == CHIMERA_MEDIA_OK);

    evpl_iovec_release(evpl, &iov);
} /* write_block */

/* Read one block at `lba` and assert it equals pattern `seed`. */
static void
read_verify_block(
    const struct chimera_media_provider *p,
    struct evpl                         *evpl,
    struct chimera_media_space          *space,
    chimera_media_block_t                lba,
    unsigned                             seed)
{
    struct evpl_iovec iov;
    struct io_wait    w = { 0, CHIMERA_MEDIA_E_IO };
    uint8_t           expect[CHIMERA_MEDIA_BLOCK_SIZE];
    int               n;

    n = evpl_iovec_alloc(evpl, CHIMERA_MEDIA_BLOCK_SIZE, CHIMERA_MEDIA_BLOCK_SIZE,
                         1, 0, &iov);
    assert(n == 1);

    p->read(space, &iov, 1, lba, 1, on_io, &w);
    pump(evpl, &w.done);
    assert(w.status == CHIMERA_MEDIA_OK);

    fill_pattern(expect, seed);
    assert(memcmp(evpl_iovec_data(&iov), expect, CHIMERA_MEDIA_BLOCK_SIZE) == 0);

    evpl_iovec_release(evpl, &iov);
} /* read_verify_block */

int
main(void)
{
    const char                          *tmp = getenv("TMPDIR");
    char                                 dir[512];
    char                                 path[600];
    struct evpl                         *evpl;
    const struct chimera_media_provider *p;
    struct open_wait                     ow = { 0, CHIMERA_MEDIA_E_IO, NULL };
    struct io_wait                       w;
    struct chimera_media_space          *space;
    struct stat                          st;
    uint64_t                             bytes, nblocks;
    enum chimera_media_error             err;
    struct evpl_iovec                    iov;
    int                                  n;

    snprintf(dir, sizeof(dir), "%s/media_mbt_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    assert(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/device-0.img", dir);

    evpl = evpl_create(NULL);
    assert(evpl != NULL);

    p = chimera_media_loopback();
    assert(p != NULL);

    /* The path does not exist yet: the provider creates and sizes it. */
    err = chimera_media_open(p, evpl, path, ONE_GIB, NULL, on_open, &ow);
    assert(err == CHIMERA_MEDIA_OK);
    pump(evpl, &ow.done);
    assert(ow.status == CHIMERA_MEDIA_OK);
    space = ow.space;
    assert(space != NULL);

    /* Sizing: size_bytes() is a whole number of blocks and matches the file. */
    bytes   = p->size_bytes(space);
    nblocks = bytes >> CHIMERA_MEDIA_BLOCK_SHIFT;
    assert(bytes == ONE_GIB);
    assert(bytes % CHIMERA_MEDIA_BLOCK_SIZE == 0);
    assert(stat(path, &st) == 0);
    assert((uint64_t) st.st_size == bytes);

    /* Bounds: one block past the end is E_RANGE (reported synchronously). */
    n = evpl_iovec_alloc(evpl, CHIMERA_MEDIA_BLOCK_SIZE, CHIMERA_MEDIA_BLOCK_SIZE,
                         1, 0, &iov);
    assert(n == 1);

    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, &iov, 1, nblocks, 1, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_RANGE);

    /* A range that straddles the end is also E_RANGE. */
    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, &iov, 1, nblocks - 1, 2, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_RANGE);

    /* Invalid: zero blocks, a null/empty iovec, an oversized request. */
    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, &iov, 1, 0, 0, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_INVAL);

    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, &iov, 0, 0, 1, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_INVAL);

    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, &iov, 1, 0, 0xFFFFFFFFu, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_INVAL);

    /* A null iovec pointer (niov > 0) is E_INVAL. */
    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, NULL, 1, 0, 1, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_INVAL);

    /* An iovec total that does not equal nblocks * block size is E_INVAL even
    * when the range is valid: here one 4 KiB block for a claimed 2 blocks. */
    w.done = 0; w.status = CHIMERA_MEDIA_OK;
    p->read(space, &iov, 1, 0, 2, on_io, &w);
    assert(w.done == 1 && w.status == CHIMERA_MEDIA_E_INVAL);

    evpl_iovec_release(evpl, &iov);

    /* Geometry accessor reports the same block count as size_bytes(). */
    {
        struct chimera_media_space_info gi = { 0, 0, 0 };
        p->info(space, &gi);
        assert(gi.block_size == CHIMERA_MEDIA_BLOCK_SIZE);
        assert(gi.nblocks == nblocks);
        assert(gi.max_io_blocks >= 1);
    }

    /* Round-trip: an interior block (FUA) and the last block (plain + flush). */
    write_block(p, evpl, space, 1, 0xA5u, 1 /* fua */);

    write_block(p, evpl, space, nblocks - 1, 0x3Cu, 0 /* plain */);
    w.done = 0; w.status = CHIMERA_MEDIA_E_IO;
    p->flush(space, on_io, &w);
    pump(evpl, &w.done);
    assert(w.status == CHIMERA_MEDIA_OK);

    read_verify_block(p, evpl, space, 1, 0xA5u);
    read_verify_block(p, evpl, space, nblocks - 1, 0x3Cu);

    /* Close the primary space. */
    w.done = 0; w.status = CHIMERA_MEDIA_E_IO;
    p->close(space, on_io, &w);
    pump(evpl, &w.done);
    assert(w.status == CHIMERA_MEDIA_OK);

    /* Opening under a non-existent directory cannot create the backing file:
     * the open reports E_NODEV (no space), reusing the same evpl. */
    {
        char             bad[700];
        struct open_wait bw = { 0, CHIMERA_MEDIA_OK, NULL };

        snprintf(bad, sizeof(bad), "%s/nosuchdir/dev.img", dir);
        err = chimera_media_open(p, evpl, bad, ONE_GIB, NULL, on_open, &bw);
        assert(err == CHIMERA_MEDIA_OK);   /* ABI ok; the open is initiated */
        pump(evpl, &bw.done);
        assert(bw.status == CHIMERA_MEDIA_E_NODEV);
        assert(bw.space == NULL);
    }

    /* An existing file smaller than min_size is grown to min_size on open. */
    {
        char             small[700];
        struct open_wait sw = { 0, CHIMERA_MEDIA_E_IO, NULL };
        int              fd;

        snprintf(small, sizeof(small), "%s/small.img", dir);
        fd = open(small, O_CREAT | O_RDWR, 0644);
        assert(fd >= 0);
        assert(ftruncate(fd, (off_t) (8 * CHIMERA_MEDIA_BLOCK_SIZE)) == 0); /* 32 KiB */
        assert(close(fd) == 0);

        err = chimera_media_open(p, evpl, small, ONE_GIB, NULL, on_open, &sw);
        assert(err == CHIMERA_MEDIA_OK);
        pump(evpl, &sw.done);
        assert(sw.status == CHIMERA_MEDIA_OK);
        assert(sw.space != NULL);
        assert(p->size_bytes(sw.space) == ONE_GIB);   /* grown */
        assert(stat(small, &st) == 0 && (uint64_t) st.st_size == ONE_GIB);

        w.done = 0; w.status = CHIMERA_MEDIA_E_IO;
        p->close(sw.space, on_io, &w);
        pump(evpl, &w.done);
        assert(w.status == CHIMERA_MEDIA_OK);
        assert(unlink(small) == 0);
    }

    /* Tear down the loop and the fixture. */
    evpl_destroy(evpl);

    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0);

    printf("media_loopback_test: ok\n");
    return 0;
} /* main */
