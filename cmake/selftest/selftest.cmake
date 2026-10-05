# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# selftest.cmake -- P0 self-test (N1) for the patches/ overlay mechanism.
#
# It proves, every configure, that the mechanism's two safety nets actually
# catch a no-op apply:
#
#   POSITIVE        apply the real fixture patch, with isolation, to an in-tree
#                   scratch staging under ${CMAKE_BINARY_DIR}/patch-selftest/
#                   (an in-tree build dir is itself the condition that triggered
#                   the original silent-skip bug) and assert the sentinel landed
#                   and the file content changed.
#   NEGATIVE/skip   a child `cmake -P apply_one.cmake` with isolation DISABLED,
#                   run from a subdirectory of a throwaway git repo, drives a
#                   GENUINE rc=0 "Skipped" no-op; assert the child FAILS and that
#                   it failed via the Skipped detector.
#   NEGATIVE/verify a child `cmake -P apply_one.cmake` MODE=VERIFY against an
#                   UNMODIFIED staging tree; assert the child FAILS and that it
#                   failed via the sha256/sentinel verifier.
#   N4/refresh      sync a staging tree to an overlay, change one file's content
#                   (same file list), re-sync, and assert the overlay refreshed.
#
# The two negatives run as separate children so each detector is shown to fire
# INDEPENDENTLY.  Runnable from the normal configure (ApplyPatches calls
# chimera_patch_selftest) and standalone (`cmake -P cmake/selftest/selftest.cmake`).

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../PatchApply.cmake")

