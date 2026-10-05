// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Regression fixture for the generated XDR decode-length bounds.
 *
 * The XDR vector decoders and the generator's contig fixed-opaque[N] copy are
 * expected to bound a declared wire length against the bytes actually present
 * across the iovec chain.  This fixture drives each generated
 * unmarshall_<struct>(out, iov, niov, rdma_chunk, dbuf) wrapper with a
 * hand-built iovec chain carrying a hostile length and asserts the correct
 * outcome: the decoder cleanly rejects the input (returns < 0) without reading
 * past the received bytes.
 *
 * Every iovec points at its own exactly-sized heap buffer and the iovec array
 * itself is exactly sized, so AddressSanitizer (Debug) reports any over-read.
 * Each case runs in a forked child: a child that over-reads is killed by the
 * sanitizer, a child that accepts the overlong length exits non-zero, and a
 * child that rejects it exits zero.  The parent classifies every case and
 * sets the process exit code, so one over-read does not hide the other cases.
 *
 * The wrapper selects the contig path when niov == 1 and the vector path when
 * niov > 1; every case is therefore exercised in a _single (contig) and a
 * _multi (vector) buffer variant.  A declared length near UINT32_MAX and a
 * declared length exactly one byte over the real remaining bytes are both used
 * for the length-prefixed helpers; the fixed opaque[N] helper, which has no
 * wire length, is driven with fewer than N bytes present.
 *
 * The unmarshall wrappers and the decode builtins they inline are the unit
 * under test, so this file is compiled against the generated translation unit
 * directly; the iovec container is the generator's self-contained fallback
 * (data pointer + length), the decode-length arithmetic being independent of
 * the evpl iovec refcount machinery.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "xdr_bounds_xdr.h"

/*
 * Make a sanitizer error abort (SIGABRT) rather than _exit(1): the parent then
 * tells an over-read (a signal) apart from a decoder that accepted the overlong
 * length (a non-zero exit).  Leak detection is off because each child
 * deliberately _exit()s while still holding its hand-built buffers.
 */
const char *
__asan_default_options(
    void);
const char *
__asan_default_options(void)
{
    return "abort_on_error=1:detect_leaks=0";
} /* __asan_default_options */

static int g_fail = 0;

#define CHECK(rejected, name, outcome)                                 \
        do {                                                           \
            printf("%-4s %-34s %s\n",                                  \
                   (rejected) ? "PASS" : "FAIL", (name), (outcome));   \
            if (!(rejected)) {                                         \
                g_fail = 1;                                            \
            }                                                          \
        } while (0)

typedef int (*decode_fn)(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf);

static int
dec_fixed(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf)
{
    struct xdrt_fixed out;

    memset(&out, 0, sizeof(out));
    return unmarshall_xdrt_fixed(&out, iov, niov, NULL, dbuf);
} /* dec_fixed */

static int
dec_bopaque(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf)
{
    struct xdrt_bopaque out;

    memset(&out, 0, sizeof(out));
    return unmarshall_xdrt_bopaque(&out, iov, niov, NULL, dbuf);
} /* dec_bopaque */

static int
dec_uopaque(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf)
{
    struct xdrt_uopaque out;

    memset(&out, 0, sizeof(out));
    return unmarshall_xdrt_uopaque(&out, iov, niov, NULL, dbuf);
} /* dec_uopaque */

static int
dec_string(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf)
{
    struct xdrt_string out;

    memset(&out, 0, sizeof(out));
    return unmarshall_xdrt_string(&out, iov, niov, NULL, dbuf);
} /* dec_string */

static int
dec_zc(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf)
{
    struct xdrt_zc out;

    memset(&out, 0, sizeof(out));
    return unmarshall_xdrt_zc(&out, iov, niov, NULL, dbuf);
} /* dec_zc */

