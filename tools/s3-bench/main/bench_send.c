/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "bench.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "halo_gfsk.h"
#include "halo_payload.h"

/* Git-ignored so the real lamp address never lands in the project. It holds
 * one line: #define BENCH_LAMP_ADDRESS 0x1A2B3C4Du
 */
#if __has_include("bench_local.h")
#include "bench_local.h"
#endif

#ifdef BENCH_LAMP_ADDRESS
#define BENCH_COMMANDS 20
#define BENCH_ATTEMPTS 5
#define BENCH_GAIN     8     /* the only gain halo-esp32s3 ever tested */
#define RETRY_DELAY_MS 100   /* halo-esp32s3's, so results compare with it */

typedef struct {
    bool ack;
    unsigned attempts;
    uint32_t turnaround_us, ack_start_us;
    float ack_offset_hz;
    int64_t total_us;
} result_t;

/* One command the way halo-esp32s3 sends it: settle RX, then up to N tries
 * with the same PID, then settle RX again.
 */
static result_t send_command(uint8_t level, uint8_t pid)
{
    result_t r = {0};
    int64_t start = esp_timer_get_time();
    halo_payload_t p = {.kind = HALO_PAYLOAD_BRIGHTNESS, .power = true, .front = true, .back = true,
                        .value = halo_brightness_from_level(level)};
    halo_esb_frame_t f = {.address = BENCH_LAMP_ADDRESS, .pid = pid, .length = 2};
    halo_payload_encode(&p, f.payload);
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t nbits = halo_esb_encode(&f, bits);
    static halo_gfsk_t g;
    halo_gfsk_prepare(&g, bits, nbits, BENCH_LEAD_SILENCE);

    phy_s3_reset_rx();
    for (unsigned a = 0; a < BENCH_ATTEMPTS && !r.ack; a++) {
        if (a) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
        phy_s3_tx_result_t tx = phy_s3_send(bench_gfsk_fill, &g, BENCH_GAIN);
        r.attempts++;
        r.turnaround_us = tx.turnaround_us;
        if (!tx.captured) {
            continue;
        }
        size_t n = halo_fm_demod(phy_s3_capture(), PHY_S3_CAPTURE_SAMPLES, bench_hz, BENCH_NFM);
        size_t k = halo_detect(bench_hz, n, bench_found, BENCH_FOUND);
        const halo_detection_t *ack = halo_detect_find_ack(bench_found, k, BENCH_LAMP_ADDRESS, pid);
        if (ack) {
            r.ack = true;
            r.ack_start_us = ack->start_us;
            r.ack_offset_hz = ack->offset_hz;
        }
    }
    phy_s3_reset_rx();
    r.total_us = esp_timer_get_time() - start;
    return r;
}

void bench_send(void)
{
    uint8_t pid = esp_random() & 3;   /* random start: a repeat of the lamp's last PID would be dropped */
    unsigned acks = 0, first_try = 0, tries = 0;
    for (unsigned c = 0; c < BENCH_COMMANDS; c++) {
        uint8_t level = c % 2 ? 133 : 128;   /* ~50 % and ~52 %, so the lamp barely changes */
        result_t r = send_command(level, pid);
        printf("SEND %u level %u pid %u %s tries %u turnaround %lu us ack_at %lu us offset %.0f total %lld us\n", c,
               level, pid, r.ack ? "ACK" : "NO_ACK", r.attempts, (unsigned long)r.turnaround_us,
               (unsigned long)r.ack_start_us, r.ack_offset_hz, r.total_us);
        acks += r.ack;
        first_try += r.ack && r.attempts == 1;
        tries += r.attempts;
        pid = (pid + 1) & 3;   /* new command, new PID */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("SENDSUMMARY commands %u ack %u first_try %u avg_tries %.2f\n", BENCH_COMMANDS, acks, first_try,
           (double)tries / BENCH_COMMANDS);
}
#else
void bench_send(void)
{
    printf("SEND skipped: no BENCH_LAMP_ADDRESS in bench_local.h\n");
}
#endif
