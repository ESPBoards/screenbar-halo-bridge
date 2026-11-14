# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ../lib)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(halo_bridge_s3)
