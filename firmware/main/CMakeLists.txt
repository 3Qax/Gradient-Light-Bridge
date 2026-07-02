idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
                 "../managed_components/espressif__esp-zigbee-lib/include"
                 "../managed_components/espressif__esp-zboss-lib/include"
    PRIV_REQUIRES nvs_flash esp_driver_uart ieee802154
)