/*
 * Accept-path decoders for the exact-fit cases.  Each runs its generated
 * unmarshall wrapper and, on a non-negative return, reports the length the
 * decoder recorded so the harness can assert the declared length was decoded
 * intact.  A fixed opaque[N] field carries no wire length, so its decoded
 * length is the compile-time N.
 */
typedef int (*accept_fn)(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf,
    uint32_t  *decoded_len);

static int
acc_fixed(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf,
    uint32_t  *decoded_len)
{
    struct xdrt_fixed out;
    int               rc;

    memset(&out, 0, sizeof(out));
    rc = unmarshall_xdrt_fixed(&out, iov, niov, NULL, dbuf);
    if (rc >= 0) {
        *decoded_len = XDRT_FIXED_N;
    }
    return rc;
} /* acc_fixed */

static int
acc_bopaque(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf,
    uint32_t  *decoded_len)
{
    struct xdrt_bopaque out;
    int                 rc;

    memset(&out, 0, sizeof(out));
    rc = unmarshall_xdrt_bopaque(&out, iov, niov, NULL, dbuf);
    if (rc >= 0) {
        *decoded_len = out.b.len;
    }
    return rc;
} /* acc_bopaque */

static int
acc_uopaque(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf,
    uint32_t  *decoded_len)
{
    struct xdrt_uopaque out;
    int                 rc;

    memset(&out, 0, sizeof(out));
    rc = unmarshall_xdrt_uopaque(&out, iov, niov, NULL, dbuf);
    if (rc >= 0) {
        *decoded_len = out.u.len;
    }
    return rc;
} /* acc_uopaque */

static int
acc_string(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf,
    uint32_t  *decoded_len)
{
    struct xdrt_string out;
    int                rc;

    memset(&out, 0, sizeof(out));
    rc = unmarshall_xdrt_string(&out, iov, niov, NULL, dbuf);
    if (rc >= 0) {
        *decoded_len = out.s.len;
    }
    return rc;
} /* acc_string */

static int
acc_zc(
    xdr_iovec *iov,
    int        niov,
    xdr_dbuf  *dbuf,
    uint32_t  *decoded_len)
{
    struct xdrt_zc out;
    int            rc;

    memset(&out, 0, sizeof(out));
    rc = unmarshall_xdrt_zc(&out, iov, niov, NULL, dbuf);
    if (rc >= 0) {
        *decoded_len = out.z.length;
    }
    return rc;
} /* acc_zc */

/*
 * Build the iovec chain from exactly-sized heap allocations and run the
 * decoder, never returning: the result is reported through the exit status so
 * a sanitizer abort in here is observed by the parent.
 *   exit 0 -> decoder returned < 0  (hostile length rejected, correct)
 *   exit 1 -> decoder returned >= 0  (hostile length accepted, wrong)
 */
static void
run_child(
    decode_fn      fn,
    const uint8_t *wire,
    const int     *sizes,
    int            niov)
{
    xdr_iovec *iov = calloc((size_t) niov, sizeof(*iov));
    size_t     off = 0;
    int        rc;

    for (int i = 0; i < niov; i++) {
        void *buf = malloc((size_t) sizes[i]);

        memcpy(buf, wire + off, (size_t) sizes[i]);
        xdr_iovec_set_data(&iov[i], buf);
        xdr_iovec_set_len(&iov[i], (unsigned int) sizes[i]);
        off += (size_t) sizes[i];
    }

    xdr_dbuf *dbuf = xdr_dbuf_alloc(256);

    rc = fn(iov, niov, dbuf);

    _exit(rc < 0 ? 0 : 1);
} /* run_child */

static void
run_case(
    const char    *name,
    decode_fn      fn,
    const uint8_t *wire,
    const int     *sizes,
    int            niov)
{
    pid_t pid = fork();

    if (pid == 0) {
        run_child(fn, wire, sizes, niov);
        _exit(2);
    }

    int   status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            CHECK(1, name, "returns <0 (rejected overlong length)");
        } else if (code == 1) {
            CHECK(0, name, "returns >=0 (ACCEPTED overlong length: wrapped/alias)");
        } else {
            CHECK(0, name, "child harness error");
        }
    } else if (WIFSIGNALED(status)) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "killed by signal %d (ASan over-read / abort)", WTERMSIG(status));
        CHECK(0, name, buf);
    } else {
        CHECK(0, name, "unknown child disposition");
    }
} /* run_case */

