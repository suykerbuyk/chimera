# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# ApplyPatches.cmake -- build-tree overlay for the pinned libevpl submodule.
#
# The build must never edit the checked-out ext/libevpl submodule (it stays
# pinned), yet chimera needs to carry local fixes against it.  So at configure
# time we materialise a COPY of ext/libevpl under the build tree -- the
# "overlay" -- apply the tracked patch series from patches/ onto that copy, and
# point the whole build at the overlay instead of the submodule.  patches/ is
# the only place a libevpl change lives; the submodule gitlink never moves.
#
# The tracked series is applied onto that copy; with an empty series the overlay
# is a byte-faithful copy of the pristine submodule.  Either way the full build
# must succeed against the overlay.
#
# Included from the top-level CMakeLists.txt after project() and after
# cmake/Platform.cmake, and before the first ext/libevpl reference.  It exports:
#   LIBEVPL_SRC    the overlay libevpl root (what the build consumes)
#   LIBEVPL_INC    ${LIBEVPL_SRC}/include
#   PROMETHEUS_SRC ${LIBEVPL_SRC}/ext/prometheus-c
#   OTEL_SRC       ${LIBEVPL_SRC}/ext/oteltracing-c

include("${CMAKE_CURRENT_LIST_DIR}/PatchApply.cmake")

find_package(Git)

# --- exported overlay paths -----------------------------------------------
set(LIBEVPL_SRC    "${CMAKE_BINARY_DIR}/ext-overlay/libevpl")
set(LIBEVPL_INC    "${LIBEVPL_SRC}/include")
set(PROMETHEUS_SRC "${LIBEVPL_SRC}/ext/prometheus-c")
set(OTEL_SRC       "${LIBEVPL_SRC}/ext/oteltracing-c")

set(_chimera_libevpl_pristine "${CMAKE_SOURCE_DIR}/ext/libevpl")
set(_chimera_overlay_staging_parent "${CMAKE_BINARY_DIR}/ext-overlay-staging")
set(_chimera_overlay_staging "${_chimera_overlay_staging_parent}/libevpl")

