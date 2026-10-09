# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.

add_library(chimera_vfs_snfs ${CHIMERA_VFS_MODULE_TYPE} snfs.c)

target_include_directories(chimera_vfs_snfs PRIVATE /usr/cvfs/include)
target_link_directories(chimera_vfs_snfs PRIVATE /usr/cvfs/lib)
target_link_libraries(chimera_vfs_snfs cvextapi)

chimera_vfs_module(chimera_vfs_snfs)