/*
 * Exact-fit accept child: a declared length whose payload (plus XDR pad) ends
 * exactly at the end of the received bytes must be ACCEPTED and must decode to
 * the declared length -- a guard that rejected exact fits, or an off-by-pad
 * bound, would turn these legitimate decodes into spurious rejections.
 *   exit 0 -> decoder returned >= 0 and decoded the expected length (correct)
 *   exit 1 -> decoder returned < 0   (rejected an exact fit, wrong)
 *   exit 3 -> decoder returned >= 0 but decoded the wrong length (wrong)
 */
static void
run_accept_child(
    accept_fn      fn,
    const uint8_t *wire,
    const int     *sizes,
    int            niov,
    uint32_t       expect_len)
{
    xdr_iovec *iov = calloc((size_t) niov, sizeof(*iov));
    size_t     off = 0;
    uint32_t   got = 0xFFFFFFFFu;
    int        rc;

    for (int i = 0; i < niov; i++) {
        void *buf = malloc((size_t) sizes[i]);

        memcpy(buf, wire + off, (size_t) sizes[i]);
        xdr_iovec_set_data(&iov[i], buf);
        xdr_iovec_set_len(&iov[i], (unsigned int) sizes[i]);
        off += (size_t) sizes[i];
    }

    xdr_dbuf *dbuf = xdr_dbuf_alloc(256);

    rc = fn(iov, niov, dbuf, &got);

    if (rc < 0) {
        _exit(1);
    }
    _exit(got == expect_len ? 0 : 3);
} /* run_accept_child */

static void
run_accept_case(
    const char    *name,
    accept_fn      fn,
    const uint8_t *wire,
    const int     *sizes,
    int            niov,
    uint32_t       expect_len)
{
    pid_t pid = fork();

    if (pid == 0) {
        run_accept_child(fn, wire, sizes, niov, expect_len);
        _exit(2);
    }

    int   status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            CHECK(1, name, "returns >=0 (accepted exact fit, length decoded)");
        } else if (code == 1) {
            CHECK(0, name, "returns <0 (REJECTED an exact fit)");
        } else if (code == 3) {
            CHECK(0, name, "accepted but decoded the wrong length");
        } else {
            CHECK(0, name, "child harness error");
        }
    } else if (WIFSIGNALED(status)) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "killed by signal %d (ASan over-read / abort)", WTERMSIG(status));
        CHECK(0, name, buf);
    } else {
        CHECK(0, name, "unknown child disposition");
    }
} /* run_accept_case */

static void
put_be32(
    uint8_t *p,
    uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
} /* put_be32 */

/*
 * Fill a wire buffer of `total` bytes.  With a length prefix the first four
 * bytes carry `len` big-endian and the rest are payload; without one the whole
 * buffer is payload (a fixed opaque[N] field).
 */
static void
mk_wire(
    uint8_t *buf,
    int      has_prefix,
    uint32_t len,
    int      total)
{
    if (has_prefix) {
        memset(buf, 0x55, (size_t) total);
        put_be32(buf, len);
    } else {
        memset(buf, 0x41, (size_t) total);
    }
} /* mk_wire */

#define NEAR_MAX 0xFFFFFFFFu

