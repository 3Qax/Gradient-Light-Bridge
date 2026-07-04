/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 *
 * Derived from Espressif's HA_color_dimmable_light example and compatibility
 * fixes documented by the reverse-engineering community.
 */

#include "argb_to_hue.h"
#include "local_led_backend.h"
#include "xy_to_rgb.h"
#include "trust_center_key.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "aps/esp_zigbee_aps.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl_utility.h"
#include "esp_mac.h"
#include "esp_zigbee_cluster.h"
#include "zboss_api.h"
#include "zboss_api_buf.h"
#include "zcl/zb_zcl_commands.h"
#include "zgp/zgp_internal.h"


#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile light (Router) source code.
#endif

static const char *TAG = "ARGB_BRIDGE";

/* Manufacturer-specific cluster IDs found on real gradient lights. */
#define MFG_CLUSTER_GRADIENT_ID         0xFC03 /* gradient / multiColor */
#define MFG_CLUSTER_CERT_ID             0xFC01 /* certification/proprietary */
#define MFG_CLUSTER_AUX_ID              0xFC04 /* unknown manufacturer-specific cluster */
#define TOUCHLINK_CLUSTER_ID               0x1000 /* present on real gradient endpoint */
#define GP_ENDPOINT                        242
#define GP_PROFILE_ID                      0xA1E0
#define GP_DEVICE_ID                       0x0061
#define GP_CLUSTER_ID                      0x0021
#define OTA_IMAGE_NOTIFY_CMD_ID            0x00
#define OTA_QUERY_NEXT_IMAGE_REQ_CMD_ID    0x01
#define OTA_LCX004_IMAGE_TYPE              0x0118
#define OTA_FILE_VERSION                   0x01002000
#define SCENES_MFG_COMPACT_SCENE_CMD_ID    0x00
#define SCENES_MFG_STORE_CMD_ID            0x02
#define SCENES_REMOVE_SCENE_CMD_ID         0x02
#define SCENES_RECALL_SCENE_CMD_ID         0x05
#define SCENES_KEY_PAYLOAD_LEN             3
#define SCENES_REAL_CAPACITY               50
#define SCENE_FC03_CACHE_ENTRIES           48
#define SCENE_FC03_MAX_PAYLOAD_LEN         64
#define SCENE_FC03_CACHE_NVS_NAMESPACE     "scene_cache"
#define SCENE_FC03_CACHE_NVS_KEY           "fc03_v1"
#define SCENE_FC03_CACHE_MAGIC             0x53464C43U /* SFLC */
#define SCENE_FC03_CACHE_VERSION           1
#define POWER_RECOVERY_NVS_NAMESPACE       "power_rec"
#define POWER_RECOVERY_NVS_KEY             "state_v1"
#define POWER_RECOVERY_NVS_MAGIC           0x50575231U /* PWR1 */
#define POWER_RECOVERY_NVS_VERSION         1
#define STANDARD_ZCL_FADE_TENTHS           4

#define FC03_FLAG_ON_OFF                   0x0001
#define FC03_FLAG_BRIGHTNESS               0x0002
#define FC03_FLAG_COLOR_MIREK              0x0004
#define FC03_FLAG_COLOR_XY                 0x0008
#define FC03_FLAG_FADE_SPEED               0x0010
#define FC03_FLAG_EFFECT_TYPE              0x0020
#define FC03_FLAG_GRADIENT_PARAMS          0x0040
#define FC03_FLAG_EFFECT_SPEED             0x0080
#define FC03_FLAG_GRADIENT_COLORS          0x0100
#define FC03_KNOWN_FLAGS                   (FC03_FLAG_ON_OFF | \
                                            FC03_FLAG_BRIGHTNESS | \
                                            FC03_FLAG_COLOR_MIREK | \
                                            FC03_FLAG_COLOR_XY | \
                                            FC03_FLAG_FADE_SPEED | \
                                            FC03_FLAG_EFFECT_TYPE | \
                                            FC03_FLAG_GRADIENT_PARAMS | \
                                            FC03_FLAG_EFFECT_SPEED | \
                                            FC03_FLAG_GRADIENT_COLORS)

#define GRADIENT_STYLE_LINEAR              0x00
#define GRADIENT_STYLE_SCATTERED           0x02
#define GRADIENT_STYLE_MIRRORED            0x04
#define GRADIENT_STYLE_SEGMENTED           0x06

#define COLOR_POINT_RX_ID                  0x0032
#define COLOR_POINT_RY_ID                  0x0033
#define COLOR_POINT_GX_ID                  0x0036
#define COLOR_POINT_GY_ID                  0x0037
#define COLOR_POINT_BX_ID                  0x003A
#define COLOR_POINT_BY_ID                  0x003B

#define ZDO_NODE_DESC_REQ_CLUSTER_ID       0x0002
#define ZDO_NODE_DESC_RSP_CLUSTER_ID       0x8002
#define ZDO_ACTIVE_EP_REQ_CLUSTER_ID       0x0005
#define ZDO_ACTIVE_EP_RSP_CLUSTER_ID       0x8005
#define ZDO_SIMPLE_DESC_REQ_CLUSTER_ID     0x0004
#define ZDO_SIMPLE_DESC_RSP_CLUSTER_ID     0x8004
#define ZDO_SUCCESS_STATUS                 0x00
#define ZCL_STATUS_INSUFFICIENT_SPACE      0x89

/* Experiment: bypass the local endpoint table for ZDO discovery and answer
 * like a real LCX004, including Green Power endpoint 242, without registering
 * endpoint 242 in the ESP-Zigbee endpoint list. */
#define ZDO_DESCRIPTOR_OVERRIDE            1
#define FC01_EXPLICIT_DEFAULT_RESPONSE     1

#ifndef ARGB_SERIAL_DEBUG
#define ARGB_SERIAL_DEBUG               0
#endif

#define ARGB_BACKEND_SERIAL_JSON        1
#define ARGB_BACKEND_LOCAL_LED          2

#ifndef ARGB_BACKEND
#define ARGB_BACKEND                    ARGB_BACKEND_SERIAL_JSON
#endif

#if ARGB_BACKEND != ARGB_BACKEND_SERIAL_JSON && ARGB_BACKEND != ARGB_BACKEND_LOCAL_LED
#error Unsupported ARGB_BACKEND value
#endif

/* Real LCX004 devices also expose Green Power endpoint 242.
 *
 * Modes:
 *   0 - disabled; known discoverable baseline
 *   1 - ESP-Zigbee gateway endpoint; avoided crash, but no light was created
 *   2 - native ZBOSS Green Power Proxy Basic endpoint; current experiment
 */
#define GP_ENDPOINT_MODE                   0

/* Manufacturer code for Signify Netherlands B.V. */
#define SIGNIFY_MANUFACTURER_CODE 0x100B

/* Captured from real LCX004 frame 499 in
 * sniffer-capture-20260703-real-headboard-rejoin-rxidle. */
static const uint8_t FC03_DISCOVERY_READ_ATTRS[] = {
    0x01, 0x00, 0x02, 0x00, 0x10, 0x00, 0x11, 0x00,
    0x12, 0x00, 0x13, 0x00, 0x30, 0x00, 0x38, 0x00,
    0x37, 0x00, 0x33, 0x00, 0x32, 0x00,
};
static const uint8_t FC03_DISCOVERY_ATTR2_STATE[] = {
    0x06, 0x07, 0x00, 0x01, 0xFE, 0x6E, 0x01,
};

#ifndef ARGB_EUI_SUFFIX
#define ARGB_EUI_SUFFIX "FF:FE:05"
#endif

/* Spoof a Signify Netherlands B.V. extended address so the bridge treats
 * the device as a genuine product. The displayed EUI-64 prefix remains
 * 00:17:88:01:0b; ARGB_EUI_SUFFIX supplies the final three displayed bytes.
 *
 * Use ARGB_EUI_SUFFIX=auto for additional boards. In auto mode the suffix comes
 * from the last three bytes of the ESP factory MAC, so one firmware image can
 * be flashed to multiple boards without Zigbee EUI collisions.
 *
 * esp_zb_set_long_address() stores the EUI-64 in little-endian order, so the
 * array passed to the stack is the reverse of the displayed uniqueid prefix. */
static esp_zb_ieee_addr_t s_spoofed_long_addr;

static bool parse_eui_suffix(const char *text, uint8_t suffix[3])
{
    if (!text || !suffix) {
        return false;
    }

    if (strcasecmp(text, "auto") == 0) {
        uint8_t factory_mac[6] = {0};
        esp_err_t err = esp_read_mac(factory_mac, ESP_MAC_BASE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read ESP base MAC for EUI suffix: %s", esp_err_to_name(err));
            return false;
        }
        suffix[0] = factory_mac[3];
        suffix[1] = factory_mac[4];
        suffix[2] = factory_mac[5];
        ESP_LOGI(TAG,
                 "Using ESP factory MAC suffix for EUI: %02x:%02x:%02x from %02x:%02x:%02x:%02x:%02x:%02x",
                 suffix[0],
                 suffix[1],
                 suffix[2],
                 factory_mac[0],
                 factory_mac[1],
                 factory_mac[2],
                 factory_mac[3],
                 factory_mac[4],
                 factory_mac[5]);
        return true;
    }

    unsigned int parsed[3] = {0};
    if (sscanf(text, "%2x:%2x:%2x", &parsed[0], &parsed[1], &parsed[2]) != 3 &&
        sscanf(text, "%2x-%2x-%2x", &parsed[0], &parsed[1], &parsed[2]) != 3 &&
        sscanf(text, "%2x%2x%2x", &parsed[0], &parsed[1], &parsed[2]) != 3) {
        ESP_LOGE(TAG, "Invalid ARGB_EUI_SUFFIX `%s`; expected auto or three hex bytes", text);
        return false;
    }

    for (int i = 0; i < 3; i++) {
        if (parsed[i] > 0xff) {
            ESP_LOGE(TAG, "Invalid ARGB_EUI_SUFFIX byte %u in `%s`", parsed[i], text);
            return false;
        }
        suffix[i] = (uint8_t)parsed[i];
    }
    return true;
}

static void init_spoofed_long_addr(void)
{
    uint8_t suffix[3] = {0xff, 0xfe, 0x05};
    if (!parse_eui_suffix(ARGB_EUI_SUFFIX, suffix)) {
        ESP_LOGW(TAG, "Falling back to legacy EUI suffix FF:FE:05");
    }

    s_spoofed_long_addr[0] = suffix[2];
    s_spoofed_long_addr[1] = suffix[1];
    s_spoofed_long_addr[2] = suffix[0];
    s_spoofed_long_addr[3] = 0x0b;
    s_spoofed_long_addr[4] = 0x01;
    s_spoofed_long_addr[5] = 0x88;
    s_spoofed_long_addr[6] = 0x17;
    s_spoofed_long_addr[7] = 0x00;

    ESP_LOGI(TAG,
             "Spoofed EUI-64: 00:17:88:01:0b:%02x:%02x:%02x",
             suffix[0],
             suffix[1],
             suffix[2]);
}

/* Zigbee strings are octet strings: first byte is the length.
 * These values are from an active read of a real LCX004 Basic cluster. */
static const char *BASIC_MANUFACTURER_NAME = "\x18" "Signify Netherlands B.V.";
static const char *BASIC_MODEL_IDENTIFIER  = "\x06" "LCX004";

/* Minimal initial gradient state for the FC03 cluster attribute 0x0002.
 * This is the raw mode-based format used by real gradient lights for the
 * read-only state attribute (distinct from the flags-based multiColor command):
 *   mode (2 LE) | onOff (1) | brightness (1) | unknown (4) |
 *   length (1) | ncolors<<4 (1) | style (1) | reserved (2) |
 *   colors (3*N) | segments (1) | offset (1)
 * mode = 0x014B (gradient), ncolors = 2, segments = 10.
 * The leading byte is the OCTET_STRING length. */
/* FC03 state attribute buffer.  Large enough for up to 9 gradient colors:
 * 1 (len) + 2 (mode) + 1 (onOff) + 1 (bri) + 4 (unknown) + 1 (len2) +
 * 1 (ncolors) + 1 (style) + 2 (reserved) + 3*N (colors) + 1 (segments) +
 * 1 (offset) = 16 + 3*N bytes.  For N=9 this is 43 bytes. */
static const uint8_t FC03_DEFAULT_STATE[] = {
    0x1E,                   /* length of payload (30 bytes) */
    0x4B, 0x01,             /* mode = gradient */
    0x00,                   /* onOff = off */
    0xFE,                   /* brightness = 254 */
    0x8F, 0x9F,             /* currentX = 0x9f8f */
    0x06, 0x5C,             /* currentY = 0x5c06 */
    0x13,                   /* length of color payload */
    0x50,                   /* color count << 4 (5 colors), style linear */
    0x00, 0x00, 0x00,       /* style + reserved */
    0x92, 0x2D, 0x6D,
    0x92, 0x2D, 0x6D,
    0x92, 0x2D, 0x6D,
    0x92, 0x2D, 0x6D,
    0x92, 0x2D, 0x6D,
    0x28,                   /* segments */
    0x00,                   /* offset */
};
static uint8_t s_fc03_state[ARGB_ENDPOINT_COUNT][64];

/* FC01 certification/proprietary attributes observed on a real LCX004:
 *   0x0000 - 8-bit bitmap = 0x0B
 *   0x0001 - 8-bit enum  = 0x00
 *   0x0002 - 8-bit unsigned = 0x0A
 *   0x0003 - 8-bit unsigned = 0x04
 */
static uint8_t s_fc01_attr0[ARGB_ENDPOINT_COUNT] = { 0x0B };
static uint8_t s_fc01_attr1[ARGB_ENDPOINT_COUNT] = { 0x00 };
static uint8_t s_fc01_attr2[ARGB_ENDPOINT_COUNT] = { 0x0A };
static uint8_t s_fc01_attr3[ARGB_ENDPOINT_COUNT] = { 0x04 };
static uint32_t s_fc03_attr1[ARGB_ENDPOINT_COUNT] = { 0x0000000F };
static uint16_t s_fc03_attr10[ARGB_ENDPOINT_COUNT] = { 0x0001 };
static uint64_t s_fc03_attr11[ARGB_ENDPOINT_COUNT] = { 0x000000000003FE0EULL };
static uint32_t s_fc03_attr12[ARGB_ENDPOINT_COUNT] = { 0x00000003 };
static uint16_t s_fc03_attr13[ARGB_ENDPOINT_COUNT] = { 0x000F };
static uint8_t s_fc03_attr32[ARGB_ENDPOINT_COUNT] = { 0x00 };
static uint8_t s_fc03_attr33[ARGB_ENDPOINT_COUNT] = { 0x00 };
static uint8_t s_fc03_attr34[ARGB_ENDPOINT_COUNT] = { 0x00 };
static uint16_t s_fc03_attr38[ARGB_ENDPOINT_COUNT] = { 0x000A };
static uint16_t s_fc04_attr0[ARGB_ENDPOINT_COUNT] = { 0x1007 };
static uint8_t s_basic_attr51[ARGB_ENDPOINT_COUNT] = { 0x01 };
static uint32_t s_basic_attr53[ARGB_ENDPOINT_COUNT] = { 0x0F4C913C };
static const uint8_t BASIC_ATTR54_DEFAULT[16] = {
    0xc7, 0x54, 0x2a, 0xfa, 0x34, 0x8c, 0xf3, 0x83,
    0x78, 0xa0, 0x2d, 0x5d, 0x1c, 0x74, 0xf1, 0xe6,
};
static uint8_t s_basic_attr54[ARGB_ENDPOINT_COUNT][16];
static const uint8_t BASIC_POWER_ON_CONFIG[] = "\x09" "0:PWRON@1";
static const uint8_t BASIC_PRODUCT_LABEL[] = "\x1A" "Philips-LCX004-1-GALSECLv1";
static uint32_t s_basic_attr1[ARGB_ENDPOINT_COUNT] = { 0x00000000 };
static uint32_t s_basic_attr21[ARGB_ENDPOINT_COUNT] = { 0x001517EC };
static uint32_t s_basic_attr41[ARGB_ENDPOINT_COUNT] = { 0xC4C1C739 };
static uint32_t s_basic_attr50[ARGB_ENDPOINT_COUNT] = { 0x00000001 };

static const uint8_t BASIC_CMD_C1_RESPONSE[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00,
    0x00, 0x00, 0x35, 0x0A, 0x53, 0x08, 0x01, 0x15,
    0x66, 0x04, 0x03, 0x3E, 0x1A, 0x4A, 0x0A, 0x06,
    0x4C, 0x43, 0x58, 0x30, 0x30, 0x34, 0x12, 0x18,
    0x53, 0x69, 0x67, 0x6E, 0x69, 0x66, 0x79, 0x20,
    0x4E, 0x65, 0x74, 0x68, 0x65, 0x72, 0x6C, 0x61,
    0x6E, 0x64, 0x73, 0x20, 0x42, 0x2E, 0x56, 0x2E,
    0x42, 0x17, 0x48, 0x75, 0x65, 0x20, 0x67, 0x72,
};

static const uint8_t BASIC_CMD_C1_RESPONSE_0035[] = {
    0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x55, 0x00,
    0x00, 0x00, 0x20, 0x61, 0x64, 0x69, 0x65, 0x6E,
    0x74, 0x20, 0x6C, 0x69, 0x67, 0x68, 0x74, 0x73,
    0x74, 0x72, 0x69, 0x70, 0x48, 0x66, 0x52, 0x0B,
    0x08, 0x0B, 0x12, 0x07, 0x08, 0xB0, 0x09, 0x10,
    0x03, 0x18, 0x01,
};

light_state_t g_light_state[ARGB_ENDPOINT_COUNT] = {
    {
        .on = false,
        .bri = 0xFE,
        .x = 0x9F8F,
        .y = 0x5C06,
        .last_bri = 0xFE,
    },
};

static uint8_t s_startup_on_off[ARGB_ENDPOINT_COUNT] = { 0xFF };
static uint8_t s_startup_current_level[ARGB_ENDPOINT_COUNT] = { 0xFF };
static uint16_t s_startup_color_temperature[ARGB_ENDPOINT_COUNT] = { 0xFFFF };
static uint16_t s_startup_current_x[ARGB_ENDPOINT_COUNT] = { 0xFFFF };
static uint16_t s_startup_current_y[ARGB_ENDPOINT_COUNT] = { 0xFFFF };

typedef struct {
    uint8_t on;
    uint8_t bri;
    uint8_t last_bri;
    uint8_t startup_on_off;
    uint16_t x;
    uint16_t y;
    uint8_t startup_current_level;
    uint8_t reserved0;
    uint16_t startup_color_temperature;
    uint16_t startup_current_x;
    uint16_t startup_current_y;
} power_recovery_nvs_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_count;
    power_recovery_nvs_entry_t entries[ARGB_ENDPOINT_COUNT];
} power_recovery_nvs_blob_t;

static power_recovery_nvs_blob_t s_power_recovery_last_saved;
static bool s_power_recovery_has_last_saved;

static void print_hex_bytes(const char *prefix, const uint8_t *payload, uint16_t len);
static void init_endpoint_defaults(void);
static void load_power_recovery_state(void);
static void save_power_recovery_state(const char *reason);
static void apply_power_recovery_startup(uint8_t endpoint);

static void send_basic_c1_response_raw(const zb_zcl_parsed_hdr_t *hdr,
                                       const uint8_t *payload,
                                       uint16_t payload_len)
{
    if (!hdr || hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "BASIC_CMD_C0_RESP_C1: no ZBOSS output buffer");
        return;
    }

    const uint8_t *resp_payload = BASIC_CMD_C1_RESPONSE;
    uint16_t resp_payload_len = sizeof(BASIC_CMD_C1_RESPONSE);
    static const uint8_t request_0035[] = { 0x00, 0x35, 0x00, 0x00, 0x00, 0x40 };
    if (payload && payload_len == sizeof(request_0035) &&
        memcmp(payload, request_0035, sizeof(request_0035)) == 0) {
        resp_payload = BASIC_CMD_C1_RESPONSE_0035;
        resp_payload_len = sizeof(BASIC_CMD_C1_RESPONSE_0035);
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                           ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                           ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        0xC1);
    ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, resp_payload, resp_payload_len);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "BASIC_CMD_C0_RESP_C1_RAW: tsn=0x%02x to=0x%04x ep=%u size=%u",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)hdr->addr_data.common_data.src_endpoint,
             (unsigned)resp_payload_len);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                hdr->addr_data.common_data.dst_endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
}

