/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <array>

#include "esphome/components/uart/uart.h"

#include "halo_wire.h"

namespace esphome {
namespace halo_bridge {

/* The request side of the UART: one request on the wire at a time, resent until
 * the S3 answers. It doesn't know what requests mean, so whatever it gives up on
 * goes back to the caller to decide whether it's still wanted.
 */
class WireLink {
 public:
  explicit WireLink(uart::UARTDevice *uart) : uart_(uart) {}

  /* HELLO_REQ, CONFIG and PAIR wait in a slot of their own; a newer one replaces the one waiting. */
  void queue(const halo_wire_msg_t &m);
  bool queued(uint8_t type) const;
  bool idle() const { return !this->in_flight_; }
  /* Sends the next waiting request, if any may go. False if none did. */
  bool send_queued(bool online);
  /* Puts a request on the wire now. Only while idle(). */
  void send(halo_wire_msg_t m);
  /* Resends when due. True when it gave up, with that request in *lost. */
  bool poll(uint32_t now, halo_wire_msg_t *lost);
  /* True when a BUSY used up its budget, with that request in *lost. */
  bool on_ack(uint8_t seq, halo_wire_status_t status, halo_wire_msg_t *lost);

 protected:
  void write_(const halo_wire_msg_t &m);

  uart::UARTDevice *uart_;
  uint8_t seq_{0};
  std::array<halo_wire_msg_t, 3> slots_{};  // type 0 means empty
  bool in_flight_{false};
  halo_wire_msg_t flying_{};
  uint32_t sent_at_{0};
  uint8_t tries_{0};
  uint8_t busy_tries_{0};
  bool busy_{false};  // the S3 asked us to send the one in flight again
};

}  // namespace halo_bridge
}  // namespace esphome
