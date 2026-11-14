# SPDX-License-Identifier: GPL-3.0-only
idf_component_register(
    SRCS "bench_main.c" "bench.c" "bench_timing.c" "bench_send.c" "bench_capture.c" "bench_ring.c"
    PRIV_REQUIRES listener modem lamp phy_s3 esp_timer esp_hw_support freertos)
