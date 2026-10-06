# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# The in-memory conformance fixture: the harness's self-test double and the
# siblings' honest/cheat backend.  Not the product backend (E14); not installed.

add_library(chimera_sc_fixture ${CHIMERA_LIBRARY_TYPE} sc_fixture.c)

target_include_directories(chimera_sc_fixture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Inherits the harness and contract include paths from chimera_sc_conformance;
# evpl for the deferral marshal and iovec buffers.
target_link_libraries(chimera_sc_fixture PUBLIC chimera_sc_conformance evpl)