static void send_ota_query_next_image_raw(const zb_zcl_parsed_hdr_t *hdr)
{
    if (!hdr || hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "OTA_QUERY_NEXT_IMAGE_REQ_RAW: no ZBOSS output buffer");
        return;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_REQ_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_SRV,
                                                          ZB_ZCL_NOT_MANUFACTURER_SPECIFIC,
                                                          ZB_ZCL_ENABLE_DEFAULT_RESPONSE);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER(cmd_ptr,
                                    ZB_ZCL_GET_SEQ_NUM(),
                                    OTA_QUERY_NEXT_IMAGE_REQ_CMD_ID);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x00); /* field control: no hardware version */
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, SIGNIFY_MANUFACTURER_CODE);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, OTA_LCX004_IMAGE_TYPE);
    ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, OTA_FILE_VERSION);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "OTA_QUERY_NEXT_IMAGE_REQ_RAW: to=0x%04x ep=%u manuf=0x%04x image=0x%04x file=0x%08lx",
             (unsigned)dst_addr,
             (unsigned)hdr->addr_data.common_data.src_endpoint,
             (unsigned)SIGNIFY_MANUFACTURER_CODE,
             (unsigned)OTA_LCX004_IMAGE_TYPE,
             (unsigned long)OTA_FILE_VERSION);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                hdr->addr_data.common_data.dst_endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
}

static bool send_write_attr_success_raw(const zb_zcl_parsed_hdr_t *hdr, const char *log_prefix)
{
    if (!hdr || hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return false;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "%s: no ZBOSS output buffer", log_prefix);
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(
        cmd_ptr,
        ZB_ZCL_FRAME_DIRECTION_TO_CLI,
        hdr->is_manuf_specific ? ZB_ZCL_MANUFACTURER_SPECIFIC : ZB_ZCL_NOT_MANUFACTURER_SPECIFIC);
    if (hdr->is_manuf_specific) {
        ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                            hdr->seq_number,
                                            ZB_TRUE,
                                            hdr->manuf_specific,
                                            ZB_ZCL_CMD_WRITE_ATTRIB_RESP);
    } else {
        ZB_ZCL_CONSTRUCT_COMMAND_HEADER(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_ZCL_CMD_WRITE_ATTRIB_RESP);
    }
    ZB_ZCL_GENERAL_SUCCESS_WRITE_ATTR_RESP(cmd_ptr);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    ESP_LOGI(TAG, "%s: tsn=0x%02x to=0x%04x ep=%u status=success",
             log_prefix,
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static bool handle_basic_write_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                         const uint8_t *payload,
                                         uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;

    uint16_t offset = 0;
    while (offset + 3 <= payload_len) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        uint8_t attr_type = payload[offset + 2];
        const uint8_t *value = &payload[offset + 3];
        uint16_t value_len = 0;

        switch (attr_id) {
        case 0x0051:
            if (attr_type != ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM || offset + 4 > payload_len) {
                return false;
            }
            s_basic_attr51[idx] = value[0];
            value_len = 1;
            ESP_LOGI(TAG, "BASIC_WRITE_ATTR_RAW: ep=%u attr=0x0051 type=0x%02x value=0x%02x",
                     (unsigned)endpoint, (unsigned)attr_type, (unsigned)s_basic_attr51[idx]);
            break;
        case 0x0053:
            if (attr_type != ESP_ZB_ZCL_ATTR_TYPE_U32 || offset + 7 > payload_len) {
                return false;
            }
            s_basic_attr53[idx] = (uint32_t)value[0] |
                                  ((uint32_t)value[1] << 8) |
                                  ((uint32_t)value[2] << 16) |
                                  ((uint32_t)value[3] << 24);
            value_len = 4;
            ESP_LOGI(TAG, "BASIC_WRITE_ATTR_RAW: ep=%u attr=0x0053 type=0x%02x value=0x%08lx",
                     (unsigned)endpoint, (unsigned)attr_type, (unsigned long)s_basic_attr53[idx]);
            break;
        case 0x0054:
            if (attr_type != ESP_ZB_ZCL_ATTR_TYPE_128_BIT_KEY || offset + 19 > payload_len) {
                return false;
            }
            memcpy(s_basic_attr54[idx], value, sizeof(s_basic_attr54[idx]));
            value_len = sizeof(s_basic_attr54[idx]);
            ESP_LOGI(TAG, "BASIC_WRITE_ATTR_RAW: ep=%u attr=0x0054 type=0x%02x value=[redacted]",
                     (unsigned)endpoint, (unsigned)attr_type);
            break;
        default:
            return false;
        }

        offset += 3 + value_len;
    }

    if (offset != payload_len) {
        return false;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "BASIC_WRITE_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_WRITE_ATTRIB_RESP);
    ZB_ZCL_GENERAL_SUCCESS_WRITE_ATTR_RESP(cmd_ptr);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "BASIC_WRITE_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u status=success",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);

    return true;
}

static bool handle_power_recovery_write_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                                  const uint8_t *payload,
                                                  uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;

    bool changed = false;
    bool handled = false;
    uint16_t offset = 0;
    while (offset + 3 <= payload_len) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        uint8_t attr_type = payload[offset + 2];
        const uint8_t *value = &payload[offset + 3];
        uint16_t value_len = 0;

        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
            !hdr->is_manuf_specific &&
            attr_id == 0x4003 &&
            attr_type == ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM &&
            offset + 4 <= payload_len) {
            uint8_t old = s_startup_on_off[idx];
            s_startup_on_off[idx] = value[0];
            changed |= old != s_startup_on_off[idx];
            handled = true;
            value_len = 1;
            ESP_LOGI(TAG, "POWER_RECOVERY_WRITE: ep=%u attr=on_off_startup value=0x%02x",
                     (unsigned)endpoint,
                     (unsigned)s_startup_on_off[idx]);
        } else if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
                   !hdr->is_manuf_specific &&
                   attr_id == 0x4000 &&
                   attr_type == ESP_ZB_ZCL_ATTR_TYPE_U8 &&
                   offset + 4 <= payload_len) {
            uint8_t old = s_startup_current_level[idx];
            s_startup_current_level[idx] = value[0];
            changed |= old != s_startup_current_level[idx];
            handled = true;
            value_len = 1;
            ESP_LOGI(TAG, "POWER_RECOVERY_WRITE: ep=%u attr=level_startup value=0x%02x",
                     (unsigned)endpoint,
                     (unsigned)s_startup_current_level[idx]);
        } else if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
                   !hdr->is_manuf_specific &&
                   attr_id == 0x4010 &&
                   attr_type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
                   offset + 5 <= payload_len) {
            uint16_t old = s_startup_color_temperature[idx];
            s_startup_color_temperature[idx] = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
            changed |= old != s_startup_color_temperature[idx];
            handled = true;
            value_len = 2;
            ESP_LOGI(TAG, "POWER_RECOVERY_WRITE: ep=%u attr=color_temp_startup value=0x%04x",
                     (unsigned)endpoint,
                     (unsigned)s_startup_color_temperature[idx]);
        } else if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
                   hdr->is_manuf_specific &&
                   hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
                   (attr_id == 0x0003 || attr_id == 0x0004) &&
                   attr_type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
                   offset + 5 <= payload_len) {
            uint16_t attr_value = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
            uint16_t *slot = attr_id == 0x0003 ? &s_startup_current_x[idx] : &s_startup_current_y[idx];
            uint16_t old = *slot;
            *slot = attr_value;
            changed |= old != *slot;
            handled = true;
            value_len = 2;
            ESP_LOGI(TAG, "POWER_RECOVERY_WRITE: ep=%u attr=startup_%c value=0x%04x",
                     (unsigned)endpoint,
                     attr_id == 0x0003 ? 'x' : 'y',
                     (unsigned)*slot);
        } else {
            return false;
        }

        offset += 3 + value_len;
    }

    if (!handled || offset != payload_len) {
        return false;
    }

    if (changed) {
        save_power_recovery_state("startup_attrs");
    }
    return send_write_attr_success_raw(hdr, "POWER_RECOVERY_WRITE_RESP_RAW");
}

static bool handle_basic_read_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                        const uint8_t *payload,
                                        uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        (payload_len % 2) != 0) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;

    bool has_signify_attr = false;
    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        switch (attr_id) {
        case 0x0000:
        case 0x0001:
        case 0x0003:
        case 0x0020:
        case 0x0021:
        case 0x0040:
        case 0x0041:
        case 0x0050:
            has_signify_attr = true;
            break;
        default:
            return false;
        }
    }
    if (!has_signify_attr) {
        return false;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "BASIC_READ_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);

        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
        switch (attr_id) {
        case 0x0020:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING);
            ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, BASIC_POWER_ON_CONFIG,
                                     sizeof(BASIC_POWER_ON_CONFIG) - 1);
            break;
        case 0x0021:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U32);
            ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, s_basic_attr21[idx]);
            break;
        case 0x0001:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U32);
            ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, s_basic_attr1[idx]);
            break;
        case 0x0040:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING);
            ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, BASIC_PRODUCT_LABEL,
                                     sizeof(BASIC_PRODUCT_LABEL) - 1);
            break;
        case 0x0041:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U32);
            ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, s_basic_attr41[idx]);
            break;
        case 0x0050:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_32BITMAP);
            ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, s_basic_attr50[idx]);
            break;
        default:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_UNSUP_ATTRIB);
            break;
        }
    }

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "BASIC_READ_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attrs=%u",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)(payload_len / 2));
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);

    return true;
}

static bool handle_identify_read_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                           const uint8_t *payload,
                                           uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        payload_len != 2) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t attr_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (attr_id != 0x0000) {
        return false;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "IDENTIFY_READ_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_16BITMAP);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, 0x000B);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "IDENTIFY_READ_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attr=0x0000 value=0x000b",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);

    return true;
}

static bool handle_level_read_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                        const uint8_t *payload,
                                        uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        (payload_len % 2) != 0) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        if (attr_id != 0x0003 && attr_id != 0x0004) {
            return false;
        }
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "LEVEL_READ_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
        if (attr_id == 0x0003) {
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U16);
            ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, 0x000A);
        } else {
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_UNSUP_ATTRIB);
        }
    }

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "LEVEL_READ_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attrs=%u",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)(payload_len / 2));
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static bool handle_color_mfg_read_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                            const uint8_t *payload,
                                            uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        (payload_len % 2) != 0) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        if (attr_id != 0x0003 && attr_id != 0x0004) {
            return false;
        }
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "COLOR_MFG_READ_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        uint16_t attr_value = attr_id == 0x0003 ? s_startup_current_x[idx] : s_startup_current_y[idx];
        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
        ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
        ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U16);
        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_value);
    }

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "COLOR_MFG_READ_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attrs=%u",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)(payload_len / 2));
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static bool handle_scenes_read_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                         const uint8_t *payload,
                                         uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        payload_len != 2) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t attr_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (attr_id != 0x0001) {
        return false;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "SCENES_READ_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_32BITMAP);
    ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, 0x00000003);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "SCENES_READ_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attr=0x0001 value=0x00000003",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static bool handle_scenes_get_membership_raw(const zb_zcl_parsed_hdr_t *hdr,
                                             const uint8_t *payload,
                                             uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        payload_len != 2) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t group_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "SCENES_GET_MEMBERSHIP_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                           ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                           ZB_ZCL_NOT_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER(cmd_ptr,
                                    hdr->seq_number,
                                    0x06); /* Get Scene Membership Response */
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, SCENES_REAL_CAPACITY);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, group_id);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x00); /* No scene IDs. */

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "SCENES_GET_MEMBERSHIP_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u group=0x%04x capacity=%u scenes=0",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)SCENES_REAL_CAPACITY);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static void group_add_confirm_cb(zb_uint8_t param)
{
    zb_apsme_add_group_conf_t *conf = ZB_BUF_GET_PARAM(param, zb_apsme_add_group_conf_t);
    ESP_LOGI(TAG, "GROUPS_APS_ADD_CONFIRM: group=0x%04x ep=%u status=%d in_group=%u",
             (unsigned)conf->group_address,
             (unsigned)conf->endpoint,
             (int)conf->status,
             (unsigned)zb_aps_is_endpoint_in_group(conf->group_address, conf->endpoint));
    zb_buf_free(param);
}

static void join_group(uint16_t group_id, uint8_t endpoint)
{
    if (zb_aps_is_endpoint_in_group(group_id, endpoint)) {
        ESP_LOGI(TAG, "GROUPS_APS_ADD: group=0x%04x ep=%u already present",
                 (unsigned)group_id,
                 (unsigned)endpoint);
        return;
    }

    zb_bufid_t group_buf = zb_buf_get_out();
    if (!group_buf) {
        ESP_LOGE(TAG, "GROUPS_APS_ADD: no ZBOSS output buffer for group=0x%04x ep=%u",
                 (unsigned)group_id,
                 (unsigned)endpoint);
        return;
    }

    zb_apsme_add_group_req_t *req = ZB_BUF_GET_PARAM(group_buf, zb_apsme_add_group_req_t);
    req->group_address = group_id;
    req->endpoint = endpoint;
    req->confirm_cb = group_add_confirm_cb;
    ESP_LOGI(TAG, "GROUPS_APS_ADD: requesting group=0x%04x ep=%u",
             (unsigned)group_id,
             (unsigned)endpoint);
    zb_zdo_add_group_req(group_buf);
}

static bool handle_groups_add_group_raw(const zb_zcl_parsed_hdr_t *hdr,
                                        const uint8_t *payload,
                                        uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        payload_len < 3) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t group_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint8_t name_len = payload[2];
    if ((uint16_t)(3 + name_len) != payload_len) {
        return false;
    }
    join_group(group_id, endpoint);

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "GROUPS_ADD_GROUP_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                           ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                           ZB_ZCL_NOT_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER(cmd_ptr,
                                    hdr->seq_number,
                                    0x00); /* Add Group Response */
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, group_id);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "GROUPS_ADD_GROUP_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u group=0x%04x",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)group_id);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static bool handle_fc03_read_attrs_raw(const zb_zcl_parsed_hdr_t *hdr,
                                       const uint8_t *payload,
                                       uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        (payload_len % 2) != 0) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;

    bool has_fc03_attr = false;
    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);
        switch (attr_id) {
        case 0x0001:
        case 0x0002:
        case 0x0010:
        case 0x0011:
        case 0x0012:
        case 0x0013:
        case 0x0030:
        case 0x0032:
        case 0x0033:
        case 0x0034:
        case 0x0037:
        case 0x0038:
            has_fc03_attr = true;
            break;
        default:
            return false;
        }
    }
    if (!has_fc03_attr) {
        return false;
    }
    bool is_lcx004_discovery_read =
        payload_len == sizeof(FC03_DISCOVERY_READ_ATTRS) &&
        memcmp(payload, FC03_DISCOVERY_READ_ATTRS, sizeof(FC03_DISCOVERY_READ_ATTRS)) == 0;

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "FC03_READ_ATTR_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);

        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
        switch (attr_id) {
        case 0x0001:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_32BITMAP);
            ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, s_fc03_attr1[idx]);
            break;
        case 0x0002:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING);
            if (is_lcx004_discovery_read) {
                ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr,
                                         FC03_DISCOVERY_ATTR2_STATE,
                                         sizeof(FC03_DISCOVERY_ATTR2_STATE));
            } else {
                ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, s_fc03_state[idx], s_fc03_state[idx][0] + 1);
            }
            break;
        case 0x0010:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_16BITMAP);
            ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, s_fc03_attr10[idx]);
            break;
        case 0x0011:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_64BITMAP);
            ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, &s_fc03_attr11[idx], sizeof(s_fc03_attr11[idx]));
            break;
        case 0x0012:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_32BITMAP);
            ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, s_fc03_attr12[idx]);
            break;
        case 0x0013:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_16BITMAP);
            ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, s_fc03_attr13[idx]);
            break;
        case 0x0032:
            if (is_lcx004_discovery_read) {
                ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZCL_STATUS_INSUFFICIENT_SPACE);
                break;
            }
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U8);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, s_fc03_attr32[idx]);
            break;
        case 0x0033:
            if (is_lcx004_discovery_read) {
                ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZCL_STATUS_INSUFFICIENT_SPACE);
                break;
            }
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U8);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, s_fc03_attr33[idx]);
            break;
        case 0x0034:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U8);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, s_fc03_attr34[idx]);
            break;
        case 0x0038:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U16);
            ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, s_fc03_attr38[idx]);
            break;
        default:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_UNSUP_ATTRIB);
            break;
        }
    }

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "FC03_READ_ATTR_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attrs=%u",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)(payload_len / 2));
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);

    return true;
}

static bool handle_fc03_discover_attr_ext_raw(const zb_zcl_parsed_hdr_t *hdr,
                                              const uint8_t *payload,
                                              uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        payload_len != 3) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t start_attr = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint8_t max_attrs = payload[2];
    if (start_attr != 0x0032 || max_attrs != 3) {
        return false;
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "FC03_DISC_ATTR_EXT_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                          ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                          ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_DISCOVER_ATTR_EXT_RES);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x00); /* discovery not complete */
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, 0x0032);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_U8);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x1C);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, 0x2000);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x07);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x1C);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "FC03_DISC_ATTR_EXT_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u attrs=0x0032,0x2000 access=0x1c",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static uint16_t u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool get_light_state_for_endpoint(uint8_t endpoint, uint8_t *idx_out, light_state_t **st_out)
{
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    if (idx_out) {
        *idx_out = idx;
    }
    if (st_out) {
        *st_out = &g_light_state[idx];
    }
    return true;
}

static uint8_t remembered_brightness(const light_state_t *st)
{
    uint8_t bri = st->bri ? st->bri : st->last_bri;
    return bri ? bri : 0xFE;
}

static uint8_t output_brightness(const light_state_t *st)
{
    return st->on ? remembered_brightness(st) : 0;
}

static rgb_t light_state_rgb(const light_state_t *st)
{
    return xy_to_rgb(st->x / 65535.0f, st->y / 65535.0f, output_brightness(st));
}

#if ARGB_BACKEND == ARGB_BACKEND_LOCAL_LED
static void local_led_stop_dynamic(uint8_t endpoint);
static esp_err_t local_led_start_solid_transition(uint8_t endpoint,
                                                  rgb_t color,
                                                  bool has_fade,
                                                  uint16_t fade);
#endif

static void sync_fc03_state_prefix(uint8_t idx)
{
    if (idx >= ARGB_ENDPOINT_COUNT) {
        return;
    }

    light_state_t *st = &g_light_state[idx];
    uint8_t *state = s_fc03_state[idx];
    uint8_t effective_bri = remembered_brightness(st);

    state[3] = st->on ? 0x01 : 0x00;
    state[4] = effective_bri;
    state[5] = (uint8_t)(st->x & 0xff);
    state[6] = (uint8_t)(st->x >> 8);
    state[7] = (uint8_t)(st->y & 0xff);
    state[8] = (uint8_t)(st->y >> 8);
}

static void init_endpoint_defaults(void)
{
    for (uint8_t idx = 0; idx < ARGB_ENDPOINT_COUNT; idx++) {
        memset(s_fc03_state[idx], 0, sizeof(s_fc03_state[idx]));
        memcpy(s_fc03_state[idx], FC03_DEFAULT_STATE, sizeof(FC03_DEFAULT_STATE));

        s_fc01_attr0[idx] = 0x0B;
        s_fc01_attr1[idx] = 0x00;
        s_fc01_attr2[idx] = 0x0A;
        s_fc01_attr3[idx] = 0x04;
        s_fc03_attr1[idx] = 0x0000000F;
        s_fc03_attr10[idx] = 0x0001;
        s_fc03_attr11[idx] = 0x000000000003FE0EULL;
        s_fc03_attr12[idx] = 0x00000003;
        s_fc03_attr13[idx] = 0x000F;
        s_fc03_attr32[idx] = 0x00;
        s_fc03_attr33[idx] = 0x00;
        s_fc03_attr34[idx] = 0x00;
        s_fc03_attr38[idx] = 0x000A;
        s_fc04_attr0[idx] = 0x1007;
        s_basic_attr1[idx] = 0x00000000;
        s_basic_attr21[idx] = 0x001517EC;
        s_basic_attr41[idx] = 0xC4C1C739;
        s_basic_attr50[idx] = 0x00000001;
        s_basic_attr51[idx] = 0x01;
        s_basic_attr53[idx] = 0x0F4C913C;
        memcpy(s_basic_attr54[idx], BASIC_ATTR54_DEFAULT, sizeof(BASIC_ATTR54_DEFAULT));

        g_light_state[idx] = (light_state_t) {
            .on = false,
            .bri = 0xFE,
            .x = 0x9F8F,
            .y = 0x5C06,
            .last_bri = 0xFE,
        };
        sync_fc03_state_prefix(idx);
    }
}

