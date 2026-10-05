# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
#
foreach(name IN ITEMS chimera-example.json chimera-snfs.json)
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/${name}.in
    ${CMAKE_CURRENT_BINARY_DIR}/${name}
  )
  list(APPEND DOC_FILES ${CMAKE_CURRENT_BINARY_DIR}/${name})
endforeach()
install(FILES ${DOC_FILES} DESTINATION share/examples )
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../suppressions.txt DESTINATION ${DESTDIR}/etc/chimera )
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/chimera.service DESTINATION ${DESTDIR}/usr/lib/systemd/system )
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/chimera.default DESTINATION ${DESTDIR}/etc/default RENAME chimera )

# The headers an out-of-tree VFS module builds against are installed by
# src/vfs/CMakeLists.txt, not here: upstream defines the module SDK as the
# backend boundary and ships it as include/chimera/vfs/sdk/ plus the three
# external headers it includes (prometheus-c.h, stopwatch.h, oteltracing.h)
# at include/chimera/.  A module compiles with
# -I<prefix>/include/chimera and #include "vfs/sdk/chimera_vfs_sdk.h",
# the same form used in-tree.  This directory used to install its own flat
# include/vfs/ tree globbed from src/vfs/*.h; that glob silently stopped
# covering nine of those headers the moment they moved into sdk/, so the
# duplicate is gone rather than repaired.
#
# The one thing upstream's install does not do is pin OTEL_TRACING, so the
# packaged oteltracing.h is still generated and installed below.

# The packaged oteltracing.h must pin OTEL_TRACING to the value the packaged
# binaries were compiled with: the header defaults to 1 (4 KiB otel_span
# embedded in chimera_vfs_request) but chimera is built with OTEL_TRACING=0
# (zero-size span) whenever protobuf-c is absent, and an out-of-tree VFS
# module compiled with the other value gets a skewed request layout and
# crashes at runtime.
if(TARGET oteltracing-c)
	set(QTM_OTEL_TRACING 1)
else()
	set(QTM_OTEL_TRACING 0)
endif()
file(READ ${OTEL_SRC}/oteltracing.h
	QTM_OTEL_HEADER)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/oteltracing.h
	"/* OTEL_TRACING pinned by chimera packaging to match the packaged binaries. */
#ifndef OTEL_TRACING
#define OTEL_TRACING ${QTM_OTEL_TRACING}
#endif
${QTM_OTEL_HEADER}")
# Overwrites the unpinned copy src/vfs/CMakeLists.txt installs at the same
# path: CMake honours the last install() rule for a destination file, and
# the top-level CMakeLists.txt adds this directory after src/ for exactly
# that reason.  Keep those two facts together if either moves.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/oteltracing.h
	DESTINATION include/chimera)

# Quantum-specific: install the standalone test suites under <prefix>/tests
# so a deployed server can be used to validate VFS modules (e.g. snfs) and
# protocol servers in place.

# Install every executable target defined in a test source directory to
# <prefix>/tests/<dest>, rather than duplicating the per-suite program lists.
function(qtm_install_test_targets srcdir dest)
	get_property(suite_targets
		DIRECTORY ${CMAKE_SOURCE_DIR}/${srcdir}
		PROPERTY BUILDSYSTEM_TARGETS)
	foreach(target IN LISTS suite_targets)
		get_target_property(target_type ${target} TYPE)
		if(target_type STREQUAL "EXECUTABLE")
			install(TARGETS ${target} RUNTIME DESTINATION tests/${dest})
		endif()
	endforeach()
endfunction()

# The posix tests are always built; the client and smb test directories are
# only processed (and their binaries only built) when DISABLE_TESTS is off,
# so a package built with -DDISABLE_TESTS=ON ships the posix suite only.
qtm_install_test_targets(src/posix/tests posix)
if(NOT DISABLE_TESTS)
	qtm_install_test_targets(src/client/tests client)
	qtm_install_test_targets(src/server/smb/tests smb)
endif()

# Ship the caller-side chimera_vfs headers with the test suites.  The module
# SDK installed by src/vfs/CMakeLists.txt is the boundary a VFS *backend*
# builds against; these are the other side of the library -- what an
# in-process *caller* of chimera_vfs uses (chimera_vfs_init, thread_init,
# mount, get_root_fh, the dispatch and xattr procs), the same layer the
# in-tree NFS/SMB servers sit on.  A downstream test fixture that loads the
# packaged libchimera_vfs.so needs them, and with nothing shipped it vendors
# its own copies: those still compile after an upstream signature change --
# the local prototypes are the only ones the compiler sees -- and fail at run
# time instead.  Deliberately under tests/ rather than beside the SDK, and in
# the quantum-chimera-tests package rather than the server one, so no
# stability is implied: these are internals that move with the library.
#
# The list is the include closure of vfs/vfs_procs.h, not just the two
# headers a caller names, and it is maintained by hand -- an upstream header
# that grows a new #include silently falls outside it.
# qtm_build/verify_test_includes.sh is what catches that: it compiles a
# standalone translation unit against the staged install tree, and the
# package build tasks in qb.yaml run it right after `ninja install`.  Keep
# the two in step, along with the include roots documented in
# tests-include.README.
set(QTM_TEST_VFS_HEADERS
	vfs.h            # chimera_vfs, _thread, mounts, attr vocabulary
	vfs_procs.h      # the dispatch and xattr procs callers drive
	vfs_dump.h       # included by vfs.h
	vfs_pnfs.h       # included by vfs.h
	vfs_clock.h)     # included by vfs.h
