# SPDX-License-Identifier: GPL-3.0-only
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import light
from esphome.const import (
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_ID,
    CONF_LIGHT,
    CONF_OUTPUT_ID,
    CONF_PLATFORM,
)

from . import CONF_HALO_BRIDGE_ID, HaloBridge, halo_ns

DEPENDENCIES = ["halo_bridge"]

CONF_ZONE = "zone"

HaloLight = halo_ns.class_("HaloLight", light.LightOutput, cg.Parented.template(HaloBridge))
Zone = halo_ns.enum("Zone")
ZONES = {"front": Zone.ZONE_FRONT, "back": Zone.ZONE_BACK}

CONFIG_SCHEMA = light.light_schema(HaloLight, light.LightType.BRIGHTNESS_ONLY).extend(
    {
        cv.GenerateID(CONF_HALO_BRIDGE_ID): cv.use_id(HaloBridge),
        cv.Required(CONF_ZONE): cv.enum(ZONES, lower=True),
        # No fades: every transition step would be a radio frame.
        cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds,
    }
)


def _one_light_per_zone(config):
    # The bridge holds one light per zone; a second would silently replace the first.
    lights = [
        c for c in fv.full_config.get().get(CONF_LIGHT, []) if c.get(CONF_PLATFORM) == "halo_bridge"
    ]
    for other in lights:
        if other[CONF_ID] == config[CONF_ID]:
            break
        if other[CONF_HALO_BRIDGE_ID] == config[CONF_HALO_BRIDGE_ID] and other[CONF_ZONE] == config[CONF_ZONE]:
            raise cv.Invalid(f"zone '{config[CONF_ZONE]}' already has a light on this bridge", path=[CONF_ZONE])
    return config


FINAL_VALIDATE_SCHEMA = _one_light_per_zone


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    await cg.register_parented(var, config[CONF_HALO_BRIDGE_ID])
    cg.add(var.set_zone(config[CONF_ZONE]))
    bridge = await cg.get_variable(config[CONF_HALO_BRIDGE_ID])
    cg.add(bridge.add_light(var, config[CONF_ZONE]))
