# SPDX-License-Identifier: GPL-3.0-only
idf_component_register(
    SRCS "src/bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES lamp link
    PRIV_REQUIRES wire radio listener esp_timer esp_system esp_driver_uart esp_driver_usb_serial_jtag log
                  freertos)
