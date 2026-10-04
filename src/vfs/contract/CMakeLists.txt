# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Storage contract v1 interface: headers only.

set(CHIMERA_SC_HEADERS
    chimera_sc.h
    sc_version.h
    sc_error.h
    sc_types.h
    sc_request.h
    sc_backend.h)

# Self-containment proof (contract D1): every header must compile on its own
# with nothing but this directory on the include path.  Directory-level
# include_directories() inherited from the tree (src/, libevpl, prometheus,
# 3rdparty, uthash, xxhash) are replaced rather than appended to, so a header
# that reaches for the VFS SDK, libevpl or any other chimera header stops
# building here.
set(_sc_selfcontained_srcs "")
foreach(_hdr ${CHIMERA_SC_HEADERS})
    string(REPLACE ".h" "" _name ${_hdr})
    set(_tu ${CMAKE_CURRENT_BINARY_DIR}/sc_selfcontained_${_name}.c)
    file(WRITE ${_tu} "#include \"${_hdr}\"\n")
    list(APPEND _sc_selfcontained_srcs ${_tu})
endforeach()

add_library(sc_selfcontained OBJECT ${_sc_selfcontained_srcs})
set_property(TARGET sc_selfcontained
             PROPERTY INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
    set_target_properties(sc_selfcontained PROPERTIES C_EXTENSIONS OFF)
    target_compile_options(sc_selfcontained PRIVATE -pedantic)
    # The ABI structures carry no implicit padding: every gap is an explicit
    # reserved field.  Checked on LP64, the layout the size assertions in
    # tests/sc_header_test.c pin.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_options(sc_selfcontained PRIVATE -Wpadded)
    endif()
endif()

# The compiler's built-in system include path cannot be removed from the
# target above, so a header including something that happens to be installed
# in /usr/include would still build.  Check the directives themselves: only
# <stdint.h>, <stddef.h> and the contract's own sc_*.h headers are allowed.
foreach(_hdr ${CHIMERA_SC_HEADERS})
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr} _includes
         REGEX "^[ \t]*#[ \t]*include")
    foreach(_inc ${_includes})
        if(NOT _inc MATCHES "<std(int|def)\\.h>|\"sc_[a-z]+\\.h\"")
            message(FATAL_ERROR
                "src/vfs/contract/${_hdr}: '${_inc}' is not allowed; contract "
                "headers include only <stdint.h>, <stddef.h> and sc_*.h")
        endif()
    endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CHIMERA_SC_HEADERS})

install(DIRECTORY ./
        DESTINATION include/chimera/vfs/contract
        FILES_MATCHING PATTERN "*.h"
        PATTERN tests EXCLUDE)

if(NOT DISABLE_TESTS)
    add_subdirectory(tests)
endif()
