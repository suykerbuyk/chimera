// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#ifdef _WIN32
#include <evpl/evpl_platform.h>
#include <limits.h>
#else // ifdef _WIN32
#include <iconv.h>
#endif // ifdef _WIN32
#include <stdint.h>
#include <string.h>

#include "smb_common/smb2.h"

struct chimera_smb_iconv_ctx {
#ifdef _WIN32
    int     unused;
#else // ifdef _WIN32
    iconv_t utf16le_to_utf8;
    iconv_t utf8_to_utf16le;
#endif // ifdef _WIN32
};

static void
chimera_smb_iconv_init(struct chimera_smb_iconv_ctx *ctx)
{
#ifdef _WIN32
    ctx->unused = 0;
#else // ifdef _WIN32
    ctx->utf16le_to_utf8 = iconv_open("UTF-8", "UTF-16LE");
    ctx->utf8_to_utf16le = iconv_open("UTF-16LE", "UTF-8");
#endif // ifdef _WIN32
} /* chimera_smb_iconv_init */

static void
chimera_smb_iconv_destroy(struct chimera_smb_iconv_ctx *ctx)
{
#ifdef _WIN32
    (void) ctx;
#else // ifdef _WIN32
    iconv_close(ctx->utf16le_to_utf8);
    iconv_close(ctx->utf8_to_utf16le);
#endif // ifdef _WIN32
} /* chimera_smb_iconv_destroy */

static inline int
chimera_smb_slash_forward_to_back(
    char  *path,
    size_t len)
{
    char *p;

    for (p = path; p < path + len; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    return 0;
} /* chimera_smb_slash_forward_to_back */

static inline int
chimera_smb_slash_back_to_forward(
    char  *path,
    size_t len)
{
    char *p;

    for (p = path; p < path + len; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return 0;
} /* chimera_smb_slash_back_to_forward */

static inline int
chimera_smb_utf16le_to_utf8(
    struct chimera_smb_iconv_ctx *ctx,
    const uint16_t               *src,
    size_t                        srclen,
    char                         *dst,
    size_t                        dstmaxlen)
{
#ifdef _WIN32
    int count;
    (void) ctx;
    if ((srclen & 1) || srclen / 2 > INT_MAX || !dstmaxlen || dstmaxlen > INT_MAX) {
        return -1;
    }
    if (!srclen) {
        dst[0] = 0;
        return 0;
    }
    if (dstmaxlen <= 1) {
        return -1;
    }
    count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (const WCHAR *) src,
                                (int) (srclen / 2), dst, (int) dstmaxlen - 1, NULL, NULL);
    if (!count) {
        return -1;
    }
    dst[count] = 0;
    return count;
#else // ifdef _WIN32
    int    rc;
    size_t srcleft = srclen, dstleft;
    char  *dstleftp = dst;
    char  *srcleftp = (char *) src;

    if (!dstmaxlen) {
        return -1;
    }
    dstleft = dstmaxlen - 1;
    rc      = iconv(ctx->utf16le_to_utf8, &srcleftp, &srcleft, &dstleftp, &dstleft);

    if (rc != 0) {
        return -1;
    }

    *dstleftp = '\0';

    return dstleftp - dst;
#endif // ifdef _WIN32
} /* smb_utf16le_to_utf8 */

static inline int
chimera_smb_utf8_to_utf16le(
    struct chimera_smb_iconv_ctx *ctx,
    const char                   *src,
    size_t                        srclen,
    uint16_t                     *dst,
    size_t                        dstmaxlen)
{
#ifdef _WIN32
    int count;
    (void) ctx;
    if (srclen > INT_MAX || dstmaxlen > INT_MAX) {
        return -1;
    }
    if (!srclen) {
        return 0;
    }
    if (dstmaxlen < 2) {
        return -1;
    }
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, (int) srclen,
                                (WCHAR *) dst, (int) (dstmaxlen / 2));
    return count ? count * 2 : -1;
#else // ifdef _WIN32
    int    rc;
    size_t dstlen, srcleft = srclen, dstleft = dstmaxlen;
    char  *dstleftp = (char *) dst;
    char  *srcleftp = (char *) src;

    rc = iconv(ctx->utf8_to_utf16le, &srcleftp, &srcleft, &dstleftp, &dstleft);

    if (rc != 0) {
        return -1;
    }

    dstlen = dstmaxlen - dstleft;

    return dstlen;
#endif // ifdef _WIN32
} /* chimera_smb_utf8_to_utf16le */

