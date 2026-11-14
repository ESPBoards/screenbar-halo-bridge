/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <array>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "halo_lamp.h"
#include "halo_wire.h"
#include "halo_wire_link.h"

namespace esphome {
namespace halo_bridge {

enum Zone : uint8_t { ZONE_FRONT = 0, ZONE_BACK = 1 };
static constexpr uint8_t ZONE_COUNT = 2;

class HaloLight;

/* Owns the UART link to the S3. The S3 does the radio work and the retries;
 * this side keeps HA's two lights and the lamp in step.
 */
class HaloBridge : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_preset_address(uint32_t address) { this->preset_address_ = address; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_listen(bool listen) { this->listen_ = listen; }
  void set_gain(uint8_t gain, uint8_t retry_gain) {
    this->gain_ = gain;
    this->retry_gain_ = retry_gain;
  }
  void set_max_attempts(uint8_t n) { this->max_attempts_ = n; }
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_ = s; }
  void set_online_sensor(binary_sensor::BinarySensor *s) { this->online_sensor_ = s; }
  void set_pair_button(button::Button *b) { this->pair_button_ = b; }

  void add_light(HaloLight *light, Zone zone) { this->lights_[zone] = light; }
  /* From a light's write_state. Only records the target: loop() sends it. */
  void request(Zone zone, bool on, float brightness, float mireds);
  void start_pairing();

 protected:
  void receive_(const halo_wire_msg_t &m);
  void on_hello_(const halo_wire_hello_t &h);
  void on_state_(const halo_wire_state_t &s);
  void on_heard_(const halo_wire_heard_t &h);
  void on_heartbeat_(const halo_wire_heartbeat_t &b);
  void give_up_(const halo_wire_msg_t &m);
  void send_next_();
  void ask_hello_();
  void queue_config_();
  void publish_lights_(const halo_known_t &k);
  void check_online_();
  void set_status_(const char *s);
  void set_ready_status_();
  void save_address_(uint32_t address);

  uint32_t preset_address_{0};
  uint32_t address_{0};
  bool address_paired_{false};   // came from flash, not from YAML
  GPIOPin *reset_pin_{nullptr};
  bool listen_{true};
  uint8_t gain_{4}, retry_gain_{6}, max_attempts_{5};
  text_sensor::TextSensor *status_{nullptr};
  binary_sensor::BinarySensor *online_sensor_{nullptr};
  button::Button *pair_button_{nullptr};
  std::array<HaloLight *, ZONE_COUNT> lights_{};
  ESPPreferenceObject pref_;

  halo_wire_rx_t rx_{};
  WireLink link_{this};

  bool compatible_{false};   // a HELLO with our protocol version came in
  int16_t bad_version_{-1};    // the mismatched version we last complained about
  uint8_t reset_reason_{0};  // from the last HELLO, for the restart warning
  bool online_{false};
  bool heard_once_{false};
  uint32_t last_rx_{0}, last_hello_req_{0};
  uint32_t last_reset_{0};  // boot counts as one: the S3 may be starting up with us
  uint32_t uptime_{0};

  /* The next SET: HA's changes on top of what each STATE says the lamp has. */
  halo_lamp_t want_{};
  std::array<bool, ZONE_COUNT> zone_on_{};
  bool dirty_{false};
  bool levels_{false};   // want_ holds real brightness and temperature, not zeros
  bool synced_{false};   // a STATE has come in since boot

  struct Candidate {
    uint32_t address;
    uint8_t changes;     // the first payload, then each that differed from the one before
    uint8_t payload[2];  // the last one, to tell repeats apart
  };
  bool pairing_{false};
  uint32_t pair_started_{0};
  std::array<Candidate, 8> pair_heard_{};
  uint8_t pair_heard_n_{0};
};

class PairButton : public button::Button, public Parented<HaloBridge> {
 protected:
  void press_action() override { this->parent_->start_pairing(); }
};

}  // namespace halo_bridge
}  // namespace esphome
