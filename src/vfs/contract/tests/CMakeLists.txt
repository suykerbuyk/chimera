# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Header-level probes of the storage contract interface.  They need no
# privilege, I/O or network, so they run in the quick tier and never skip.
set(CHIMERA_TEST_TIER quick)

add_executable(sc_header_test sc_header_test.c)
target_include_directories(sc_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(sc_header_test PRIVATE /UNDEBUG)
else()
    target_compile_options(sc_header_test PRIVATE -UNDEBUG)
endif()
add_test(NAME chimera/contract/header COMMAND sc_header_test)

# Compile the same assertions with -fshort-enums: if an ABI field were
# enum-typed instead of uint32_t, it would shrink here and fail to build.
if(NOT MSVC)
    add_library(sc_header_short_enums OBJECT sc_header_test.c)
    target_include_directories(sc_header_short_enums PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_options(sc_header_short_enums PRIVATE -fshort-enums)
endif()

add_executable(sc_null_backend_test sc_null_backend_test.c)
target_include_directories(sc_null_backend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(sc_null_backend_test PRIVATE /UNDEBUG)
else()
    target_compile_options(sc_null_backend_test PRIVATE -UNDEBUG)
endif()
add_test(NAME chimera/contract/null_backend COMMAND sc_null_backend_test)
