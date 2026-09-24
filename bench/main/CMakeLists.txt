idf_component_register(SRCS "bench.c"
                       INCLUDE_DIRS "."
                       REQUIRES esphttpd
                       PRIV_REQUIRES esp_timer esp_netif vfs esp_hw_support)
# Sink the server's socket writes (see bench.c): esphttpd reaches lwIP through
# lwip_send / lwip_writev, which --wrap redirects for the whole link.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lwip_send" "-Wl,--wrap=lwip_writev")
