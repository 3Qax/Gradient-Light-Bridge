idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
                 "../managed_components/espressif__esp-zigbee-lib/include"
                 "../managed_components/espressif__esp-zboss-lib/include"
    PRIV_REQUIRES nvs_flash esp_driver_uart ieee802154
)

set(HUE_EUI_SUFFIX_VALUE "FF:FE:05")
if(DEFINED HUE_EUI_SUFFIX)
    set(HUE_EUI_SUFFIX_VALUE "${HUE_EUI_SUFFIX}")
elseif(DEFINED ENV{HUE_EUI_SUFFIX})
    set(HUE_EUI_SUFFIX_VALUE "$ENV{HUE_EUI_SUFFIX}")
endif()

set(HUE_SERIAL_DEBUG_VALUE "0")
if(DEFINED HUE_SERIAL_DEBUG)
    set(HUE_SERIAL_DEBUG_VALUE "${HUE_SERIAL_DEBUG}")
elseif(DEFINED ENV{HUE_SERIAL_DEBUG})
    set(HUE_SERIAL_DEBUG_VALUE "$ENV{HUE_SERIAL_DEBUG}")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HUE_EUI_SUFFIX="${HUE_EUI_SUFFIX_VALUE}"
    HUE_SERIAL_DEBUG=${HUE_SERIAL_DEBUG_VALUE}
)