# Stage a pristine copy: every file except any '.git' component, TEXT files
# normalised to LF with their modes preserved, non-text files verbatim.
function(_chimera_overlay_stage _src _staging)
    file(REMOVE_RECURSE "${_staging}")
    get_filename_component(_sp "${_staging}" DIRECTORY)
    file(MAKE_DIRECTORY "${_sp}")
    # file(COPY) preserves modes, copies dotfiles, is binary-safe, and the
    # PATTERN drops gitlink files/dirs named exactly '.git' (keeping .gitignore,
    # .github, .gitmodules, .reuse, ...).
    file(COPY "${_src}/" DESTINATION "${_staging}" PATTERN ".git" EXCLUDE)

    # Normalise line endings.  On an LF checkout nothing matches, so nothing is
    # rewritten and every mode is left exactly as file(COPY) placed it.  On a
    # CRLF checkout the rewrite would drop the mode, so the exec bit is restored.
    file(GLOB_RECURSE _files LIST_DIRECTORIES false "${_staging}/*")
    foreach(_f IN LISTS _files)
        file(READ "${_f}" _hex LIMIT 8192 HEX)
        # A HEX dump is two characters per byte, so a whole NUL byte is "00" at
        # an EVEN character offset.  A plain FIND for "00" also matches a pair
        # that straddles a byte boundary (e.g. 0xA0 0x0B -> "a00b" has "00" at an
        # odd offset), which wrongly flags LF text such as xdr_builtin.c as
        # binary and skips its CRLF normalisation.  Fast-path on the FIND, then
        # confirm the match sits at an even offset (a real NUL byte) with an
        # anchored byte-pair regex, which only runs for the rare file that has
        # any "00" in the first 8 KiB.
        string(FIND "${_hex}" "00" _nul)
        if(NOT _nul EQUAL -1)
            string(REGEX MATCH "^(..)*00" _byte_nul "${_hex}")
            if(NOT _byte_nul STREQUAL "")
                continue()  # binary: a whole NUL byte at an even hex offset
            endif()
        endif()
        file(READ "${_f}" _content)
        string(FIND "${_content}" "\r" _cr)
        if(_cr EQUAL -1)
            continue()  # already LF
        endif()
        set(_was_exec FALSE)
        if(NOT WIN32)
            execute_process(COMMAND test -x "${_f}" RESULT_VARIABLE _xrc)
            if(_xrc EQUAL 0)
                set(_was_exec TRUE)
            endif()
        endif()
        string(REPLACE "\r\n" "\n" _content "${_content}")
        file(WRITE "${_f}" "${_content}")
        if(_was_exec)
            file(CHMOD "${_f}" PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif()
    endforeach()
endfunction()

# Parse a series.sha control file: 'repo-root <rel>', 'base <sha>', and zero or
# more 'sentinel <patch> <file> <token>' rows.  Sentinels are returned as a list
# of tab-joined "patch<TAB>file<TAB>token" triples.
function(_chimera_parse_series_sha _file _root_var _base_var _sent_var)
    set(_root "")
    set(_base "")
    set(_sents "")
    chimera_patch_read_lines("${_file}" _lines)
    foreach(_l IN LISTS _lines)
        separate_arguments(_toks UNIX_COMMAND "${_l}")
        list(LENGTH _toks _n)
        if(_n EQUAL 0)
            continue()
        endif()
        list(GET _toks 0 _key)
        if(_key STREQUAL "repo-root" AND _n GREATER_EQUAL 2)
            list(GET _toks 1 _root)
        elseif(_key STREQUAL "base" AND _n GREATER_EQUAL 2)
            list(GET _toks 1 _base)
        elseif(_key STREQUAL "sentinel" AND _n GREATER_EQUAL 4)
            list(GET _toks 1 _sp)
            list(GET _toks 2 _sf)
            list(GET _toks 3 _st)
            list(APPEND _sents "${_sp}\t${_sf}\t${_st}")
        endif()
    endforeach()
    set(${_root_var} "${_root}" PARENT_SCOPE)
    set(${_base_var} "${_base}" PARENT_SCOPE)
    set(${_sent_var} "${_sents}" PARENT_SCOPE)
endfunction()

# Best-effort upstream-already-fixed signal (step 8): if the pinned submodule
# has been advanced past the SHA this series was authored against, warn.  Runs
# with the same git isolation, tolerates any failure quietly, and never fails
# configure.
function(_chimera_upstream_probe _repo_src _base _label)
    if(NOT GIT_EXECUTABLE OR _base STREQUAL "" OR NOT IS_DIRECTORY "${_repo_src}")
        return()
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
            "${GIT_EXECUTABLE}" -C "${_repo_src}" rev-parse HEAD
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _head
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _rc EQUAL 0 OR _head STREQUAL "" OR _head STREQUAL "${_base}")
        return()
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
            "${GIT_EXECUTABLE}" -C "${_repo_src}" merge-base --is-ancestor "${_base}" "${_head}"
        RESULT_VARIABLE _anc ERROR_QUIET OUTPUT_QUIET)
    if(_anc EQUAL 0)
        message(WARNING
            "patches/${_label}: pinned submodule (${_head}) has moved past the "
            "series base (${_base}); some patches may already be upstream -- "
            "re-check the series and series.sha.")
    endif()
endfunction()

# --- build the overlay -----------------------------------------------------
_chimera_overlay_stage("${_chimera_libevpl_pristine}" "${_chimera_overlay_staging}")

set(_chimera_patch_control "")   # control + .patch files, for CMAKE_CONFIGURE_DEPENDS

