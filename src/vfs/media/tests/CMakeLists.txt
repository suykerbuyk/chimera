# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Media interface tests.  The header probes need no privilege, I/O or network;
# the loopback round-trip uses the pread block protocol on a temp file and so
# also runs unprivileged.  All run in the quick tier and none skip.
set(CHIMERA_TEST_TIER quick)

add_executable(media_header_test media_header_test.c)
target_include_directories(media_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(media_header_test PRIVATE /UNDEBUG)
else()
    target_compile_options(media_header_test PRIVATE -UNDEBUG)
endif()
add_test(NAME chimera/vfs/media/header COMMAND media_header_test)
set_tests_properties(chimera/vfs/media/header PROPERTIES TIMEOUT 30)

# Compile the same assertions with -fshort-enums: if an ABI field were
# enum-typed instead of uint32_t, it would shrink here and fail to build.
if(NOT MSVC)
    add_library(media_header_short_enums OBJECT media_header_test.c)
    target_include_directories(media_header_short_enums PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_options(media_header_short_enums PRIVATE -fshort-enums)
endif()

add_executable(media_null_provider_test media_null_provider_test.c)
target_include_directories(media_null_provider_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(media_null_provider_test PRIVATE /UNDEBUG)
else()
    target_compile_options(media_null_provider_test PRIVATE -UNDEBUG)
endif()
add_test(NAME chimera/vfs/media/null_provider COMMAND media_null_provider_test)
set_tests_properties(chimera/vfs/media/null_provider PROPERTIES TIMEOUT 30)

# The functional proof: the loopback provider over a temp 1 GiB file, driven
# through a real evpl loop.  Links evpl and the loopback provider library.
add_executable(media_loopback_test media_loopback_test.c)
target_include_directories(media_loopback_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../loopback)
if(MSVC)
    target_compile_options(media_loopback_test PRIVATE /UNDEBUG)
else()
    target_compile_options(media_loopback_test PRIVATE -UNDEBUG)
endif()
target_link_libraries(media_loopback_test chimera_media_loopback evpl)
add_test(NAME chimera/vfs/media/loopback COMMAND media_loopback_test)
set_tests_properties(chimera/vfs/media/loopback PROPERTIES
    LABELS "media_mbt"
    TIMEOUT 300)

# ---- crash-sim provider ----

# Layout + PRNG determinism.  Libevpl-free.
add_executable(media_crashsim_header_test media_crashsim_header_test.c)
target_include_directories(media_crashsim_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(media_crashsim_header_test PRIVATE /UNDEBUG)
else()
    target_compile_options(media_crashsim_header_test PRIVATE -UNDEBUG)
endif()
add_test(NAME chimera/vfs/media/crashsim_header COMMAND media_crashsim_header_test)
set_tests_properties(chimera/vfs/media/crashsim_header PROPERTIES TIMEOUT 30)

# Same assertions under -fshort-enums: catches an ABI field accidentally typed
# as an enum (which would shrink here and fail the layout asserts).
if(NOT MSVC)
    add_library(media_crashsim_header_short_enums OBJECT media_crashsim_header_test.c)
    target_include_directories(media_crashsim_header_short_enums PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_options(media_crashsim_header_short_enums PRIVATE -fshort-enums)
endif()

# Create/ABI-gate validation.  No loop driven; links the loopback for a
# conformant delegate descriptor.
add_executable(media_crashsim_null_provider_test media_crashsim_null_provider_test.c)
target_include_directories(media_crashsim_null_provider_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(media_crashsim_null_provider_test PRIVATE /UNDEBUG)
else()
    target_compile_options(media_crashsim_null_provider_test PRIVATE -UNDEBUG)
endif()
target_link_libraries(media_crashsim_null_provider_test chimera_media_crashsim chimera_media_loopback evpl)
add_test(NAME chimera/vfs/media/crashsim_null_provider COMMAND media_crashsim_null_provider_test)
set_tests_properties(chimera/vfs/media/crashsim_null_provider PROPERTIES TIMEOUT 30)

# The functional proof: the crash-sim over the loopback delegate on a temp 1 GiB
# file, driven through a real evpl loop.
add_executable(media_crashsim_test media_crashsim_test.c)
target_include_directories(media_crashsim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(media_crashsim_test PRIVATE /UNDEBUG)
else()
    target_compile_options(media_crashsim_test PRIVATE -UNDEBUG)
endif()
target_link_libraries(media_crashsim_test chimera_media_crashsim chimera_media_loopback evpl)
add_test(NAME chimera/vfs/media/crashsim COMMAND media_crashsim_test)
set_tests_properties(chimera/vfs/media/crashsim PROPERTIES
    LABELS "media_mbt"
    TIMEOUT 300)
