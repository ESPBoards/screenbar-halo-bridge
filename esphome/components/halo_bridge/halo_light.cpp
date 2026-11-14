/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_light.h"

#include "halo_payload.h"

namespace esphome {
namespace halo_bridge {

light::LightTraits HaloLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(HALO_MIRED_COLD);
  traits.set_max_mireds(HALO_MIRED_WARM);
  return traits;
}

void HaloLight::write_state(light::LightState *state) {
  const auto &v = state->current_values;
  // A publish schedules exactly one write_state. Only that one can be the echo:
  // after it, the same values again are a real command.
  if (this->echo_pending_) {
    this->echo_pending_ = false;
    if (v == this->published_)
      return;
  }
  this->parent_->request(this->zone_, v.is_on(), v.get_brightness(), v.get_color_temperature());
}

void HaloLight::publish(bool on, float brightness, float mireds) {
  if (this->state_ == nullptr)
    return;
  // Already showing it: a call would only echo back and spam HA.
  const auto &shown = this->state_->remote_values;
  if (shown.is_on() == on && (brightness < 0 || shown.get_brightness() == brightness) &&
      (mireds < 0 || shown.get_color_temperature() == mireds))
    return;
  auto call = this->state_->make_call();
  call.set_state(on);
  if (brightness >= 0)
    call.set_brightness(brightness);
  if (mireds >= 0)
    call.set_color_temperature(mireds);
  call.set_transition_length(0);
  call.perform();
  this->published_ = this->state_->remote_values;
  this->echo_pending_ = true;
}

}  // namespace halo_bridge
}  // namespace esphome
