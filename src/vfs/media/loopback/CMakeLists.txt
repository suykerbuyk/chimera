# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# The loopback-file media provider: the first implementation of the media
# interface, built on libevpl's block layer (PREAD protocol).

add_library(chimera_media_loopback ${CHIMERA_LIBRARY_TYPE} media_loopback.c)

# Consumers get the media interface headers (..) and this provider's header.
target_include_directories(chimera_media_loopback PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR})

# evpl is an implementation detail: the provider wraps evpl_block.
target_link_libraries(chimera_media_loopback PRIVATE evpl)

install(TARGETS chimera_media_loopback DESTINATION lib)