static void emit_state_json_internal(uint8_t endpoint,
                                     const char *source,
                                     bool has_fade,
                                     uint16_t fade,
                                     bool has_fc03_flags,
                                     uint16_t fc03_flags)
{
    light_state_t *st = NULL;
    if (!get_light_state_for_endpoint(endpoint, NULL, &st)) {
        return;
    }

    rgb_t rgb = light_state_rgb(st);

#if ARGB_BACKEND == ARGB_BACKEND_SERIAL_JSON
    float xf = st->x / 65535.0f;
    float yf = st->y / 65535.0f;
    /* Prefix with DATA: so the PC daemon can ignore regular log lines. */
    printf("DATA: {\"endpoint\":%u", (unsigned)endpoint);
    if (source) {
        printf(",\"source\":\"%s\"", source);
    }
    if (has_fc03_flags) {
        printf(",\"fc03_flags\":%u", (unsigned)fc03_flags);
    }
    if (has_fade) {
        printf(",\"fade\":%u", (unsigned)fade);
    }
    printf(",\"on\":%s,\"bri\":%u,\"x\":%.4f,\"y\":%.4f,\"r\":%u,\"g\":%u,\"b\":%u}\n",
           st->on ? "true" : "false",
           (unsigned)st->bri,
           (double)xf,
           (double)yf,
           (unsigned)rgb.r,
           (unsigned)rgb.g,
           (unsigned)rgb.b);
#else
    local_led_stop_dynamic(endpoint);
    esp_err_t err = local_led_start_solid_transition(endpoint, rgb, has_fade, fade);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "local LED solid update failed: %s", esp_err_to_name(err));
    }
    (void)source;
    (void)has_fc03_flags;
    (void)fc03_flags;
#endif
}

void emit_state_json(uint8_t endpoint)
{
    emit_state_json_internal(endpoint, NULL, false, 0, false, 0);
}

#define FC03_MULTICOLOR_CMD_ID 0x00
#define FC03_MAX_COLORS        9

typedef struct {
    float x;
    float y;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} gradient_color_t;

/* Decode the 3-byte scaled XY encoding used in FC03 multiColor payloads.
     * Real gradient lights pack two 12-bit values into 3 octets:
 *   x = (byte0 | (byte1 & 0x0F) << 8) * 0.7347 / 4095
 *   y = ((byte1 & 0xF0) >> 4 | byte2 << 4) * 0.8413 / 4095
 */
static void scaled_gradient_to_xy(const uint8_t *p, float *x, float *y)
{
    uint16_t xs = p[0] | ((p[1] & 0x0F) << 8);
    uint16_t ys = ((p[1] & 0xF0) >> 4) | (p[2] << 4);
    *x = (xs * 0.7347f) / 4095.0f;
    *y = (ys * 0.8413f) / 4095.0f;
}

static float clamp_unit_float(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static uint16_t xy_float_to_u16(float v)
{
    return (uint16_t)lroundf(clamp_unit_float(v) * 65535.0f);
}

#if ARGB_BACKEND == ARGB_BACKEND_LOCAL_LED
static uint8_t clamp_u8_from_float(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)lroundf(v);
}

static rgb_t interpolate_rgb(rgb_t a, rgb_t b, float t)
{
    t = clamp_unit_float(t);
    return (rgb_t) {
        .r = clamp_u8_from_float((float)a.r + ((float)b.r - (float)a.r) * t),
        .g = clamp_u8_from_float((float)a.g + ((float)b.g - (float)a.g) * t),
        .b = clamp_u8_from_float((float)a.b + ((float)b.b - (float)a.b) * t),
    };
}

static rgb_t gradient_sample_at(const gradient_color_t *colors,
                                uint8_t n_colors,
                                float position,
                                bool wrap)
{
    rgb_t off = {0, 0, 0};
    if (!colors || n_colors == 0) {
        return off;
    }
    if (n_colors == 1) {
        return (rgb_t) {colors[0].r, colors[0].g, colors[0].b};
    }

    if (wrap) {
        position = fmodf(position, (float)n_colors);
        if (position < 0.0f) {
            position += (float)n_colors;
        }
        uint8_t idx = (uint8_t)position;
        uint8_t next_idx = (idx + 1) % n_colors;
        return interpolate_rgb((rgb_t) {colors[idx].r, colors[idx].g, colors[idx].b},
                               (rgb_t) {colors[next_idx].r, colors[next_idx].g, colors[next_idx].b},
                               position - (float)idx);
    }

    position = fmaxf(0.0f, fminf((float)n_colors - 1.0f, position));
    uint8_t idx = (uint8_t)position;
    if (idx >= n_colors - 1) {
        return (rgb_t) {colors[n_colors - 1].r, colors[n_colors - 1].g, colors[n_colors - 1].b};
    }
    return interpolate_rgb((rgb_t) {colors[idx].r, colors[idx].g, colors[idx].b},
                           (rgb_t) {colors[idx + 1].r, colors[idx + 1].g, colors[idx + 1].b},
                           position - (float)idx);
}

static uint8_t gcd_u8(uint8_t a, uint8_t b)
{
    while (b != 0) {
        uint8_t r = a % b;
        a = b;
        b = r;
    }
    return a ? a : 1;
}

static uint8_t coprime_palette_step(uint8_t n_colors)
{
    if (n_colors <= 1) {
        return 1;
    }

    uint8_t step = (uint8_t)lroundf((float)n_colors * 0.61803398875f);
    if (step == 0) {
        step = 1;
    }
    while (gcd_u8(step, n_colors) != 1) {
        step++;
    }
    return step;
}

static void render_gradient_pixels(const gradient_color_t *colors,
                                   uint8_t n_colors,
                                   uint8_t style,
                                   float scale,
                                   float offset,
                                   bool wrap,
                                   bool on,
                                   rgb_t *pixels,
                                   size_t led_count)
{
    rgb_t off = {0, 0, 0};
    if (!pixels || led_count == 0) {
        return;
    }
    if (!on || !colors || n_colors == 0) {
        for (size_t i = 0; i < led_count; i++) {
            pixels[i] = off;
        }
        return;
    }

    if (scale <= 0.0f) {
        scale = (float)n_colors;
    }

    if (style == GRADIENT_STYLE_SCATTERED) {
        int phase_i = (int)floorf(offset);
        phase_i %= n_colors;
        if (phase_i < 0) {
            phase_i += n_colors;
        }
        uint8_t phase = (uint8_t)phase_i;
        uint8_t step = coprime_palette_step(n_colors);
        for (size_t i = 0; i < led_count; i++) {
            uint8_t idx = (uint8_t)((phase + i * step) % n_colors);
            pixels[i] = (rgb_t) {colors[idx].r, colors[idx].g, colors[idx].b};
        }
        return;
    }

    if (style == GRADIENT_STYLE_SEGMENTED) {
        float visible_segments = fmaxf(1.0f, scale);
        for (size_t i = 0; i < led_count; i++) {
            float position = offset + ((float)i / (float)led_count) * visible_segments;
            int idx = (int)floorf(position);
            if (wrap) {
                idx %= n_colors;
                if (idx < 0) {
                    idx += n_colors;
                }
            } else if (idx < 0) {
                idx = 0;
            } else if (idx >= n_colors) {
                idx = n_colors - 1;
            }
            pixels[i] = (rgb_t) {colors[idx].r, colors[idx].g, colors[idx].b};
        }
        return;
    }

    if (style == GRADIENT_STYLE_MIRRORED) {
        float center = ((float)led_count - 1.0f) / 2.0f;
        float max_distance = fmaxf(center, 1.0f);
        float span = fmaxf(0.0f, scale - 1.0f);
        for (size_t i = 0; i < led_count; i++) {
            float position = offset + (fabsf((float)i - center) / max_distance) * span;
            pixels[i] = gradient_sample_at(colors, n_colors, position, wrap);
        }
        return;
    }

    float span = fmaxf(0.0f, scale - 1.0f);
    if (led_count <= 1) {
        pixels[0] = gradient_sample_at(colors, n_colors, offset, wrap);
        return;
    }
    for (size_t i = 0; i < led_count; i++) {
        float position = offset + ((float)i / ((float)led_count - 1.0f)) * span;
        pixels[i] = gradient_sample_at(colors, n_colors, position, wrap);
    }
}

#define LOCAL_DYNAMIC_FRAME_MS          33
#define LOCAL_DYNAMIC_MIN_CYCLE_SECONDS 1.5f
#define LOCAL_DYNAMIC_MAX_CYCLE_SECONDS 90.0f
#define LOCAL_FADE_UNIT_MS             100U
#define LOCAL_FADE_MAX_MS              10000U

typedef struct {
    bool active;
    bool on;
    uint8_t n_colors;
    gradient_color_t colors[FC03_MAX_COLORS];
    uint8_t style;
    float scale;
    float offset;
    uint8_t effect_speed;
    TickType_t started_tick;
} local_led_gradient_runtime_t;

typedef struct {
    bool active;
    rgb_t start[ARGB_LED_COUNT];
    rgb_t target[ARGB_LED_COUNT];
    TickType_t started_tick;
    uint32_t duration_ms;
} local_led_fade_runtime_t;

static local_led_gradient_runtime_t s_local_gradient_runtime[ARGB_ENDPOINT_COUNT];
static local_led_fade_runtime_t s_local_fade_runtime[ARGB_ENDPOINT_COUNT];
static rgb_t s_local_current_pixels[ARGB_LED_COUNT];
static bool s_local_current_pixels_valid;
static SemaphoreHandle_t s_local_animation_lock;
static TaskHandle_t s_local_dynamic_task;

static void local_led_ensure_dynamic_task(void);

static bool local_led_take_animation_lock(void)
{
    if (!s_local_animation_lock) {
        s_local_animation_lock = xSemaphoreCreateMutex();
        if (!s_local_animation_lock) {
            ESP_LOGW(TAG, "failed to create local LED animation mutex");
            return false;
        }
    }

    xSemaphoreTake(s_local_animation_lock, portMAX_DELAY);
    return true;
}

static void local_led_give_animation_lock(void)
{
    if (s_local_animation_lock) {
        xSemaphoreGive(s_local_animation_lock);
    }
}

static uint32_t local_fade_duration_ms(bool has_fade, uint16_t fade)
{
    if (!has_fade || fade == 0) {
        return 0;
    }

    uint32_t duration_ms = (uint32_t)fade * LOCAL_FADE_UNIT_MS;
    return duration_ms > LOCAL_FADE_MAX_MS ? LOCAL_FADE_MAX_MS : duration_ms;
}

static void local_led_fill_pixels(rgb_t *pixels, size_t led_count, rgb_t color)
{
    for (size_t i = 0; i < led_count; i++) {
        pixels[i] = color;
    }
}

static size_t local_led_total_pixel_count(void)
{
    size_t led_count = local_led_backend_pixel_count();
    return led_count > ARGB_LED_COUNT ? ARGB_LED_COUNT : led_count;
}

static bool local_led_slice_for_idx(uint8_t idx, size_t *start_out, size_t *count_out)
{
    size_t led_count = local_led_total_pixel_count();
    size_t start = (size_t)idx * (size_t)ARGB_ENDPOINT_LED_COUNT;
    if (start >= led_count) {
        ESP_LOGW(TAG,
                 "local LED endpoint slice out of range: idx=%u start=%u leds=%u",
                 (unsigned)idx,
                 (unsigned)start,
                 (unsigned)led_count);
        return false;
    }

    size_t count = ARGB_ENDPOINT_LED_COUNT;
    if (count > led_count - start) {
        count = led_count - start;
    }
    if (start_out) {
        *start_out = start;
    }
    if (count_out) {
        *count_out = count;
    }
    return true;
}

static void local_led_copy_endpoint_slice(rgb_t *frame,
                                          size_t frame_count,
                                          uint8_t idx,
                                          const rgb_t *pixels,
                                          size_t count)
{
    size_t start = 0;
    size_t slice_count = 0;
    rgb_t off = {0, 0, 0};
    if (!frame || !local_led_slice_for_idx(idx, &start, &slice_count)) {
        return;
    }

    if (start >= frame_count) {
        return;
    }
    if (start + slice_count > frame_count) {
        slice_count = frame_count - start;
    }
    for (size_t i = 0; i < slice_count; i++) {
        frame[start + i] = (pixels && i < count) ? pixels[i] : off;
    }
}

static void local_led_current_pixels_locked(rgb_t *out, size_t led_count)
{
    rgb_t off = {0, 0, 0};
    if (!s_local_current_pixels_valid) {
        local_led_fill_pixels(out, led_count, off);
        return;
    }

    memcpy(out, s_local_current_pixels, led_count * sizeof(out[0]));
}

static esp_err_t local_led_show_pixels_now_locked(const rgb_t *pixels, size_t count)
{
    rgb_t full_pixels[ARGB_LED_COUNT];
    rgb_t off = {0, 0, 0};
    size_t led_count = local_led_total_pixel_count();
    for (size_t i = 0; i < led_count; i++) {
        full_pixels[i] = (pixels && i < count) ? pixels[i] : off;
    }

    esp_err_t err = local_led_backend_show_pixels(full_pixels, led_count);
    if (err == ESP_OK) {
        memcpy(s_local_current_pixels, full_pixels, led_count * sizeof(full_pixels[0]));
        s_local_current_pixels_valid = true;
    }
    return err;
}

static esp_err_t local_led_start_pixel_transition(uint8_t endpoint,
                                                  const rgb_t *target,
                                                  size_t count,
                                                  bool has_fade,
                                                  uint16_t fade)
{
    uint8_t idx = 0;
    if (!get_light_state_for_endpoint(endpoint, &idx, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    rgb_t target_pixels[ARGB_LED_COUNT];
    size_t led_count = local_led_total_pixel_count();
    if (!local_led_slice_for_idx(idx, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t duration_ms = local_fade_duration_ms(has_fade, fade);

    if (!local_led_take_animation_lock()) {
        return ESP_ERR_NO_MEM;
    }

    local_led_current_pixels_locked(target_pixels, led_count);
    local_led_copy_endpoint_slice(target_pixels, led_count, idx, target, count);

    s_local_fade_runtime[idx].active = false;
    if (duration_ms == 0) {
        esp_err_t err = local_led_show_pixels_now_locked(target_pixels, led_count);
        local_led_give_animation_lock();
        return err;
    }

    local_led_current_pixels_locked(s_local_fade_runtime[idx].start, led_count);
    memcpy(s_local_fade_runtime[idx].target, target_pixels, led_count * sizeof(target_pixels[0]));
    s_local_fade_runtime[idx].started_tick = xTaskGetTickCount();
    s_local_fade_runtime[idx].duration_ms = duration_ms;
    s_local_fade_runtime[idx].active = true;
    local_led_give_animation_lock();
    local_led_ensure_dynamic_task();
    return ESP_OK;
}

static esp_err_t local_led_start_solid_transition(uint8_t endpoint,
                                                  rgb_t color,
                                                  bool has_fade,
                                                  uint16_t fade)
{
    rgb_t pixels[ARGB_ENDPOINT_LED_COUNT];
    local_led_fill_pixels(pixels, ARGB_ENDPOINT_LED_COUNT, color);
    return local_led_start_pixel_transition(endpoint, pixels, ARGB_ENDPOINT_LED_COUNT, has_fade, fade);
}

static float local_dynamic_cycle_seconds(uint8_t effect_speed)
{
    uint8_t speed = effect_speed;
    if (speed == 0) {
        speed = 1;
    } else if (speed > 254) {
        speed = 254;
    }

    float speed_ratio = (float)speed / 254.0f;
    float remaining_ratio = 1.0f - speed_ratio;
    return LOCAL_DYNAMIC_MIN_CYCLE_SECONDS +
           ((LOCAL_DYNAMIC_MAX_CYCLE_SECONDS - LOCAL_DYNAMIC_MIN_CYCLE_SECONDS) *
            remaining_ratio *
            remaining_ratio);
}

static float local_dynamic_offset(const local_led_gradient_runtime_t *state, TickType_t now)
{
    float cycle_seconds = local_dynamic_cycle_seconds(state->effect_speed);
    if (cycle_seconds <= 0.0f || state->n_colors == 0) {
        return state->offset;
    }

    if ((int32_t)(now - state->started_tick) <= 0) {
        return state->offset;
    }

    TickType_t elapsed_ticks = now - state->started_tick;
    float elapsed_seconds = ((float)pdTICKS_TO_MS(elapsed_ticks)) / 1000.0f;
    return state->offset + (elapsed_seconds / cycle_seconds) * (float)state->n_colors;
}

static void local_led_dynamic_task(void *arg)
{
    (void)arg;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (local_led_take_animation_lock()) {
            size_t led_count = local_led_total_pixel_count();
            rgb_t frame[ARGB_LED_COUNT];
            bool frame_changed = false;
            local_led_current_pixels_locked(frame, led_count);

            for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
                size_t slice_start = 0;
                size_t slice_count = 0;
                if (!local_led_slice_for_idx(i, &slice_start, &slice_count)) {
                    continue;
                }

                if (s_local_fade_runtime[i].active) {
                    uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - s_local_fade_runtime[i].started_tick);
                    float t = s_local_fade_runtime[i].duration_ms
                        ? (float)elapsed_ms / (float)s_local_fade_runtime[i].duration_ms
                        : 1.0f;
                    if (t >= 1.0f) {
                        t = 1.0f;
                        s_local_fade_runtime[i].active = false;
                    }
                    for (size_t led = 0; led < slice_count; led++) {
                        size_t pos = slice_start + led;
                        frame[pos] = interpolate_rgb(s_local_fade_runtime[i].start[pos],
                                                     s_local_fade_runtime[i].target[pos],
                                                     t);
                    }
                    frame_changed = true;
                    continue;
                }

                local_led_gradient_runtime_t state = s_local_gradient_runtime[i];
                if (!state.active || !state.on) {
                    continue;
                }
                rgb_t pixels[ARGB_ENDPOINT_LED_COUNT];
                render_gradient_pixels(state.colors,
                                       state.n_colors,
                                       state.style,
                                       state.scale,
                                       local_dynamic_offset(&state, now),
                                       true,
                                       state.on,
                                       pixels,
                                       slice_count);
                local_led_copy_endpoint_slice(frame, led_count, i, pixels, slice_count);
                frame_changed = true;
            }

            if (frame_changed) {
                esp_err_t err = local_led_show_pixels_now_locked(frame, led_count);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "local LED animation frame failed: %s", esp_err_to_name(err));
                }
            }
            local_led_give_animation_lock();
        }
        vTaskDelay(pdMS_TO_TICKS(LOCAL_DYNAMIC_FRAME_MS));
    }
}

static void local_led_ensure_dynamic_task(void)
{
    if (s_local_dynamic_task) {
        return;
    }

    BaseType_t ok = xTaskCreate(local_led_dynamic_task,
                                "local_led_dyn",
                                4096,
                                NULL,
                                1,
                                &s_local_dynamic_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "failed to start local LED dynamic task");
        s_local_dynamic_task = NULL;
    }
}

static void local_led_stop_dynamic(uint8_t endpoint)
{
    uint8_t idx = 0;
    if (!get_light_state_for_endpoint(endpoint, &idx, NULL)) {
        return;
    }
    if (!local_led_take_animation_lock()) {
        return;
    }
    s_local_gradient_runtime[idx].active = false;
    local_led_give_animation_lock();
}

static void local_led_update_dynamic_speed(uint8_t endpoint, uint8_t effect_speed)
{
    uint8_t idx = 0;
    if (!get_light_state_for_endpoint(endpoint, &idx, NULL)) {
        return;
    }

    local_led_gradient_runtime_t *state = &s_local_gradient_runtime[idx];
    if (!local_led_take_animation_lock()) {
        return;
    }
    if (state->n_colors == 0) {
        local_led_give_animation_lock();
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (state->active && state->on) {
        state->offset = local_dynamic_offset(state, now);
    }
    state->effect_speed = effect_speed;
    state->started_tick = now;
    state->active = state->on;

    bool active = state->active;
    local_led_give_animation_lock();
    if (active) {
        local_led_ensure_dynamic_task();
    }
}

static void local_led_show_gradient_update(uint8_t endpoint,
                                           const gradient_color_t *colors,
                                           uint8_t n_colors,
                                           bool on,
                                           uint8_t style,
                                           uint8_t scale_raw,
                                           uint8_t offset_raw,
                                           bool has_fade,
                                           uint16_t fade,
                                           bool has_effect_speed,
                                           uint8_t effect_speed)
{
    uint8_t idx = 0;
    if (!get_light_state_for_endpoint(endpoint, &idx, NULL)) {
        return;
    }

    size_t led_count = local_led_total_pixel_count();
    size_t slice_count = 0;
    if (!local_led_slice_for_idx(idx, NULL, &slice_count)) {
        return;
    }

    local_led_gradient_runtime_t *state = &s_local_gradient_runtime[idx];
    uint32_t fade_ms = local_fade_duration_ms(has_fade, fade);
    TickType_t now = xTaskGetTickCount();
    local_led_gradient_runtime_t snapshot = {
        .active = has_effect_speed && on,
        .on = on,
        .n_colors = n_colors,
        .style = style,
        .scale = (float)scale_raw / 8.0f,
        .offset = (float)offset_raw / 8.0f,
        .effect_speed = effect_speed,
        .started_tick = now + pdMS_TO_TICKS(fade_ms),
    };
    memcpy(snapshot.colors, colors, n_colors * sizeof(colors[0]));

    rgb_t target_pixels[ARGB_ENDPOINT_LED_COUNT];
    render_gradient_pixels(snapshot.colors,
                           snapshot.n_colors,
                           snapshot.style,
                           snapshot.scale,
                           snapshot.offset,
                           has_effect_speed,
                           snapshot.on,
                           target_pixels,
                           slice_count);

    if (!local_led_take_animation_lock()) {
        return;
    }
    *state = snapshot;
    s_local_fade_runtime[idx].active = false;
    if (fade_ms == 0) {
        rgb_t frame[ARGB_LED_COUNT];
        local_led_current_pixels_locked(frame, led_count);
        local_led_copy_endpoint_slice(frame, led_count, idx, target_pixels, slice_count);
        esp_err_t err = local_led_show_pixels_now_locked(frame, led_count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "local LED gradient update failed: %s", esp_err_to_name(err));
        }
    } else {
        local_led_current_pixels_locked(s_local_fade_runtime[idx].start, led_count);
        memcpy(s_local_fade_runtime[idx].target,
               s_local_fade_runtime[idx].start,
               led_count * sizeof(s_local_fade_runtime[idx].target[0]));
        local_led_copy_endpoint_slice(s_local_fade_runtime[idx].target,
                                      led_count,
                                      idx,
                                      target_pixels,
                                      slice_count);
        s_local_fade_runtime[idx].started_tick = now;
        s_local_fade_runtime[idx].duration_ms = fade_ms;
        s_local_fade_runtime[idx].active = true;
    }
    local_led_give_animation_lock();

    if (snapshot.active || fade_ms > 0) {
        local_led_ensure_dynamic_task();
    }
}
#endif

