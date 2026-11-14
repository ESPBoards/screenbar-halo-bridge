/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "check.h"
#include "halo_esb.h"
#include "prng.h"

/* Fake address so the real lamp's stays private. The payload/PID pairs are ones
 * the real lamp ACKed; CRCs come from halo-esp32s3, whose CRC it accepted.
 */
#define LAMP 0x1a2b3c4du

static const struct {
    uint8_t flags, value, pid;
    uint16_t crc;
} lamp_commands[] = {
    {0xc5, 0xa4, 0, 0x78b0}, {0xc5, 0xa4, 2, 0xa470}, {0xc5, 0xa4, 3, 0xca10},
    {0xc5, 0xb6, 2, 0x9603}, {0xc5, 0xb6, 3, 0xf863}, {0x44, 0x4c, 1, 0x425f},
    {0xc3, 0x32, 0, 0x3169}, {0xc3, 0x32, 2, 0xeda9},
};

static const uint16_t lamp_ack_crc[4] = {0x63fa, 0x43b8, 0x237e, 0x033c};

static uint16_t crc_field(const uint8_t *bits, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = n - 16; i < n; i++) {
        crc = (uint16_t)((crc << 1) | bits[i]);
    }
    return crc;
}

static void put_bits(uint8_t *bits, size_t *n, uint32_t value, unsigned width)
{
    while (width--) {
        bits[(*n)++] = (value >> width) & 1u;
    }
}

static void test_lamp_vectors(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    for (size_t i = 0; i < sizeof(lamp_commands) / sizeof(lamp_commands[0]); i++) {
        halo_esb_frame_t f = {.address = LAMP, .pid = lamp_commands[i].pid, .length = 2,
                              .payload = {lamp_commands[i].flags, lamp_commands[i].value}};
        size_t n = halo_esb_encode(&f, bits);
        CHECK(n == 81, "command is %zu bits", n);
        CHECK(crc_field(bits, n) == lamp_commands[i].crc, "%02X%02X pid %u: crc %04X, want %04X",
              f.payload[0], f.payload[1], f.pid, crc_field(bits, n), lamp_commands[i].crc);
    }
    for (uint8_t pid = 0; pid < 4; pid++) {
        halo_esb_frame_t ack = {.address = LAMP, .pid = pid, .no_ack = true, .length = 0};
        size_t n = halo_esb_encode(&ack, bits);
        CHECK(n == 65, "ACK is %zu bits", n);
        CHECK(crc_field(bits, n) == lamp_ack_crc[pid], "ACK pid %u: crc %04X, want %04X", pid,
              crc_field(bits, n), lamp_ack_crc[pid]);
    }
}

static void test_preamble(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    halo_esb_frame_t f = {.address = LAMP, .length = 2};
    halo_esb_encode(&f, bits);
    CHECK(bits[0] == 0 && bits[7] == 1 && bits[8] == 0, "want preamble 0x55");
    f.address = 0x92345678u;
    halo_esb_encode(&f, bits);
    CHECK(bits[0] == 1 && bits[7] == 0 && bits[8] == 1, "want preamble 0xAA");
}

static void test_round_trip_and_errors(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    test_srand(1);
    for (int t = 0; t < 2000; t++) {
        halo_esb_frame_t in = {.address = (uint32_t)test_rand() ^ ((uint32_t)test_rand() << 16), .pid = test_rand() & 3,
                               .no_ack = test_rand() & 1, .length = (t & 1) ? 2 : 0,
                               .payload = {(uint8_t)test_rand(), (uint8_t)test_rand()}};
        if (!in.length) {
            in.payload[0] = in.payload[1] = 0;
        }
        size_t n = halo_esb_encode(&in, bits);
        halo_esb_frame_t out;
        bool ok = halo_esb_decode(bits + 8, n - 8, &out);
        CHECK(ok && out.address == in.address && out.pid == in.pid && out.no_ack == in.no_ack &&
                  out.length == in.length && out.payload[0] == in.payload[0] && out.payload[1] == in.payload[1],
              "round trip failed for %08X", in.address);

        /* A flipped bit getting through would mean a wrong command reaching the lamp.
         * One flip can: dropping the length to 0 moves the CRC check onto the
         * payload bytes, which match 1 in 65536 times. That frame has no payload,
         * so it's never a command.
         */
        for (size_t flip = 8; flip < n; flip++) {
            bits[flip] ^= 1;
            bool got = halo_esb_decode(bits + 8, n - 8, &out);
            CHECK(!got || (out.length == 0 && !halo_esb_is_command(&out)), "bit %zu flipped but frame accepted",
                  flip);
            bits[flip] ^= 1;
        }
        CHECK(!halo_esb_decode(bits + 8, n - 9, &out), "truncated frame accepted");
    }
}

static void test_invalid(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    halo_esb_frame_t f = {.address = LAMP, .pid = 4, .length = 2};
    CHECK(halo_esb_encode(&f, bits) == 0, "PID 4 encoded");
    f.pid = 0;
    f.length = 1;
    CHECK(halo_esb_encode(&f, bits) == 0, "length 1 encoded");

    /* Noise can produce a valid CRC; the length check is the second line of defence. */
    size_t n = 0;
    put_bits(bits, &n, LAMP, 32);
    put_bits(bits, &n, 1, 6);
    put_bits(bits, &n, 0, 3);
    put_bits(bits, &n, 0xc5, 8);
    put_bits(bits, &n, halo_esb_crc16(bits, n), 16);
    halo_esb_frame_t out;
    CHECK(!halo_esb_decode(bits, n, &out), "length 1 with valid CRC accepted");
}

static void test_ack_and_command(void)
{
    const halo_esb_frame_t ack = {.address = LAMP, .pid = 2, .no_ack = true};
    const halo_esb_frame_t command = {.address = LAMP, .pid = 2, .length = 2, .payload = {0xc5, 0xa4}};
    CHECK(halo_esb_is_ack(&ack, LAMP, 2), "real ACK missed");
    CHECK(!halo_esb_is_ack(&ack, LAMP, 1) && !halo_esb_is_ack(&ack, LAMP, 3), "wrong PID counted");
    CHECK(!halo_esb_is_ack(&ack, LAMP ^ 1u, 2), "wrong address counted");
    CHECK(!halo_esb_is_ack(&command, LAMP, 2), "a command counted as ACK");
    halo_esb_frame_t f = ack;
    f.no_ack = false;
    CHECK(!halo_esb_is_ack(&f, LAMP, 2), "an empty frame without NO_ACK counted as ACK");

    CHECK(halo_esb_is_command(&command), "command missed");
    CHECK(!halo_esb_is_command(&ack), "an ACK counted as command");
    f = command;
    f.no_ack = true;
    CHECK(!halo_esb_is_command(&f), "a NO_ACK frame counted as command");

    /* And the same after a trip over the air. */
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t n = halo_esb_encode(&ack, bits);
    CHECK(halo_esb_decode(bits + 8, n - 8, &f) && halo_esb_is_ack(&f, LAMP, 2), "decoded ACK not an ACK");
    n = halo_esb_encode(&command, bits);
    CHECK(halo_esb_decode(bits + 8, n - 8, &f) && halo_esb_is_command(&f), "decoded command not a command");
}

int main(void)
{
    test_lamp_vectors();
    test_preamble();
    test_round_trip_and_errors();
    test_invalid();
    test_ack_and_command();
    return check_report("test_esb");
}
