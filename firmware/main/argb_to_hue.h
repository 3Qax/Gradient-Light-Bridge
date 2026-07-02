#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ARGB_ENDPOINT_COUNT 4

#define HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE 1

/* The registry versions of esp-zigbee-lib do not ship the example-only
 * configuration macros, so define the ones we need locally. */
#define ESP_ZB_PRIMARY_CHANNEL_MASK     ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                           \
    {                                                           \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                     \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                            \
    {                                                           \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,   \
    }

#define ESP_ZB_ZR_CONFIG()                                      \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,               \
        .install_code_policy = false,                           \
        .nwk_cfg.zczr_cfg = {                                   \
            .max_children = 10,                                 \
        },                                                      \
    }

typedef struct {
    bool on;
    uint8_t bri;
    uint16_t x;
    uint16_t y;
    uint8_t last_bri;  /* last non-zero brightness, used when On restores level */
} light_state_t;

extern light_state_t g_light_state[ARGB_ENDPOINT_COUNT];

void emit_state_json(uint8_t endpoint);
