// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * A labelled non-conformant media provider, umbrella + libc only, no libevpl.
 * It is NOT a usable provider -- open/read/write/flush/close complete
 * synchronously and store nothing to media.  It exists to prove the interface
 * shape and the registration discipline:
 *
 *   - chimera_media_open refuses a provider whose abi_version does not match
 *     CHIMERA_MEDIA_ABI_VERSION, and does not call its open;
 *   - chimera_media_open accepts a matching provider and drives it;
 *   - the vtable is complete enough to carry one open/write(fua)/read/flush/
 *     close round, with the fua flag reaching the provider.
 *
 * Passing NULL for the forward-declared evpl / prometheus pointers is why this
 * links no libevpl: the interface only ever passes them through.
 */

#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "chimera_media.h"

/* The stub's concrete space. */
struct chimera_media_space {
    uint64_t nbytes;
};

static struct stub_state {
    int open_called;
    int last_write_fua;
    int close_called;
} g_stub;

static struct chimera_media_space g_space = { .nbytes = 1ULL << 30 };

static void
stub_open(
    struct evpl               *evpl,
    const char                *uri,
    uint64_t                   min_size,
    struct prometheus_metrics *metrics,
    chimera_media_open_cb_t    cb,
    void                      *private_data)
{
    (void) evpl;
    (void) uri;
    (void) min_size;
    (void) metrics;
    g_stub.open_called = 1;
    cb(CHIMERA_MEDIA_OK, &g_space, private_data);
} /* stub_open */

static void
stub_read(
    struct chimera_media_space *space,
    struct evpl_iovec          *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    (void) space;
    (void) iov;
    (void) niov;
    (void) lba;
    (void) nblocks;
    cb(CHIMERA_MEDIA_OK, private_data);
} /* stub_read */

static void
stub_write(
    struct chimera_media_space *space,
    const struct evpl_iovec    *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    int                         fua,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    (void) space;
    (void) iov;
    (void) niov;
    (void) lba;
    (void) nblocks;
    g_stub.last_write_fua = fua;
    cb(CHIMERA_MEDIA_OK, private_data);
} /* stub_write */

static void
stub_flush(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    (void) space;
    cb(CHIMERA_MEDIA_OK, private_data);
} /* stub_flush */

static void
stub_close(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    (void) space;
    g_stub.close_called = 1;
    cb(CHIMERA_MEDIA_OK, private_data);
} /* stub_close */

static uint64_t
stub_size_bytes(struct chimera_media_space *space)
{
    return space->nbytes;
} /* stub_size_bytes */

static void
stub_info(
    struct chimera_media_space      *space,
    struct chimera_media_space_info *out)
{
    out->nblocks       = space->nbytes >> CHIMERA_MEDIA_BLOCK_SHIFT;
    out->block_size    = CHIMERA_MEDIA_BLOCK_SIZE;
    out->max_io_blocks = 1;
} /* stub_info */

static const struct chimera_media_provider stub_provider = {
    .abi_version = CHIMERA_MEDIA_ABI_VERSION,
    .name        = "null",
    .open        = stub_open,
    .read        = stub_read,
    .write       = stub_write,
    .flush       = stub_flush,
    .close       = stub_close,
    .size_bytes  = stub_size_bytes,
    .info        = stub_info,
};

/* open-callback context */
struct open_ctx {
    enum chimera_media_error status;
    struct chimera_media_space *space;
    int                         fired;
};

static void
on_open(
    enum chimera_media_error    status,
    struct chimera_media_space *space,
    void                       *private_data)
{
    struct open_ctx *ctx = private_data;

    ctx->status = status;
    ctx->space  = space;
    ctx->fired  = 1;
} /* on_open */

static enum chimera_media_error g_io_status;

static void
on_io(
    enum chimera_media_error status,
    void                    *private_data)
{
    (void) private_data;
    g_io_status = status;
} /* on_io */

static void
test_abi_refused(void)
{
    struct chimera_media_provider bad = stub_provider;
    struct open_ctx               ctx = { 0 };

    bad.abi_version = CHIMERA_MEDIA_ABI_VERSION + 1;
    memset(&g_stub, 0, sizeof(g_stub));

    /* A mismatched ABI is refused synchronously; the provider's open is not
     * called, and on_open does not fire. */
    assert(chimera_media_open(&bad, NULL, "ignored", 1ULL << 30, NULL,
                              on_open, &ctx) == CHIMERA_MEDIA_E_NOT_SUPPORTED);
    assert(g_stub.open_called == 0);
    assert(ctx.fired == 0);

    /* A NULL provider is refused the same way. */
    assert(chimera_media_open(NULL, NULL, "ignored", 1ULL << 30, NULL,
                              on_open, &ctx) == CHIMERA_MEDIA_E_NOT_SUPPORTED);
} /* test_abi_refused */

static void
test_round_trip(void)
{
    struct open_ctx ctx = { 0 };

    memset(&g_stub, 0, sizeof(g_stub));

    assert(chimera_media_open(&stub_provider, NULL, "ignored", 1ULL << 30, NULL,
                              on_open, &ctx) == CHIMERA_MEDIA_OK);
    assert(g_stub.open_called == 1);
    assert(ctx.fired == 1);
    assert(ctx.status == CHIMERA_MEDIA_OK);
    assert(ctx.space != NULL);

    assert(stub_provider.size_bytes(ctx.space) == (1ULL << 30));

    {
        struct chimera_media_space_info gi = { 0, 0, 0 };
        stub_provider.info(ctx.space, &gi);
        assert(gi.block_size == CHIMERA_MEDIA_BLOCK_SIZE);
        assert(gi.nblocks == ((1ULL << 30) >> CHIMERA_MEDIA_BLOCK_SHIFT));
        assert(gi.max_io_blocks == 1);
    }

    g_io_status = CHIMERA_MEDIA_E_IO;
    stub_provider.write(ctx.space, NULL, 0, 0, 1, CHIMERA_MEDIA_WRITE_FUA, on_io, NULL);
    assert(g_io_status == CHIMERA_MEDIA_OK);
    assert(g_stub.last_write_fua == CHIMERA_MEDIA_WRITE_FUA);

    g_io_status = CHIMERA_MEDIA_E_IO;
    stub_provider.read(ctx.space, NULL, 0, 0, 1, on_io, NULL);
    assert(g_io_status == CHIMERA_MEDIA_OK);

    g_io_status = CHIMERA_MEDIA_E_IO;
    stub_provider.flush(ctx.space, on_io, NULL);
    assert(g_io_status == CHIMERA_MEDIA_OK);

    g_io_status = CHIMERA_MEDIA_E_IO;
    stub_provider.close(ctx.space, on_io, NULL);
    assert(g_io_status == CHIMERA_MEDIA_OK);
    assert(g_stub.close_called == 1);
} /* test_round_trip */

int
main(void)
{
    test_abi_refused();
    test_round_trip();
    printf("media_null_provider_test: ok\n");
    return 0;
} /* main */