/* Convert "mirek" color temperature (micro reciprocal kelvin) into the
 * same xy state used by the rest of the FC03 path. The polynomial is the
 * common CCT-to-CIE approximation used for warm/cool white lamps. */
static bool mirek_to_xy(uint16_t mirek, uint16_t *x_out, uint16_t *y_out)
{
    if (mirek == 0 || x_out == NULL || y_out == NULL) {
        return false;
    }

    float kelvin = 1000000.0f / (float)mirek;
    if (kelvin < 1667.0f) {
        kelvin = 1667.0f;
    } else if (kelvin > 25000.0f) {
        kelvin = 25000.0f;
    }

    float t = kelvin;
    float x;
    if (t <= 4000.0f) {
        x = (-0.2661239e9f / (t * t * t)) -
            (0.2343580e6f / (t * t)) +
            (0.8776956e3f / t) +
            0.179910f;
    } else {
        x = (-3.0258469e9f / (t * t * t)) +
            (2.1070379e6f / (t * t)) +
            (0.2226347e3f / t) +
            0.240390f;
    }

    float y;
    if (t <= 2222.0f) {
        y = (-1.1063814f * x * x * x) -
            (1.34811020f * x * x) +
            (2.18555832f * x) -
            0.20219683f;
    } else if (t <= 4000.0f) {
        y = (-0.9549476f * x * x * x) -
            (1.37418593f * x * x) +
            (2.09137015f * x) -
            0.16748867f;
    } else {
        y = (3.0817580f * x * x * x) -
            (5.87338670f * x * x) +
            (3.75112997f * x) -
            0.37001483f;
    }

    x = clamp_unit_float(x);
    y = clamp_unit_float(y);
    *x_out = (uint16_t)lroundf(x * 65535.0f);
    *y_out = (uint16_t)lroundf(y * 65535.0f);
    return true;
}

static void set_standard_attr(uint8_t endpoint,
                              uint16_t cluster_id,
                              uint16_t attr_id,
                              void *value)
{
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(endpoint,
                                                              cluster_id,
                                                              ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                              attr_id,
                                                              value,
                                                              false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "standard attr sync failed: ep=%u cluster=0x%04x attr=0x%04x status=0x%02x",
                 (unsigned)endpoint,
                 (unsigned)cluster_id,
                 (unsigned)attr_id,
                 (unsigned)status);
    }
}

static void sync_fc03_to_standard_attrs(uint8_t endpoint,
                                        const light_state_t *st,
                                        bool has_on,
                                        bool has_bri,
                                        bool has_color,
                                        bool has_mirek,
                                        uint16_t mirek)
{
    if (!st) {
        return;
    }

    if (has_on) {
        bool on = st->on;
        set_standard_attr(endpoint,
                          ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                          ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                          &on);
    }
    if (has_bri) {
        uint8_t bri = st->bri;
        set_standard_attr(endpoint,
                          ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                          ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
                          &bri);
    }
    if (has_color) {
        uint16_t x = st->x;
        uint16_t y = st->y;
        set_standard_attr(endpoint,
                          ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID,
                          &x);
        set_standard_attr(endpoint,
                          ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID,
                          &y);
    }
    if (has_mirek) {
        uint16_t color_temperature = mirek;
        set_standard_attr(endpoint,
                          ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                          &color_temperature);
    }
}

static void power_recovery_set_defaults(void)
{
    for (uint8_t idx = 0; idx < ARGB_ENDPOINT_COUNT; idx++) {
        s_startup_on_off[idx] = 0xFF;
        s_startup_current_level[idx] = 0xFF;
        s_startup_color_temperature[idx] = 0xFFFF;
        s_startup_current_x[idx] = 0xFFFF;
        s_startup_current_y[idx] = 0xFFFF;

        if (g_light_state[idx].bri == 0) {
            g_light_state[idx].bri = 0xFE;
        }
        if (g_light_state[idx].last_bri == 0) {
            g_light_state[idx].last_bri = g_light_state[idx].bri;
        }
        sync_fc03_state_prefix(idx);
    }
}

static void power_recovery_fill_blob(power_recovery_nvs_blob_t *blob)
{
    if (!blob) {
        return;
    }

    memset(blob, 0, sizeof(*blob));
    blob->magic = POWER_RECOVERY_NVS_MAGIC;
    blob->version = POWER_RECOVERY_NVS_VERSION;
    blob->entry_count = ARGB_ENDPOINT_COUNT;
    for (uint8_t idx = 0; idx < ARGB_ENDPOINT_COUNT; idx++) {
        const light_state_t *st = &g_light_state[idx];
        power_recovery_nvs_entry_t *entry = &blob->entries[idx];
        entry->on = st->on ? 1 : 0;
        entry->bri = st->bri;
        entry->last_bri = st->last_bri;
        entry->startup_on_off = s_startup_on_off[idx];
        entry->x = st->x;
        entry->y = st->y;
        entry->startup_current_level = s_startup_current_level[idx];
        entry->startup_color_temperature = s_startup_color_temperature[idx];
        entry->startup_current_x = s_startup_current_x[idx];
        entry->startup_current_y = s_startup_current_y[idx];
    }
}

static void load_power_recovery_state(void)
{
    power_recovery_set_defaults();

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(POWER_RECOVERY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "POWER_RECOVERY_LOAD: no persisted state");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POWER_RECOVERY_LOAD: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    power_recovery_nvs_blob_t blob;
    size_t blob_size = sizeof(blob);
    err = nvs_get_blob(nvs, POWER_RECOVERY_NVS_KEY, &blob, &blob_size);
    nvs_close(nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "POWER_RECOVERY_LOAD: no persisted state");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POWER_RECOVERY_LOAD: failed: %s", esp_err_to_name(err));
        return;
    }
    if (blob_size != sizeof(blob) ||
        blob.magic != POWER_RECOVERY_NVS_MAGIC ||
        blob.version != POWER_RECOVERY_NVS_VERSION ||
        blob.entry_count != ARGB_ENDPOINT_COUNT) {
        ESP_LOGW(TAG, "POWER_RECOVERY_LOAD: ignored incompatible blob");
        return;
    }

    for (uint8_t idx = 0; idx < ARGB_ENDPOINT_COUNT; idx++) {
        const power_recovery_nvs_entry_t *entry = &blob.entries[idx];
        light_state_t *st = &g_light_state[idx];
        st->on = entry->on != 0;
        st->bri = entry->bri ? entry->bri : 0xFE;
        st->last_bri = entry->last_bri ? entry->last_bri : st->bri;
        st->x = entry->x;
        st->y = entry->y;
        s_startup_on_off[idx] = entry->startup_on_off;
        s_startup_current_level[idx] = entry->startup_current_level;
        s_startup_color_temperature[idx] = entry->startup_color_temperature;
        s_startup_current_x[idx] = entry->startup_current_x;
        s_startup_current_y[idx] = entry->startup_current_y;
        sync_fc03_state_prefix(idx);
    }

    s_power_recovery_last_saved = blob;
    s_power_recovery_has_last_saved = true;
    ESP_LOGI(TAG, "POWER_RECOVERY_LOAD: entries=%u", (unsigned)ARGB_ENDPOINT_COUNT);
}

static void save_power_recovery_state(const char *reason)
{
    power_recovery_nvs_blob_t blob;
    power_recovery_fill_blob(&blob);

    if (s_power_recovery_has_last_saved &&
        memcmp(&blob, &s_power_recovery_last_saved, sizeof(blob)) == 0) {
        return;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(POWER_RECOVERY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POWER_RECOVERY_SAVE: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs, POWER_RECOVERY_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POWER_RECOVERY_SAVE: failed: %s", esp_err_to_name(err));
        return;
    }

    s_power_recovery_last_saved = blob;
    s_power_recovery_has_last_saved = true;
    ESP_LOGI(TAG, "POWER_RECOVERY_SAVE: reason=%s entries=%u",
             reason ? reason : "-",
             (unsigned)ARGB_ENDPOINT_COUNT);
}

static void apply_power_recovery_startup(uint8_t endpoint)
{
    uint8_t idx = 0;
    light_state_t *st = NULL;
    if (!get_light_state_for_endpoint(endpoint, &idx, &st)) {
        return;
    }

    bool has_on = false;
    bool has_bri = false;
    bool has_color = false;
    bool has_mirek = false;
    uint16_t mirek = 0;

    switch (s_startup_on_off[idx]) {
    case 0x00:
        st->on = false;
        has_on = true;
        break;
    case 0x01:
        st->on = true;
        has_on = true;
        break;
    case 0x02:
        st->on = !st->on;
        has_on = true;
        break;
    case 0xFF:
    default:
        break;
    }

    if (s_startup_current_level[idx] != 0xFF) {
        st->bri = s_startup_current_level[idx];
        if (st->bri) {
            st->last_bri = st->bri;
        }
        has_bri = true;
    }

    if (s_startup_current_x[idx] != 0xFFFF && s_startup_current_y[idx] != 0xFFFF) {
        st->x = s_startup_current_x[idx];
        st->y = s_startup_current_y[idx];
        has_color = true;
    } else if (s_startup_color_temperature[idx] != 0xFFFF &&
               mirek_to_xy(s_startup_color_temperature[idx], &st->x, &st->y)) {
        mirek = s_startup_color_temperature[idx];
        has_color = true;
        has_mirek = true;
    }

    if (st->on && st->bri == 0) {
        st->bri = st->last_bri ? st->last_bri : 0xFE;
        st->last_bri = st->bri;
        has_bri = true;
    }

    sync_fc03_to_standard_attrs(endpoint, st, has_on, has_bri, has_color, has_mirek, mirek);
    sync_fc03_state_prefix(idx);

    ESP_LOGI(TAG,
             "POWER_RECOVERY_APPLY: ep=%u startup_on=0x%02x startup_level=0x%02x startup_ct=0x%04x startup_xy=0x%04x/0x%04x on=%u bri=%u xy=0x%04x/0x%04x",
             (unsigned)endpoint,
             (unsigned)s_startup_on_off[idx],
             (unsigned)s_startup_current_level[idx],
             (unsigned)s_startup_color_temperature[idx],
             (unsigned)s_startup_current_x[idx],
             (unsigned)s_startup_current_y[idx],
             st->on ? 1U : 0U,
             (unsigned)st->bri,
             (unsigned)st->x,
             (unsigned)st->y);
    emit_state_json_internal(endpoint, "startup", false, 0, false, 0);
    save_power_recovery_state("startup_apply");
}

static void emit_gradient_json(uint8_t endpoint,
                               uint16_t fc03_flags,
                               uint8_t n_colors,
                               const gradient_color_t *colors,
                               bool on,
                               uint8_t bri,
                               uint8_t style,
                               uint8_t scale_raw,
                               uint8_t offset_raw,
                               bool has_fade,
                               uint16_t fade,
                               bool has_effect_speed,
                               uint8_t effect_speed)
{
#if ARGB_BACKEND == ARGB_BACKEND_SERIAL_JSON
    printf("DATA: {\"endpoint\":%u,\"source\":\"fc03\",\"fc03_flags\":%u,\"on\":%s,\"bri\":%u,\"gradient\":true,\"n\":%u,\"style\":%u,\"segments\":%u,\"scale\":%.3f,\"scale_raw\":%u,\"offset\":%u,\"offset_raw\":%u",
           (unsigned)endpoint,
           (unsigned)fc03_flags,
           on ? "true" : "false",
           (unsigned)bri,
           (unsigned)n_colors,
           (unsigned)style,
           (unsigned)(scale_raw >> 3),
           (double)scale_raw / 8.0,
           (unsigned)scale_raw,
           (unsigned)(offset_raw >> 3),
           (unsigned)offset_raw);
    if (has_fade) {
        printf(",\"fade\":%u", (unsigned)fade);
    }
    if (has_effect_speed) {
        printf(",\"effect_speed\":%u", (unsigned)effect_speed);
    }
    printf(",\"colors\":[");
    for (uint8_t i = 0; i < n_colors; i++) {
        if (i) {
            printf(",");
        }
        printf("{\"x\":%.4f,\"y\":%.4f,\"r\":%u,\"g\":%u,\"b\":%u}",
               (double)colors[i].x, (double)colors[i].y,
               (unsigned)colors[i].r, (unsigned)colors[i].g, (unsigned)colors[i].b);
    }
    printf("]}\n");
#else
    local_led_show_gradient_update(endpoint,
                                   colors,
                                   n_colors,
                                   on,
                                   style,
                                   scale_raw,
                                   offset_raw,
                                   has_fade,
                                   fade,
                                   has_effect_speed,
                                   effect_speed);
    (void)fc03_flags;
    (void)bri;
#endif
}

static void emit_effect_speed_update(uint8_t endpoint,
                                     uint16_t fc03_flags,
                                     bool has_fade,
                                     uint16_t fade,
                                     uint8_t effect_speed)
{
#if ARGB_BACKEND == ARGB_BACKEND_SERIAL_JSON
    printf("DATA: {\"endpoint\":%u,\"source\":\"fc03\",\"fc03_flags\":%u",
           (unsigned)endpoint,
           (unsigned)fc03_flags);
    if (has_fade) {
        printf(",\"fade\":%u", (unsigned)fade);
    }
    printf(",\"effect_speed\":%u}\n", (unsigned)effect_speed);
#else
    local_led_update_dynamic_speed(endpoint, effect_speed);
    (void)fc03_flags;
    (void)has_fade;
    (void)fade;
#endif
}

static bool fc03_require_bytes(size_t offset, size_t need, size_t size, const char *field)
{
    if (offset > size || need > size - offset) {
        ESP_LOGW(TAG, "FC03 payload truncated before %s: need %u at %u, have %u",
                 field, (unsigned)need, (unsigned)offset, (unsigned)size);
        return false;
    }
    return true;
}

/* Parse a FC03 command-0 payload.
 *
 * The first two bytes are a little-endian property bitset. Fields then appear
 * in the fixed order documented in references/specs/bifrost-hue-zigbee-format-
 * fc03.md. That makes the older "formats" observed in captures just specific
 * flag combinations:
 *   0x0150: fade + gradient colors + gradient params
 *   0x0151: on/off + fade + gradient colors + gradient params
 *   0x0011: on/off + fade, used by the app for on/off toggles
 */
static void handle_fc03_multicolor_command(uint8_t endpoint, const uint8_t *data, size_t size)
{
    if (size < 2) {
        ESP_LOGW(TAG, "FC03 payload too short (%d bytes)", (int)size);
        return;
    }

    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        ESP_LOGW(TAG, "FC03 command for unsupported endpoint %u", (unsigned)endpoint);
        return;
    }

    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    light_state_t *st = &g_light_state[idx];
    uint16_t flags = u16_le(data);
    if (flags & ~FC03_KNOWN_FLAGS) {
        ESP_LOGW(TAG, "FC03 update has unknown flags 0x%04x", (unsigned)(flags & ~FC03_KNOWN_FLAGS));
    }

    size_t off = 2;
    bool has_on = false;
    bool has_bri = false;
    bool has_mirek = false;
    bool has_xy = false;
    bool has_fade = false;
    bool has_effect = false;
    bool has_effect_speed = false;
    bool has_gradient = false;
    bool has_gradient_params = false;
    uint16_t fade = 0;
    uint16_t mirek = 0;
    uint8_t effect = 0;
    uint8_t effect_speed = 0;
    uint8_t n_colors = 0;
    uint8_t style = 0;
    uint8_t scale_raw = 0;
    uint8_t offset_raw = 0;
    gradient_color_t colors[FC03_MAX_COLORS];
    const uint8_t *gradient_color_bytes = NULL;

    if (flags & FC03_FLAG_ON_OFF) {
        if (!fc03_require_bytes(off, 1, size, "on/off")) {
            return;
        }
        st->on = data[off++] != 0;
        has_on = true;
    }

    if (flags & FC03_FLAG_BRIGHTNESS) {
        if (!fc03_require_bytes(off, 1, size, "brightness")) {
            return;
        }
        uint8_t bri = data[off++];
        if (bri == 0 || bri == 0xff) {
            ESP_LOGW(TAG, "FC03 brightness value out of expected range: %u", (unsigned)bri);
        } else {
            st->bri = bri;
            st->last_bri = bri;
            has_bri = true;
        }
    }

    if (flags & FC03_FLAG_COLOR_MIREK) {
        if (!fc03_require_bytes(off, 2, size, "mirek")) {
            return;
        }
        mirek = u16_le(&data[off]);
        off += 2;
        if (mirek_to_xy(mirek, &st->x, &st->y)) {
            has_mirek = true;
        } else {
            ESP_LOGW(TAG, "FC03 mirek value out of range: %u", (unsigned)mirek);
        }
    }

    if (flags & FC03_FLAG_COLOR_XY) {
        if (!fc03_require_bytes(off, 4, size, "xy")) {
            return;
        }
        st->x = u16_le(&data[off]);
        st->y = u16_le(&data[off + 2]);
        off += 4;
        has_xy = true;
    }

    if (flags & FC03_FLAG_FADE_SPEED) {
        if (!fc03_require_bytes(off, 2, size, "fade speed")) {
            return;
        }
        fade = u16_le(&data[off]);
        off += 2;
        has_fade = true;
    }

    if (flags & FC03_FLAG_EFFECT_TYPE) {
        if (!fc03_require_bytes(off, 1, size, "effect type")) {
            return;
        }
        effect = data[off++];
        has_effect = true;
    }

    if (flags & FC03_FLAG_GRADIENT_COLORS) {
        if (!fc03_require_bytes(off, 5, size, "gradient color header")) {
            return;
        }

        uint8_t color_payload_len = data[off];
        if (!fc03_require_bytes(off, 1 + color_payload_len, size, "gradient colors")) {
            return;
        }

        n_colors = data[off + 1] >> 4;
        if (data[off + 1] & 0x0f) {
            ESP_LOGW(TAG, "FC03 gradient color count low nibble is non-zero: 0x%02x",
                     (unsigned)data[off + 1]);
        }
        if (n_colors == 0 || n_colors > FC03_MAX_COLORS) {
            ESP_LOGW(TAG, "FC03 gradient unsupported color count %u", (unsigned)n_colors);
            return;
        }

        uint8_t expected_color_payload_len = 4 + 3 * n_colors;
        if (color_payload_len != expected_color_payload_len) {
            ESP_LOGW(TAG, "FC03 gradient length %u does not match %u colors (expected %u)",
                     (unsigned)color_payload_len,
                     (unsigned)n_colors,
                     (unsigned)expected_color_payload_len);
        }

        size_t colors_offset = off + 5;
        if (!fc03_require_bytes(colors_offset, 3 * n_colors, off + 1 + color_payload_len,
                                "gradient color data")) {
            return;
        }

        style = data[off + 2];
        gradient_color_bytes = &data[colors_offset];
        uint8_t color_bri = st->bri ? st->bri : st->last_bri;
        if (color_bri == 0) {
            color_bri = 0xFE;
        }
        for (uint8_t i = 0; i < n_colors; i++) {
            const uint8_t *p = &gradient_color_bytes[3 * i];
            scaled_gradient_to_xy(p, &colors[i].x, &colors[i].y);
            rgb_t rgb = xy_to_rgb(colors[i].x, colors[i].y, color_bri);
            colors[i].r = rgb.r;
            colors[i].g = rgb.g;
            colors[i].b = rgb.b;
        }
        st->x = xy_float_to_u16(colors[0].x);
        st->y = xy_float_to_u16(colors[0].y);

        off += 1 + color_payload_len;
        has_gradient = true;
    }

    if (flags & FC03_FLAG_EFFECT_SPEED) {
        if (!fc03_require_bytes(off, 1, size, "effect speed")) {
            return;
        }
        effect_speed = data[off++];
        has_effect_speed = true;
    }

    if (flags & FC03_FLAG_GRADIENT_PARAMS) {
        if (!fc03_require_bytes(off, 2, size, "gradient params")) {
            return;
        }
        scale_raw = data[off++];
        offset_raw = data[off++];
        has_gradient_params = true;
    }

    if (off != size) {
        ESP_LOGW(TAG, "FC03 payload has %u trailing byte(s)", (unsigned)(size - off));
    }

    if (has_on && st->on && st->bri == 0) {
        st->bri = st->last_bri ? st->last_bri : 0xFE;
        st->last_bri = st->bri;
        has_bri = true;
    }

    sync_fc03_to_standard_attrs(endpoint, st, has_on, has_bri, has_mirek || has_xy || has_gradient, has_mirek, mirek);
    sync_fc03_state_prefix(idx);

    /* Keep the FC03 state attribute in sync so ZHA/bridge reads reflect the
     * currently active gradient. */
    if (has_gradient) {
        uint8_t *state = s_fc03_state[idx];
        uint8_t payload_len = 15 + 3 * n_colors;
        state[0] = payload_len;
        state[1] = 0x4B;
        state[2] = 0x01;
        sync_fc03_state_prefix(idx);
        state[9] = 4 + 3 * n_colors;
        state[10] = n_colors << 4;
        state[11] = style;
        state[12] = state[13] = 0x00;
        memcpy(&state[14], gradient_color_bytes, 3 * n_colors);
        state[14 + 3 * n_colors] = scale_raw;
        state[15 + 3 * n_colors] = offset_raw;
    }

    ESP_LOGI(TAG, "FC03 update: flags=0x%04x on=%s bri=%s mirek=%s xy=%s fade=%s gradient=%u effect=%d speed=%d params=%s scale=%u offset=%u",
             (unsigned)flags,
             has_on ? (st->on ? "true" : "false") : "-",
             has_bri ? "set" : "-",
             has_mirek ? "set" : "-",
             has_xy ? "set" : "-",
             has_fade ? "set" : "-",
             (unsigned)n_colors,
             has_effect ? (int)effect : -1,
             has_effect_speed ? (int)effect_speed : -1,
             has_gradient_params ? "set" : "-",
             (unsigned)(scale_raw >> 3),
             (unsigned)(offset_raw >> 3));

    if (has_gradient) {
        emit_gradient_json(endpoint,
                           flags,
                           n_colors,
                           colors,
                           st->on,
                           st->bri,
                           style,
                           scale_raw,
                           offset_raw,
                           has_fade,
                           fade,
                           has_effect_speed,
                           effect_speed);
    } else if (has_effect_speed) {
        emit_effect_speed_update(endpoint,
                                 flags,
                                 has_fade,
                                 fade,
                                 effect_speed);
    } else if (has_on || has_bri || has_mirek || has_xy) {
        emit_state_json_internal(endpoint, "fc03", has_fade, fade, true, flags);
    }

    if (has_on || has_bri || has_mirek || has_xy || has_gradient) {
        save_power_recovery_state("fc03");
    }
}

