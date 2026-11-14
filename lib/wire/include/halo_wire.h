/* SPDX-License-Identifier: GPL-3.0-only */
/* The C3 <-> S3 serial link.
 *
 *   frame:  framing 1 | type 1 | seq 1 | len 1 | payload len | crc16 2 (CCITT, over all before it)
 *   wire:   COBS(frame) 0x00
 *
 * COBS means 0x00 only ever ends a frame, so the reader always resyncs at the
 * next one. Multi-byte fields are little-endian. Every C3 request gets an ACK
 * with its seq; the S3 sends everything else on its own.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_lamp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The framing byte only changes if the frame layout itself does. Message
 * versions travel in HELLO instead, so a mismatched peer still gets its HELLO
 * through and can be told apart from a missing one.
 */
#define HALO_WIRE_FRAMING     1u
#define HALO_WIRE_VERSION     1u
#define HALO_WIRE_MAX_PAYLOAD 24u
#define HALO_WIRE_HEADER      4u   /* framing, type, seq, len */
#define HALO_WIRE_CRC         2u
#define HALO_WIRE_MAX_FRAME   (HALO_WIRE_HEADER + HALO_WIRE_MAX_PAYLOAD + HALO_WIRE_CRC)
#define HALO_WIRE_MAX_ENCODED (HALO_WIRE_MAX_FRAME + 2u)   /* one COBS code byte, plus the 0x00 */

typedef enum {
    /* C3 -> S3, each answered with an ACK */
    HALO_WIRE_MSG_HELLO_REQ = 0x01,   /* ACK, HELLO and a STATE; or ACK BUSY alone: ask again */
    HALO_WIRE_MSG_CONFIG = 0x02,
    HALO_WIRE_MSG_SET = 0x03,         /* the lamp should look like this; latest wins */
    HALO_WIRE_MSG_PAIR = 0x04,        /* report every remote command heard, any address, for n seconds */
    /* S3 -> C3 */
    HALO_WIRE_MSG_HELLO = 0x81,       /* at boot and on request */
    HALO_WIRE_MSG_ACK = 0x82,
    HALO_WIRE_MSG_STATE = 0x83,       /* whenever what we know about the lamp changes */
    HALO_WIRE_MSG_HEARD = 0x84,       /* while pairing */
    HALO_WIRE_MSG_HEARTBEAT = 0x85,   /* every second */
} halo_wire_type_t;

typedef enum {
    HALO_WIRE_STATUS_OK,
    HALO_WIRE_STATUS_UNKNOWN_TYPE,
    HALO_WIRE_STATUS_BAD_LENGTH,
    HALO_WIRE_STATUS_BUSY,   /* couldn't queue it; send again */
} halo_wire_status_t;

typedef struct {
    uint8_t type, seq, len;
    uint8_t payload[HALO_WIRE_MAX_PAYLOAD];
} halo_wire_msg_t;

/* Returns the bytes written to out, the 0x00 included. */
size_t halo_wire_encode(const halo_wire_msg_t *m, uint8_t out[HALO_WIRE_MAX_ENCODED]);

typedef struct {
    uint8_t buf[HALO_WIRE_MAX_ENCODED];
    size_t n;
    bool overflow;
    uint32_t frames, bad;         /* bad: CRC, framing byte or length wrong, or too long */
} halo_wire_rx_t;

void halo_wire_rx_init(halo_wire_rx_t *rx);

/* Feed bytes one at a time. True when m holds a whole, checked frame. */
bool halo_wire_rx(halo_wire_rx_t *rx, uint8_t byte, halo_wire_msg_t *m);

/* Payloads. put_* fill m (type, len, payload; seq is the caller's), get_* check
 * type, length and enum values first.
 */
typedef struct {
    uint8_t version;              /* HALO_WIRE_VERSION of the sender */
    uint16_t firmware;
    uint8_t reset_reason;         /* esp_reset_reason_t */
    uint32_t address;             /* 0 until a CONFIG arrives */
} halo_wire_hello_t;

typedef struct {
    uint32_t address;             /* 0: don't send anything */
    uint8_t gain, retry_gain, max_attempts;
    bool listen;                  /* keep listening for the remote when idle */
} halo_wire_config_t;

typedef enum {
    HALO_WIRE_EVENT_SYNC,        /* asked for, or nothing new happened */
    HALO_WIRE_EVENT_DONE,        /* the lamp ACKed what we asked for */
    HALO_WIRE_EVENT_FAILED,      /* no ACK after every try; what it carried is now unknown */
    HALO_WIRE_EVENT_REMOTE,      /* the remote changed the lamp */
    HALO_WIRE_EVENT_CANCELLED,   /* ...while a SET was still being sent */
} halo_wire_event_t;

typedef struct {
    halo_wire_event_t event;
    halo_known_t known;
} halo_wire_state_t;

typedef struct {
    uint32_t address;
    uint8_t pid;
    uint8_t payload[2];
    int16_t offset_khz;
} halo_wire_heard_t;

typedef struct {
    uint32_t uptime_s;
    uint32_t sent, acked;         /* radio frames */
    uint16_t failed;              /* SETs given up on */
    uint16_t heard_permille;      /* time the listener heard, last second */
    uint16_t dropped;             /* listener windows lost, since boot */
    uint16_t restarts;            /* unplanned ring restarts, since boot */
    uint16_t wire_bad;            /* bad frames received, since boot */
} halo_wire_heartbeat_t;

void halo_wire_put_hello(halo_wire_msg_t *m, const halo_wire_hello_t *h);
void halo_wire_put_config(halo_wire_msg_t *m, const halo_wire_config_t *c);
void halo_wire_put_set(halo_wire_msg_t *m, const halo_lamp_t *target);
void halo_wire_put_pair(halo_wire_msg_t *m, uint8_t seconds);
void halo_wire_put_ack(halo_wire_msg_t *m, halo_wire_status_t status);
void halo_wire_put_state(halo_wire_msg_t *m, const halo_wire_state_t *s);
void halo_wire_put_heard(halo_wire_msg_t *m, const halo_wire_heard_t *h);
void halo_wire_put_heartbeat(halo_wire_msg_t *m, const halo_wire_heartbeat_t *h);
void halo_wire_put_empty(halo_wire_msg_t *m, halo_wire_type_t type);

bool halo_wire_get_hello(const halo_wire_msg_t *m, halo_wire_hello_t *h);
bool halo_wire_get_config(const halo_wire_msg_t *m, halo_wire_config_t *c);
bool halo_wire_get_set(const halo_wire_msg_t *m, halo_lamp_t *target);
bool halo_wire_get_pair(const halo_wire_msg_t *m, uint8_t *seconds);
bool halo_wire_get_ack(const halo_wire_msg_t *m, halo_wire_status_t *status);
bool halo_wire_get_state(const halo_wire_msg_t *m, halo_wire_state_t *s);
bool halo_wire_get_heard(const halo_wire_msg_t *m, halo_wire_heard_t *h);
bool halo_wire_get_heartbeat(const halo_wire_msg_t *m, halo_wire_heartbeat_t *h);

/* The payload length each type must have; -1 for unknown types. */
int halo_wire_payload_len(uint8_t type);

#ifdef __cplusplus
}
#endif
