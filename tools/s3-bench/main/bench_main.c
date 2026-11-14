/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "bench.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RUN_TX_BENCH 0   /* 1: send a 20-command burst to the lamp at boot */
#define RING_LISTEN  1   /* 1: the always-on listener; 0: one capture at a time */

void app_main(void)
{
    int64_t t0 = esp_timer_get_time();
    phy_s3_init();
    int64_t phy_init_us = esp_timer_get_time() - t0;
    vTaskDelay(pdMS_TO_TICKS(3000));   /* time to attach the host before the one-off TIMING lines */
    printf("TIMING phy_init_us %lld\n", phy_init_us);
    bench_time_tx_path();
    bench_time_demod();
    bench_time_bank_reads();
    if (RUN_TX_BENCH) {
        bench_send();
    }
    if (RING_LISTEN) {
        bench_ring_forever();
    }
    bench_capture_forever();
}