typedef struct {
    bool valid;
    uint8_t endpoint;
    uint16_t group_id;
    uint8_t scene_id;
    uint8_t fc03_len;
    uint8_t fc03[SCENE_FC03_MAX_PAYLOAD_LEN];
    uint32_t age;
} scene_fc03_cache_entry_t;

typedef struct {
    uint8_t valid;
    uint8_t endpoint;
    uint8_t scene_id;
    uint8_t fc03_len;
    uint16_t group_id;
    uint16_t reserved;
    uint32_t age;
    uint8_t fc03[SCENE_FC03_MAX_PAYLOAD_LEN];
} scene_fc03_cache_nvs_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_count;
    uint32_t cache_age;
    scene_fc03_cache_nvs_entry_t entries[SCENE_FC03_CACHE_ENTRIES];
} scene_fc03_cache_nvs_blob_t;

typedef struct {
    bool valid;
    uint8_t endpoint;
    uint16_t group_id;
    uint8_t scene_id;
} pending_scene_recall_t;

static scene_fc03_cache_entry_t s_scene_fc03_cache[SCENE_FC03_CACHE_ENTRIES];
static uint32_t s_scene_fc03_cache_age;
static bool s_scene_fc03_cache_loaded;
static pending_scene_recall_t s_pending_scene_recall;

static size_t scene_fc03_cache_valid_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SCENE_FC03_CACHE_ENTRIES; i++) {
        if (s_scene_fc03_cache[i].valid) {
            count++;
        }
    }
    return count;
}

static void save_scene_fc03_cache(void)
{
    scene_fc03_cache_nvs_blob_t *blob = calloc(1, sizeof(*blob));
    if (!blob) {
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_SAVE: no heap for blob");
        return;
    }
    blob->magic = SCENE_FC03_CACHE_MAGIC;
    blob->version = SCENE_FC03_CACHE_VERSION;
    blob->entry_count = SCENE_FC03_CACHE_ENTRIES;
    blob->cache_age = s_scene_fc03_cache_age;

    for (size_t i = 0; i < SCENE_FC03_CACHE_ENTRIES; i++) {
        const scene_fc03_cache_entry_t *src = &s_scene_fc03_cache[i];
        scene_fc03_cache_nvs_entry_t *dst = &blob->entries[i];
        dst->valid = src->valid ? 1 : 0;
        dst->endpoint = src->endpoint;
        dst->group_id = src->group_id;
        dst->scene_id = src->scene_id;
        dst->fc03_len = src->fc03_len;
        dst->age = src->age;
        memcpy(dst->fc03, src->fc03, sizeof(dst->fc03));
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SCENE_FC03_CACHE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_SAVE: nvs_open failed: %s", esp_err_to_name(err));
        free(blob);
        return;
    }

    err = nvs_set_blob(nvs, SCENE_FC03_CACHE_NVS_KEY, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    free(blob);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_SAVE: failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SCENE_FC03_CACHE_SAVE: entries=%u", (unsigned)scene_fc03_cache_valid_count());
}

static void load_scene_fc03_cache(void)
{
    if (s_scene_fc03_cache_loaded) {
        return;
    }
    s_scene_fc03_cache_loaded = true;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SCENE_FC03_CACHE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "SCENE_FC03_CACHE_LOAD: no persisted cache");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_LOAD: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    scene_fc03_cache_nvs_blob_t *blob = calloc(1, sizeof(*blob));
    if (!blob) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_LOAD: no heap for blob");
        return;
    }

    size_t blob_size = sizeof(*blob);
    err = nvs_get_blob(nvs, SCENE_FC03_CACHE_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "SCENE_FC03_CACHE_LOAD: no persisted cache");
        free(blob);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_LOAD: failed: %s", esp_err_to_name(err));
        free(blob);
        return;
    }
    if (blob_size != sizeof(*blob) ||
        blob->magic != SCENE_FC03_CACHE_MAGIC ||
        blob->version != SCENE_FC03_CACHE_VERSION ||
        blob->entry_count != SCENE_FC03_CACHE_ENTRIES) {
        ESP_LOGW(TAG, "SCENE_FC03_CACHE_LOAD: ignored incompatible blob");
        free(blob);
        return;
    }

    memset(s_scene_fc03_cache, 0, sizeof(s_scene_fc03_cache));
    s_scene_fc03_cache_age = blob->cache_age;
    for (size_t i = 0; i < SCENE_FC03_CACHE_ENTRIES; i++) {
        const scene_fc03_cache_nvs_entry_t *src = &blob->entries[i];
        scene_fc03_cache_entry_t *dst = &s_scene_fc03_cache[i];
        if (!src->valid ||
            src->fc03_len < 2 ||
            src->fc03_len > SCENE_FC03_MAX_PAYLOAD_LEN) {
            continue;
        }
        dst->valid = true;
        dst->endpoint = src->endpoint;
        dst->group_id = src->group_id;
        dst->scene_id = src->scene_id;
        dst->fc03_len = src->fc03_len;
        dst->age = src->age;
        memcpy(dst->fc03, src->fc03, src->fc03_len);
        if (dst->age > s_scene_fc03_cache_age) {
            s_scene_fc03_cache_age = dst->age;
        }
    }

    ESP_LOGI(TAG, "SCENE_FC03_CACHE_LOAD: entries=%u", (unsigned)scene_fc03_cache_valid_count());
    free(blob);
}

static bool scene_payload_key(const uint8_t *payload,
                              uint16_t payload_len,
                              uint16_t *group_id,
                              uint8_t *scene_id)
{
    if (!payload || payload_len < SCENES_KEY_PAYLOAD_LEN) {
        return false;
    }
    if (group_id) {
        *group_id = u16_le(payload);
    }
    if (scene_id) {
        *scene_id = payload[2];
    }
    return true;
}

static bool scene_fc03_payload_has_known_flags(const uint8_t *payload, uint16_t payload_len)
{
    if (!payload || payload_len < 2) {
        return false;
    }

    uint16_t flags = u16_le(payload);
    return (flags & ~FC03_KNOWN_FLAGS) == 0 && flags != 0;
}

static void remember_pending_scene_recall(uint8_t endpoint, uint16_t group_id, uint8_t scene_id)
{
    s_pending_scene_recall.valid = true;
    s_pending_scene_recall.endpoint = endpoint;
    s_pending_scene_recall.group_id = group_id;
    s_pending_scene_recall.scene_id = scene_id;
}

static bool take_pending_scene_recall(uint8_t endpoint, uint16_t group_id, uint8_t scene_id)
{
    if (!s_pending_scene_recall.valid ||
        s_pending_scene_recall.endpoint != endpoint ||
        s_pending_scene_recall.group_id != group_id ||
        s_pending_scene_recall.scene_id != scene_id) {
        return false;
    }

    s_pending_scene_recall.valid = false;
    return true;
}

static bool scene_compact_payload_to_keyed_fc03(const uint8_t *payload,
                                                uint16_t payload_len,
                                                bool include_effect_speed,
                                                uint8_t *out,
                                                uint16_t out_size,
                                                uint16_t *out_len)
{
    if (!payload || !out || !out_len ||
        payload_len < SCENES_KEY_PAYLOAD_LEN + 2 ||
        out_size < SCENES_KEY_PAYLOAD_LEN + 6) {
        return false;
    }

    const uint8_t *body = payload + SCENES_KEY_PAYLOAD_LEN;
    uint16_t body_len = payload_len - SCENES_KEY_PAYLOAD_LEN;
    if (body[0] != 0x0c) {
        return false;
    }

    uint8_t color_payload_len = body[1];
    uint16_t color_block_len = 1 + color_payload_len;
    if (body_len < 1 + color_block_len) {
        return false;
    }

    uint8_t n_colors = body[2] >> 4;
    uint8_t expected_color_payload_len = 4 + 3 * n_colors;
    if (n_colors == 0 ||
        n_colors > FC03_MAX_COLORS ||
        color_payload_len != expected_color_payload_len) {
        ESP_LOGW(TAG,
                 "SCENES_MFG_COMPACT_SKIP: color_len=%u colors=%u expected=%u",
                 (unsigned)color_payload_len,
                 (unsigned)n_colors,
                 (unsigned)expected_color_payload_len);
        return false;
    }

    uint16_t trailing_len = body_len - (1 + color_block_len);
    bool has_effect_speed = include_effect_speed && trailing_len >= 1;
    if (trailing_len > 1) {
        ESP_LOGW(TAG,
                 "SCENES_MFG_COMPACT has %u trailing byte(s); using first as effect speed",
                 (unsigned)trailing_len);
    }

    uint16_t fc03_len = 2 + 2 + color_block_len + (has_effect_speed ? 1 : 0) + 2;
    uint16_t keyed_len = SCENES_KEY_PAYLOAD_LEN + fc03_len;
    if (keyed_len > out_size) {
        return false;
    }

    memcpy(out, payload, SCENES_KEY_PAYLOAD_LEN);
    uint8_t *fc03 = out + SCENES_KEY_PAYLOAD_LEN;
    uint16_t flags = FC03_FLAG_FADE_SPEED |
                     FC03_FLAG_GRADIENT_COLORS |
                     FC03_FLAG_GRADIENT_PARAMS;
    if (has_effect_speed) {
        flags |= FC03_FLAG_EFFECT_SPEED;
    }
    fc03[0] = (uint8_t)(flags & 0xff);
    fc03[1] = (uint8_t)(flags >> 8);
    fc03[2] = 0x04; /* Match the fade used by app scene updates. */
    fc03[3] = 0x00;
    memcpy(&fc03[4], &body[1], color_block_len);
    uint16_t off = 4 + color_block_len;
    if (has_effect_speed) {
        fc03[off++] = body[1 + color_block_len];
    }
    fc03[off++] = 0x28; /* Real scene FC03 stores use scale 5. */
    fc03[off++] = 0x00;

    *out_len = keyed_len;
    return true;
}

static bool fc03_copy_without_effect_speed(const uint8_t *fc03,
                                           uint16_t fc03_len,
                                           uint8_t *out,
                                           uint16_t out_size,
                                           uint16_t *out_len)
{
    if (!fc03 || !out || !out_len ||
        fc03_len < 2 ||
        fc03_len > out_size ||
        !scene_fc03_payload_has_known_flags(fc03, fc03_len)) {
        return false;
    }

    uint16_t flags = u16_le(fc03);
    if (!(flags & FC03_FLAG_EFFECT_SPEED)) {
        memcpy(out, fc03, fc03_len);
        *out_len = fc03_len;
        return true;
    }

    size_t off = 2;
    if (flags & FC03_FLAG_ON_OFF) {
        if (!fc03_require_bytes(off, 1, fc03_len, "cached on/off")) {
            return false;
        }
        off += 1;
    }
    if (flags & FC03_FLAG_BRIGHTNESS) {
        if (!fc03_require_bytes(off, 1, fc03_len, "cached brightness")) {
            return false;
        }
        off += 1;
    }
    if (flags & FC03_FLAG_COLOR_MIREK) {
        if (!fc03_require_bytes(off, 2, fc03_len, "cached mirek")) {
            return false;
        }
        off += 2;
    }
    if (flags & FC03_FLAG_COLOR_XY) {
        if (!fc03_require_bytes(off, 4, fc03_len, "cached xy")) {
            return false;
        }
        off += 4;
    }
    if (flags & FC03_FLAG_FADE_SPEED) {
        if (!fc03_require_bytes(off, 2, fc03_len, "cached fade")) {
            return false;
        }
        off += 2;
    }
    if (flags & FC03_FLAG_EFFECT_TYPE) {
        if (!fc03_require_bytes(off, 1, fc03_len, "cached effect type")) {
            return false;
        }
        off += 1;
    }
    if (flags & FC03_FLAG_GRADIENT_COLORS) {
        if (!fc03_require_bytes(off, 1, fc03_len, "cached gradient length")) {
            return false;
        }
        uint8_t color_payload_len = fc03[off];
        if (!fc03_require_bytes(off, 1 + color_payload_len, fc03_len, "cached gradient colors")) {
            return false;
        }
        off += 1 + color_payload_len;
    }

    if (!fc03_require_bytes(off, 1, fc03_len, "cached effect speed")) {
        return false;
    }
    size_t speed_off = off;
    off += 1;
    if (flags & FC03_FLAG_GRADIENT_PARAMS) {
        if (!fc03_require_bytes(off, 2, fc03_len, "cached gradient params")) {
            return false;
        }
        off += 2;
    }

    uint16_t static_len = fc03_len - 1;
    if (static_len > out_size) {
        return false;
    }
    memcpy(out, fc03, speed_off);
    memcpy(out + speed_off, fc03 + speed_off + 1, fc03_len - speed_off - 1);

    uint16_t static_flags = flags & ~FC03_FLAG_EFFECT_SPEED;
    out[0] = (uint8_t)(static_flags & 0xff);
    out[1] = (uint8_t)(static_flags >> 8);
    *out_len = static_len;
    return true;
}

static bool cache_scene_fc03_payload(uint8_t endpoint, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t group_id = 0;
    uint8_t scene_id = 0;
    if (!scene_payload_key(payload, payload_len, &group_id, &scene_id)) {
        return false;
    }

    uint16_t fc03_len = payload_len - SCENES_KEY_PAYLOAD_LEN;
    const uint8_t *fc03 = payload + SCENES_KEY_PAYLOAD_LEN;
    if (fc03_len < 2 || fc03_len > SCENE_FC03_MAX_PAYLOAD_LEN) {
        ESP_LOGW(TAG,
                 "SCENE_FC03_CACHE_SKIP: group=0x%04x scene=0x%02x fc03_len=%u",
                 (unsigned)group_id,
                 (unsigned)scene_id,
                 (unsigned)fc03_len);
        return false;
    }
    if (!scene_fc03_payload_has_known_flags(fc03, fc03_len)) {
        ESP_LOGW(TAG,
                 "SCENE_FC03_CACHE_SKIP: group=0x%04x scene=0x%02x invalid_fc03_flags=0x%04x",
                 (unsigned)group_id,
                 (unsigned)scene_id,
                 (unsigned)u16_le(fc03));
        return false;
    }

    scene_fc03_cache_entry_t *slot = NULL;
    scene_fc03_cache_entry_t *oldest = &s_scene_fc03_cache[0];
    for (size_t i = 0; i < SCENE_FC03_CACHE_ENTRIES; i++) {
        scene_fc03_cache_entry_t *entry = &s_scene_fc03_cache[i];
        if (entry->valid &&
            entry->endpoint == endpoint &&
            entry->group_id == group_id &&
            entry->scene_id == scene_id) {
            slot = entry;
            break;
        }
        if (!entry->valid && !slot) {
            slot = entry;
        }
        if (entry->age < oldest->age) {
            oldest = entry;
        }
    }
    if (!slot) {
        slot = oldest;
    }

    bool changed = !slot->valid ||
        slot->endpoint != endpoint ||
        slot->group_id != group_id ||
        slot->scene_id != scene_id ||
        slot->fc03_len != fc03_len ||
        memcmp(slot->fc03, fc03, fc03_len) != 0;

    slot->valid = true;
    slot->endpoint = endpoint;
    slot->group_id = group_id;
    slot->scene_id = scene_id;
    slot->fc03_len = (uint8_t)fc03_len;
    memcpy(slot->fc03, fc03, fc03_len);
    slot->age = ++s_scene_fc03_cache_age;

    ESP_LOGI(TAG,
             "SCENE_FC03_CACHE_STORE: endpoint=%u group=0x%04x scene=0x%02x fc03_len=%u changed=%u",
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)scene_id,
             (unsigned)fc03_len,
             changed ? 1U : 0U);
    return changed;
}

static bool remove_scene_fc03_cache_entry(uint8_t endpoint, uint16_t group_id, uint8_t scene_id)
{
    bool changed = false;
    for (size_t i = 0; i < SCENE_FC03_CACHE_ENTRIES; i++) {
        scene_fc03_cache_entry_t *entry = &s_scene_fc03_cache[i];
        if (!entry->valid ||
            entry->endpoint != endpoint ||
            entry->group_id != group_id ||
            entry->scene_id != scene_id) {
            continue;
        }

        memset(entry, 0, sizeof(*entry));
        changed = true;
    }

    if (take_pending_scene_recall(endpoint, group_id, scene_id)) {
        changed = true;
    }

    ESP_LOGI(TAG,
             "SCENE_FC03_CACHE_REMOVE: endpoint=%u group=0x%04x scene=0x%02x changed=%u",
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)scene_id,
             changed ? 1U : 0U);
    return changed;
}

static bool apply_cached_scene_fc03(uint8_t endpoint, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t group_id = 0;
    uint8_t scene_id = 0;
    if (!scene_payload_key(payload, payload_len, &group_id, &scene_id)) {
        return false;
    }

    for (size_t i = 0; i < SCENE_FC03_CACHE_ENTRIES; i++) {
        scene_fc03_cache_entry_t *entry = &s_scene_fc03_cache[i];
        if (!entry->valid ||
            entry->endpoint != endpoint ||
            entry->group_id != group_id ||
            entry->scene_id != scene_id) {
            continue;
        }

        ESP_LOGI(TAG,
                 "SCENE_FC03_CACHE_APPLY: endpoint=%u group=0x%04x scene=0x%02x fc03_len=%u",
                 (unsigned)endpoint,
                 (unsigned)group_id,
                 (unsigned)scene_id,
                 (unsigned)entry->fc03_len);
        entry->age = ++s_scene_fc03_cache_age;
        uint8_t static_fc03[SCENE_FC03_MAX_PAYLOAD_LEN];
        uint16_t static_fc03_len = 0;
        if (fc03_copy_without_effect_speed(entry->fc03,
                                           entry->fc03_len,
                                           static_fc03,
                                           sizeof(static_fc03),
                                           &static_fc03_len)) {
            if (static_fc03_len != entry->fc03_len) {
                ESP_LOGI(TAG,
                         "SCENE_FC03_CACHE_STATIC_RECALL: endpoint=%u group=0x%04x scene=0x%02x fc03_len=%u static_len=%u",
                         (unsigned)endpoint,
                         (unsigned)group_id,
                         (unsigned)scene_id,
                         (unsigned)entry->fc03_len,
                         (unsigned)static_fc03_len);
            }
            handle_fc03_multicolor_command(endpoint, static_fc03, static_fc03_len);
        } else {
            handle_fc03_multicolor_command(endpoint, entry->fc03, entry->fc03_len);
        }
        return true;
    }

    ESP_LOGI(TAG,
             "SCENE_FC03_CACHE_MISS: endpoint=%u group=0x%04x scene=0x%02x",
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)scene_id);
    remember_pending_scene_recall(endpoint, group_id, scene_id);
    return false;
}

