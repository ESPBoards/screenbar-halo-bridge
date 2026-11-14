# SPDX-License-Identifier: GPL-3.0-only
# Bring-up bench: a dev-only S3 firmware that measures timing, sends test
# commands, dumps raw captures for tools/replay.c and stress-tests the ring.
# It borrows the bridge's libraries and sdkconfig defaults, plus a few of its own.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ../../lib ../../s3/components/phy_s3 ../../s3/components/listener)
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../s3/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(halo_s3_bench)
