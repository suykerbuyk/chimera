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

# Quantum-specific: install VFS headers consumed by downstream packaging.
# Glob the public headers but skip internal-only ones (internal helpers, the
# RCU caches, the proc dispatch table, and the open-cache release inlines,
# which include the excluded vfs_open_cache.h).
file(GLOB VFS_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/vfs/*.h)
list(FILTER VFS_HEADERS EXCLUDE REGEX "/(vfs_internal|vfs_release|vfs_[^/]*_cache)\\.h$")
install(FILES ${VFS_HEADERS} DESTINATION include/vfs)
install(FILES
	${CMAKE_SOURCE_DIR}/ext/libevpl/ext/prometheus-c/prometheus-c.h
	${CMAKE_SOURCE_DIR}/ext/libevpl/ext/prometheus-c/ext/stopwatch/stopwatch.h
	${CMAKE_SOURCE_DIR}/ext/libevpl/ext/oteltracing-c/oteltracing.h
	DESTINATION include/vfs)

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