static bool handle_scenes_remove_scene_raw(const zb_zcl_parsed_hdr_t *hdr,
                                           const uint8_t *payload,
                                           uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT ||
        payload_len != SCENES_KEY_PAYLOAD_LEN) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t group_id = 0;
    uint8_t scene_id = 0;
    if (!scene_payload_key(payload, payload_len, &group_id, &scene_id)) {
        return false;
    }

    if (remove_scene_fc03_cache_entry(endpoint, group_id, scene_id)) {
        save_scene_fc03_cache();
    }

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "SCENES_REMOVE_SCENE_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                           ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                           ZB_ZCL_NOT_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER(cmd_ptr,
                                    hdr->seq_number,
                                    SCENES_REMOVE_SCENE_CMD_ID);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, group_id);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, scene_id);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG,
             "SCENES_REMOVE_SCENE_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u group=0x%04x scene=0x%02x",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)scene_id);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    return true;
}

static bool handle_scenes_mfg_store_raw(const zb_zcl_parsed_hdr_t *hdr,
                                        const uint8_t *payload,
                                        uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t group_id = 0;
    uint8_t scene_id = 0;
    if (!scene_payload_key(payload, payload_len, &group_id, &scene_id)) {
        return false;
    }

    if (cache_scene_fc03_payload(endpoint, payload, payload_len)) {
        save_scene_fc03_cache();
    }
    bool apply_pending = take_pending_scene_recall(endpoint, group_id, scene_id);

    zb_bufid_t out = zb_buf_get_out();
    if (!out) {
        ESP_LOGE(TAG, "SCENES_MFG_STORE_RESP_RAW: no ZBOSS output buffer");
        return true;
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                           ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                           ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        SIGNIFY_MANUFACTURER_CODE,
                                        SCENES_MFG_STORE_CMD_ID);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, group_id);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, scene_id);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG,
             "SCENES_MFG_STORE_RESP_RAW: tsn=0x%02x to=0x%04x ep=%u group=0x%04x scene=0x%02x",
             (unsigned)hdr->seq_number,
             (unsigned)dst_addr,
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)scene_id);
    ZB_ZCL_FINISH_N_SEND_PACKET(out,
                                cmd_ptr,
                                dst_addr,
                                ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                hdr->addr_data.common_data.src_endpoint,
                                endpoint,
                                hdr->profile_id,
                                hdr->cluster_id,
                                NULL);
    if (apply_pending) {
        ESP_LOGI(TAG,
                 "SCENE_FC03_CACHE_RECOVER: endpoint=%u group=0x%04x scene=0x%02x source=mfg_store",
                 (unsigned)endpoint,
                 (unsigned)group_id,
                 (unsigned)scene_id);
        handle_fc03_multicolor_command(endpoint,
                                      payload + SCENES_KEY_PAYLOAD_LEN,
                                      payload_len - SCENES_KEY_PAYLOAD_LEN);
    }
    return true;
}

static bool handle_scenes_mfg_compact_scene_raw(const zb_zcl_parsed_hdr_t *hdr,
                                                const uint8_t *payload,
                                                uint16_t payload_len)
{
    if (!hdr || !payload ||
        hdr->addr_data.common_data.source.addr_type != ZB_ZCL_ADDR_TYPE_SHORT) {
        return false;
    }

    uint8_t endpoint = hdr->addr_data.common_data.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return false;
    }

    uint16_t group_id = 0;
    uint8_t scene_id = 0;
    if (!scene_payload_key(payload, payload_len, &group_id, &scene_id)) {
        return false;
    }

    uint8_t live_keyed_fc03[SCENES_KEY_PAYLOAD_LEN + SCENE_FC03_MAX_PAYLOAD_LEN];
    uint16_t live_keyed_fc03_len = 0;
    if (!scene_compact_payload_to_keyed_fc03(payload,
                                             payload_len,
                                             true,
                                             live_keyed_fc03,
                                             sizeof(live_keyed_fc03),
                                             &live_keyed_fc03_len)) {
        return false;
    }

    uint8_t static_keyed_fc03[SCENES_KEY_PAYLOAD_LEN + SCENE_FC03_MAX_PAYLOAD_LEN];
    uint16_t static_keyed_fc03_len = 0;
    if (!scene_compact_payload_to_keyed_fc03(payload,
                                             payload_len,
                                             false,
                                             static_keyed_fc03,
                                             sizeof(static_keyed_fc03),
                                             &static_keyed_fc03_len)) {
        return false;
    }

    bool changed = cache_scene_fc03_payload(endpoint, static_keyed_fc03, static_keyed_fc03_len);
    if (changed) {
        save_scene_fc03_cache();
    }
    bool matched_pending = take_pending_scene_recall(endpoint, group_id, scene_id);

    ESP_LOGI(TAG,
             "SCENES_MFG_COMPACT_SCENE: endpoint=%u group=0x%04x scene=0x%02x live_len=%u cache_len=%u changed=%u pending=%u",
             (unsigned)endpoint,
             (unsigned)group_id,
             (unsigned)scene_id,
             (unsigned)(live_keyed_fc03_len - SCENES_KEY_PAYLOAD_LEN),
             (unsigned)(static_keyed_fc03_len - SCENES_KEY_PAYLOAD_LEN),
             changed ? 1U : 0U,
             matched_pending ? 1U : 0U);

    handle_fc03_multicolor_command(endpoint,
                                  live_keyed_fc03 + SCENES_KEY_PAYLOAD_LEN,
                                  live_keyed_fc03_len - SCENES_KEY_PAYLOAD_LEN);
    return true;
}

static esp_err_t deferred_driver_init(void)
{
#if ARGB_BACKEND == ARGB_BACKEND_LOCAL_LED
    return local_led_backend_init();
#else
    return ESP_OK;
#endif
}

#if ZDO_DESCRIPTOR_OVERRIDE
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static esp_err_t send_zdo_response(uint16_t dst_short_addr,
                                   uint16_t cluster_id,
                                   const uint8_t *payload,
                                   uint8_t payload_len)
{
    esp_zb_apsde_data_req_t req = {
        .dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .dst_endpoint = 0,
        .profile_id = ZB_AF_ZDO_PROFILE_ID,
        .cluster_id = cluster_id,
        .src_endpoint = 0,
        .asdu_length = payload_len,
        .asdu = (uint8_t *)payload,
        .tx_options = ESP_ZB_APSDE_TX_OPT_ACK_TX,
        .radius = 0,
    };
    req.dst_addr.addr_short = dst_short_addr;

    esp_err_t err = esp_zb_aps_data_request(&req);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ZDO override: response cluster=0x%04x to=0x%04x failed: %s",
                 (unsigned)cluster_id, (unsigned)dst_short_addr, esp_err_to_name(err));
    }
    return err;
}

static bool send_node_desc_response(const esp_zb_apsde_data_ind_t *ind, uint8_t tsn, uint16_t nwk_addr)
{
    uint8_t payload[] = {
        tsn,
        ZDO_SUCCESS_STATUS,
        0x00, 0x00, /* nwk_addr */
        0x01, 0x40, /* node_desc_flags = 0x4001 */
        0x8e,       /* mac_capability_flags */
        0x0b, 0x10, /* manufacturer_code = 0x100b */
        0x52,       /* max_buf_size = 82 */
        0x80, 0x00, /* max_incoming_transfer_size = 128 */
        0x00, 0x2c, /* server_mask = 0x2c00 */
        0x80, 0x00, /* max_outgoing_transfer_size = 128 */
        0x00,       /* desc_capability_field */
    };
    put_le16(&payload[2], nwk_addr);

    ESP_LOGI(TAG, "ZDO override: Node_Desc_rsp nwk=0x%04x flags=0x4001 mac=0x8e server=0x2c00 to=0x%04x",
             (unsigned)nwk_addr, (unsigned)ind->src_short_addr);
    return send_zdo_response(ind->src_short_addr, ZDO_NODE_DESC_RSP_CLUSTER_ID,
                             payload, sizeof(payload)) == ESP_OK;
}

static bool send_active_ep_response(const esp_zb_apsde_data_ind_t *ind, uint8_t tsn, uint16_t nwk_addr)
{
    uint8_t payload[5 + ARGB_ENDPOINT_COUNT + 1] = {
        tsn,
        ZDO_SUCCESS_STATUS,
        0x00, 0x00, /* nwk_addr */
        ARGB_ENDPOINT_COUNT + 1,
    };
    put_le16(&payload[2], nwk_addr);
    for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
        payload[5 + i] = HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i;
    }
    payload[5 + ARGB_ENDPOINT_COUNT] = GP_ENDPOINT;

    ESP_LOGI(TAG, "ZDO override: Active_EP_rsp nwk=0x%04x light_eps=%u..%u gp_ep=%u to=0x%04x",
             (unsigned)nwk_addr,
             (unsigned)HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
             (unsigned)(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT - 1),
             (unsigned)GP_ENDPOINT,
             (unsigned)ind->src_short_addr);
    return send_zdo_response(ind->src_short_addr, ZDO_ACTIVE_EP_RSP_CLUSTER_ID,
                             payload, sizeof(payload)) == ESP_OK;
}

static bool send_simple_desc_response(const esp_zb_apsde_data_ind_t *ind,
                                      uint8_t tsn,
                                      uint16_t nwk_addr,
                                      uint8_t endpoint)
{
    static const uint8_t light_simple_desc_template[] = {
        0x00,       /* endpoint is patched per logical light */
        0x04, 0x01, /* HA profile */
        0x0d, 0x01, /* extended color light */
        0x01,
        0x0b,
        0x00, 0x00, /* Basic */
        0x03, 0x00, /* Identify */
        0x04, 0x00, /* Groups */
        0x05, 0x00, /* Scenes */
        0x06, 0x00, /* On/Off */
        0x08, 0x00, /* Level Control */
        0x00, 0x10, /* Touchlink */
        0x03, 0xfc, /* FC03 gradient */
        0x00, 0x03, /* Color Control */
        0x01, 0xfc, /* FC01 certification/proprietary */
        0x04, 0xfc, /* FC04 auxiliary */
        0x01,
        0x19, 0x00, /* OTA Upgrade client */
    };
    static const uint8_t ep242_simple_desc[] = {
        GP_ENDPOINT,
        0xe0, 0xa1, /* Green Power profile */
        0x61, 0x00, /* Green Power Proxy Basic */
        0x00,
        0x00,
        0x01,
        0x21, 0x00, /* Green Power client */
    };

    const uint8_t *desc = NULL;
    uint8_t light_desc[sizeof(light_simple_desc_template)];
    uint8_t desc_len = 0;
    if (endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE &&
        endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        memcpy(light_desc, light_simple_desc_template, sizeof(light_desc));
        light_desc[0] = endpoint;
        desc = light_desc;
        desc_len = sizeof(light_desc);
    } else if (endpoint == GP_ENDPOINT) {
        desc = ep242_simple_desc;
        desc_len = sizeof(ep242_simple_desc);
    } else {
        return false;
    }

    uint8_t payload[4 + 1 + sizeof(light_simple_desc_template)] = {
        tsn,
        ZDO_SUCCESS_STATUS,
        0x00, 0x00, /* nwk_addr */
        desc_len,
    };
    put_le16(&payload[2], nwk_addr);
    memcpy(&payload[5], desc, desc_len);

    ESP_LOGI(TAG, "ZDO override: Simple_Desc_rsp nwk=0x%04x ep=%u len=%u to=0x%04x",
             (unsigned)nwk_addr, (unsigned)endpoint, (unsigned)desc_len,
             (unsigned)ind->src_short_addr);
    return send_zdo_response(ind->src_short_addr, ZDO_SIMPLE_DESC_RSP_CLUSTER_ID,
                             payload, (uint8_t)(5 + desc_len)) == ESP_OK;
}

static bool zdo_descriptor_override_cb(esp_zb_apsde_data_ind_t ind)
{
    if (ind.profile_id != ZB_AF_ZDO_PROFILE_ID ||
        ind.dst_endpoint != 0 ||
        ind.src_endpoint != 0 ||
        ind.asdu == NULL ||
        ind.asdu_length < 3) {
        return false;
    }

    uint8_t tsn = ind.asdu[0];
    uint16_t nwk_addr = get_le16(&ind.asdu[1]);
    uint16_t local_short = esp_zb_get_short_address();
    if (nwk_addr != local_short) {
        return false;
    }

    if (ind.cluster_id == ZDO_NODE_DESC_REQ_CLUSTER_ID) {
        ESP_LOGI(TAG, "ZDO override: Node_Desc_req nwk=0x%04x from=0x%04x",
                 (unsigned)nwk_addr, (unsigned)ind.src_short_addr);
        return send_node_desc_response(&ind, tsn, nwk_addr);
    }

    if (ind.cluster_id == ZDO_ACTIVE_EP_REQ_CLUSTER_ID) {
        ESP_LOGI(TAG, "ZDO override: Active_EP_req nwk=0x%04x from=0x%04x",
                 (unsigned)nwk_addr, (unsigned)ind.src_short_addr);
        return send_active_ep_response(&ind, tsn, nwk_addr);
    }

    if (ind.cluster_id == ZDO_SIMPLE_DESC_REQ_CLUSTER_ID && ind.asdu_length >= 4) {
        uint8_t endpoint = ind.asdu[3];
        ESP_LOGI(TAG, "ZDO override: Simple_Desc_req nwk=0x%04x ep=%u from=0x%04x",
                 (unsigned)nwk_addr, (unsigned)endpoint, (unsigned)ind.src_short_addr);
        return send_simple_desc_response(&ind, tsn, nwk_addr, endpoint);
    }

    return false;
}
#endif

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
                        , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            esp_err_t init_err = deferred_driver_init();
            ESP_LOGI(TAG, "Deferred driver initialization %s", init_err ? "failed" : "successful");
            if (init_err == ESP_OK) {
                for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
                    apply_power_recovery_startup(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i);
                }
            }
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            /* Always attempt steering/rejoin on startup. When the bridge is
             * in "add light" mode the network is open; otherwise the stack
             * retries until it succeeds. */
            ESP_LOGI(TAG, "Start network steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            if (*(uint8_t *)esp_zb_app_signal_get_params(p_sg_p)) {
                ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds",
                         esp_zb_get_pan_id(), *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p));
            } else {
                ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", esp_zb_get_pan_id());
            }
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        // Rejoin automatically after a network reset / kick.
        // https://github.com/espressif/esp-zigbee-sdk/issues/66#issuecomment-1667314481
        {
            esp_zb_zdo_signal_leave_params_t *leave_params =
                (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (leave_params) {
                if (leave_params->leave_type == ESP_ZB_NWK_LEAVE_TYPE_RESET) {
                    ESP_LOGI(TAG, "ZDO leave: with reset, status: %s", esp_err_to_name(err_status));
                    esp_zb_nvram_erase_at_start(true);
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    ESP_LOGI(TAG, "ZDO leave: leave_type: %d, status: %s",
                             leave_params->leave_type, esp_err_to_name(err_status));
                }
            } else {
                ESP_LOGI(TAG, "ZDO leave: (no params), status: %s", esp_err_to_name(err_status));
            }
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *annce =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (annce) {
            ESP_LOGI(TAG, "ZDO device_annce: short=0x%04hx, ieee=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, cap=0x%02x",
                     annce->device_short_addr,
                     annce->ieee_addr[7], annce->ieee_addr[6], annce->ieee_addr[5], annce->ieee_addr[4],
                     annce->ieee_addr[3], annce->ieee_addr[2], annce->ieee_addr[1], annce->ieee_addr[0],
                     annce->capability);
        } else {
            ESP_LOGI(TAG, "ZDO device_annce (no params)");
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

static void refresh_state_from_cluster(uint8_t endpoint,
                                       const esp_zb_zcl_set_attr_value_message_t *skip_message)
{
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    if (idx >= ARGB_ENDPOINT_COUNT) {
        return;
    }

    light_state_t *st = &g_light_state[idx];

    /* If the caller just gave us a fresh value for an attribute, don't let the
     * cluster read (which can be stale or, in some cases, inverted) overwrite it. */
    bool skip_onoff = skip_message &&
                      skip_message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
                      skip_message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
    bool skip_level = skip_message &&
                      skip_message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
                      skip_message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID;
    bool skip_x = skip_message &&
                  skip_message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
                  skip_message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID;
    bool skip_y = skip_message &&
                  skip_message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
                  skip_message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID;

    if (!skip_onoff) {
        const esp_zb_zcl_attr_t *on_off_attr = esp_zb_zcl_get_attribute(
            endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
        if (on_off_attr && on_off_attr->data_p) {
            st->on = *(bool *)on_off_attr->data_p;
        }
    }

    if (!skip_level) {
        const esp_zb_zcl_attr_t *level_attr = esp_zb_zcl_get_attribute(
            endpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
        if (level_attr && level_attr->data_p) {
            st->bri = *(uint8_t *)level_attr->data_p;
            if (st->bri) {
                st->last_bri = st->bri;
            }
        }
    }

    if (!skip_x) {
        const esp_zb_zcl_attr_t *x_attr = esp_zb_zcl_get_attribute(
            endpoint, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID);
        if (x_attr && x_attr->data_p) {
            st->x = *(uint16_t *)x_attr->data_p;
        }
    }

    if (!skip_y) {
        const esp_zb_zcl_attr_t *y_attr = esp_zb_zcl_get_attribute(
            endpoint, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID);
        if (y_attr && y_attr->data_p) {
            st->y = *(uint16_t *)y_attr->data_p;
        }
    }
}

static void update_state_from_message(light_state_t *st, const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message->attribute.data.value == NULL || message->attribute.data.size == 0) {
        return;
    }

    switch (message->info.cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
        if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            st->on = *(bool *)message->attribute.data.value;
        }
        break;
    case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
        if (message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
            st->bri = *(uint8_t *)message->attribute.data.value;
            if (st->bri) {
                st->last_bri = st->bri;
            }
        }
        break;
    case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
        if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID) {
            st->x = *(uint16_t *)message->attribute.data.value;
        } else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID) {
            st->y = *(uint16_t *)message->attribute.data.value;
        }
        break;
    default:
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG,
                        "Received message: error status(%d)", message->info.status);

    uint8_t endpoint = message->info.dst_endpoint;
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)",
             endpoint, message->info.cluster, message->attribute.id, message->attribute.data.size);

    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    light_state_t *st = &g_light_state[idx];

    /* Prefer the value carried in the callback message; fall back to reading
     * the cluster for attributes that were not part of this notification. */
    update_state_from_message(st, message);
    refresh_state_from_cluster(endpoint, message);

    switch (message->info.cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
    case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
    case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
        emit_state_json_internal(endpoint, NULL, true, STANDARD_ZCL_FADE_TENTHS, false, 0);
        save_power_recovery_state("zcl_attr");
        break;
    default:
        break;
    }

    return ret;
}

