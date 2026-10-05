# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# PatchApply.cmake -- shared primitives for the patches/ overlay mechanism.
#
# These functions are the single implementation of "apply one patch" and
# "verify one applied patch".  They are included both by cmake/ApplyPatches.cmake
# (which drives the real patch series over the libevpl overlay at configure
# time) and by cmake/selftest/apply_one.cmake (the child `cmake -P` the P0
# self-test asserts must FAIL for the two no-op conditions).  Keeping one copy
# means the self-test exercises the very code the build relies on.
#
# Every failure is a message(FATAL_ERROR): it aborts configure when called from
# ApplyPatches, and exits non-zero when called from a `cmake -P` child, which is
# exactly what the negative self-tests assert on.

# Read a text control file (series / series.sha) into a list, dropping blank
# lines and '#' comments.  Missing file yields an empty list.
function(chimera_patch_read_lines _file _out)
    set(_lines "")
    if(EXISTS "${_file}")
        file(STRINGS "${_file}" _raw)
        foreach(_l IN LISTS _raw)
            string(STRIP "${_l}" _s)
            if(_s STREQUAL "" OR _s MATCHES "^#")
                continue()
            endif()
            list(APPEND _lines "${_s}")
        endforeach()
    endif()
    set(${_out} "${_lines}" PARENT_SCOPE)
endfunction()