list(TRANSFORM QTM_TEST_VFS_HEADERS PREPEND ${CMAKE_SOURCE_DIR}/src/vfs/)
install(FILES ${QTM_TEST_VFS_HEADERS} DESTINATION tests/include/vfs)

# vfs_dump.h includes common/logging.h and vfs.h includes common/tcp_flavor.h,
# so the C utility layer these two reach for travels with them.  The Windows
# port (2026-09-28 sync) grew that layer: vfs.h now includes common/thread.h
# and common/platform.h, vfs_clock.h common/atomic.h, and logging.h
# common/compiler.h.  platform.h includes common/windows.h only under _WIN32,
# so that one stays out of a package that only ever builds on Linux.
install(FILES
	${CMAKE_SOURCE_DIR}/src/common/logging.h
	${CMAKE_SOURCE_DIR}/src/common/tcp_flavor.h
	${CMAKE_SOURCE_DIR}/src/common/thread.h
	${CMAKE_SOURCE_DIR}/src/common/platform.h
	${CMAKE_SOURCE_DIR}/src/common/atomic.h
	${CMAKE_SOURCE_DIR}/src/common/compiler.h
	DESTINATION tests/include/common)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/tests-include.README
	DESTINATION tests/include RENAME README)

# WPTS / pike SMB conformance fixtures (ptfconfig files + case maps) and the
# wrapper scripts that drive the installed daemon.  The wrappers locate their
# external prerequisites and these fixtures via environment variables
# (WPTS_BIN_DIR, DOTNET, WPTS_PTFCONFIG_DIR, PIKE_DIR).
install(DIRECTORY
	${CMAKE_SOURCE_DIR}/src/server/smb/tests/wpts
	${CMAKE_SOURCE_DIR}/src/server/smb/tests/pike
	DESTINATION tests/smb)
install(PROGRAMS
	${CMAKE_SOURCE_DIR}/scripts/netns_test_wrapper.sh
	${CMAKE_SOURCE_DIR}/scripts/wpts_smb_test_wrapper.sh
	${CMAKE_SOURCE_DIR}/scripts/pike_smb_test_wrapper.sh
	${CMAKE_SOURCE_DIR}/scripts/trx_to_junit.py
	DESTINATION tests/scripts)

# fio / elbencho benchmark configs.  The source templates reference the build
# tree (plugin .so, diskfs module .so, test-state directory); re-render them
# against the installed layout.  The templates use one FIO_TEST_BINARY /
# ELBENCHO_TEST_BINARY variable for two different things -- the .fio files
# locate their chimera JSON config with it, while the JSON files place the
# diskfs device image with it -- so the substitution value is per-file:
# installed config dir for the former, a writable scratch dir (which must
# exist before running) for the latter.
set(QTM_TEST_SCRATCH /var/tmp/chimera-tests)

function(qtm_render_test_config template outrel testdir)
	get_filename_component(outdir ${outrel} DIRECTORY)
	file(READ ${template} content)
	string(REPLACE [[${CMAKE_BINARY_DIR}/src/vfs/diskfs]]
		"${CMAKE_INSTALL_PREFIX}/lib" content "${content}")
	set(PLUGIN_PATH ${CMAKE_INSTALL_PREFIX}/lib/libchimera_fio.so)
	set(FIO_TEST_BINARY ${testdir})
	set(ELBENCHO_TEST_BINARY ${testdir})
	string(CONFIGURE "${content}" content)
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${outrel} "${content}")
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${outrel}
		DESTINATION tests/${outdir})
endfunction()

# The plugins are only built when their host trees (/fio, /elbencho) are
# present in the build container; install the configs only alongside them.
if(TARGET chimera_fio)
	foreach(cfg memfs_basic diskfs_io_uring_basic diskfs_aio_basic)
		qtm_render_test_config(${CMAKE_SOURCE_DIR}/src/fio/tests/${cfg}.fio
			fio/${cfg}.fio ${CMAKE_INSTALL_PREFIX}/tests/fio)
		qtm_render_test_config(${CMAKE_SOURCE_DIR}/src/fio/tests/${cfg}.json
			fio/${cfg}.json ${QTM_TEST_SCRATCH})
	endforeach()
endif()

if(TARGET elbencho_chimera)
	qtm_render_test_config(${CMAKE_SOURCE_DIR}/src/elbencho/tests/memfs_basic.json
		elbencho/memfs_basic.json ${QTM_TEST_SCRATCH})
	qtm_render_test_config(${CMAKE_SOURCE_DIR}/src/elbencho/tests/diskfs_io_uring.json
		elbencho/diskfs_io_uring.json ${QTM_TEST_SCRATCH})
	install(PROGRAMS ${CMAKE_SOURCE_DIR}/src/elbencho/tests/verify_async.sh
		DESTINATION tests/elbencho)
endif()
