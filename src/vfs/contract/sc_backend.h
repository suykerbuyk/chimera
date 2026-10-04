// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage contract: the backend descriptor.
 *
 * A backend fills one struct chimera_sc_backend.  The consumer (the
 * namespace module, or the conformance harness) checks abi_version, calls
 * init, set_upcalls and, on each of its evpl threads, thread_init; then it
 * dispatches ATTACH before anything else.
 *
 * Threading:
 *   - dispatch() is called on a Chimera evpl thread and never blocks.  There
 *     is no "blocking backend" mode: a slow operation (media, a peer, a
 *     recall) completes later (D6, RCL-3).
 *   - request->complete(request) runs on the dispatching thread: the thread
 *     whose thread_init returned the thread_private passed to dispatch.  A
 *     backend that finishes work elsewhere marshals the completion back
 *     through the struct evpl * it received in thread_init.
 *   - Many requests may be in flight per thread, and the caller may dispatch
 *     a new request from inside complete().
 *   - watch_event runs on a thread that ran thread_init and holds a live
 *     watch; its rec is valid only until the upcall returns (S11).
 *     deleted != 0: the record was deleted; rec->key names it and rec->value
 *     is empty.  overflow != 0: events under the watch's prefix were lost;
 *     rec is NULL, and the consumer must rescan the prefix (NS_SCAN) to
 *     resynchronise.  The watch stays live after an overflow.
 *
 * Deployment.  PROVISIONAL (S10): this is the in-process binding of the
 * contract, one backend instance per Chimera node.  How backend instances on
 * different nodes coordinate (fence epochs, shared counters, the shared hot
 * tier) is behind this interface; a wire protocol is out of scope.
 */

#include <stdint.h>
#include "sc_request.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct evpl;
struct prometheus_metrics;

struct chimera_sc_upcalls {
    void (*watch_event)(
        uint64_t                        watch_id,
        const struct chimera_sc_record *rec,
        int                             deleted,
        int                             overflow,
        void                           *up_private);
};

struct chimera_sc_backend {
    /* Required.  CHIMERA_SC_ABI_VERSION; a mismatch is refused. */
    uint32_t    abi_version;
    uint32_t    reserved;   /* must be zero */

    /* Required.  Short name of the backend implementation. */
    const char *name;

    /* Required.  Global state from the backend's JSON configuration. */
    void      * (*init)(
        const char                *cfgdata,
        struct prometheus_metrics *metrics);

    void        (*destroy)(
        void *private_data);

    /* Required.  Per-thread state; evpl is the thread's event loop, used to
     * marshal completions back to it. */
    void      * (*thread_init)(
        struct evpl *evpl,
        void        *private_data);

    void        (*thread_destroy)(
        void *thread_private);

    /* Required.  Never blocks; completes through request->complete(). */
    void        (*dispatch)(
        struct chimera_sc_request *request,
        void                      *thread_private);

    /* Required.  Registers the consumer's upcalls before the first ATTACH. */
    void        (*set_upcalls)(
        void                            *private_data,
        const struct chimera_sc_upcalls *upcalls,
        void                            *up_private);
};

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
