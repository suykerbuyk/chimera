/*
 * SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/*
 * One field per decode helper so every generated unmarshall path is reachable
 * from a hand-built iovec chain:
 *
 *   xdrt_fixed    fixed opaque[N]   -> contig memcpy / vector extract
 *   xdrt_bopaque  bounded opaque<N> -> __unmarshall_opaque_{vector,contig}
 *   xdrt_uopaque  unbounded opaque<>-> __unmarshall_opaque_{vector,contig}
 *   xdrt_string   string<>          -> __unmarshall_xdr_string_{vector,contig}
 *   xdrt_zc       zcopaque<>        -> __unmarshall_opaque_zerocopy_{vector,contig}
 *
 * Each struct carries exactly one member so its generated
 * unmarshall_<struct>(out, iov, niov, rdma_chunk, dbuf) wrapper drives that one
 * decoder: it picks the contig path when niov == 1 and the vector path when
 * niov > 1.
 */

const XDRT_FIXED_N = 64;
const XDRT_OPAQUE_BOUND = 64;

struct xdrt_fixed {
    opaque f[XDRT_FIXED_N];
};

struct xdrt_bopaque {
    opaque b<XDRT_OPAQUE_BOUND>;
};

struct xdrt_uopaque {
    opaque u<>;
};

struct xdrt_string {
    string s<>;
};

struct xdrt_zc {
    zcopaque z<>;
};
