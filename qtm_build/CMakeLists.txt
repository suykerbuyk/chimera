# SPDX-FileCopyrightText: 2025 Ben Jarvis
#
# SPDX-License-Identifier: LGPL-2.1-only

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
