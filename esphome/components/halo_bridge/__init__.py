# SPDX-License-Identifier: GPL-3.0-only
"""ESP32-C3 side of the Halo bridge: talks to the S3 radio over UART."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, button, text_sensor, uart
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_RESET_PIN,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "button", "light", "text_sensor"]

CONF_HALO_BRIDGE_ID = "halo_bridge_id"
CONF_LISTEN = "listen"
CONF_GAIN = "gain"
CONF_RETRY_GAIN = "retry_gain"
CONF_MAX_ATTEMPTS = "max_attempts"
CONF_STATUS = "status"
CONF_ONLINE = "online"
CONF_PAIR = "pair"

GAIN_MAX = 10  # top of the S3's TX gain table

halo_ns = cg.esphome_ns.namespace("halo_bridge")
HaloBridge = halo_ns.class_("HaloBridge", cg.Component, uart.UARTDevice)
PairButton = halo_ns.class_("PairButton", button.Button, cg.Parented.template(HaloBridge))

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HaloBridge),
            # Only used until a remote is paired: the paired address is kept in flash and wins.
            cv.Optional(CONF_ADDRESS): cv.hex_uint32_t,
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_LISTEN, default=True): cv.boolean,
            # Index into the S3's TX gain table. The S3's radio is driven well outside
            # its normal use to talk to the lamp, so keep it low: at desk distance every
            # gain from 1 to 9 got the same ACK rate. Raise it if the lamp is far away.
            # retry_gain is used once the first attempts went unanswered.
            cv.Optional(CONF_GAIN, default=4): cv.int_range(0, GAIN_MAX),
            cv.Optional(CONF_RETRY_GAIN, default=6): cv.int_range(0, GAIN_MAX),
            cv.Optional(CONF_MAX_ATTEMPTS, default=5): cv.int_range(1, 10),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:radio-tower"
            ),
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PAIR): button.button_schema(
                PairButton, entity_category=ENTITY_CATEGORY_CONFIG, icon="mdi:remote"
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "halo_bridge", baud_rate=115200, require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_ADDRESS in config:
        cg.add(var.set_preset_address(config[CONF_ADDRESS]))
    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(config[CONF_RESET_PIN])))
    cg.add(var.set_listen(config[CONF_LISTEN]))
    cg.add(var.set_gain(config[CONF_GAIN], config[CONF_RETRY_GAIN]))
    cg.add(var.set_max_attempts(config[CONF_MAX_ATTEMPTS]))
    if CONF_STATUS in config:
        cg.add(var.set_status_sensor(await text_sensor.new_text_sensor(config[CONF_STATUS])))
    if CONF_ONLINE in config:
        cg.add(var.set_online_sensor(await binary_sensor.new_binary_sensor(config[CONF_ONLINE])))
    if CONF_PAIR in config:
        b = await button.new_button(config[CONF_PAIR])
        await cg.register_parented(b, var)
        cg.add(var.set_pair_button(b))
