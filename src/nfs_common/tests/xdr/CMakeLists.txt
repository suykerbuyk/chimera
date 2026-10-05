# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# Register this directory's tests in the quick tier (see the add_test() wrapper
# in the top-level CMakeLists.txt).
set(CHIMERA_TEST_TIER quick)

# Compile the fixture's XDR with the same generator the sibling nfs_common
# sources use.  No -r/-e: this .x declares no RPC program and no enum.
set(XDR_BOUNDS_X ${CMAKE_CURRENT_SOURCE_DIR}/xdr_bounds.x)
set(XDR_BOUNDS_C ${CMAKE_CURRENT_BINARY_DIR}/xdr_bounds_xdr.c)
set(XDR_BOUNDS_H ${CMAKE_CURRENT_BINARY_DIR}/xdr_bounds_xdr.h)

add_custom_command(
    OUTPUT ${XDR_BOUNDS_C} ${XDR_BOUNDS_H}
    COMMAND ${XDRZCC} ${XDR_BOUNDS_X} ${XDR_BOUNDS_C} ${XDR_BOUNDS_H}
    DEPENDS ${XDR_BOUNDS_X} ${XDRZCC}
    COMMENT "Compiling ${XDR_BOUNDS_X}"
)

# Suppress warnings in the generated source, as the sibling nfs_common library
# does for its generated translation units.
set_source_files_properties(${XDR_BOUNDS_C}
    PROPERTIES COMPILE_OPTIONS "${CHIMERA_GENERATED_C_OPTIONS}")

# The fixture isolates each decode case in a forked child so an ASan abort in
# one case cannot mask the others; that needs fork()/waitpid() (POSIX), so the
# target and its test are registered on non-Windows platforms only.
if(NOT WIN32)
add_executable(xdr_bounds_test
    ${XDR_BOUNDS_C} ${XDR_BOUNDS_H}
    xdr_bounds_test.c)

# The decode-length arithmetic under test is independent of the evpl iovec
# refcount machinery, so this fixture drives the generator's self-contained
# fallback iovec (data pointer + length) and its built-in dump_output.  These
# -U flags follow the project-wide -D defaults on the command line and so win,
# letting the fixture build and link without the evpl runtime.
target_compile_options(xdr_bounds_test PRIVATE
    -UXDR_CUSTOM_IOVEC -UEVPL_IOVEC_TRACE -UXDR_CUSTOM_DUMP)

target_include_directories(xdr_bounds_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_dependencies(xdr_bounds_test xdrzcc)

add_test(chimera/nfs_common/xdr_bounds_test xdr_bounds_test)
endif()
