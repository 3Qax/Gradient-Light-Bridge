#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Mimic a Hue gradient lightstrip (LCX004).
 * All Hue lights use endpoint 0x0B in the API responses. */
#define GRADIENT_PROBE_ENDPOINT 0x0B

/* The registry versions of esp-zigbee-lib do not ship the example-only
 * configuration macros, so define the ones we need locally.
 * Lock to the Hue bridge's Zigbee channel (25) for fast pairing. */
#define ESP_ZB_PRIMARY_CHANNEL_MASK     (1 << 25)

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
