# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Media interface: the backend facade's lower interface (headers), plus the
# loopback-file provider.  The headers are self-contained (libc only); the
# loopback provider is built on libevpl's block layer.

set(CHIMERA_MEDIA_HEADERS
    chimera_media.h
    media_version.h
    media_error.h
    media_types.h
    media_provider.h)

# Self-containment proof: every header must compile on its own with nothing
# but this directory on the include path.  The directory-level
# include_directories() inherited from the tree (src/, libevpl, prometheus,
# 3rdparty, uthash, xxhash) are replaced rather than appended to, so a header
# that reaches for libevpl, the VFS SDK or any other chimera header stops
# building here.
set(_media_selfcontained_srcs "")
foreach(_hdr ${CHIMERA_MEDIA_HEADERS})
    string(REPLACE ".h" "" _name ${_hdr})
    set(_tu ${CMAKE_CURRENT_BINARY_DIR}/media_selfcontained_${_name}.c)
    file(WRITE ${_tu} "#include \"${_hdr}\"\n")
    list(APPEND _media_selfcontained_srcs ${_tu})
endforeach()

add_library(media_selfcontained OBJECT ${_media_selfcontained_srcs})
set_property(TARGET media_selfcontained
             PROPERTY INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
    set_target_properties(media_selfcontained PROPERTIES C_EXTENSIONS OFF)
    target_compile_options(media_selfcontained PRIVATE -pedantic)
    # The descriptor and info structures carry no implicit padding.  Checked
    # on LP64, the layout the size assertions in tests/media_header_test.c pin.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_options(media_selfcontained PRIVATE -Wpadded)
    endif()
endif()

# The compiler's built-in system include path cannot be removed from the
# target above, so a header including something that happens to be installed
# in /usr/include would still build.  Check the directives themselves: only
# <stdint.h>, <stddef.h> and the media interface's own media_*.h headers are
# allowed.
foreach(_hdr ${CHIMERA_MEDIA_HEADERS})
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr} _includes
         REGEX "^[ \t]*#[ \t]*include")
    foreach(_inc ${_includes})
        # Anchored to the directive, so a comment naming an allowed header
        # cannot excuse a disallowed one.
        if(NOT _inc MATCHES "^[ \t]*#[ \t]*include[ \t]*(<std(int|def)\\.h>|\"media_[a-z]+\\.h\")[ \t]*$")
            message(FATAL_ERROR
                "src/vfs/media/${_hdr}: '${_inc}' is not allowed; media "
                "headers include only <stdint.h>, <stddef.h> and media_*.h")
        endif()
    endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CHIMERA_MEDIA_HEADERS})

install(DIRECTORY ./
        DESTINATION include/chimera/vfs/media
        FILES_MATCHING PATTERN "*.h"
        PATTERN tests EXCLUDE
        PATTERN loopback EXCLUDE)

add_subdirectory(loopback)

if(NOT DISABLE_TESTS)
    add_subdirectory(tests)
endif()
