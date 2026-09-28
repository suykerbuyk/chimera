#!/bin/bash -e
# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
#
# Prove that the caller-side chimera_vfs headers shipped in
# quantum-chimera-tests are a complete, self-contained include closure:
# compile a standalone translation unit that includes vfs/vfs_procs.h against
# nothing but the staged install tree and the system headers.
#
# The install list in qtm_build/CMakeLists.txt is maintained by hand, so an
# upstream header that grows a new #include drops out of it silently; the
# package would then ship an include directory that cannot be compiled
# against, and the breakage would land on the consumer.  This check is what
# turns that into a build failure here.  It also pins the documented include
# roots -- if this command line has to change, so must
# qtm_build/tests-include.README, which is what downstream follows.
#
# Usage: verify_test_includes.sh <staged-install-prefix>
#
# Run from the package build tasks in qb.yaml straight after `ninja install`,
# with ${DESTDIR}/opt/quantum/chimera as the argument.  Deliberately not a
# ctest case: what is under test is the *installed* layout, which does not
# exist until install time.

PREFIX=${1:?usage: $0 <staged-install-prefix>}

# The Ubuntu builders provide cc; Rocky's gcc package does too, but fall
# through the alternatives rather than failing with "command not found" if a
# future builder image drops the symlink.
if [ -z "${CC:-}" ]; then
    for candidate in cc gcc clang; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            CC=${candidate}
            break
        fi
    done
fi
if [ -z "${CC:-}" ]; then
    echo "ERROR: no C compiler found (tried \$CC, cc, gcc, clang)" >&2
    exit 1
fi

if [ ! -d "${PREFIX}" ]; then
    echo "ERROR: staged prefix ${PREFIX} does not exist" >&2
    exit 1
fi

# Everything qtm_build/CMakeLists.txt is expected to have installed.  Checked
# by name first so a missing file is reported as such rather than as a
# compiler error three includes deep.
EXPECTED="
tests/include/README
tests/include/vfs/vfs.h
tests/include/vfs/vfs_procs.h
tests/include/vfs/vfs_dump.h
tests/include/vfs/vfs_pnfs.h
tests/include/vfs/vfs_clock.h
tests/include/common/logging.h
tests/include/common/tcp_flavor.h
tests/include/common/thread.h
tests/include/common/platform.h
tests/include/common/atomic.h
tests/include/common/compiler.h
"

missing=0
for f in ${EXPECTED}; do
    if [ ! -f "${PREFIX}/${f}" ]; then
        echo "ERROR: not installed: ${PREFIX}/${f}" >&2
        missing=1
    fi
done
[ "${missing}" -eq 0 ] || exit 1

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# A caller names vfs/vfs_procs.h and nothing else; every other header in the
# closure has to arrive through it.  Taking the address of the entry points a
# downstream fixture actually drives makes the check fail on a header that
# still parses but no longer declares them -- without restating a single
# signature here, which would be the same vendoring this whole exercise is
# meant to retire.
cat > "${WORK}/tu.c" <<'TU'
#include "vfs/vfs_procs.h"

void *const qtm_vfs_caller_api[] = {
    (void *) chimera_vfs_init,
    (void *) chimera_vfs_destroy,
    (void *) chimera_vfs_register,
    (void *) chimera_vfs_thread_init,
    (void *) chimera_vfs_thread_destroy,
    (void *) chimera_vfs_get_root_fh,
    (void *) chimera_vfs_mount,
    (void *) chimera_vfs_umount,
    (void *) chimera_vfs_lookup,
    (void *) chimera_vfs_getattr,
    (void *) chimera_vfs_open_at,
    (void *) chimera_vfs_read,
    (void *) chimera_vfs_write,
    (void *) chimera_vfs_close,
    (void *) chimera_vfs_readdir,
    (void *) chimera_vfs_get_xattr,
    (void *) chimera_vfs_set_xattr,
    (void *) chimera_vfs_list_xattrs,
    (void *) chimera_vfs_remove_xattr,
};
TU

# The include roots documented in tests-include.README.  The chimera source
# tree is deliberately absent from the list: a consumer has only the package.
"${CC}" -std=gnu11 -fsyntax-only \
    -I"${PREFIX}/tests/include" \
    -I"${PREFIX}/include/chimera" \
    -I"${PREFIX}/include/chimera/vfs" \
    -I"${PREFIX}/include" \
    "${WORK}/tu.c"

echo "Caller-side VFS test headers verified against ${PREFIX}"
