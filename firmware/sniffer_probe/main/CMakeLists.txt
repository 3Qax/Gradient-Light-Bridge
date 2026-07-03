idf_component_register(SRCS "sniffer_probe.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer ieee802154 nvs_flash)