int
main(void)
{
    uint8_t wire[256];

    /*
     * Fixed opaque[64]: no wire length.  Fewer than 64 bytes are present; the
     * contig path (niov == 1) memcpy()s 64 bytes out of the short buffer, the
     * vector path (niov > 1) walks the chain.
     */
    mk_wire(wire, 0, 0, 63);
    run_case("fixed_contig_truncated_single", dec_fixed, wire,
             (int[]) { 63 }, 1);

    mk_wire(wire, 0, 0, 63);
    run_case("fixed_contig_truncated_multi", dec_fixed, wire,
             (int[]) { 32, 31 }, 2);

    mk_wire(wire, 0, 0, 8);
    run_case("fixed_vector_overlong_single", dec_fixed, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 0, 0, 8);
    run_case("fixed_vector_overlong_multi", dec_fixed, wire,
             (int[]) { 4, 2, 2 }, 3);

    /*
     * Unbounded opaque<>: 4-byte length prefix then payload.  Declared length
     * near UINT32_MAX and exactly one byte over the real remaining bytes.
     */
    mk_wire(wire, 1, NEAR_MAX, 8);
    run_case("opaque_vector_overlong_single", dec_uopaque, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, NEAR_MAX, 8);
    run_case("opaque_vector_overlong_multi", dec_uopaque, wire,
             (int[]) { 4, 4 }, 2);

    mk_wire(wire, 1, 5, 8);                 /* 4 payload bytes present, len 5 */
    run_case("opaque_vector_over1_single", dec_uopaque, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, 5, 8);
    run_case("opaque_vector_over1_multi", dec_uopaque, wire,
             (int[]) { 4, 4 }, 2);

    /* Bounded opaque<64>: a declared length past the bound must be refused. */
    mk_wire(wire, 1, 65, 8);
    run_case("opaque_bounded_overbound_single", dec_bopaque, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, 65, 8);
    run_case("opaque_bounded_overbound_multi", dec_bopaque, wire,
             (int[]) { 4, 4 }, 2);

    /* string<>: same shape as unbounded opaque. */
    mk_wire(wire, 1, NEAR_MAX, 8);
    run_case("string_vector_overlong_single", dec_string, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, NEAR_MAX, 8);
    run_case("string_vector_overlong_multi", dec_string, wire,
             (int[]) { 4, 4 }, 2);

    mk_wire(wire, 1, 5, 8);
    run_case("string_vector_over1_single", dec_string, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, 5, 8);
    run_case("string_vector_over1_multi", dec_string, wire,
             (int[]) { 4, 4 }, 2);

    /*
     * zcopaque<>: the zero-copy decoder aliases receive-buffer iovecs rather
     * than copying.  Declared length near UINT32_MAX and one byte over.
     */
    mk_wire(wire, 1, NEAR_MAX, 8);
    run_case("zerocopy_vector_overlong_single", dec_zc, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, NEAR_MAX, 12);
    run_case("zerocopy_vector_overlong_multi", dec_zc, wire,
             (int[]) { 4, 8 }, 2);

    mk_wire(wire, 1, 5, 8);
    run_case("zerocopy_vector_over1_single", dec_zc, wire,
             (int[]) { 8 }, 1);

    mk_wire(wire, 1, 5, 8);
    run_case("zerocopy_vector_over1_multi", dec_zc, wire,
             (int[]) { 4, 4 }, 2);

    /*
     * Exact-fit ACCEPT cases: a declared length whose payload (and pad) ends
     * exactly at the end of the received bytes is legitimate and must be
     * accepted, decoding to the declared length.  Each is driven single (contig)
     * and multi (vector) buffer.
     */

    /* fixed opaque[64]: exactly N bytes present, no wire length. */
    mk_wire(wire, 0, 0, XDRT_FIXED_N);
    run_accept_case("fixed_exact_single", acc_fixed, wire,
                    (int[]) { XDRT_FIXED_N }, 1, XDRT_FIXED_N);

    mk_wire(wire, 0, 0, XDRT_FIXED_N);
    run_accept_case("fixed_exact_multi", acc_fixed, wire,
                    (int[]) { 32, 32 }, 2, XDRT_FIXED_N);

    /*
     * opaque<>: length exactly equal to the remaining bytes INCLUDING pad.
     * len 5 -> 5 payload + 3 pad = 8 bytes after the 4-byte length prefix.
     */
    mk_wire(wire, 1, 5, 12);
    run_accept_case("opaque_exact_incl_pad_single", acc_uopaque, wire,
                    (int[]) { 12 }, 1, 5);

    mk_wire(wire, 1, 5, 12);
    run_accept_case("opaque_exact_incl_pad_multi", acc_uopaque, wire,
                    (int[]) { 4, 8 }, 2, 5);

    /*
     * opaque<>: a pad-0 payload ending exactly at the chain end (len 4 -> pad 0),
     * exercising the zero-byte skip after the payload consumes the chain exactly.
     */
    mk_wire(wire, 1, 4, 8);
    run_accept_case("opaque_pad0_exact_end_single", acc_uopaque, wire,
                    (int[]) { 8 }, 1, 4);

    mk_wire(wire, 1, 4, 8);
    run_accept_case("opaque_pad0_exact_end_multi", acc_uopaque, wire,
                    (int[]) { 4, 4 }, 2, 4);

    /* opaque<>: a zero-length payload (len 0 -> pad 0), nothing after the prefix. */
    mk_wire(wire, 1, 0, 4);
    run_accept_case("opaque_zero_length_single", acc_uopaque, wire,
                    (int[]) { 4 }, 1, 0);

    /*
     * Zero-length payload over a vector chain: the 4-byte length prefix straddles
     * the iovec seam and the chain carries further bytes, so the zero-length
     * field is decoded with the read cursor still inside the chain.
     */
    mk_wire(wire, 1, 0, 8);
    run_accept_case("opaque_zero_length_multi", acc_uopaque, wire,
                    (int[]) { 2, 6 }, 2, 0);

    /* string<>: length exactly equal to remaining bytes including pad (len 5). */
    mk_wire(wire, 1, 5, 12);
    run_accept_case("string_exact_incl_pad_single", acc_string, wire,
                    (int[]) { 12 }, 1, 5);

    mk_wire(wire, 1, 5, 12);
    run_accept_case("string_exact_incl_pad_multi", acc_string, wire,
                    (int[]) { 4, 8 }, 2, 5);

    /*
     * bounded opaque<64>: a declared length exactly AT the bound with exactly
     * that many bytes present must be accepted (the guard rejects past the bound,
     * not at it).  len 64 -> pad 0, 64 payload bytes after the prefix.
     */
    mk_wire(wire, 1, XDRT_OPAQUE_BOUND, 4 + XDRT_OPAQUE_BOUND);
    run_accept_case("bounded_exact_at_bound_single", acc_bopaque, wire,
                    (int[]) { 4 + XDRT_OPAQUE_BOUND }, 1, XDRT_OPAQUE_BOUND);

    mk_wire(wire, 1, XDRT_OPAQUE_BOUND, 4 + XDRT_OPAQUE_BOUND);
    run_accept_case("bounded_exact_at_bound_multi", acc_bopaque, wire,
                    (int[]) { 4, XDRT_OPAQUE_BOUND }, 2, XDRT_OPAQUE_BOUND);

    /* zcopaque<>: a pad-0 payload ending exactly at the chain end (len 4). */
    mk_wire(wire, 1, 4, 8);
    run_accept_case("zerocopy_pad0_exact_end_single", acc_zc, wire,
                    (int[]) { 8 }, 1, 4);

    mk_wire(wire, 1, 4, 8);
    run_accept_case("zerocopy_pad0_exact_end_multi", acc_zc, wire,
                    (int[]) { 4, 4 }, 2, 4);

    if (g_fail) {
        printf("xdr_bounds_test: FAILURES present "
               "(expected while the decode-length bounds are unfixed)\n");
    } else {
        printf("xdr_bounds_test: all cases correct "
               "(overlong lengths rejected, exact fits accepted)\n");
    }

    return g_fail;
} /* main */
