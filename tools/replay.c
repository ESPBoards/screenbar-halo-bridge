/* SPDX-License-Identifier: GPL-3.0-only */
/* Replays raw S3 captures through the real demodulator and detector, and
 * shows why a frame wasn't found.
 *
 *   ./build-host/replay serial.log ...
 *
 * Takes a saved serial log from tools/s3-bench's capture mode as it is: each
 * capture sits between "RAW n" and "RAWEND n" lines as 5-digit hex words, and
 * every other line is skipped. A bare hex dump of one capture works too.
 */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_detect.h"
#include "halo_fm.h"

#define SAMPLES 16380u   /* one phy_s3 capture, PHY_S3_CAPTURE_SAMPLES */
#define NFM     (SAMPLES / HALO_FM_DECIMATION)
#define SPB     HALO_FM_SPB

static uint32_t raw[SAMPLES];
static float hz[NFM];

/* Where does anything that looks like a preamble sit, and how big is its swing?
 * The detector wants 8 alternations of at least 80 kHz; this relaxes that to see
 * near misses.
 */
static void near_misses(size_t n)
{
    float best_swing = 0;
    size_t best_at = 0;
    float best_offset = 0;
    for (size_t p = 0; p + SPB / 2 + 8 * SPB < n; p++) {
        float sum = 0;
        for (unsigned k = 0; k < 8; k++) {
            sum += hz[p + SPB / 2 + k * SPB];
        }
        float mean = sum / 8, min_swing = 1e9f, prev = 0;
        bool alternates = true;
        for (unsigned k = 0; k < 8; k++) {
            float d = hz[p + SPB / 2 + k * SPB] - mean;
            if (k && (d > 0) == (prev > 0)) {
                alternates = false;
                break;
            }
            min_swing = fabsf(d) < min_swing ? fabsf(d) : min_swing;
            prev = d;
        }
        if (alternates && min_swing > best_swing) {
            best_swing = min_swing;
            best_at = p;
            best_offset = mean;
        }
    }
    if (best_swing > 0) {
        printf("  best alternating run: at %zu us, weakest swing %.0f kHz, offset %.0f kHz\n", (best_at + 1) / 2,
               best_swing / 1000, best_offset / 1000);
    } else {
        printf("  nothing alternates for 8 bits\n");
    }
}

/* GFSK never strays more than ~200 kHz from its carrier, so a long stretch whose
 * spread stays inside that band is a Halo-like signal; noise and Wi-Fi swing
 * across the whole +-1 MHz. Tells a cut-off frame from interference.
 */
static void clean_span(size_t n)
{
    const size_t win = 32;   /* 16 us */
    size_t run = 0, best = 0, best_end = 0;
    for (size_t w = 0; w + win <= n; w += win) {
        float lo = 1e9f, hi = -1e9f;
        for (size_t j = w; j < w + win; j++) {
            lo = hz[j] < lo ? hz[j] : lo;
            hi = hz[j] > hi ? hz[j] : hi;
        }
        run = hi - lo < 450000 ? run + 1 : 0;
        if (run > best) {
            best = run;
            best_end = w + win;
        }
    }
    size_t us = best * win / 2, end_us = best_end / 2;
    printf("  longest GFSK-like stretch: %zu us, %zu..%zu us%s\n", us, end_us - us, end_us,
           us >= 200 && (end_us - us < 20 || end_us > 1000) ? "  <- touches a capture edge" : "");
}

static void replay(const char *name, size_t count)
{
    printf("%s: %zu samples\n", name, count);
    if (count < SAMPLES) {
        return;
    }
    size_t n = halo_fm_demod(raw, count, hz, NFM);

    float lo = 1e9f, hi = -1e9f;
    for (size_t j = 0; j < n; j++) {
        lo = hz[j] < lo ? hz[j] : lo;
        hi = hz[j] > hi ? hz[j] : hi;
    }
    printf("  frequency range %.0f .. %.0f kHz\n", lo / 1000, hi / 1000);

    halo_detection_t found[8];
    size_t k = halo_detect(hz, n, found, 8);
    for (size_t j = 0; j < k; j++) {
        printf("  FRAME %08X len %u pid %u payload %02X%02X offset %.0f kHz at %u us\n", found[j].frame.address,
               found[j].frame.length, found[j].frame.pid, found[j].frame.payload[0], found[j].frame.payload[1],
               found[j].offset_hz / 1000, found[j].start_us);
    }
    if (!k) {
        near_misses(n);
    }
    clean_span(n);
}

static unsigned hex_value(char c)
{
    return isdigit((unsigned char)c) ? (unsigned)(c - '0') : (unsigned)(tolower((unsigned char)c) - 'a' + 10);
}

static bool all_hex(const char *line)
{
    for (; *line; line++) {
        if (!isxdigit((unsigned char)*line)) {
            return false;
        }
    }
    return true;
}

/* Replays every capture in one file; returns false if it can't be read. */
static bool replay_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char line[512], name[600];
    snprintf(name, sizeof name, "%s", path);
    size_t n = 0;
    unsigned digits = 0;
    uint32_t word = 0;
    bool open = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        unsigned id;
        if (sscanf(line, "RAWEND %u", &id) == 1) {
            replay(name, n);
            open = false;
            n = 0;
        } else if (sscanf(line, "RAW %u", &id) == 1) {
            snprintf(name, sizeof name, "%s RAW %u", path, id);
            open = true;
            n = digits = 0;
        } else if (*line && all_hex(line)) {
            open = true;   /* a bare dump has no markers */
            for (const char *c = line; *c && n < SAMPLES; c++) {
                word = word << 4 | hex_value(*c);
                if (++digits == 5) {
                    raw[n++] = word;
                    digits = 0;
                    word = 0;
                }
            }
        }
    }
    if (open) {
        replay(name, n);   /* a bare dump, or a log cut off mid-capture */
    }
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s serial.log ...\n", argv[0]);
        return 2;
    }
    int status = 0;
    for (int a = 1; a < argc; a++) {
        if (!replay_file(argv[a])) {
            perror(argv[a]);
            status = 1;
        }
    }
    return status;
}