static esp_err_t zb_custom_cluster_handler(const esp_zb_zcl_custom_cluster_command_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty custom cluster message");

    ESP_LOGI(TAG, "Custom cluster command: endpoint(%d), cluster(0x%x), cmd(0x%x), manuf(0x%x), size(%d)",
             (int)message->info.dst_endpoint,
             (unsigned)message->info.cluster,
             (unsigned)message->info.command.id,
             (unsigned)message->info.header.manuf_code,
             (unsigned)message->data.size);
    if (message->info.cluster == MFG_CLUSTER_CERT_ID) {
        ESP_LOGI(TAG, "FC01_CMD: cmd=0x%02x tsn=0x%02x fc=0x%02x disable_default_resp=%u manuf=0x%04x size=%u",
                 (unsigned)message->info.command.id,
                 (unsigned)message->info.header.tsn,
                 (unsigned)message->info.header.fc,
                 (unsigned)((message->info.header.fc >> 4) & 0x01),
                 (unsigned)message->info.header.manuf_code,
                 (unsigned)message->data.size);
        if (message->data.value && message->data.size) {
            print_hex_bytes("FC01_PAYLOAD_HEX: ", (const uint8_t *)message->data.value,
                            message->data.size);
        }
    }

    if (message->info.cluster == MFG_CLUSTER_GRADIENT_ID &&
        message->info.command.id == FC03_MULTICOLOR_CMD_ID &&
        message->info.header.manuf_code == SIGNIFY_MANUFACTURER_CODE) {
        handle_fc03_multicolor_command(message->info.dst_endpoint,
                                      (const uint8_t *)message->data.value,
                                      message->data.size);
    }

    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
        message->info.command.id == 0xC0 &&
        message->info.header.manuf_code == SIGNIFY_MANUFACTURER_CODE) {
        esp_zb_zcl_custom_cluster_cmd_resp_t resp = {
            .zcl_basic_cmd = {
                .dst_addr_u.addr_short = message->info.src_address.u.short_addr,
                .dst_endpoint = message->info.src_endpoint,
                .src_endpoint = message->info.dst_endpoint,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .profile_id = message->info.profile,
            .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_BASIC,
            .manuf_specific = 1,
            .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
            .dis_default_resp = 1,
            .manuf_code = SIGNIFY_MANUFACTURER_CODE,
            .custom_cmd_id = 0xC1,
            .data = {
                .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
                .size = sizeof(BASIC_CMD_C1_RESPONSE),
                .value = (void *)BASIC_CMD_C1_RESPONSE,
            },
        };
        ESP_LOGI(TAG, "BASIC_CMD_C0_RESP_C1: tsn=0x%02x size=%u",
                 (unsigned)message->info.header.tsn,
                 (unsigned)sizeof(BASIC_CMD_C1_RESPONSE));
        esp_zb_zcl_custom_cluster_cmd_resp(&resp);
    }

    /* Returning ESP_OK lets the stack send a default response. The exact
     * FC01 response behavior still needs a fresh real LCX004 passive capture. */
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: {
        const esp_zb_zcl_cmd_default_resp_message_t *resp =
            (const esp_zb_zcl_cmd_default_resp_message_t *)message;
        if (resp) {
            ESP_LOGI(TAG, "ZCL_DEFAULT_RESP_RX: cluster=0x%04x resp_to_cmd=0x%02x status=0x%02x src_ep=%u dst_ep=%u tsn=0x%02x manuf=0x%04x",
                     (unsigned)resp->info.cluster,
                     (unsigned)resp->resp_to_cmd,
                     (unsigned)resp->status_code,
                     (unsigned)resp->info.src_endpoint,
                     (unsigned)resp->info.dst_endpoint,
                     (unsigned)resp->info.header.tsn,
                     (unsigned)resp->info.header.manuf_code);
        }
        break;
    }
    case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID:
    case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_RESP_CB_ID:
        ret = zb_custom_cluster_handler((esp_zb_zcl_custom_cluster_command_message_t *)message);
        break;
    default:
#if ARGB_SERIAL_DEBUG
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
#endif
        break;
    }
    return ret;
}

static const char *zcl_global_cmd_name(uint8_t cmd_id)
{
    switch (cmd_id) {
    case ZB_ZCL_CMD_READ_ATTRIB:
        return "read_attr";
    case ZB_ZCL_CMD_READ_ATTRIB_RESP:
        return "read_attr_resp";
    case ZB_ZCL_CMD_WRITE_ATTRIB:
        return "write_attr";
    case ZB_ZCL_CMD_WRITE_ATTRIB_RESP:
        return "write_attr_resp";
    case ZB_ZCL_CMD_DISC_ATTRIB:
        return "discover_attr";
    case ZB_ZCL_CMD_DISC_ATTRIB_RESP:
        return "discover_attr_resp";
    case ZB_ZCL_CMD_DISCOVER_COMMANDS_RECEIVED:
        return "discover_commands_received";
    case ZB_ZCL_CMD_DISCOVER_COMMANDS_GENERATED:
        return "discover_commands_generated";
    case ZB_ZCL_CMD_DISCOVER_ATTR_EXT:
        return "discover_attr_ext";
    default:
        return "other";
    }
}

static const char *zcl_cmd_name(const zb_zcl_parsed_hdr_t *hdr)
{
    if (!hdr) {
        return "other";
    }
    if (!hdr->is_common_command &&
        hdr->cluster_id == MFG_CLUSTER_GRADIENT_ID &&
        hdr->cmd_id == FC03_MULTICOLOR_CMD_ID) {
        return "multi_color";
    }
    if (!hdr->is_common_command &&
        hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
        !hdr->is_manuf_specific &&
        hdr->cmd_id == SCENES_REMOVE_SCENE_CMD_ID) {
        return "remove_scene";
    }
    if (!hdr->is_common_command &&
        hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
        !hdr->is_manuf_specific &&
        hdr->cmd_id == SCENES_RECALL_SCENE_CMD_ID) {
        return "recall_scene";
    }
    if (!hdr->is_common_command &&
        hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
        hdr->is_manuf_specific) {
        return "mfg_scene_cmd";
    }
    return zcl_global_cmd_name(hdr->cmd_id);
}

static void print_hex_bytes(const char *prefix, const uint8_t *payload, uint16_t len)
{
#if ARGB_SERIAL_DEBUG
    uint16_t shown = len < 48 ? len : 48;
    printf("%s", prefix);
    for (uint16_t i = 0; i < shown; i++) {
        printf("%02x", payload[i]);
    }
    if (shown < len) {
        printf("...");
    }
    printf("\n");
#else
    (void)prefix;
    (void)payload;
    (void)len;
#endif
}

static void log_zcl_payload_summary(const zb_zcl_parsed_hdr_t *hdr, const uint8_t *payload, uint16_t len)
{
#if !ARGB_SERIAL_DEBUG
    (void)hdr;
    (void)payload;
    (void)len;
    return;
#endif

    if (!hdr || !payload || len == 0) {
        return;
    }

    if (!hdr->is_common_command) {
        if (hdr->cluster_id == MFG_CLUSTER_GRADIENT_ID &&
            hdr->cmd_id == FC03_MULTICOLOR_CMD_ID &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE) {
            print_hex_bytes("FC03_MULTICOLOR_HEX: ", payload, len);
        } else if (hdr->is_manuf_specific &&
                   hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES) {
            print_hex_bytes("SCENES_MFG_CMD_HEX: ", payload, len);
        } else if (hdr->is_manuf_specific) {
            print_hex_bytes("ZCL_MFG_CMD_HEX: ", payload, len);
        } else if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
                   hdr->cmd_id == SCENES_REMOVE_SCENE_CMD_ID) {
            print_hex_bytes("SCENES_REMOVE_HEX: ", payload, len);
        } else if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
                   hdr->cmd_id == SCENES_RECALL_SCENE_CMD_ID) {
            print_hex_bytes("SCENES_RECALL_HEX: ", payload, len);
        }
        return;
    }

    if (hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB) {
        printf("ZCL_READ_ATTRS: cluster=0x%04x attrs=[", (unsigned)hdr->cluster_id);
        for (uint16_t off = 0; off + 1 < len; off += 2) {
            uint16_t attr = payload[off] | ((uint16_t)payload[off + 1] << 8);
            if (off) {
                printf(",");
            }
            printf("0x%04x", (unsigned)attr);
        }
        printf("]\n");
    } else if (hdr->cmd_id == ZB_ZCL_CMD_DISC_ATTRIB ||
               hdr->cmd_id == ZB_ZCL_CMD_DISCOVER_ATTR_EXT) {
        if (len >= 3) {
            uint16_t start_attr = payload[0] | ((uint16_t)payload[1] << 8);
            printf("ZCL_DISC_ATTRS: cluster=0x%04x cmd=%s start=0x%04x max=%u\n",
                   (unsigned)hdr->cluster_id,
                   zcl_global_cmd_name(hdr->cmd_id),
                   (unsigned)start_attr,
                   (unsigned)payload[2]);
        }
    } else if (hdr->cmd_id == ZB_ZCL_CMD_DISCOVER_COMMANDS_RECEIVED ||
               hdr->cmd_id == ZB_ZCL_CMD_DISCOVER_COMMANDS_GENERATED) {
        if (len >= 2) {
            printf("ZCL_DISC_COMMANDS: cluster=0x%04x cmd=%s start=0x%02x max=%u\n",
                   (unsigned)hdr->cluster_id,
                   zcl_global_cmd_name(hdr->cmd_id),
                   (unsigned)payload[0],
                   (unsigned)payload[1]);
        }
    }

    print_hex_bytes("ZCL_PAYLOAD_HEX: ", payload, len);
}

/* Low-level ZBoss device-callback logger.  This runs before the SDK's own
 * handler and lets us see every ZCL command the bridge sends during
 * discovery: read attributes, discover attributes, cluster commands, etc.
 * Returning false means "not handled, let the stack process it normally". */
static bool zb_device_cb_id_handler(uint8_t bufid)
{
    zb_zcl_device_callback_param_t *param = ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t);
    if (!param) {
        return false;
    }

    const zb_zcl_parsed_hdr_t *hdr = param->cb_param.gnr.in_cmd_info;
    const uint8_t *payload = (const uint8_t *)zb_buf_begin(bufid);
    uint16_t payload_len = (uint16_t)zb_buf_len(bufid);
    if (hdr) {
        ESP_LOGI(TAG, "ZCL_RX cb=%d status=%d ep=%d->%d cluster=0x%04x profile=0x%04x cmd=0x%02x/%s dir=%s manuf=0x%04x payload_len=%u",
                 (int)param->device_cb_id,
                 (int)param->status,
                 hdr->addr_data.common_data.src_endpoint,
                 hdr->addr_data.common_data.dst_endpoint,
                 hdr->cluster_id,
                 hdr->profile_id,
                 hdr->cmd_id,
                 zcl_cmd_name(hdr),
                 hdr->cmd_direction ? "to_cli" : "to_srv",
                 hdr->is_manuf_specific ? hdr->manuf_specific : 0,
                 (unsigned)payload_len);
        log_zcl_payload_summary(hdr, payload, payload_len);
    } else {
        ESP_LOGI(TAG, "ZB device_cb_id=%d status=%d endpoint=%u attr_type=0x%02x payload_len=%u (no parsed header)",
                 (int)param->device_cb_id,
                 (int)param->status,
                 (unsigned)param->endpoint,
                 (unsigned)param->attr_type,
                 (unsigned)payload_len);
    }

    return false;
}

static bool zb_raw_command_handler(uint8_t bufid)
{
    const zb_zcl_parsed_hdr_t *hdr = ZB_BUF_GET_PARAM(bufid, zb_zcl_parsed_hdr_t);
    const uint8_t *payload = (const uint8_t *)zb_buf_begin(bufid);
    uint16_t payload_len = (uint16_t)zb_buf_len(bufid);

    if (hdr) {
        ESP_LOGI(TAG, "ZCL_RAW ep=%d->%d cluster=0x%04x profile=0x%04x cmd=0x%02x/%s dir=%s manuf=0x%04x common=%u payload_len=%u",
                 hdr->addr_data.common_data.src_endpoint,
                 hdr->addr_data.common_data.dst_endpoint,
                 hdr->cluster_id,
                 hdr->profile_id,
                 hdr->cmd_id,
                 zcl_cmd_name(hdr),
                 hdr->cmd_direction ? "to_cli" : "to_srv",
                 hdr->is_manuf_specific ? hdr->manuf_specific : 0,
                 hdr->is_common_command ? 1 : 0,
                 (unsigned)payload_len);
        log_zcl_payload_summary(hdr, payload, payload_len);
#if FC01_EXPLICIT_DEFAULT_RESPONSE
        if (hdr->cluster_id == MFG_CLUSTER_CERT_ID &&
            hdr->cmd_id == 0x03 &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV) {
            ESP_LOGI(TAG, "FC01_EXPLICIT_DEFAULT_RESP: cmd=0x03 tsn=0x%02x status=0x00",
                     (unsigned)zb_zcl_get_tsn_from_packet(bufid));
            (void)zb_zcl_send_default_handler(bufid, hdr, ZB_ZCL_STATUS_SUCCESS);
            return true;
        }
#endif
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_basic_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE &&
            hdr->cmd_id == OTA_IMAGE_NOTIFY_CMD_ID &&
            !hdr->is_common_command &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI) {
            send_ota_query_next_image_raw(hdr);
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_identify_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_level_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_color_mfg_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_scenes_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_GROUPS &&
            hdr->cmd_id == 0x00 &&
            !hdr->is_common_command &&
            !hdr->is_manuf_specific &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_groups_add_group_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
            hdr->cmd_id == 0x06 &&
            !hdr->is_common_command &&
            !hdr->is_manuf_specific &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_scenes_get_membership_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
            hdr->cmd_id == SCENES_REMOVE_SCENE_CMD_ID &&
            !hdr->is_common_command &&
            !hdr->is_manuf_specific &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_scenes_remove_scene_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
            hdr->cmd_id == SCENES_MFG_COMPACT_SCENE_CMD_ID &&
            !hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_scenes_mfg_compact_scene_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
            hdr->cmd_id == SCENES_MFG_STORE_CMD_ID &&
            !hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_scenes_mfg_store_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_SCENES &&
            hdr->cmd_id == SCENES_RECALL_SCENE_CMD_ID &&
            !hdr->is_common_command &&
            !hdr->is_manuf_specific &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV) {
            (void)apply_cached_scene_fc03(hdr->addr_data.common_data.dst_endpoint, payload, payload_len);
        }
        if (hdr->cluster_id == MFG_CLUSTER_GRADIENT_ID &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_fc03_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == MFG_CLUSTER_GRADIENT_ID &&
            hdr->cmd_id == ZB_ZCL_CMD_DISCOVER_ATTR_EXT &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_fc03_discover_attr_ext_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB &&
            hdr->is_common_command &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_power_recovery_write_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
            hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB &&
            hdr->is_common_command &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_basic_write_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
            hdr->cmd_id == 0xC0 &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV) {
            send_basic_c1_response_raw(hdr, payload, payload_len);
            zb_buf_free(bufid);
            return true;
        }
    } else {
        ESP_LOGI(TAG, "ZCL_RAW no parsed header payload_len=%u", (unsigned)payload_len);
        if (payload && payload_len) {
            print_hex_bytes("ZCL_RAW_HEX: ", payload, payload_len);
        }
    }

    return false;
}

