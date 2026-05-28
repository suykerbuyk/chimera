# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.

add_library(chimera_vfs_snfs SHARED  snfs.c )

target_include_directories(chimera_vfs_snfs PRIVATE /usr/cvfs/include)
target_link_directories(chimera_vfs_snfs PRIVATE /usr/cvfs/lib)
target_link_libraries(chimera_vfs_snfs PRIVATE cvextapi)

install(TARGETS chimera_vfs_snfs DESTINATION lib)
