idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esphttpd
        nvs_flash
        esp_event
        esp_netif
        esp_timer
        esp_eth
        driver
)