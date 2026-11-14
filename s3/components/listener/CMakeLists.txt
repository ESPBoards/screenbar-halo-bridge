# SPDX-License-Identifier: GPL-3.0-only
idf_component_register(
    SRCS "src/listener.c"
    INCLUDE_DIRS "include"
    REQUIRES modem
    PRIV_REQUIRES phy_s3 esp_timer esp_hw_support freertos)