/* ---- SMB path-component name validation ---------------------------------
 *
 * A component the client names on the wire ('\' is the only separator) must not
 * carry a path separator or an embedded NUL, and a create-type or reconnect
 * name must not carry a ".." component.  These primitives are pure so the
 * hardening test can drive them directly; CREATE applies them inline (its
 * pre-existing ".." block keeps the distinct PATH_SYNTAX_BAD / INVALID_PARAMETER
 * statuses MS-SMB2 3.3.5.9 pins), while reconnect and SET_INFO rename/link use
 * chimera_smb_name_check for the single OBJECT_NAME_INVALID verdict. */

/* Non-zero if s[0..len) contains a NUL byte (an embedded U+0000). */
static inline int
chimera_smb_name_has_nul(
    const char *s,
    size_t      len)
{
    return memchr(s, '\0', len) != NULL;
} /* chimera_smb_name_has_nul */

/* Non-zero if s[0..len) contains '/': not a wire separator and not allowed in a
 * component (MS-FSCC 2.1.5.2). */
static inline int
chimera_smb_name_has_slash(
    const char *s,
    size_t      len)
{
    return memchr(s, '/', len) != NULL;
} /* chimera_smb_name_has_slash */

/* Non-zero if any '\'- or '/'-separated component of s[0..len) is exactly
 * "..".  A "." component is allowed (WPTS CreateAndClose*WithDotDir). */
static inline int
chimera_smb_wire_path_has_dotdot(
    const char *s,
    size_t      len)
{
    size_t i = 0;

    while (i < len) {
        size_t start = i;

        while (i < len && s[i] != '\\' && s[i] != '/') {
            i++;
        }
        if (i - start == 2 && s[start] == '.' && s[start + 1] == '.') {
            return 1;
        }
        if (i < len) {
            i++;  /* skip the separator */
        }
    }
    return 0;
} /* chimera_smb_wire_path_has_dotdot */

/* UTF-16 byte length of a UTF-8 run: two bytes per code unit, four for a
 * supplementary (4-byte UTF-8) code point.  Used for a SymbolicLinkErrorResponse
 * UnparsedPathLength, which MS-SMB2 2.2.2.2.1 counts in UTF-16 bytes, not UTF-8
 * bytes -- the two differ for any non-ASCII component. */
static inline uint16_t
chimera_smb_utf8_utf16_bytes(
    const char *s,
    size_t      len)
{
    size_t   i     = 0;
    uint16_t units = 0;

    while (i < len) {
        unsigned char c = (unsigned char) s[i];

        if (c < 0x80) {
            i     += 1;
            units += 2;   /* one UTF-16 code unit */
        } else if (c < 0xE0) {
            i     += 2;
            units += 2;
        } else if (c < 0xF0) {
            i     += 3;
            units += 2;
        } else {
            i     += 4;
            units += 4;   /* surrogate pair: two code units */
        }
    }
    return units;
} /* chimera_smb_utf8_utf16_bytes */

/* The single name verdict for a reconnect or a SET_INFO rename/link target:
 * SMB2_STATUS_SUCCESS, or the OBJECT_NAME_INVALID to answer.  `len` is the whole
 * UTF-8 name; `base_len` is the portion before any stream ':' (equal to `len`
 * when there is no stream).  A "/" is judged only over the base so a stream
 * suffix is not misread; a NUL and a ".." component are judged over the whole
 * name; a component longer than SMB_FILENAME_MAX bytes is rejected. */
static inline uint32_t
chimera_smb_name_check(
    const char *s,
    size_t      len,
    size_t      base_len)
{
    size_t i = 0, comp = 0;

    if (chimera_smb_name_has_nul(s, len) ||
        chimera_smb_name_has_slash(s, base_len) ||
        chimera_smb_wire_path_has_dotdot(s, len)) {
        return SMB2_STATUS_OBJECT_NAME_INVALID;
    }

    for (i = 0; i < len; i++) {
        if (s[i] == '\\' || s[i] == '/' || s[i] == ':') {
            comp = 0;
        } else if (++comp > SMB_FILENAME_MAX) {
            return SMB2_STATUS_OBJECT_NAME_INVALID;
        }
    }
    return SMB2_STATUS_SUCCESS;
} /* chimera_smb_name_check */
