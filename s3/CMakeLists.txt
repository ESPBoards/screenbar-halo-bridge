# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ../lib)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
# ESP-IDF hides build paths only inside the project; lib/ is outside, so its
# asserts would carry the builder's own path into the firmware.
get_filename_component(LIB_DIR ../lib ABSOLUTE)
idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${LIB_DIR}=lib" APPEND)
project(halo_bridge_s3)
