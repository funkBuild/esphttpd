idf_component_register(SRCS "esphttpd.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mbedtls  esp_ringbuf esp_timer)
