// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#include <string.h>
#include "nfs_internal.h"
#include "nfs4_open_state.h"
#include "nfs4_pnfs.h"
#include "vfs/sdk/vfs_error.h"

/* Completion of the SEQUENCE + PUTFH + COMMIT compound.  Mirrors
 * chimera_nfs4_write_callback's op-status checks.  On OP_COMMIT OK it carries
 * the remote server's writeverf up into request->commit.r_verifier so a proxied
 * v4 WRITE and COMMIT publish the same remote token (D5); a change in that token
 * across a remote restart reaches the upper NFS reply and the client resends. */
static void
chimera_nfs4_commit_callback(
    struct evpl                 *evpl,
    const struct evpl_rpc2_verf *verf,
    struct COMPOUND4res         *res,
    int                          status,
    void                        *private_data)
{
    struct chimera_vfs_request *request = private_data;
    struct COMMIT4res          *commit_res;

    if (unlikely(status)) {
        request->status = CHIMERA_VFS_EFAULT;
        request->complete(request);
        return;
    }

    /* Compound-level status BEFORE touching any resarray slot (mirror the write
     * path, nfs4_write.c).  The XDR decoder leaves a union arm unread for a
     * non-matching op and the dbuf is not memset, so a zeroed/recycled slot
     * would otherwise read status==0 (false OK) and publish verifier 0 with the
     * valid bit set -- core would then skip the boot-stamp fallback and ship a
     * false durability token (a peer can force it with OP_ILLEGAL in slot 2).
     * On any compound error we do NOT set r_verifier_valid. */
    if (res == NULL || res->status != NFS4_OK) {
        request->status = res ? chimera_nfs4_status_to_errno(res->status)
            : CHIMERA_VFS_EIO;
        request->complete(request);
        return;
    }

    /* SEQUENCE + PUTFH + COMMIT */
    if (res->num_resarray < 1 || res->resarray[0].opsequence.sr_status != NFS4_OK) {
        request->status = CHIMERA_VFS_EIO;
        request->complete(request);
        return;
    }
    if (res->num_resarray < 2 || res->resarray[1].opputfh.status != NFS4_OK) {
        request->status = CHIMERA_VFS_EIO;
        request->complete(request);
        return;
    }
    if (res->num_resarray < 3) {
        request->status = CHIMERA_VFS_EIO;
        request->complete(request);
        return;
    }

    /* Slot 2 must actually be a decoded COMMIT before its union arm is read;
     * otherwise opcommit/writeverf is unread (stale) memory. */
    if (res->resarray[2].resop != OP_COMMIT) {
        request->status = CHIMERA_VFS_EIO;
        request->complete(request);
        return;
    }

    commit_res = &res->resarray[2].opcommit;
    if (commit_res->status != NFS4_OK) {
        request->status = chimera_nfs4_status_to_errno(commit_res->status);
        request->complete(request);
        return;
    }

    memcpy(&request->commit.r_verifier, commit_res->resok4.writeverf,
           sizeof(request->commit.r_verifier));
    request->commit.r_verifier_valid = 1;
    request->status                  = CHIMERA_VFS_OK;
    request->complete(request);
} /* chimera_nfs4_commit_callback */

void
chimera_vfs_nfs4_commit(
    struct chimera_nfs_thread  *thread,
    struct chimera_nfs_shared  *shared,
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct chimera_nfs_client_server_thread *server_thread =
        chimera_nfs_thread_get_server_thread(thread, request->fh, request->fh_len);
    struct chimera_nfs4_open_state          *open_state =
        request->commit.handle ?
        (struct chimera_nfs4_open_state *) request->commit.handle->vfs_private : NULL;
    struct chimera_nfs_client_server        *server;
    struct chimera_nfs4_client_session      *session;
    struct COMPOUND4args                     args;
    struct nfs_argop4                        argarray[3];
    struct evpl_rpc2_cred                    rpc2_cred;
    uint8_t                                 *fh;
    int                                      fhlen;

    /* If a pNFS layout collected dirty writes (sent to the data server), turn
     * this COMMIT into a LAYOUTCOMMIT so the MDS learns the new file size before
     * we acknowledge -- otherwise a re-open would race the deferred close-time
     * LAYOUTCOMMIT and read back a stale size.  LAYOUTCOMMIT carries no
     * writeverf, so that completion leaves commit.r_verifier_valid at the core
     * preset (0) and core substitutes the boot stamp.  The DS write path forces
     * UNSTABLE and does not copy the DS verf either (nfs4_pnfs.c), so the upper
     * WRITE verf for that path is also the boot stamp -- the two stay consistent
     * (do not copy the DS verf to split them). */
    if (open_state &&
        chimera_nfs4_pnfs_commit(thread, shared, request, open_state)) {
        return;
    }

    if (!server_thread) {
        request->status = CHIMERA_VFS_ESTALE;
        request->complete(request);
        return;
    }

    server  = server_thread->server;
    session = server->nfs4_session;

    if (!session) {
        request->status = CHIMERA_VFS_ESTALE;
        request->complete(request);
        return;
    }

    chimera_nfs4_map_fh(request->fh, request->fh_len, &fh, &fhlen);

    /* Build compound: SEQUENCE + PUTFH + COMMIT (no OP_COMMIT was sent before
     * this change -- a non-pNFS v4 proxy COMMIT completed OK with no remote verf,
     * so WRITE returned a remote token and COMMIT the boot stamp and the client
     * resent every unstable write). */
    memset(&args, 0, sizeof(args));
    args.tag.len      = 0;
    args.minorversion = 1;
    args.argarray     = argarray;
    args.num_argarray = 3;

    argarray[0].argop = OP_SEQUENCE;

    argarray[1].argop               = OP_PUTFH;
    argarray[1].opputfh.object.data = fh;
    argarray[1].opputfh.object.len  = fhlen;

    argarray[2].argop           = OP_COMMIT;
    argarray[2].opcommit.offset = request->commit.offset;
    argarray[2].opcommit.count  = request->commit.length;      /* 0 == to EOF */

    chimera_nfs_init_rpc2_cred(&rpc2_cred, request->cred,
                               request->thread->vfs->machine_name,
                               request->thread->vfs->machine_name_len);

    chimera_nfs4_compound_call(
        thread,
        shared,
        server_thread,
        request,
        &args,
        &rpc2_cred,
        0, 0, NULL, 0, 0,
        chimera_nfs4_commit_callback,
        request,
        chimera_vfs_nfs4_dispatch, private_data);
} /* chimera_vfs_nfs4_commit */
