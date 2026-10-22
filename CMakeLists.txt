# LAYER 2 — ESP-IDF implementation of the Exarcia HAL.
#
# Depends on LAYER 1 (exarcia_ota) for the interface types only, and contains no
# protocol logic. See include/exarcia_hal_espidf.h for the boot-slot invariant
# this component must uphold.

idf_component_register(
    SRCS
        "src/exarcia_hal_espidf.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        exarcia_ota
    PRIV_REQUIRES
        app_update           # esp_ota_ops.h
        esp_app_format       # esp_app_desc.h
        esp_event
        esp_http_client
        esp_https_ota
        esp_hw_support       # esp_mac.h
        esp_netif            # esp_netif_sntp.h
        esp_partition        # esp_partition.h
        esp_system           # esp_restart
        esp_timer
        log
        mbedtls              # psa/crypto.h
        nvs_flash
        utils
)