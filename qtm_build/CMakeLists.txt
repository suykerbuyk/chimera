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
# RCU caches, and the proc dispatch table).
file(GLOB VFS_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/vfs/*.h)
list(FILTER VFS_HEADERS EXCLUDE REGEX "/(vfs_internal|vfs_[^/]*_cache)\\.h$")
install(FILES ${VFS_HEADERS} DESTINATION include/vfs)
install(FILES
	${CMAKE_SOURCE_DIR}/ext/libevpl/ext/prometheus-c/prometheus-c.h
	${CMAKE_SOURCE_DIR}/ext/libevpl/ext/prometheus-c/ext/stopwatch/stopwatch.h
	DESTINATION include/vfs)
