# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.

add_library(chimera_vfs_snfs SHARED  snfs.c )

install(TARGETS chimera_vfs_snfs DESTINATION lib)
