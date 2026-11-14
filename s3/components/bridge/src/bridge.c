/* SPDX-License-Identifier: GPL-3.0-only */
#include "bridge.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "halo_wire.h"
#include "listener.h"
#include "radio.h"

#ifdef CONFIG_HALO_BRIDGE_USB
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define UART UART_NUM_1
#define BAUD 115200   /* fixed: the C3 component only accepts this rate */
#endif

#define FIRMWARE          1u      /* in HELLO, so the C3 can tell builds apart */
#define OUTBOX            24
#define RX_BUFFER         512
#define TX_BUFFER         1024
#define READ_CHUNK        64
#define READ_WAIT_MS      5       /* the longest a request or a queued message waits for us */
#define USB_WRITE_WAIT_MS 20      /* a PC that stopped reading mustn't stall the task for long */
#define HEARTBEAT_US      1000000

#define TASK_STACK    4096
#define TASK_CORE     0
/* Above the listener's worker, which can demod for 50 ms at a stretch: the C3
 * shouldn't wait that long for an ACK.
 */
#define TASK_PRIORITY (LISTENER_WORKER_PRIORITY + 1)

static const char *TAG = "bridge";

static QueueHandle_t outbox;
static _Atomic uint32_t dropped_out;   /* bumped by the radio task and the listener's worker */
static uint8_t next_seq;

static void wire_init(void)
{
#ifdef CONFIG_HALO_BRIDGE_USB
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = RX_BUFFER, .tx_buffer_size = TX_BUFFER};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
#else
    uart_config_t cfg = {.baud_rate = BAUD,
                         .data_bits = UART_DATA_8_BITS,
                         .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                         .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(UART, RX_BUFFER, TX_BUFFER, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART, CONFIG_HALO_BRIDGE_UART_TX, CONFIG_HALO_BRIDGE_UART_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
#endif
}

static int wire_read(uint8_t *buf, size_t n, TickType_t wait)
{
#ifdef CONFIG_HALO_BRIDGE_USB
    return usb_serial_jtag_read_bytes(buf, n, wait);
#else
    return uart_read_bytes(UART, buf, n, wait);
#endif
}

static void wire_write(const uint8_t *buf, size_t n)
{
#ifdef CONFIG_HALO_BRIDGE_USB
    usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(USB_WRITE_WAIT_MS));
#else
    uart_write_bytes(UART, buf, n);
#endif
}

static void write_msg(halo_wire_msg_t *m, bool own_seq)
{
    if (own_seq) {
        m->seq = next_seq++;
    }
    uint8_t buf[HALO_WIRE_MAX_ENCODED];
    wire_write(buf, halo_wire_encode(m, buf));
}

static void queue_msg(const halo_wire_msg_t *m)
{
    if (xQueueSend(outbox, m, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&dropped_out, 1, memory_order_relaxed);
    }
}

void bridge_state(halo_link_event_t event, const halo_known_t *known)
{
    static const halo_wire_event_t map[] = {
        [HALO_LINK_NONE] = HALO_WIRE_EVENT_SYNC,
        [HALO_LINK_DONE] = HALO_WIRE_EVENT_DONE,
        [HALO_LINK_FAILED] = HALO_WIRE_EVENT_FAILED,
        [HALO_LINK_REMOTE] = HALO_WIRE_EVENT_REMOTE,
        [HALO_LINK_CANCELLED] = HALO_WIRE_EVENT_CANCELLED,
    };
    halo_wire_msg_t m;
    halo_wire_put_state(&m, &(halo_wire_state_t){.event = map[event], .known = *known});
    queue_msg(&m);
}

void bridge_heard(const halo_esb_frame_t *frame, float offset_hz)
{
    halo_wire_msg_t m;
    halo_wire_put_heard(&m, &(halo_wire_heard_t){.address = frame->address,
                                                 .pid = frame->pid,
                                                 .payload = {frame->payload[0], frame->payload[1]},
                                                 .offset_khz = (int16_t)(offset_hz / 1000)});
    queue_msg(&m);
}

static void send_hello(void)
{
    halo_wire_msg_t m;
    halo_wire_put_hello(&m, &(halo_wire_hello_t){.version = HALO_WIRE_VERSION,
                                                 .firmware = FIRMWARE,
                                                 .reset_reason = (uint8_t)esp_reset_reason(),
                                                 .address = radio_address()});
    write_msg(&m, true);
}

static void ack(uint8_t seq, halo_wire_status_t status)
{
    halo_wire_msg_t m;
    halo_wire_put_ack(&m, status);
    m.seq = seq;
    write_msg(&m, false);
}