static void add_proprietary_clusters(esp_zb_cluster_list_t *cluster_list, uint8_t idx)
{
    /* FC03: the gradient cluster. */
    esp_zb_attribute_list_t *fc03_attr_list =
        esp_zb_zcl_attr_list_create(MFG_CLUSTER_GRADIENT_ID);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0002,
                                          ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          s_fc03_state[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0001,
                                          ESP_ZB_ZCL_ATTR_TYPE_32BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr1[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0010,
                                          ESP_ZB_ZCL_ATTR_TYPE_16BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr10[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0011,
                                          ESP_ZB_ZCL_ATTR_TYPE_64BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr11[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0012,
                                          ESP_ZB_ZCL_ATTR_TYPE_32BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr12[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0013,
                                          ESP_ZB_ZCL_ATTR_TYPE_16BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr13[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0032,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                          &s_fc03_attr32[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0033,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                          &s_fc03_attr33[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0034,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                          &s_fc03_attr34[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc03_attr_list, 0x0038,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr38[idx]);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, fc03_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* FC01: Signify certification/proprietary cluster.
     * A real gradient lightstrip returns:
     *   0x0000 - 8-bit bitmap = 0x0B
     *   0x0001 - 8-bit enum  = 0x00
     */
    esp_zb_attribute_list_t *fc01_attr_list =
        esp_zb_zcl_attr_list_create(MFG_CLUSTER_CERT_ID);
    esp_zb_custom_cluster_add_custom_attr(fc01_attr_list, 0x0000,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr0[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc01_attr_list, 0x0001,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr1[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc01_attr_list, 0x0002,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr2[idx]);
    esp_zb_custom_cluster_add_custom_attr(fc01_attr_list, 0x0003,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr3[idx]);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, fc01_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* FC04: present on real devices, purpose unknown. */
    esp_zb_attribute_list_t *fc04_attr_list =
        esp_zb_zcl_attr_list_create(MFG_CLUSTER_AUX_ID);
    esp_zb_custom_cluster_add_custom_attr(fc04_attr_list, 0x0000,
                                          ESP_ZB_ZCL_ATTR_TYPE_16BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc04_attr0[idx]);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, fc04_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Real gradient endpoints also expose the Touchlink/ZLL commissioning
     * cluster as a server cluster in their simple descriptor. */
    esp_zb_attribute_list_t *touchlink_attr_list =
        esp_zb_zcl_attr_list_create(TOUCHLINK_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, touchlink_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void log_fake_simple_descriptor(uint8_t endpoint)
{
#if ARGB_SERIAL_DEBUG
    printf("FAKE_DESCRIPTOR: endpoint=%u profile=0x%04x device=0x%04x version=%u "
           "server_clusters=[0x0000,0x0003,0x0004,0x0005,0x0006,0x0008,0x1000,0xfc03,0x0300,0xfc01,0xfc04] "
           "client_clusters=[0x0019]\n",
           (unsigned)endpoint,
           (unsigned)ESP_ZB_AF_HA_PROFILE_ID,
           0x010D,
           1);
#else
    (void)endpoint;
#endif
}

#if GP_ENDPOINT_MODE == 1
static void add_green_power_endpoint(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *gp_attr_list = esp_zb_zcl_attr_list_create(GP_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, gp_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = GP_ENDPOINT,
        .app_profile_id = GP_PROFILE_ID,
        .app_device_id = GP_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_gateway_ep(ep_list, cluster_list, endpoint_config));

    if (ARGB_SERIAL_DEBUG) {
        printf("FAKE_DESCRIPTOR: endpoint=%u profile=0x%04x device=0x%04x version=%u "
               "server_clusters=[] client_clusters=[0x%04x]\n",
               GP_ENDPOINT,
               GP_PROFILE_ID,
               GP_DEVICE_ID,
               0,
               GP_CLUSTER_ID);
    }
}
#elif GP_ENDPOINT_MODE == 2
static zb_uint8_t s_gp_shared_security_key_type;
static zb_uint8_t s_gp_shared_security_key[16];
static zb_uint8_t s_gp_link_key[16];
static zb_uint8_t s_gp_max_proxy_table_entries;
static zb_uint32_t s_gp_functionality = 0x0009ac2f;
static zb_uint32_t s_gp_active_functionality = 0x0009ac2f;
static zb_uint8_t s_gp_proxy_table[1] = { 0 };

ZB_ZCL_DECLARE_GPPB_ATTRIB_LIST_CLI(s_gp_attr_list_cli,
                                    &s_gp_shared_security_key_type,
                                    s_gp_shared_security_key,
                                    s_gp_link_key,
                                    &s_gp_max_proxy_table_entries,
                                    &s_gp_functionality,
                                    &s_gp_active_functionality,
                                    s_gp_proxy_table);

ZB_ZCL_START_DECLARE_CLUSTER_LIST(s_gp_cluster_list)
    ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_GREEN_POWER,
                        ZB_ZCL_ARRAY_SIZE(s_gp_attr_list_cli, zb_zcl_attr_t),
                        s_gp_attr_list_cli,
                        ZB_ZCL_CLUSTER_CLIENT_ROLE,
                        ZB_ZCL_MANUF_CODE_INVALID)
ZB_ZCL_FINISH_DECLARE_CLUSTER_LIST;

ZB_ZCL_DECLARE_GPPB_EP(s_gp_endpoint_desc, GP_ENDPOINT, s_gp_cluster_list);

static zb_af_endpoint_desc_t *s_device_ep_list_with_gp[2];
static zb_af_device_ctx_t s_device_ctx_with_gp = {
    .ep_count = 2,
    .ep_desc_list = s_device_ep_list_with_gp,
};

static void register_native_green_power_endpoint(void)
{
    zb_af_endpoint_desc_t *light_ep = zb_af_get_endpoint_desc(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
    if (!light_ep) {
        ESP_LOGE(TAG, "Cannot register native GP endpoint: endpoint %u is not registered",
                 HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
        return;
    }

    s_device_ep_list_with_gp[0] = light_ep;
    s_device_ep_list_with_gp[1] = &s_gp_endpoint_desc;
    ZB_AF_REGISTER_DEVICE_CTX(&s_device_ctx_with_gp);

    if (ARGB_SERIAL_DEBUG) {
        printf("FAKE_DESCRIPTOR: endpoint=%u profile=0x%04x device=0x%04x version=%u "
               "server_clusters=[] client_clusters=[0x%04x] source=zboss_gppb\n",
               GP_ENDPOINT,
               GP_PROFILE_ID,
               GP_DEVICE_ID,
               0,
               GP_CLUSTER_ID);
    }
}
#endif

static void add_color_dimmable_light_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();

    /* Defaults captured from a real LCX004 at bridge discovery time. */
    light_cfg.level_cfg.current_level = 0xFE;
    light_cfg.color_cfg.current_x = 0x9F8F;
    light_cfg.color_cfg.current_y = 0x5C06;
    light_cfg.color_cfg.color_mode = 0x01;
    light_cfg.color_cfg.enhanced_color_mode = 0x00;
    light_cfg.color_cfg.color_capabilities = 0x001F;

    /* Extended color light (0x010D) is what gradient-capable lights use. */
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = 0x010D,
        .app_device_version = 1,
    };

    esp_zb_ep_list_add_ep(ep_list, esp_zb_color_dimmable_light_clusters_create(&light_cfg),
                          endpoint_config);
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = (char *)BASIC_MANUFACTURER_NAME,
        .model_identifier = (char *)BASIC_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, endpoint, &info);

    /* Add extra Basic cluster attributes so the bridge shows a more complete device
     * record (date code, software build). */
    esp_zb_cluster_list_t *basic_cluster_list = esp_zb_ep_list_get_ep(ep_list, endpoint);
    esp_zb_attribute_list_t *basic_attr_list = esp_zb_cluster_list_get_cluster(
        basic_cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    /* Spoof the firmware identifiers of a real LCX004 so the bridge looks
     * up the product record and enables the gradient UI. */
    char date_code[] = "\x08" "20251117";
    char product_code[] = "\x00";
    char sw_build_id[] = "\x07" "1.129.5";
    uint8_t stack_version = 1;
    uint8_t hw_version = 1;
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_PRODUCT_CODE_ID, product_code);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw_build_id);
    esp_zb_basic_cluster_add_attr(basic_attr_list, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0020,
                                          ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          (void *)BASIC_POWER_ON_CONFIG);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0021,
                                          ESP_ZB_ZCL_ATTR_TYPE_U32,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_basic_attr21[idx]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0040,
                                          ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          (void *)BASIC_PRODUCT_LABEL);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0041,
                                          ESP_ZB_ZCL_ATTR_TYPE_U32,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_basic_attr41[idx]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0050,
                                          ESP_ZB_ZCL_ATTR_TYPE_32BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_basic_attr50[idx]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0051,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_basic_attr51[idx]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0053,
                                          ESP_ZB_ZCL_ATTR_TYPE_U32,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_basic_attr53[idx]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0054,
                                          ESP_ZB_ZCL_ATTR_TYPE_128_BIT_KEY,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          s_basic_attr54[idx]);

    // Fix "Off with effect" command from the bridge.
    // https://github.com/espressif/esp-zigbee-sdk/issues/457#issuecomment-2426128314
    uint16_t on_off_on_time = 0;
    uint16_t on_off_off_wait_time = 0;
    bool on_off_global_scene_control = false;
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, endpoint);
    esp_zb_attribute_list_t *onoff_attr_list = esp_zb_cluster_list_get_cluster(
        cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME,
                                   &on_off_on_time);
    esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_OFF_WAIT_TIME,
                                   &on_off_off_wait_time);
    esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL,
                                   &on_off_global_scene_control);
    esp_zb_custom_cluster_add_custom_attr(onoff_attr_list, 0x4003,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_startup_on_off[idx]);

    // Restore previous brightness when the bridge sends "On" without a level command.
    uint8_t level_on_level = 0xFF; /* 0xFF == use previous level */
    uint16_t level_on_transition_time = 0;
    uint16_t level_off_transition_time = 0;
    uint16_t level_signify_attr3 = 0x000A;
    esp_zb_attribute_list_t *level_attr_list = esp_zb_cluster_list_get_cluster(
        cluster_list, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_level_cluster_add_attr(level_attr_list, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_LEVEL_ID,
                                  &level_on_level);
    esp_zb_level_cluster_add_attr(level_attr_list,
                                  ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_TRANSITION_TIME_ID,
                                  &level_on_transition_time);
    esp_zb_level_cluster_add_attr(level_attr_list,
                                  ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_OFF_TRANSITION_TIME_ID,
                                  &level_off_transition_time);
    esp_zb_custom_cluster_add_custom_attr(level_attr_list, 0x0003,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &level_signify_attr3);
    esp_zb_custom_cluster_add_custom_attr(level_attr_list, 0x4000,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_startup_current_level[idx]);

    /* Add the extended Color Control attributes that the default color dimmable
     * light config does not include. ColorCapabilities was already set via
     * light_cfg.color_cfg.color_capabilities above. */
    static uint16_t enhanced_current_hue = 0x0D10;
    static uint8_t color_loop_active = 0;
    static uint8_t current_saturation = 0xFE;
    static uint16_t color_temperature = 500;
    static uint16_t color_temp_min = 153;
    static uint16_t color_temp_max = 500;
    /* LCX004 / gamut C color points, encoded as Zigbee normalized U16 XY. */
    static uint16_t color_point_rx = 0xB105; /* real LCX004 read attr 0x0032 */
    static uint16_t color_point_ry = 0x4EEC; /* real LCX004 read attr 0x0033 */
    static uint16_t color_point_gx = 0x2B85; /* real LCX004 read attr 0x0036 */
    static uint16_t color_point_gy = 0xB333; /* real LCX004 read attr 0x0037 */
    static uint16_t color_point_bx = 0x2738; /* real LCX004 read attr 0x003a */
    static uint16_t color_point_by = 0x0C2C; /* real LCX004 read attr 0x003b */

    esp_zb_attribute_list_t *color_attr_list = esp_zb_cluster_list_get_cluster(
        cluster_list, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &enhanced_current_hue);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &current_saturation);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_temperature);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_ACTIVE_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_loop_active);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_temp_min);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_temp_max);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          0x4010,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_startup_color_temperature[idx]);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          COLOR_POINT_RX_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_rx);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          COLOR_POINT_RY_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_ry);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          COLOR_POINT_GX_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_gx);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          COLOR_POINT_GY_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_gy);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          COLOR_POINT_BX_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_bx);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          COLOR_POINT_BY_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_by);

    /* Add the standard OTA Upgrade client cluster.  The real LCX004 response
     * captured with gradient_probe is:
     *   file_version    = 0x01002000
     *   manufacturer ID = 0x100b
     *   image type      = 0xffff
     *   image stamp     = unsupported
     *
     * We build it as a plain attribute list rather than using the SDK's OTA
     * cluster helper, because the helper expects a full OTA client state
     * machine and will assert if the bridge sends OTA commands before we
     * have configured callbacks.  We only need the read-only attributes here. */
    {
        static uint8_t ota_server_id[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        static uint32_t ota_file_offset = 0xFFFFFFFF;
        static uint32_t ota_file_version = 0x01002000;
        static uint16_t ota_stack_version = 0x0002;   /* Zigbee Pro */
        static uint32_t ota_downloaded_file_version = 0xFFFFFFFF;
        static uint16_t ota_downloaded_stack_version = 0xFFFF;
        static uint8_t ota_image_status = 0x00;
        static uint16_t ota_manufacturer = SIGNIFY_MANUFACTURER_CODE;
        static uint16_t ota_image_type = 0xFFFF;
        static uint16_t ota_min_block_period = 0;

        esp_zb_attribute_list_t *ota_attr_list =
            esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_IEEE_ADDR,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, ota_server_id);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_OFFSET_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U32,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_file_offset);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_VERSION_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U32,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_file_version);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_STACK_VERSION_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_stack_version);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U32,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_downloaded_file_version);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_DOWNLOADED_STACK_VERSION_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_downloaded_stack_version);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_STATUS_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U8,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_image_status);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MANUFACTURE_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_manufacturer);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_TYPE_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_image_type);
        esp_zb_custom_cluster_add_custom_attr(ota_attr_list, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MIN_BLOCK_REQUE_ID,
                                              ESP_ZB_ZCL_ATTR_TYPE_U16,
                                              ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &ota_min_block_period);
        esp_zb_cluster_list_add_custom_cluster(cluster_list, ota_attr_list,
                                               ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    }

    /* Add Signify proprietary clusters (FC01/FC03/FC04) so the bridge
     * recognises this as a certified gradient light. */
    add_proprietary_clusters(cluster_list, idx);
    log_fake_simple_descriptor(endpoint);
}

static void set_real_lcx004_discovery_attrs(uint8_t endpoint)
{
    static uint8_t enhanced_color_mode = 0x00;
    static uint16_t bridge_color_capabilities = 0x0099;
    static uint16_t bridge_startup_color_temperature = 500;

    struct {
        uint16_t attr_id;
        void *value;
    } attrs[] = {
        { 0x4002, &enhanced_color_mode },
        { 0x400B, &bridge_color_capabilities },
        { 0x400C, &bridge_startup_color_temperature },
    };

    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
            endpoint,
            ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            attrs[i].attr_id,
            attrs[i].value,
            false);
        if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "LCX004 discovery attr 0x%04x set failed: 0x%02x",
                     (unsigned)attrs[i].attr_id, (unsigned)status);
        }
    }
}

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t count = 0;
    while (*hex && count < out_len) {
        while (*hex && isspace((unsigned char)*hex)) {
            hex++;
        }
        if (!hex[0] || !hex[1]) {
            break;
        }
        char pair[3] = { hex[0], hex[1], '\0' };
        out[count++] = (uint8_t)strtoul(pair, NULL, 16);
        hex += 2;
    }
    return count;
}

#if ARGB_BACKEND == ARGB_BACKEND_LOCAL_LED
static bool parse_rgb_hex_arg(const char *arg, rgb_t *color)
{
    if (!arg || !color) {
        return false;
    }

    while (*arg && isspace((unsigned char)*arg)) {
        arg++;
    }
    if (*arg == '#') {
        arg++;
    }

    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)arg[i])) {
            return false;
        }
    }

    char pair[3] = {0};
    pair[0] = arg[0];
    pair[1] = arg[1];
    color->r = (uint8_t)strtoul(pair, NULL, 16);
    pair[0] = arg[2];
    pair[1] = arg[3];
    color->g = (uint8_t)strtoul(pair, NULL, 16);
    pair[0] = arg[4];
    pair[1] = arg[5];
    color->b = (uint8_t)strtoul(pair, NULL, 16);
    return true;
}

static void print_led_result(const char *action, esp_err_t err)
{
    if (err == ESP_OK) {
        printf("LED: %s ok\n", action);
    } else {
        printf("LED: %s failed: %s\n", action, esp_err_to_name(err));
    }
}

static void run_led_gradient_test(void)
{
    local_led_stop_dynamic(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);

    rgb_t pixels[ARGB_LED_COUNT];
    size_t led_count = 0;
    if (!local_led_slice_for_idx(0, NULL, &led_count)) {
        print_led_result("gradient", ESP_ERR_INVALID_STATE);
        return;
    }
    rgb_t red = {255, 0, 0};
    rgb_t green = {0, 255, 0};
    rgb_t blue = {0, 0, 255};

    for (size_t i = 0; i < led_count; i++) {
        if (led_count <= 1) {
            pixels[i] = red;
            continue;
        }
        float t = (float)i / ((float)led_count - 1.0f);
        pixels[i] = t < 0.5f
            ? interpolate_rgb(red, green, t * 2.0f)
            : interpolate_rgb(green, blue, (t - 0.5f) * 2.0f);
    }

    print_led_result("gradient",
                     local_led_start_pixel_transition(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                                      pixels,
                                                      led_count,
                                                      false,
                                                      0));
}

static void run_led_chase_test(void)
{
    local_led_stop_dynamic(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);

    rgb_t pixels[ARGB_LED_COUNT];
    size_t led_count = 0;
    if (!local_led_slice_for_idx(0, NULL, &led_count)) {
        print_led_result("chase", ESP_ERR_INVALID_STATE);
        return;
    }
    const rgb_t colors[] = {
        {255, 0, 0},
        {0, 255, 0},
        {0, 0, 255},
        {255, 255, 255},
    };

    for (size_t color_i = 0; color_i < sizeof(colors) / sizeof(colors[0]); color_i++) {
        for (size_t pos = 0; pos < led_count; pos++) {
            memset(pixels, 0, sizeof(pixels));
            pixels[pos] = colors[color_i];
            esp_err_t err = local_led_start_pixel_transition(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                                             pixels,
                                                             led_count,
                                                             false,
                                                             0);
            if (err != ESP_OK) {
                print_led_result("chase", err);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(45));
        }
    }
    print_led_result("chase", ESP_OK);
}

static bool handle_led_cli_command(const char *line)
{
    if (strcmp(line, "led") != 0 && strncmp(line, "led ", 4) != 0) {
        return false;
    }

    const char *cmd = line + 3;
    while (*cmd && isspace((unsigned char)*cmd)) {
        cmd++;
    }

    if (strcmp(cmd, "off") == 0) {
        rgb_t off = {0, 0, 0};
        local_led_stop_dynamic(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
        print_led_result("off",
                         local_led_start_solid_transition(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                                          off,
                                                          false,
                                                          0));
    } else if (strncmp(cmd, "solid ", 6) == 0) {
        rgb_t color;
        if (!parse_rgb_hex_arg(cmd + 6, &color)) {
            printf("USAGE: led solid <rrggbb>\n");
            return true;
        }
        local_led_stop_dynamic(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
        print_led_result("solid",
                         local_led_start_solid_transition(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                                          color,
                                                          false,
                                                          0));
    } else if (strcmp(cmd, "gradient") == 0) {
        run_led_gradient_test();
    } else if (strcmp(cmd, "chase") == 0) {
        run_led_chase_test();
    } else {
        printf("USAGE: led off | led solid <rrggbb> | led gradient | led chase\n");
    }
    return true;
}
#else
static bool handle_led_cli_command(const char *line)
{
    if (strcmp(line, "led") != 0 && strncmp(line, "led ", 4) != 0) {
        return false;
    }
    printf("LED commands require ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED\n");
    return true;
}
#endif

/* Minimal serial CLI for self-testing the gradient parser without re-pairing
     * the device through the app every time.  Typing:
 *   g 51010104001350000000c2ad57c2ad57c2ad57c2ad57c2ad572800
 * will emit the same DATA line the daemon will see when the bridge/ZHA sends
 * an FC03 multiColor command. */
static void serial_cmd_task(void *pvParameters)
{
    (void)pvParameters;
    char line[256];

    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Drop trailing newline. */
        line[strcspn(line, "\r\n")] = '\0';

        const char *gradient_hex = NULL;
        if (strncmp(line, "gradient ", 9) == 0) {
            gradient_hex = line + 9;
        } else if (strncmp(line, "g ", 2) == 0) {
            gradient_hex = line + 2;
        }

        if (gradient_hex) {
            uint8_t payload[64];
            size_t len = hex_to_bytes(gradient_hex, payload, sizeof(payload));
            if (len) {
                handle_fc03_multicolor_command(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                              payload, len);
            } else {
                printf("USAGE: g <hex payload>\n");
            }
        } else if (strncmp(line, "scene ", 6) == 0 ||
                   strncmp(line, "scene-apply ", 12) == 0) {
            bool apply = strncmp(line, "scene-apply ", 12) == 0;
            const char *scene_hex = apply ? line + 12 : line + 6;
            uint8_t payload[SCENES_KEY_PAYLOAD_LEN + SCENE_FC03_MAX_PAYLOAD_LEN];
            size_t len = hex_to_bytes(scene_hex, payload, sizeof(payload));
            uint16_t group_id = 0;
            uint8_t scene_id = 0;
            if (len < SCENES_KEY_PAYLOAD_LEN + 2 ||
                !scene_payload_key(payload, len, &group_id, &scene_id)) {
                printf("USAGE: scene <SCENES_MFG_CMD_HEX payload>\n");
                printf("USAGE: scene-apply <SCENES_MFG_CMD_HEX payload>\n");
                continue;
            }
            uint8_t cache_keyed_fc03[SCENES_KEY_PAYLOAD_LEN + SCENE_FC03_MAX_PAYLOAD_LEN];
            uint8_t apply_keyed_fc03[SCENES_KEY_PAYLOAD_LEN + SCENE_FC03_MAX_PAYLOAD_LEN];
            const uint8_t *cache_payload = payload;
            const uint8_t *apply_payload = payload;
            uint16_t cache_payload_len = (uint16_t)len;
            uint16_t apply_payload_len = (uint16_t)len;
            uint16_t cache_keyed_fc03_len = 0;
            uint16_t apply_keyed_fc03_len = 0;
            if (!scene_fc03_payload_has_known_flags(payload + SCENES_KEY_PAYLOAD_LEN,
                                                    (uint16_t)(len - SCENES_KEY_PAYLOAD_LEN))) {
                if (!scene_compact_payload_to_keyed_fc03(payload,
                                                         (uint16_t)len,
                                                         false,
                                                         cache_keyed_fc03,
                                                         sizeof(cache_keyed_fc03),
                                                         &cache_keyed_fc03_len) ||
                    !scene_compact_payload_to_keyed_fc03(payload,
                                                         (uint16_t)len,
                                                         true,
                                                         apply_keyed_fc03,
                                                         sizeof(apply_keyed_fc03),
                                                         &apply_keyed_fc03_len)) {
                    printf("SCENE_CACHE: unsupported scene payload\n");
                    continue;
                }
                cache_payload = cache_keyed_fc03;
                cache_payload_len = cache_keyed_fc03_len;
                apply_payload = apply_keyed_fc03;
                apply_payload_len = apply_keyed_fc03_len;
            }
            bool changed = cache_scene_fc03_payload(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                                    cache_payload,
                                                    cache_payload_len);
            if (changed) {
                save_scene_fc03_cache();
            }
            printf("SCENE_CACHE: group=0x%04x scene=0x%02x len=%u changed=%u\n",
                   (unsigned)group_id,
                   (unsigned)scene_id,
                   (unsigned)(cache_payload_len - SCENES_KEY_PAYLOAD_LEN),
                   changed ? 1U : 0U);
            if (apply) {
                handle_fc03_multicolor_command(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                              apply_payload + SCENES_KEY_PAYLOAD_LEN,
                                              (uint16_t)(apply_payload_len - SCENES_KEY_PAYLOAD_LEN));
            }
        } else if (strncmp(line, "group ", 6) == 0) {
            char *end = NULL;
            uint16_t group_id = (uint16_t)strtoul(line + 6, &end, 0);
            uint8_t endpoint = HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
            if (end && *end) {
                endpoint = (uint8_t)strtoul(end, NULL, 0);
            }
            join_group(group_id, endpoint);
        } else if (strcmp(line, "state") == 0) {
            emit_state_json(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
        } else if (strcmp(line, "discover") == 0 || strcmp(line, "reset") == 0) {
            printf("DATA: {\"event\":\"factory_reset\"}\n");
            esp_zb_factory_reset();
        } else if (handle_led_cli_command(line)) {
            continue;
        } else if (strcmp(line, "help") == 0) {
            printf("Commands: g <hex>, gradient <hex>, group <id> [endpoint], state, "
                   "discover/reset, scene <hex>, scene-apply <hex>, led off, "
                   "led solid <rrggbb>, led gradient, led chase, help\n");
        }
    }
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    /* Pretend to be a Signify device at the Zigbee MAC and node-descriptor level. */
    init_spoofed_long_addr();
    ESP_ERROR_CHECK(esp_zb_set_long_address(s_spoofed_long_addr));
    esp_zb_set_node_descriptor_manufacturer_code(SIGNIFY_MANUFACTURER_CODE);
    ESP_LOGI(TAG, "Node descriptor manufacturer code set to 0x%04x", (unsigned)SIGNIFY_MANUFACTURER_CODE);

    // Allow joining distributed-security Zigbee networks.
    esp_zb_enable_joining_to_distributed(true);
    esp_zb_secur_TC_standard_distributed_key_set(ZIGBEE_TRUST_CENTER_KEY);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
        uint8_t endpoint = HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i;
        add_color_dimmable_light_endpoint(ep_list, endpoint);
    }
#if GP_ENDPOINT_MODE == 1
    add_green_power_endpoint(ep_list);
#endif

    esp_zb_device_register(ep_list);
    for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
        set_real_lcx004_discovery_attrs(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i);
    }
#if GP_ENDPOINT_MODE == 2
    register_native_green_power_endpoint();
#endif
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_device_cb_id_handler_register(zb_device_cb_id_handler);
    esp_zb_raw_command_handler_register(zb_raw_command_handler);
#if ZDO_DESCRIPTOR_OVERRIDE
    esp_zb_aps_data_indication_handler_register(zdo_descriptor_override_cb);
    ESP_LOGI(TAG, "ZDO descriptor override enabled: endpoint list will advertise light_eps=%u..%u gp_ep=%u",
             (unsigned)HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
             (unsigned)(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT - 1),
             (unsigned)GP_ENDPOINT);
#endif
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
#if !ARGB_SERIAL_DEBUG
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_WARN);
#endif

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    init_endpoint_defaults();
    load_power_recovery_state();
    load_scene_fc03_cache();
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
    xTaskCreate(serial_cmd_task, "serial_cli", 8192, NULL, 1, NULL);
}
