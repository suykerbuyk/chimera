# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# Backend facade: the block allocator, per-object extent maps, never-reused
# 128-bit object identity, the durability log and crash recovery, and the
# namespace record store, built on the media interface (src/vfs/media).

add_library(chimera_facade ${CHIMERA_LIBRARY_TYPE}
    facade_super.c
    facade_alloc.c
    facade_objid.c
    facade_extent.c
    facade_log.c
    facade_object.c
    facade_recover.c
    facade_ns.c)

# Consumers get the facade headers, the media interface, and the storage
# contract types the facade speaks (object ids, extents, segments).
target_include_directories(chimera_facade PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../media
    ${CMAKE_CURRENT_SOURCE_DIR}/../contract)

# chimera_facade does every media access through the media provider vtable and
# calls no libevpl function; it references struct evpl_iovec only as the vtable's
# buffer type.  So it links no evpl.  The object content-MD5 at SEAL uses the
# common crypto primitives, which depend only on OpenSSL::Crypto -- NOT evpl,
# so the core stays evpl-free.
target_link_libraries(chimera_facade PUBLIC chimera_crypto)

install(TARGETS chimera_facade DESTINATION lib)

# The minimal real storage-contract backend over the facade.  Separate target:
# it touches evpl iovecs and the contract/conformance headers, which the core
# allocator library does not.
add_library(chimera_facade_sc_backend ${CHIMERA_LIBRARY_TYPE} facade_sc_backend.c)
target_include_directories(chimera_facade_sc_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../media
    ${CMAKE_CURRENT_SOURCE_DIR}/../contract
    ${CMAKE_CURRENT_SOURCE_DIR}/../contract/conformance)
target_link_libraries(chimera_facade_sc_backend PUBLIC chimera_facade evpl)
install(TARGETS chimera_facade_sc_backend DESTINATION lib)

if(NOT DISABLE_TESTS)
    add_subdirectory(tests)
endif()
