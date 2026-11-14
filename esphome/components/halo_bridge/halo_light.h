/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/core/helpers.h"

#include "halo_bridge.h"

namespace esphome {
namespace halo_bridge {

/* One of the lamp's two lights. Brightness and temperature are shared with the
 * other one: the bridge keeps both in step.
 */
class HaloLight : public light::LightOutput, public Parented<HaloBridge> {
 public:
  void set_zone(Zone zone) { this->zone_ = zone; }
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { this->state_ = state; }
  void write_state(light::LightState *state) override;
  /* Show what the lamp really does. Brightness and mireds < 0: unknown, leave as is. */
  void publish(bool on, float brightness, float mireds);

 protected:
  Zone zone_{ZONE_FRONT};
  light::LightState *state_{nullptr};
  light::LightColorValues published_;
  bool echo_pending_{false};  // the next write_state may be our own publish coming back
};

}  // namespace halo_bridge
}  // namespace esphome
