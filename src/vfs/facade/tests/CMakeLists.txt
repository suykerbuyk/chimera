# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Facade allocator tests.  The four exit-criteria tests run against an in-RAM
# media fixture (no privilege, I/O or network); the loopback integration test
# uses the PREAD block protocol on a temp file and so also runs unprivileged.
# All run in the quick tier and none skip.
set(CHIMERA_TEST_TIER quick)

# The in-RAM media fixture: a chimera_media provider over a per-uri backing
# buffer that survives close/open.  Links no evpl (reads only iovec data/length).
add_library(facade_test_media ${CHIMERA_LIBRARY_TYPE} facade_test_media.c)
target_include_directories(facade_test_media PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media)

# Exit-criteria tests over the fixture.  Compiled -UNDEBUG so assert() stays
# live; load-bearing invariants also use TCHECK(), which survives NDEBUG.
foreach(_t facade_alloc_test facade_objid_test facade_enospc_test
           facade_objid_race_test facade_objid_teardown_test)
    add_executable(${_t} ${_t}.c)
    target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${_t} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${_t} PRIVATE -UNDEBUG)
    endif()
    target_link_libraries(${_t} chimera_facade facade_test_media)
    add_test(NAME chimera/vfs/facade/${_t} COMMAND ${_t})
    set_tests_properties(chimera/vfs/facade/${_t} PROPERTIES TIMEOUT 30)
endforeach()

# The extent-map test needs no media at all.
add_executable(facade_extent_test facade_extent_test.c)
target_include_directories(facade_extent_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(facade_extent_test PRIVATE /UNDEBUG)
else()
    target_compile_options(facade_extent_test PRIVATE -UNDEBUG)
endif()
target_link_libraries(facade_extent_test chimera_facade)
add_test(NAME chimera/vfs/facade/facade_extent_test COMMAND facade_extent_test)
set_tests_properties(chimera/vfs/facade/facade_extent_test PROPERTIES TIMEOUT 30)

# Integration: the allocator over the real loopback (PREAD) provider, driven
# through an evpl loop.  Links evpl and the loopback provider.
add_executable(facade_loopback_test facade_loopback_test.c)
target_include_directories(facade_loopback_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/loopback)
if(MSVC)
    target_compile_options(facade_loopback_test PRIVATE /UNDEBUG)
else()
    target_compile_options(facade_loopback_test PRIVATE -UNDEBUG)
endif()
target_link_libraries(facade_loopback_test chimera_facade chimera_media_loopback evpl)
add_test(NAME chimera/vfs/facade/facade_loopback_test COMMAND facade_loopback_test)
set_tests_properties(chimera/vfs/facade/facade_loopback_test PROPERTIES
    LABELS "facade_mbt"
    TIMEOUT 300)

# media-log-and-recovery: crash-recovery over the crash-sim + loopback providers,
# and the real conformance-durability-power-loss suite via the facade backend.
foreach(_t facade_recover_crash_test facade_conformance_durability_test)
    add_executable(${_t} ${_t}.c)
    target_include_directories(${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../media
        ${CMAKE_CURRENT_SOURCE_DIR}/../../media/loopback
        ${CMAKE_CURRENT_SOURCE_DIR}/../../contract
        ${CMAKE_CURRENT_SOURCE_DIR}/../../contract/conformance)
    if(MSVC)
        target_compile_options(${_t} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${_t} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME chimera/vfs/facade/${_t} COMMAND ${_t})
    set_tests_properties(chimera/vfs/facade/${_t} PROPERTIES
        LABELS "facade_mbt"
        TIMEOUT 300)
endforeach()
target_link_libraries(facade_recover_crash_test
    chimera_facade chimera_media_crashsim chimera_media_loopback evpl)
target_link_libraries(facade_conformance_durability_test
    chimera_facade_sc_backend chimera_sc_conformance
    chimera_media_crashsim chimera_media_loopback evpl)

# In-RAM log/format/teardown unit test over the FUA-honest fixture.
add_executable(facade_log_test facade_log_test.c)
target_include_directories(facade_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(facade_log_test PRIVATE /UNDEBUG)
else()
    target_compile_options(facade_log_test PRIVATE -UNDEBUG)
endif()
target_link_libraries(facade_log_test chimera_facade facade_test_media)
add_test(NAME chimera/vfs/facade/facade_log_test COMMAND facade_log_test)
set_tests_properties(chimera/vfs/facade/facade_log_test PROPERTIES TIMEOUT 30)
