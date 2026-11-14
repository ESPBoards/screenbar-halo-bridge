# SPDX-License-Identifier: GPL-3.0-only
idf_component_register(
    SRCS "src/halo_gfsk.c" "src/halo_fm.c" "src/halo_detect.c" "src/halo_blocks.c" "src/halo_listen.c"
    INCLUDE_DIRS "include"
    REQUIRES lamp)
