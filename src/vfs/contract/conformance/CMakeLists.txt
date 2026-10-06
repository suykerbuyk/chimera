# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Storage-contract conformance harness: the driver and the obligation
# vocabulary that a conformance suite is built from.  It is a CONSUMER of the
# contract, so unlike the contract headers it may include libevpl; it is not
# part of the contract's libc-only self-containment check.

add_library(chimera_sc_conformance ${CHIMERA_LIBRARY_TYPE} sc_conformance.c)

# Consumers get the harness headers (.) and the contract headers (..).
target_include_directories(chimera_sc_conformance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The driver owns the evpl loop it pumps.
target_link_libraries(chimera_sc_conformance PUBLIC evpl)

# The harness ships with the contract (D12), so a backend author can run it.
install(TARGETS chimera_sc_conformance DESTINATION lib)
install(FILES sc_conformance.h sc_conformance_expect.h
        DESTINATION include/chimera/vfs/contract/conformance)

# The in-memory self-test fixture (its own library, so each sibling suite links
# it independently -- the media header/loopback split).
add_subdirectory(fixture)

if(NOT DISABLE_TESTS)
    add_subdirectory(tests)
endif()
