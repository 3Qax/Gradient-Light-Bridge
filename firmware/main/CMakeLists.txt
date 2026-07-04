idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
                 "../managed_components/espressif__esp-zigbee-lib/include"
                 "../managed_components/espressif__esp-zboss-lib/include"
    PRIV_REQUIRES nvs_flash esp_driver_uart ieee802154
)

set(ARGB_EUI_SUFFIX_VALUE "FF:FE:05")
if(DEFINED ARGB_EUI_SUFFIX)
    set(ARGB_EUI_SUFFIX_VALUE "${ARGB_EUI_SUFFIX}")
elseif(DEFINED ENV{ARGB_EUI_SUFFIX})
    set(ARGB_EUI_SUFFIX_VALUE "$ENV{ARGB_EUI_SUFFIX}")
endif()

set(ARGB_SERIAL_DEBUG_VALUE "0")
if(DEFINED ARGB_SERIAL_DEBUG)
    set(ARGB_SERIAL_DEBUG_VALUE "${ARGB_SERIAL_DEBUG}")
elseif(DEFINED ENV{ARGB_SERIAL_DEBUG})
    set(ARGB_SERIAL_DEBUG_VALUE "$ENV{ARGB_SERIAL_DEBUG}")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARGB_EUI_SUFFIX="${ARGB_EUI_SUFFIX_VALUE}"
    ARGB_SERIAL_DEBUG=${ARGB_SERIAL_DEBUG_VALUE}
)