static void handle(const halo_wire_msg_t *m)
{
    int len = halo_wire_payload_len(m->type);
    if (len < 0) {
        ack(m->seq, HALO_WIRE_STATUS_UNKNOWN_TYPE);
        return;
    }
    if (m->len != len) {
        ack(m->seq, HALO_WIRE_STATUS_BAD_LENGTH);
        return;
    }
    bool queued = true;
    switch (m->type) {
    case HALO_WIRE_MSG_HELLO_REQ:
        /* The C3 is (re)connecting and needs a STATE. Queue that first: if the
         * radio can't take it, BUSY makes the C3 ask again.
         */
        if (!radio_sync()) {
            ack(m->seq, HALO_WIRE_STATUS_BUSY);
            return;
        }
        ack(m->seq, HALO_WIRE_STATUS_OK);
        send_hello();
        return;
    case HALO_WIRE_MSG_CONFIG: {
        halo_wire_config_t c;
        halo_wire_get_config(m, &c);
        queued = radio_config(&(radio_config_t){.address = c.address,
                                                .gain = c.gain,
                                                .retry_gain = c.retry_gain,
                                                .max_attempts = c.max_attempts,
                                                .listen = c.listen});
        break;
    }
    case HALO_WIRE_MSG_SET: {
        halo_lamp_t t;
        halo_wire_get_set(m, &t);
        queued = radio_set(&t);
        break;
    }
    case HALO_WIRE_MSG_PAIR: {
        uint8_t s;
        halo_wire_get_pair(m, &s);
        queued = radio_pair(s);
        break;
    }
    default:   /* a known type, but one only we send */
        ack(m->seq, HALO_WIRE_STATUS_UNKNOWN_TYPE);
        return;
    }
    ack(m->seq, queued ? HALO_WIRE_STATUS_OK : HALO_WIRE_STATUS_BUSY);
}

static void send_heartbeat(const halo_wire_rx_t *rx)
{
    static uint32_t segments_was, dropped_was, lost_was;
    static int64_t at_was;
    listener_stats_t l;
    radio_stats_t r;
    listener_stats(&l);
    radio_stats(&r);
    int64_t now = esp_timer_get_time();
    /* Each gate segment is 32 us of air the listener checked. */
    uint32_t heard_us = (l.listen.segments - segments_was) * (HALO_LISTEN_SEG / 2);
    uint32_t permille = at_was ? (uint32_t)((int64_t)heard_us * 1000 / (now - at_was)) : 0;
    segments_was = l.listen.segments;
    at_was = now;
    /* The heartbeat has no field for these, so they go to the log, at most once a beat. */
    uint32_t dropped = atomic_load_explicit(&dropped_out, memory_order_relaxed);
    if (dropped != dropped_was || r.lost != lost_was) {
        ESP_LOGW(TAG, "since boot: %lu messages for the C3 dropped, %lu remote frames lost", (unsigned long)dropped,
                 (unsigned long)r.lost);
        dropped_was = dropped;
        lost_was = r.lost;
    }
    halo_wire_msg_t m;
    halo_wire_put_heartbeat(&m,
                            &(halo_wire_heartbeat_t){.uptime_s = (uint32_t)(now / 1000000),
                                                     .sent = r.sent,
                                                     .acked = r.acked,
                                                     .failed = r.failed,
                                                     .heard_permille = (uint16_t)(permille > 1000 ? 1000 : permille),
                                                     .dropped = (uint16_t)l.listen.dropped,
                                                     .restarts = (uint16_t)(l.late + l.stale),
                                                     .wire_bad = (uint16_t)rx->bad});
    write_msg(&m, true);
}

static void bridge_task(void *arg)
{
    (void)arg;
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    send_hello();
    int64_t beat = esp_timer_get_time();
    for (;;) {
        esp_task_wdt_reset();
        uint8_t buf[READ_CHUNK];
        int n = wire_read(buf, sizeof buf, pdMS_TO_TICKS(READ_WAIT_MS));
        halo_wire_msg_t m;
        for (int k = 0; k < n; k++) {
            if (halo_wire_rx(&rx, buf[k], &m)) {
                handle(&m);
            }
        }
        while (xQueueReceive(outbox, &m, 0) == pdTRUE) {
            write_msg(&m, true);
        }
        if (esp_timer_get_time() - beat >= HEARTBEAT_US) {
            beat += HEARTBEAT_US;
            send_heartbeat(&rx);
        }
    }
}

void bridge_init(void)
{
#ifdef CONFIG_HALO_BRIDGE_USB
    esp_log_level_set("*", ESP_LOG_NONE);   /* the protocol has the USB port to itself */
#endif
    outbox = xQueueCreate(OUTBOX, sizeof(halo_wire_msg_t));
    configASSERT(outbox);
}

void bridge_start(void)
{
    wire_init();
    BaseType_t ok = xTaskCreatePinnedToCore(bridge_task, "bridge", TASK_STACK, NULL, TASK_PRIORITY, NULL, TASK_CORE);
    configASSERT(ok == pdPASS);
}
