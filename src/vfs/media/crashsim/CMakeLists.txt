# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# The crash-simulating media provider: a decorator over another media provider
# (the loopback file in the prototype) that stages non-durable writes and, on a
# seeded crash, drops / tears / reorders them, plus a corruption hook.  Built on
# the media interface headers and libevpl (for the iovec + deferral primitives).

add_library(chimera_media_crashsim ${CHIMERA_LIBRARY_TYPE} media_crashsim.c)

# Consumers get the media interface headers (..) and this provider's header.
target_include_directories(chimera_media_crashsim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR})

# evpl is an implementation detail: the provider stages iovecs and defers
# completions on the evpl loop.
target_link_libraries(chimera_media_crashsim PRIVATE evpl)

install(TARGETS chimera_media_crashsim DESTINATION lib)

# The parent install(DIRECTORY ...) excludes this subdir (so the private RNG
# header and the .c are not swept in); install the public header explicitly, so
# a consumer of the installed tree finds it beside the interface headers.
install(FILES chimera_media_crashsim.h DESTINATION include/chimera/vfs/media)