# Run `git apply` once.  The isolation is the whole point: unset any inherited
# GIT_DIR/GIT_WORK_TREE and cap discovery with GIT_CEILING_DIRECTORIES so git
# never binds to the enclosing superproject work tree.  Without it, a git-style
# patch applied from a subdirectory of a repo is silently SKIPPED (rc 0, file
# untouched) -- the bug this whole mechanism exists to detect.
#
# git apply has no --dry-run; the probe uses --check.  Returns rc, the combined
# stdout+stderr, and whether any "Skipped"/"skipped" line appeared.
function(chimera_patch_git_apply)
    cmake_parse_arguments(A ""
        "GIT;WORKDIR;STAGING_PARENT;DIRECTORY;PATCH;ISOLATE;CHECK;RC_VAR;OUT_VAR;SKIPPED_VAR"
        "" ${ARGN})

    if(A_ISOLATE)
        set(_launcher ${CMAKE_COMMAND} -E env
            --unset=GIT_DIR --unset=GIT_WORK_TREE
            "GIT_CEILING_DIRECTORIES=${A_STAGING_PARENT}"
            ${A_GIT})
    else()
        set(_launcher ${A_GIT})
    endif()

    # git apply rejects --directory=. ("invalid path './...'"), so for a series
    # whose repo-root IS the overlay root ("."), omit --directory entirely and
    # let the paths apply relative to WORKING_DIRECTORY (the staging root).  A
    # real subdirectory repo-root (e.g. ext/xdrzcc) is passed through unchanged.
    set(_args apply -p1 -v --whitespace=nowarn)
    if(NOT A_DIRECTORY STREQUAL "" AND NOT A_DIRECTORY STREQUAL ".")
        list(APPEND _args --directory=${A_DIRECTORY})
    endif()
    if(A_CHECK)
        list(APPEND _args --check)
    endif()

    execute_process(
        COMMAND ${_launcher} ${_args} ${A_PATCH}
        WORKING_DIRECTORY "${A_WORKDIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)

    set(_all "${_out}${_err}")
    set(_skipped 0)
    if(_all MATCHES "[Ss]kipped")
        set(_skipped 1)
    endif()

    set(${A_RC_VAR} "${_rc}" PARENT_SCOPE)
    set(${A_OUT_VAR} "${_all}" PARENT_SCOPE)
    set(${A_SKIPPED_VAR} "${_skipped}" PARENT_SCOPE)
endfunction()

# Probe with --check, then apply for real, both with isolation (unless ISOLATE
# is explicitly OFF -- the self-test does that to reproduce the silent skip).
# ANY "Skipped" line OR non-zero rc in either phase is a FATAL_ERROR naming the
# series + patch.
function(chimera_patch_apply_one)
    cmake_parse_arguments(A ""
        "GIT;WORKDIR;STAGING_PARENT;DIRECTORY;PATCH;ISOLATE;LABEL" "" ${ARGN})
    if(NOT DEFINED A_ISOLATE)
        set(A_ISOLATE ON)
    endif()

    chimera_patch_git_apply(
        GIT "${A_GIT}" WORKDIR "${A_WORKDIR}" STAGING_PARENT "${A_STAGING_PARENT}"
        DIRECTORY "${A_DIRECTORY}" PATCH "${A_PATCH}" ISOLATE ${A_ISOLATE} CHECK ON
        RC_VAR _rc OUT_VAR _out SKIPPED_VAR _skip)
    if(NOT _rc EQUAL 0 OR _skip)
        message(FATAL_ERROR
            "patch apply PROBE failed [${A_LABEL}] (rc=${_rc}, skipped=${_skip})\n"
            "  patch: ${A_PATCH}\n"
            "  into:  ${A_WORKDIR} (--directory=${A_DIRECTORY})\n"
            "${_out}")
    endif()

    chimera_patch_git_apply(
        GIT "${A_GIT}" WORKDIR "${A_WORKDIR}" STAGING_PARENT "${A_STAGING_PARENT}"
        DIRECTORY "${A_DIRECTORY}" PATCH "${A_PATCH}" ISOLATE ${A_ISOLATE} CHECK OFF
        RC_VAR _rc OUT_VAR _out SKIPPED_VAR _skip)
    if(NOT _rc EQUAL 0 OR _skip)
        message(FATAL_ERROR
            "patch APPLY failed [${A_LABEL}] (rc=${_rc}, skipped=${_skip})\n"
            "  patch: ${A_PATCH}\n"
            "  into:  ${A_WORKDIR} (--directory=${A_DIRECTORY})\n"
            "${_out}")
    endif()
endfunction()

# Parse a unified/git-style patch for its post-image targets.  With -p1 the
# leading a//b/ component is stripped, leaving paths relative to the owning
# repo root.  A "+++ /dev/null" target is a deletion: the "--- a/<path>" file is
# recorded as removed instead.
function(chimera_patch_targets _patch _targets_var _deleted_var)
    file(STRINGS "${_patch}" _lines)
    set(_targets "")
    set(_deleted "")
    set(_cur_old "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^--- ")
            string(REGEX REPLACE "^--- " "" _old "${_l}")
            string(REGEX REPLACE "\t.*$" "" _old "${_old}")
            set(_cur_old "${_old}")
        elseif(_l MATCHES "^\\+\\+\\+ ")
            string(REGEX REPLACE "^\\+\\+\\+ " "" _new "${_l}")
            string(REGEX REPLACE "\t.*$" "" _new "${_new}")
            if(_new STREQUAL "/dev/null")
                string(REGEX REPLACE "^a/" "" _delp "${_cur_old}")
                if(NOT _delp STREQUAL "/dev/null")
                    list(APPEND _deleted "${_delp}")
                endif()
            else()
                string(REGEX REPLACE "^b/" "" _newp "${_new}")
                list(APPEND _targets "${_newp}")
            endif()
        endif()
    endforeach()
    set(${_targets_var} "${_targets}" PARENT_SCOPE)
    set(${_deleted_var} "${_deleted}" PARENT_SCOPE)
endfunction()

# Verify one applied patch (N2).  For each post-image target, assert the staged
# file's content actually changed versus the pre-apply staged snapshot (sha256
# differs) -- NOT versus the pristine source, since those legitimately differ on
# a CRLF checkout after LF normalisation.  A deletion target asserts the file is
# absent.  The per-(patch,file) sentinel token must be found in at least one
# present target file.  A dead no-op (unchanged file) or an unlanded sentinel is
# a FATAL_ERROR.
function(chimera_patch_verify_one)
    cmake_parse_arguments(A ""
        "STAGING;REPO_ROOT;PATCH;PRE_DIR;SENTINEL;LABEL" "" ${ARGN})

    chimera_patch_targets("${A_PATCH}" _tgts _dels)

    foreach(_d IN LISTS _dels)
        if(EXISTS "${A_STAGING}/${A_REPO_ROOT}/${_d}")
            message(FATAL_ERROR
                "patch verify [${A_LABEL}]: deletion target still present: ${_d}")
        endif()
    endforeach()

    set(_present 0)
    set(_sentinel_hit FALSE)
    foreach(_t IN LISTS _tgts)
        set(_post "${A_STAGING}/${A_REPO_ROOT}/${_t}")
        set(_pre "${A_PRE_DIR}/${_t}")
        if(NOT EXISTS "${_post}")
            message(FATAL_ERROR
                "patch verify [${A_LABEL}]: target missing after apply: ${_t}")
        endif()
        math(EXPR _present "${_present} + 1")
        if(EXISTS "${_pre}")
            file(SHA256 "${_post}" _hpost)
            file(SHA256 "${_pre}" _hpre)
            if(_hpost STREQUAL _hpre)
                message(FATAL_ERROR
                    "patch verify [${A_LABEL}]: sha256 unchanged after apply "
                    "(silent no-op?): ${_t}")
            endif()
        endif()
        if(A_SENTINEL AND NOT A_SENTINEL STREQUAL "")
            file(READ "${_post}" _c)
            string(FIND "${_c}" "${A_SENTINEL}" _fi)
            if(NOT _fi EQUAL -1)
                set(_sentinel_hit TRUE)
            endif()
        endif()
    endforeach()

    if(A_SENTINEL AND NOT A_SENTINEL STREQUAL "" AND _present GREATER 0
            AND NOT _sentinel_hit)
        message(FATAL_ERROR
            "patch verify [${A_LABEL}]: sentinel '${A_SENTINEL}' not found in any "
            "target file")
    endif()
endfunction()

# Sync a freshly-built staging tree onto the consumed overlay with copy-if-
# different semantics (unchanged files keep their mtime, so the build does not
# rebuild libevpl on every reconfigure), preserving file modes, and pruning any
# overlay entry that is no longer in staging.
function(chimera_overlay_sync _staging _overlay)
    file(GLOB_RECURSE _staged LIST_DIRECTORIES false RELATIVE "${_staging}" "${_staging}/*")
    set(_keep "")
    foreach(_rel IN LISTS _staged)
        list(APPEND _keep "${_rel}")
        set(_s "${_staging}/${_rel}")
        set(_o "${_overlay}/${_rel}")
        set(_do_copy TRUE)
        if(EXISTS "${_o}")
            file(SHA256 "${_s}" _hs)
            file(SHA256 "${_o}" _ho)
            if(_hs STREQUAL _ho)
                set(_do_copy FALSE)
            endif()
        endif()
        if(_do_copy)
            # file(COPY) optimises out a copy when the destination timestamp
            # matches, which misses a same-second content edit; remove first so
            # a content difference always refreshes the overlay (N4).  Modes are
            # still preserved because file(COPY) carries the source's.
            get_filename_component(_od "${_o}" DIRECTORY)
            file(MAKE_DIRECTORY "${_od}")
            file(REMOVE "${_o}")
            file(COPY "${_s}" DESTINATION "${_od}")
            # file(COPY) carries the SOURCE mtime onto the overlay.  When a
            # patch is dropped the staged content reverts to the pristine file,
            # whose mtime predates the already-built objects, so a plain copy
            # would leave the overlay older than those objects and the build
            # would skip the rebuild.  Stamp the refreshed overlay file with the
            # current time so any content change always re-triggers a rebuild.
            file(TOUCH "${_o}")
        endif()
    endforeach()

    if(EXISTS "${_overlay}")
        file(GLOB_RECURSE _have LIST_DIRECTORIES false RELATIVE "${_overlay}" "${_overlay}/*")
        foreach(_rel IN LISTS _have)
            list(FIND _keep "${_rel}" _idx)
            if(_idx EQUAL -1)
                file(REMOVE "${_overlay}/${_rel}")
            endif()
        endforeach()
    endif()
endfunction()