set(_CHIMERA_SELFTEST_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(chimera_patch_selftest _work)
    set(_fix "${_CHIMERA_SELFTEST_DIR}/fixture")
    set(_patch "${_fix}/0001-add-sentinel.patch")
    set(_sentinel "CHIMERA_PATCH_SELFTEST_SENTINEL")
    set(_apply_one "${_CHIMERA_SELFTEST_DIR}/apply_one.cmake")

    if(NOT GIT_EXECUTABLE)
        find_program(GIT_EXECUTABLE git)
    endif()
    if(NOT GIT_EXECUTABLE)
        message(FATAL_ERROR "patch overlay self-test: git not found")
    endif()

    file(REMOVE_RECURSE "${_work}")

    # --- POSITIVE ---------------------------------------------------------
    set(_pos "${_work}/positive")
    set(_pos_staging "${_pos}/libevpl")
    file(MAKE_DIRECTORY "${_pos_staging}/ext/xdrzcc/src")
    file(COPY "${_fix}/hello.c" DESTINATION "${_pos_staging}/ext/xdrzcc/src")
    set(_pos_pre "${_pos}/pre")
    file(MAKE_DIRECTORY "${_pos_pre}/src")
    file(COPY "${_pos_staging}/ext/xdrzcc/src/hello.c" DESTINATION "${_pos_pre}/src")
    # Isolated apply must succeed even though _pos may be inside the superproject
    # work tree (the in-tree bug condition).  A FATAL here fails configure.
    chimera_patch_apply_one(
        GIT "${GIT_EXECUTABLE}" WORKDIR "${_pos_staging}" STAGING_PARENT "${_pos}"
        DIRECTORY "ext/xdrzcc" PATCH "${_patch}" ISOLATE ON LABEL "selftest/positive")
    chimera_patch_verify_one(
        STAGING "${_pos_staging}" REPO_ROOT "ext/xdrzcc" PATCH "${_patch}"
        PRE_DIR "${_pos_pre}" SENTINEL "${_sentinel}" LABEL "selftest/positive")

    # --- NEGATIVE / skip detector ----------------------------------------
    # Throwaway repo so the "inside a work tree" condition holds even for an
    # out-of-tree build dir.  git init alone (no commit) is enough to make git
    # bind the subdirectory and silently skip a git-style patch.
    set(_neg "${_work}/neg-skip")
    file(MAKE_DIRECTORY "${_neg}/sub/libevpl/ext/xdrzcc/src")
    execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${_neg}"
        RESULT_VARIABLE _ir OUTPUT_QUIET ERROR_QUIET)
    if(NOT _ir EQUAL 0)
        message(FATAL_ERROR "patch overlay self-test: could not init throwaway repo")
    endif()
    file(COPY "${_fix}/hello.c" DESTINATION "${_neg}/sub/libevpl/ext/xdrzcc/src")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DMODE=APPLY"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DWORKDIR=${_neg}/sub/libevpl"
            "-DSTAGING_PARENT=${_neg}/sub"
            "-DREPO_ROOT=ext/xdrzcc"
            "-DPATCH=${_patch}"
            "-DISOLATE=OFF"
            -P "${_apply_one}"
        RESULT_VARIABLE _nrc OUTPUT_VARIABLE _nout ERROR_VARIABLE _nerr)
    if(_nrc EQUAL 0)
        message(FATAL_ERROR
            "patch overlay self-test: NEGATIVE/skip did not fire -- the child "
            "apply succeeded although the patch was skipped")
    endif()
    if(NOT "${_nout}${_nerr}" MATCHES "[Ss]kipped")
        message(FATAL_ERROR
            "patch overlay self-test: NEGATIVE/skip failed for the wrong reason:\n"
            "${_nout}${_nerr}")
    endif()

    # --- NEGATIVE / sha+sentinel verifier --------------------------------
    # Unmodified staging: pre == post, so the verifier must fire on its own,
    # with no apply involved at all (independent of the skip detector).
    set(_negv "${_work}/neg-verify")
    file(MAKE_DIRECTORY "${_negv}/libevpl/ext/xdrzcc/src")
    file(COPY "${_fix}/hello.c" DESTINATION "${_negv}/libevpl/ext/xdrzcc/src")
    file(MAKE_DIRECTORY "${_negv}/pre/src")
    file(COPY "${_negv}/libevpl/ext/xdrzcc/src/hello.c" DESTINATION "${_negv}/pre/src")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DMODE=VERIFY"
            "-DSTAGING=${_negv}/libevpl"
            "-DREPO_ROOT=ext/xdrzcc"
            "-DPATCH=${_patch}"
            "-DPRE_DIR=${_negv}/pre"
            "-DSENTINEL=${_sentinel}"
            -P "${_apply_one}"
        RESULT_VARIABLE _vrc OUTPUT_VARIABLE _vout ERROR_VARIABLE _verr)
    if(_vrc EQUAL 0)
        message(FATAL_ERROR
            "patch overlay self-test: NEGATIVE/verify did not fire -- the "
            "verifier passed an unmodified staging tree")
    endif()
    if(NOT "${_vout}${_verr}" MATCHES "sha256 unchanged|sentinel")
        message(FATAL_ERROR
            "patch overlay self-test: NEGATIVE/verify failed for the wrong reason:\n"
            "${_vout}${_verr}")
    endif()
    # Independence: the verifier leg must NOT be a disguised skip.
    if("${_vout}${_verr}" MATCHES "[Ss]kipped")
        message(FATAL_ERROR
            "patch overlay self-test: NEGATIVE/verify leg reported a skip; the "
            "two detectors are not firing independently")
    endif()

    # --- N4 / overlay refresh on content change --------------------------
    set(_n4s "${_work}/n4/staging/libevpl")
    set(_n4o "${_work}/n4/overlay/libevpl")
    file(MAKE_DIRECTORY "${_n4s}/ext/xdrzcc/src")
    file(WRITE "${_n4s}/ext/xdrzcc/src/hello.c" "CHIMERA_N4_VERSION_A\n")
    chimera_overlay_sync("${_n4s}" "${_n4o}")
    if(NOT EXISTS "${_n4o}/ext/xdrzcc/src/hello.c")
        message(FATAL_ERROR "patch overlay self-test: N4 initial sync produced no overlay file")
    endif()
    # Content-only change, same file list.
    file(WRITE "${_n4s}/ext/xdrzcc/src/hello.c" "CHIMERA_N4_VERSION_B\n")
    chimera_overlay_sync("${_n4s}" "${_n4o}")
    file(READ "${_n4o}/ext/xdrzcc/src/hello.c" _n4after)
    if(NOT _n4after MATCHES "CHIMERA_N4_VERSION_B")
        message(FATAL_ERROR
            "patch overlay self-test: N4 refresh failed -- overlay did not pick "
            "up the content-only change")
    endif()

    # --- N4 / overlay refresh freshens the mtime --------------------------
    # Content equality alone is not enough: file(COPY) carries the source mtime,
    # so a reverted patch whose staged content predates the built objects could
    # refresh the overlay CONTENT while leaving its mtime stale, and the build
    # would skip the rebuild.  Drive a content change whose source file carries
    # an OLD mtime, stamp a reference time AFTER that, re-sync, and assert the
    # refreshed overlay file is NEWER than the stamp -- i.e. the sync restamped
    # it to the copy time rather than inheriting the stale source mtime.
    set(_n4c "${_n4s}/ext/xdrzcc/src/hello.c")
    file(WRITE "${_n4c}" "CHIMERA_N4_VERSION_C\n")
    # Backdate the staged source well into the past so, absent the mtime
    # refresh, the overlay would inherit a mtime older than the reference stamp
    # regardless of filesystem timestamp resolution.  POSIX-only; on Windows the
    # sequential ordering below still separates the timestamps.
    if(NOT WIN32)
        execute_process(COMMAND touch -d "@1000000000" "${_n4c}"
            RESULT_VARIABLE _bd ERROR_QUIET OUTPUT_QUIET)
    endif()
    set(_n4stamp "${_work}/n4/stamp")
    file(TOUCH "${_n4stamp}")
    chimera_overlay_sync("${_n4s}" "${_n4o}")
    set(_n4ofile "${_n4o}/ext/xdrzcc/src/hello.c")
    file(READ "${_n4ofile}" _n4c_after)
    if(NOT _n4c_after MATCHES "CHIMERA_N4_VERSION_C")
        message(FATAL_ERROR
            "patch overlay self-test: N4 freshness refresh did not pick up the "
            "content change")
    endif()
    if(NOT "${_n4ofile}" IS_NEWER_THAN "${_n4stamp}")
        message(FATAL_ERROR
            "patch overlay self-test: N4 freshness failed -- the refreshed "
            "overlay file is not newer than the pre-sync stamp, so a reverted "
            "patch carrying an old source mtime would not re-trigger a rebuild")
    endif()

    message(STATUS "patch overlay self-test: PASS "
        "(positive + skip-detector + verifier + N4 refresh + N4 freshness)")
endfunction()

# Standalone entry: `cmake -P cmake/selftest/selftest.cmake [-DSELFTEST_WORKDIR=...]`
if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT DEFINED SELFTEST_WORKDIR)
        set(SELFTEST_WORKDIR "${CMAKE_CURRENT_LIST_DIR}/../../build/patch-selftest-standalone")
    endif()
    chimera_patch_selftest("${SELFTEST_WORKDIR}")
endif()
