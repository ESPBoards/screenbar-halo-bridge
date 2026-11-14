/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_bridge.h"
#include "halo_light.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>

#include "esphome/core/log.h"

#include "halo_payload.h"

namespace esphome {
namespace halo_bridge {

static const char *const TAG = "halo_bridge";

static constexpr uint32_t PREF_KEY = 0x4A1B0001u;  // the paired lamp address
static constexpr uint32_t OFFLINE_MS = 3500;     // three missed heartbeats
static constexpr uint32_t HELLO_EVERY_MS = 2000; // until the radio is online and has sent a STATE
static constexpr uint32_t RESET_EVERY_MS = 15000;
static constexpr uint32_t RESET_PULSE_MS = 100;
static constexpr uint8_t PAIR_SECONDS = 30;
static constexpr uint8_t PAIR_CHANGES = 3;  // the remote repeats each press, so a nearby press or a stray decode isn't enough
// What the remote broadcasts while it's being paired. Valid frames, but never a lamp's address.
static constexpr uint32_t PAIRING_BEACON = 0x590100B0;

void HaloBridge::setup() {
  halo_wire_rx_init(&this->rx_);
  this->pref_ = global_preferences->make_preference<uint32_t>(PREF_KEY);
  uint32_t saved = 0;
  this->address_paired_ = (this->pref_.load(&saved) && saved != 0);
  this->address_ = this->address_paired_ ? saved : this->preset_address_;
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->digital_write(true);
  }
  if (this->online_sensor_ != nullptr)
    this->online_sensor_->publish_state(false);
  this->set_status_("waiting for the radio");
  // An S3 that's already running won't say HELLO by itself: ask right away.
  this->ask_hello_();
}

void HaloBridge::dump_config() {
  const char *source = "none: press Pair";
  if (this->address_paired_) {
    source = this->preset_address_ && this->preset_address_ != this->address_ ? "paired, overrides YAML" : "paired";
  } else if (this->address_) {
    source = "from config";
  }
  ESP_LOGCONFIG(TAG,
                "Halo bridge:\n"
                "  Lamp address: %08" PRIX32 " (%s)\n"
                "  Listen: %s\n"
                "  Gain: %u\n"
                "  Retry gain: %u\n"
                "  Max attempts: %u",
                this->address_, source, YESNO(this->listen_), this->gain_, this->retry_gain_, this->max_attempts_);
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  LOG_TEXT_SENSOR("  ", "Status", this->status_);
  LOG_BINARY_SENSOR("  ", "Online", this->online_sensor_);
  LOG_BUTTON("  ", "Pair", this->pair_button_);
}

void HaloBridge::loop() {
  uint8_t byte;
  halo_wire_msg_t m;
  while (this->available() && this->read_byte(&byte)) {
    if (halo_wire_rx(&this->rx_, byte, &m))
      this->receive_(m);
  }
  this->check_online_();

  uint32_t now = millis();
  halo_wire_msg_t lost;
  if (this->link_.poll(now, &lost))
    this->give_up_(lost);
  if (this->pairing_ && now - this->pair_started_ >= PAIR_SECONDS * 1000u) {
    this->pairing_ = false;
    if (this->online_)  // offline says so already, and that's the reason
      this->set_status_(this->pair_heard_n_ == 0 ? "pairing: no remote heard"
                                                 : "pairing: heard a remote, but too few changes");
  }
  this->send_next_();
}

void HaloBridge::check_online_() {
  uint32_t now = millis();
  bool online = this->compatible_ && now - this->last_rx_ < OFFLINE_MS;
  if (online != this->online_) {
    this->online_ = online;
    if (this->online_sensor_ != nullptr)
      this->online_sensor_->publish_state(online);
    if (!online) {
      ESP_LOGW(TAG, "Radio offline");
      this->set_status_(this->bad_version_ >= 0 ? "radio firmware mismatch" : "radio offline");
    } else {
      this->set_ready_status_();   // the link can come back without a restart, so without a HELLO
    }
  }
  // The S3's boot HELLO comes without a STATE; only the answer to ours brings one.
  if (online && this->synced_)
    return;
  if (now - this->last_hello_req_ > HELLO_EVERY_MS)
    this->ask_hello_();
  // A hung S3 comes back with a reset. Never before we've heard from it once,
  // or a missing radio would be reset forever.
  if (this->reset_pin_ != nullptr && this->heard_once_ && now - this->last_rx_ > OFFLINE_MS &&
      now - this->last_reset_ > RESET_EVERY_MS) {
    this->last_reset_ = now;
    ESP_LOGW(TAG, "Resetting the radio");
    this->reset_pin_->digital_write(false);
    this->set_timeout("reset", RESET_PULSE_MS, [this]() { this->reset_pin_->digital_write(true); });
  }
}

void HaloBridge::ask_hello_() {
  this->last_hello_req_ = millis();
  halo_wire_msg_t m;
  halo_wire_put_empty(&m, HALO_WIRE_MSG_HELLO_REQ);
  this->link_.queue(m);
}

void HaloBridge::give_up_(const halo_wire_msg_t &m) {
  // Still wanted: goes again once the radio answers. A newer one already queued wins.
  if (m.type == HALO_WIRE_MSG_SET) {
    this->dirty_ = true;
    return;
  }
  if (m.type == HALO_WIRE_MSG_HELLO_REQ || this->link_.queued(m.type))
    return;
  // A pairing start is stale once our window has closed: we'd ignore what it hears.
  uint8_t seconds;
  if (halo_wire_get_pair(&m, &seconds) && seconds > 0 && !this->pairing_)
    return;
  this->link_.queue(m);
}

/* Waiting requests go first; the lamp target goes once nothing else is waiting. */
void HaloBridge::send_next_() {
  if (!this->link_.idle() || this->link_.send_queued(this->online_))
    return;
  if (!this->dirty_ || !this->online_ || !this->address_)
    return;
  halo_wire_msg_t m;
  halo_wire_put_set(&m, &this->want_);
  this->dirty_ = false;
  this->link_.send(m);
}

void HaloBridge::receive_(const halo_wire_msg_t &m) {
  this->last_rx_ = millis();
  this->heard_once_ = true;
  // Until a HELLO says the versions match, nothing else means anything.
  if (!this->compatible_ && m.type != HALO_WIRE_MSG_ACK && m.type != HALO_WIRE_MSG_HELLO)
    return;
  switch (m.type) {
    case HALO_WIRE_MSG_ACK: {
      halo_wire_status_t status;
      halo_wire_msg_t lost;
      if (halo_wire_get_ack(&m, &status) && this->link_.on_ack(m.seq, status, &lost))
        this->give_up_(lost);
      break;
    }
    case HALO_WIRE_MSG_HELLO: {
      halo_wire_hello_t hello;
      if (halo_wire_get_hello(&m, &hello))
        this->on_hello_(hello);
      break;
    }
    case HALO_WIRE_MSG_STATE: {
      halo_wire_state_t state;
      if (halo_wire_get_state(&m, &state))
        this->on_state_(state);
      break;
    }
    case HALO_WIRE_MSG_HEARD: {
      halo_wire_heard_t heard;
      if (halo_wire_get_heard(&m, &heard))
        this->on_heard_(heard);
      break;
    }
    case HALO_WIRE_MSG_HEARTBEAT: {
      halo_wire_heartbeat_t beat;
      if (halo_wire_get_heartbeat(&m, &beat))
        this->on_heartbeat_(beat);
      break;
    }
    default:
      ESP_LOGV(TAG, "Ignoring message type 0x%02X", m.type);
      break;
  }
}

void HaloBridge::on_hello_(const halo_wire_hello_t &h) {
  if (h.version != HALO_WIRE_VERSION) {
    this->compatible_ = false;
    if (h.version != this->bad_version_) {
      this->bad_version_ = h.version;
      ESP_LOGE(TAG, "Radio speaks protocol %u, we speak %u: update the S3 firmware", h.version, HALO_WIRE_VERSION);
    }
    this->set_status_("radio firmware mismatch");
    return;
  }
  this->bad_version_ = -1;
  ESP_LOGD(TAG, "Radio: protocol %u, firmware %u, reset reason %u", h.version, h.firmware, h.reset_reason);
  // Restarts are told by the heartbeat's uptime going backwards: a HELLO also
  // answers our own HELLO_REQ, and we keep asking until the radio is online and synced.
  this->reset_reason_ = h.reset_reason;
  this->compatible_ = true;
  // The S3 keeps nothing across a restart; it only ever learns the address from us.
  this->queue_config_();
  this->set_ready_status_();
}

void HaloBridge::on_heartbeat_(const halo_wire_heartbeat_t &b) {
  if (b.uptime_s < this->uptime_) {
    ESP_LOGW(TAG, "Radio restarted (reset reason %u)", this->reset_reason_);
    this->queue_config_();  // it keeps nothing, so it lost the lamp address
    uint32_t elapsed = millis() - this->pair_started_;
    if (this->pairing_ && elapsed < PAIR_SECONDS * 1000u) {  // and our pairing window
      halo_wire_msg_t m;
      halo_wire_put_pair(&m, static_cast<uint8_t>((PAIR_SECONDS * 1000u - elapsed) / 1000 + 1));
      this->link_.queue(m);
    }
  }
  this->uptime_ = b.uptime_s;
  ESP_LOGV(TAG, "Radio: up %" PRIu32 " s, %" PRIu32 " sent, %" PRIu32 " acked, %u failed, heard %.1f%%", b.uptime_s,
           b.sent, b.acked, b.failed, b.heard_permille / 10.0f);
}

void HaloBridge::queue_config_() {
  halo_wire_config_t c = {this->address_, this->gain_, this->retry_gain_, this->max_attempts_, this->listen_};
  halo_wire_msg_t m;
  halo_wire_put_config(&m, &c);
  this->link_.queue(m);
}

void HaloBridge::on_state_(const halo_wire_state_t &s) {
  this->synced_ = true;
  // The next SET carries both levels even when HA only turns a zone off, so
  // keep the lamp's, unless HA has a change still to send.
  if (!this->dirty_) {
    if (s.known.brightness_known)
      this->want_.brightness = s.known.lamp.brightness;
    if (s.known.temperature_known)
      this->want_.temperature = s.known.lamp.temperature;
    this->levels_ |= s.known.brightness_known && s.known.temperature_known;
  }
  switch (s.event) {
    case HALO_WIRE_EVENT_FAILED:
      ESP_LOGW(TAG, "The lamp didn't answer; showing what it last confirmed");
      if (!this->pairing_)  // logged above; the pairing prompt matters more
        this->set_status_("lamp didn't answer");
      break;
    case HALO_WIRE_EVENT_DONE:
      this->set_ready_status_();
      break;
    case HALO_WIRE_EVENT_REMOTE:
    case HALO_WIRE_EVENT_CANCELLED:
      ESP_LOGD(TAG, "Changed with the remote");
      break;
    default:
      break;
  }
  this->publish_lights_(s.known);
}

void HaloBridge::publish_lights_(const halo_known_t &k) {
  float brightness = k.brightness_known ? halo_brightness_to_level(k.lamp.brightness) / 255.0f : -1.0f;
  float mireds = k.temperature_known ? static_cast<float>(halo_temperature_to_mired(k.lamp.temperature)) : -1.0f;
  const bool lit[ZONE_COUNT] = {k.lamp.front, k.lamp.back};
  for (size_t z = 0; z < ZONE_COUNT; z++) {
    // Unknown lights: keep showing what HA last asked for.
    if (k.lights_known)
      this->zone_on_[z] = k.lamp.power && lit[z];
    if (this->lights_[z] != nullptr)
      this->lights_[z]->publish(this->zone_on_[z], brightness, mireds);
  }
}

void HaloBridge::request(Zone zone, bool on, float brightness, float mireds) {
  if (!this->synced_) {
    // Right after boot this is HA's restored state, not a request: the lamp may have moved since.
    ESP_LOGD(TAG, "Not synced with the radio yet, ignoring a light change");
    return;
  }
  this->zone_on_[zone] = on;
  // The lamp has one brightness and one temperature: the light just changed sets
  // them for both. Turning one off sends them too, so until we know the lamp's,
  // take that light's (HA keeps them while it's off).
  if (on || !this->levels_) {
    this->levels_ = true;
    long level = std::clamp(std::lround(brightness * 255.0f), 1L, 255L);
    this->want_.brightness = halo_brightness_from_level(static_cast<uint8_t>(level));
    this->want_.temperature = halo_temperature_from_mired(static_cast<uint16_t>(std::lround(mireds)));
  }
  // Both off goes out as "neither lit": the S3 turns that into power off and keeps the lamp's lights.
  this->want_.front = this->zone_on_[ZONE_FRONT];
  this->want_.back = this->zone_on_[ZONE_BACK];
  this->want_.power = this->want_.front || this->want_.back;
  this->dirty_ = true;
}

void HaloBridge::start_pairing() {
  if (!this->online_) {
    this->set_status_("pairing needs the radio online");
    return;
  }
  this->pair_heard_n_ = 0;
  this->pairing_ = true;
  this->pair_started_ = millis();
  halo_wire_msg_t m;
  halo_wire_put_pair(&m, PAIR_SECONDS);
  this->link_.queue(m);
  this->set_ready_status_();
}

void HaloBridge::on_heard_(const halo_wire_heard_t &h) {
  if (!this->pairing_ || h.address == PAIRING_BEACON)
    return;
  Candidate *c = nullptr;
  for (uint8_t i = 0; i < this->pair_heard_n_; i++) {
    if (this->pair_heard_[i].address == h.address) {
      c = &this->pair_heard_[i];
      break;
    }
  }
  if (c == nullptr) {
    // Out of room: drop the newcomer rather than forget a count.
    if (this->pair_heard_n_ == this->pair_heard_.size())
      return;
    c = &this->pair_heard_[this->pair_heard_n_++];
    *c = {h.address, 0, {}};
  } else if (h.payload[0] == c->payload[0] && h.payload[1] == c->payload[1]) {
    return;  // a repeat
  }
  c->payload[0] = h.payload[0];
  c->payload[1] = h.payload[1];
  if (++c->changes < PAIR_CHANGES)
    return;
  this->pairing_ = false;
  halo_wire_msg_t m;
  halo_wire_put_pair(&m, 0);
  this->link_.queue(m);
  this->save_address_(h.address);
  this->queue_config_();
  char buf[32];
  snprintf(buf, sizeof buf, "paired: %08" PRIX32, h.address);
  ESP_LOGI(TAG, "Paired with lamp %08" PRIX32, h.address);
  this->set_status_(buf);
}

void HaloBridge::save_address_(uint32_t address) {
  this->address_ = address;
  this->address_paired_ = true;
  this->pref_.save(&address);
  global_preferences->sync();
}

void HaloBridge::set_ready_status_() {
  if (this->pairing_) {
    this->set_status_("pairing: turn the remote's dial now");
  } else {
    this->set_status_(this->address_ ? "ready" : "no lamp address: press Pair");
  }
}

void HaloBridge::set_status_(const char *s) {
  if (this->status_ != nullptr && this->status_->get_state() != s)
    this->status_->publish_state(s);
}

}  // namespace halo_bridge
}  // namespace esphome
