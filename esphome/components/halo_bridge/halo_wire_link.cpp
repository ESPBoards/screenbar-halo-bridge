/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_wire_link.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace halo_bridge {

static const char *const TAG = "halo_bridge";

static constexpr uint32_t RESEND_MS = 300;
static constexpr uint8_t RESENDS = 3;
static constexpr uint32_t BUSY_RETRY_MS = 50;
static constexpr uint8_t BUSY_RETRIES = 20;  // about a second of a full S3 inbox

// Slot order is send order: find the radio first, then tell it the lamp, then pair.
static int slot_of(uint8_t type) {
  switch (type) {
    case HALO_WIRE_MSG_HELLO_REQ:
      return 0;
    case HALO_WIRE_MSG_CONFIG:
      return 1;
    case HALO_WIRE_MSG_PAIR:
      return 2;
    default:
      return -1;
  }
}

void WireLink::queue(const halo_wire_msg_t &m) {
  int slot = slot_of(m.type);
  if (slot >= 0)
    this->slots_[static_cast<size_t>(slot)] = m;
}

bool WireLink::queued(uint8_t type) const {
  int slot = slot_of(type);
  return slot >= 0 && this->slots_[static_cast<size_t>(slot)].type == type;
}

bool WireLink::send_queued(bool online) {
  if (this->in_flight_)
    return false;
  for (auto &q : this->slots_) {
    // While the radio is offline only HELLO_REQ goes out; the rest waits for it.
    if (q.type != 0 && (online || q.type == HALO_WIRE_MSG_HELLO_REQ)) {
      halo_wire_msg_t m = q;
      q.type = 0;
      this->send(m);
      return true;
    }
  }
  return false;
}

void WireLink::send(halo_wire_msg_t m) {
  m.seq = ++this->seq_;
  this->flying_ = m;
  this->in_flight_ = true;
  this->tries_ = 0;
  this->busy_tries_ = 0;
  this->busy_ = false;
  this->write_(this->flying_);
}

void WireLink::write_(const halo_wire_msg_t &m) {
  uint8_t buf[HALO_WIRE_MAX_ENCODED];
  size_t n = halo_wire_encode(&m, buf);
  this->uart_->write_array(buf, n);
  this->sent_at_ = millis();
}

bool WireLink::poll(uint32_t now, halo_wire_msg_t *lost) {
  uint32_t wait = this->busy_ ? BUSY_RETRY_MS : RESEND_MS;
  if (!this->in_flight_ || now - this->sent_at_ <= wait)
    return false;
  if (this->busy_) {
    // It was heard, just not taken yet: doesn't count as unanswered.
    this->busy_ = false;
    this->write_(this->flying_);
    return false;
  }
  if (++this->tries_ <= RESENDS) {
    this->write_(this->flying_);
    return false;
  }
  // HELLO_REQ is how we look for a missing radio; "Radio offline" already said so once.
  if (this->flying_.type == HALO_WIRE_MSG_HELLO_REQ) {
    ESP_LOGD(TAG, "No answer to HELLO_REQ");
  } else {
    ESP_LOGW(TAG, "No answer to request type 0x%02X", this->flying_.type);
  }
  this->in_flight_ = false;
  *lost = this->flying_;
  return true;
}

bool WireLink::on_ack(uint8_t seq, halo_wire_status_t status, halo_wire_msg_t *lost) {
  if (!this->in_flight_ || seq != this->flying_.seq)
    return false;
  if (status == HALO_WIRE_STATUS_BUSY) {
    if (++this->busy_tries_ <= BUSY_RETRIES) {
      this->busy_ = true;
      this->sent_at_ = millis();
      return false;
    }
    ESP_LOGW(TAG, "Radio busy, giving up on request type 0x%02X for now", this->flying_.type);
    this->in_flight_ = false;
    *lost = this->flying_;
    return true;
  }
  if (status != HALO_WIRE_STATUS_OK)
    ESP_LOGW(TAG, "Radio refused request type 0x%02X: %u", this->flying_.type, status);
  this->in_flight_ = false;
  return false;
}

}  // namespace halo_bridge
}  // namespace esphome