file(GLOB _chimera_series_files "${CMAKE_SOURCE_DIR}/patches/*/series")
foreach(_series IN LISTS _chimera_series_files)
    get_filename_component(_repo_dir "${_series}" DIRECTORY)
    get_filename_component(_repo_name "${_repo_dir}" NAME)
    set(_sha_file "${_repo_dir}/series.sha")
    list(APPEND _chimera_patch_control "${_series}" "${_sha_file}")

    _chimera_parse_series_sha("${_sha_file}" _repo_root _base _sentinels)
    if(_repo_root STREQUAL "")
        message(FATAL_ERROR
            "patches/${_repo_name}/series.sha: missing 'repo-root <path>'")
    endif()

    # Authoring assertion: each declared sentinel must be ABSENT from the
    # pristine target, so it genuinely marks what a patch adds.
    foreach(_sent IN LISTS _sentinels)
        string(REPLACE "\t" ";" _sfields "${_sent}")
        list(GET _sfields 0 _s_patch)
        list(GET _sfields 1 _s_file)
        list(GET _sfields 2 _s_tok)
        set(_pristine_file "${_chimera_libevpl_pristine}/${_repo_root}/${_s_file}")
        if(EXISTS "${_pristine_file}")
            file(READ "${_pristine_file}" _pc)
            string(FIND "${_pc}" "${_s_tok}" _pf)
            if(NOT _pf EQUAL -1)
                message(FATAL_ERROR
                    "patches/${_repo_name}/series.sha: sentinel '${_s_tok}' for "
                    "${_s_patch}:${_s_file} is already present in the pristine "
                    "source -- it does not mark a patched change.")
            endif()
        endif()
    endforeach()

    chimera_patch_read_lines("${_series}" _patches)
    foreach(_p IN LISTS _patches)
        set(_patch_path "${_repo_dir}/${_p}")
        list(APPEND _chimera_patch_control "${_patch_path}")
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "patches/${_repo_name}/series lists '${_p}' but Git was not found")
        endif()

        # Snapshot the pre-apply staged copies of this patch's targets, so
        # verification compares against the normalised pre-apply content (N2).
        chimera_patch_targets("${_patch_path}" _tgts _dels)
        set(_pre_dir "${_chimera_overlay_staging_parent}/pre/${_repo_name}/${_p}")
        file(REMOVE_RECURSE "${_pre_dir}")
        foreach(_t IN LISTS _tgts)
            set(_staged_t "${_chimera_overlay_staging}/${_repo_root}/${_t}")
            if(EXISTS "${_staged_t}")
                get_filename_component(_tsub "${_t}" DIRECTORY)
                file(MAKE_DIRECTORY "${_pre_dir}/${_tsub}")
                file(COPY "${_staged_t}" DESTINATION "${_pre_dir}/${_tsub}")
            endif()
        endforeach()

        chimera_patch_apply_one(
            GIT "${GIT_EXECUTABLE}" WORKDIR "${_chimera_overlay_staging}"
            STAGING_PARENT "${_chimera_overlay_staging_parent}"
            DIRECTORY "${_repo_root}" PATCH "${_patch_path}" ISOLATE ON
            LABEL "${_repo_name}:${_p}")

        # Sentinel keyed to this patch (first matching row), if any.
        set(_tok "")
        foreach(_sent IN LISTS _sentinels)
            string(REPLACE "\t" ";" _sfields "${_sent}")
            list(GET _sfields 0 _s_patch)
            if(_s_patch STREQUAL "${_p}")
                list(GET _sfields 2 _tok)
                break()
            endif()
        endforeach()

        chimera_patch_verify_one(
            STAGING "${_chimera_overlay_staging}" REPO_ROOT "${_repo_root}"
            PATCH "${_patch_path}" PRE_DIR "${_pre_dir}" SENTINEL "${_tok}"
            LABEL "${_repo_name}:${_p}")
    endforeach()

    _chimera_upstream_probe(
        "${_chimera_libevpl_pristine}/${_repo_root}" "${_base}" "${_repo_name}")
endforeach()

# Publish the staged tree to the consumed overlay (copy-if-different + prune).
chimera_overlay_sync("${_chimera_overlay_staging}" "${LIBEVPL_SRC}")

# Re-trigger configure when any pristine libevpl file, or any series/patch
# control file, changes -- by set (CONFIGURE_DEPENDS glob) and by content
# (CMAKE_CONFIGURE_DEPENDS over each file).
file(GLOB_RECURSE _chimera_pristine_all LIST_DIRECTORIES false CONFIGURE_DEPENDS
     "${_chimera_libevpl_pristine}/*")
set(_chimera_dep_files "")
foreach(_f IN LISTS _chimera_pristine_all)
    if(_f MATCHES "(^|/)\\.git(/|$)")
        continue()
    endif()
    list(APPEND _chimera_dep_files "${_f}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_chimera_dep_files} ${_chimera_patch_control})

# P0 self-test (N1): proves the apply isolation, the Skipped detector, the
# sha/sentinel verifier and the overlay refresh all work, every configure.
include("${CMAKE_CURRENT_LIST_DIR}/selftest/selftest.cmake")
chimera_patch_selftest("${CMAKE_BINARY_DIR}/patch-selftest")
