# SPDX-License-Identifier: GPL-3.0-only
idf_component_register(
    SRCS "src/radio.c"
    INCLUDE_DIRS "include"
    REQUIRES lamp link
    PRIV_REQUIRES listener modem phy_s3 esp_timer esp_hw_support esp_system log freertos)
