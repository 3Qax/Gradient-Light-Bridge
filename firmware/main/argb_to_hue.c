/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 *
 * Derived from Espressif's HA_color_dimmable_light example and the fixes
 * documented in https://wejn.org/2025/01/zigbee-hue-llo-world/
 */

#include "argb_to_hue.h"
#include "xy_to_rgb.h"
#include "trust_center_key.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aps/esp_zigbee_aps.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl_utility.h"
#include "esp_zigbee_cluster.h"
#include "zboss_api.h"
#include "zboss_api_buf.h"
#include "zcl/zb_zcl_commands.h"
#include "zgp/zgp_internal.h"


#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile light (Router) source code.
#endif

static const char *TAG = "ARGB_TO_HUE";

/* Hue manufacturer-specific cluster IDs found on real gradient lights. */
#define HUE_MANU_SPECIFIC_PHILIPS2_CLUSTER_ID  0xFC03 /* gradient / multiColor */
#define HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID  0xFC01 /* certification/proprietary */
#define HUE_MANU_SPECIFIC_PHILIPS4_CLUSTER_ID  0xFC04 /* unknown Philips */
#define HUE_TOUCHLINK_CLUSTER_ID               0x1000 /* present on real gradient endpoint */
#define HUE_GP_ENDPOINT                        242
#define HUE_GP_PROFILE_ID                      0xA1E0
#define HUE_GP_DEVICE_ID                       0x0061
#define HUE_GP_CLUSTER_ID                      0x0021
#define HUE_OTA_IMAGE_NOTIFY_CMD_ID            0x00
#define HUE_OTA_QUERY_NEXT_IMAGE_REQ_CMD_ID    0x01
#define HUE_OTA_LCX004_IMAGE_TYPE              0x0118
#define HUE_OTA_FILE_VERSION                   0x01002000

#define HUE_COLOR_POINT_RX_ID                  0x0032
#define HUE_COLOR_POINT_RY_ID                  0x0033
#define HUE_COLOR_POINT_GX_ID                  0x0036
#define HUE_COLOR_POINT_GY_ID                  0x0037
#define HUE_COLOR_POINT_BX_ID                  0x003A
#define HUE_COLOR_POINT_BY_ID                  0x003B

#define HUE_ZDO_NODE_DESC_REQ_CLUSTER_ID       0x0002
#define HUE_ZDO_NODE_DESC_RSP_CLUSTER_ID       0x8002
#define HUE_ZDO_ACTIVE_EP_REQ_CLUSTER_ID       0x0005
#define HUE_ZDO_ACTIVE_EP_RSP_CLUSTER_ID       0x8005
#define HUE_ZDO_SIMPLE_DESC_REQ_CLUSTER_ID     0x0004
#define HUE_ZDO_SIMPLE_DESC_RSP_CLUSTER_ID     0x8004
#define HUE_ZDO_SUCCESS_STATUS                 0x00

/* Experiment: bypass the local endpoint table for ZDO discovery and answer
 * like a real LCX004, including Green Power endpoint 242, without registering
 * endpoint 242 in the ESP-Zigbee endpoint list. */
#define HUE_ZDO_DESCRIPTOR_OVERRIDE            1
#define HUE_FC01_EXPLICIT_DEFAULT_RESPONSE     1

/* Real LCX004 devices also expose Green Power endpoint 242.
 *
 * Modes:
 *   0 - disabled; known discoverable baseline
 *   1 - ESP-Zigbee gateway endpoint; avoided crash, but Hue created no light
 *   2 - native ZBOSS Green Power Proxy Basic endpoint; current experiment
 */
#define HUE_GP_ENDPOINT_MODE                   0

/* Manufacturer code for Signify Netherlands B.V. */
#define HUE_SIGNIFY_MANUFACTURER_CODE 0x100B

/* Spoof a Signify Netherlands B.V. extended address so the Hue bridge treats
 * the device as a genuine Hue product. The lower 5 bytes should be unique per
 * board if you run more than one argb-to-hue device on the same network. */
/* esp_zb_set_long_address() stores the EUI-64 in little-endian order, so the
 * array here must be the reverse of the address the Hue bridge displays in
 * the uniqueid. We want the bridge to see a genuine Signify OUI
 * (00:17:88:01:0b:...), so pass the bytes in reverse. */
static const esp_zb_ieee_addr_t HUE_SPOOFED_LONG_ADDR = {
    0x05, 0xfe, 0xff, 0x0b, 0x01, 0x88, 0x17, 0x00
};

/* Zigbee strings are octet strings: first byte is the length.
 * These values are from an active read of a real LCX004 Basic cluster. */
static const char *HUE_MANUFACTURER_NAME = "\x18" "Signify Netherlands B.V.";
static const char *HUE_MODEL_IDENTIFIER  = "\x06" "LCX004";

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
static uint8_t s_hue_state[ARGB_ENDPOINT_COUNT][64] = {
    {
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
    },
};

/* manuSpecificPhilips3 (FC01) attributes observed on a real LCX004:
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
static uint8_t s_basic_attr54[ARGB_ENDPOINT_COUNT][16] = {
    { 0xc7, 0x54, 0x2a, 0xfa, 0x34, 0x8c, 0xf3, 0x83,
      0x78, 0xa0, 0x2d, 0x5d, 0x1c, 0x74, 0xf1, 0xe6 },
};
static const uint8_t HUE_BASIC_POWER_ON_CONFIG[] = "\x09" "0:PWRON@1";
static const uint8_t HUE_BASIC_PRODUCT_LABEL[] = "\x1A" "Philips-LCX004-1-GALSECLv1";
static uint32_t s_basic_attr1[ARGB_ENDPOINT_COUNT] = { 0x00000000 };
static uint32_t s_basic_attr21[ARGB_ENDPOINT_COUNT] = { 0x001517EC };
static uint32_t s_basic_attr41[ARGB_ENDPOINT_COUNT] = { 0xC4C1C739 };
static uint32_t s_basic_attr50[ARGB_ENDPOINT_COUNT] = { 0x00000001 };

static const uint8_t HUE_BASIC_CMD_C1_RESPONSE[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00,
    0x00, 0x00, 0x35, 0x0A, 0x53, 0x08, 0x01, 0x15,
    0x66, 0x04, 0x03, 0x3E, 0x1A, 0x4A, 0x0A, 0x06,
    0x4C, 0x43, 0x58, 0x30, 0x30, 0x34, 0x12, 0x18,
    0x53, 0x69, 0x67, 0x6E, 0x69, 0x66, 0x79, 0x20,
    0x4E, 0x65, 0x74, 0x68, 0x65, 0x72, 0x6C, 0x61,
    0x6E, 0x64, 0x73, 0x20, 0x42, 0x2E, 0x56, 0x2E,
    0x42, 0x17, 0x48, 0x75, 0x65, 0x20, 0x67, 0x72,
};

static const uint8_t HUE_BASIC_CMD_C1_RESPONSE_0035[] = {
    0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x55, 0x00,
    0x00, 0x00, 0x20, 0x61, 0x64, 0x69, 0x65, 0x6E,
    0x74, 0x20, 0x6C, 0x69, 0x67, 0x68, 0x74, 0x73,
    0x74, 0x72, 0x69, 0x70, 0x48, 0x66, 0x52, 0x0B,
    0x08, 0x0B, 0x12, 0x07, 0x08, 0xB0, 0x09, 0x10,
    0x03, 0x18, 0x01,
};

light_state_t g_light_state[ARGB_ENDPOINT_COUNT] = {0};

static void print_hex_bytes(const char *prefix, const uint8_t *payload, uint16_t len);

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

    const uint8_t *resp_payload = HUE_BASIC_CMD_C1_RESPONSE;
    uint16_t resp_payload_len = sizeof(HUE_BASIC_CMD_C1_RESPONSE);
    static const uint8_t request_0035[] = { 0x00, 0x35, 0x00, 0x00, 0x00, 0x40 };
    if (payload && payload_len == sizeof(request_0035) &&
        memcmp(payload, request_0035, sizeof(request_0035)) == 0) {
        resp_payload = HUE_BASIC_CMD_C1_RESPONSE_0035;
        resp_payload_len = sizeof(HUE_BASIC_CMD_C1_RESPONSE_0035);
    }

    uint8_t *cmd_ptr = ZB_ZCL_START_PACKET(out);
    ZB_ZCL_CONSTRUCT_SPECIFIC_COMMAND_RESP_FRAME_CONTROL_A(cmd_ptr,
                                                           ZB_ZCL_FRAME_DIRECTION_TO_CLI,
                                                           ZB_ZCL_MANUFACTURER_SPECIFIC);
    ZB_ZCL_CONSTRUCT_COMMAND_HEADER_EXT(cmd_ptr,
                                        hdr->seq_number,
                                        ZB_TRUE,
                                        HUE_SIGNIFY_MANUFACTURER_CODE,
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
                                    HUE_OTA_QUERY_NEXT_IMAGE_REQ_CMD_ID);
    ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, 0x00); /* field control: no hardware version */
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, HUE_SIGNIFY_MANUFACTURER_CODE);
    ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, HUE_OTA_LCX004_IMAGE_TYPE);
    ZB_ZCL_PACKET_PUT_DATA32_VAL(cmd_ptr, HUE_OTA_FILE_VERSION);

    uint16_t dst_addr = hdr->addr_data.common_data.source.u.short_addr;
    ESP_LOGI(TAG, "OTA_QUERY_NEXT_IMAGE_REQ_RAW: to=0x%04x ep=%u manuf=0x%04x image=0x%04x file=0x%08lx",
             (unsigned)dst_addr,
             (unsigned)hdr->addr_data.common_data.src_endpoint,
             (unsigned)HUE_SIGNIFY_MANUFACTURER_CODE,
             (unsigned)HUE_OTA_LCX004_IMAGE_TYPE,
             (unsigned long)HUE_OTA_FILE_VERSION);
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
            print_hex_bytes("BASIC_WRITE_ATTR_RAW 0x0054: ", s_basic_attr54[idx],
                            sizeof(s_basic_attr54[idx]));
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
                                        HUE_SIGNIFY_MANUFACTURER_CODE,
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
                                        HUE_SIGNIFY_MANUFACTURER_CODE,
                                        ZB_ZCL_CMD_READ_ATTRIB_RESP);

    for (uint16_t offset = 0; offset + 1 < payload_len; offset += 2) {
        uint16_t attr_id = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1] << 8);

        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, attr_id);
        switch (attr_id) {
        case 0x0020:
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_STATUS_SUCCESS);
            ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING);
            ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, HUE_BASIC_POWER_ON_CONFIG,
                                     sizeof(HUE_BASIC_POWER_ON_CONFIG) - 1);
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
            ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, HUE_BASIC_PRODUCT_LABEL,
                                     sizeof(HUE_BASIC_PRODUCT_LABEL) - 1);
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
                                        HUE_SIGNIFY_MANUFACTURER_CODE,
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
                                        HUE_SIGNIFY_MANUFACTURER_CODE,
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
            ZB_ZCL_PACKET_PUT_DATA_N(cmd_ptr, s_hue_state[idx], s_hue_state[idx][0] + 1);
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

void emit_state_json(uint8_t endpoint)
{
    if (endpoint < HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE ||
        endpoint >= HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + ARGB_ENDPOINT_COUNT) {
        return;
    }

    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    light_state_t *st = &g_light_state[idx];

    float xf = st->x / 65535.0f;
    float yf = st->y / 65535.0f;
    /* When Hue sends "On" before a level command, bri can be 0 for one
     * update. Use the last known non-zero brightness so the daemon doesn't
     * flicker the LEDs off. */
    uint8_t effective_bri = st->on ? (st->bri ? st->bri : st->last_bri) : 0;

    rgb_t rgb = xy_to_rgb(xf, yf, effective_bri);

    // Prefix with DATA: so the PC daemon can ignore regular log lines.
    printf("DATA: {\"endpoint\":%u,\"on\":%s,\"bri\":%u,\"x\":%.4f,\"y\":%.4f,\"r\":%u,\"g\":%u,\"b\":%u}\n",
           (unsigned)endpoint,
           st->on ? "true" : "false",
           (unsigned)st->bri,
           (double)xf,
           (double)yf,
           (unsigned)rgb.r,
           (unsigned)rgb.g,
           (unsigned)rgb.b);
}

#define HUE_FC03_MULTICOLOR_CMD_ID 0x00
#define HUE_FC03_MAX_COLORS        9

typedef struct {
    float x;
    float y;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} gradient_color_t;

/* Decode the 3-byte scaled XY encoding used in FC03 multiColor payloads.
 * Real Hue gradient lights pack two 12-bit values into 3 octets:
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

static void emit_gradient_json(uint8_t endpoint,
                               uint8_t n_colors,
                               const gradient_color_t *colors,
                               uint8_t segments,
                               uint8_t offset)
{
    printf("DATA: {\"endpoint\":%u,\"gradient\":true,\"n\":%u,\"segments\":%u,\"offset\":%u,\"colors\":[",
           (unsigned)endpoint, (unsigned)n_colors, (unsigned)segments, (unsigned)offset);
    for (uint8_t i = 0; i < n_colors; i++) {
        if (i) {
            printf(",");
        }
        printf("{\"x\":%.4f,\"y\":%.4f,\"r\":%u,\"g\":%u,\"b\":%u}",
               (double)colors[i].x, (double)colors[i].y,
               (unsigned)colors[i].r, (unsigned)colors[i].g, (unsigned)colors[i].b);
    }
    printf("]}\n");
}

/* Parse a Hue FC03 "multiColor" (gradient) command payload.
 * The format observed on real lights and reproduced by ZHA/zigbee-herdsman-converters:
 *   0x50, 0x01, transition_time, 0x00,
 *   length, ncolors<<4, style, 0x00, 0x00,
 *   colors (3 bytes each),
 *   segments<<3, offset<<3
 */
static void handle_hue_multicolor_command(uint8_t endpoint, const uint8_t *data, size_t size)
{
    if (size < 9) {
        ESP_LOGW(TAG, "FC03 multiColor payload too short (%d bytes)", (int)size);
        return;
    }

    /* First two bytes are a fixed header on all gradient commands. */
    if (data[0] != 0x50 || data[1] != 0x01) {
        ESP_LOGW(TAG, "FC03 multiColor unexpected header 0x%02x 0x%02x", data[0], data[1]);
        return;
    }

    uint8_t n_colors = data[5] >> 4;
    if (n_colors == 0 || n_colors > HUE_FC03_MAX_COLORS) {
        ESP_LOGW(TAG, "FC03 multiColor unsupported color count %u", (unsigned)n_colors);
        return;
    }

    size_t expected = 11 + 3 * n_colors;
    if (size < expected) {
        ESP_LOGW(TAG, "FC03 multiColor truncated: expected %d bytes, got %d",
                 (int)expected, (int)size);
        return;
    }

    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    light_state_t *st = (idx < ARGB_ENDPOINT_COUNT) ? &g_light_state[idx] : NULL;
    uint8_t effective_bri = 255;
    if (st && st->on) {
        effective_bri = st->bri ? st->bri : st->last_bri;
    }

    gradient_color_t colors[HUE_FC03_MAX_COLORS];
    for (uint8_t i = 0; i < n_colors; i++) {
        const uint8_t *p = &data[9 + 3 * i];
        scaled_gradient_to_xy(p, &colors[i].x, &colors[i].y);
        rgb_t rgb = xy_to_rgb(colors[i].x, colors[i].y, effective_bri);
        colors[i].r = rgb.r;
        colors[i].g = rgb.g;
        colors[i].b = rgb.b;
    }

    uint8_t segments = data[9 + 3 * n_colors] >> 3;
    uint8_t offset = data[10 + 3 * n_colors] >> 3;

    /* Keep the FC03 state attribute in sync so ZHA/bridge reads reflect the
     * currently active gradient. */
    if (idx < ARGB_ENDPOINT_COUNT) {
        uint8_t *state = s_hue_state[idx];
        uint8_t payload_len = 15 + 3 * n_colors;
        state[0] = payload_len;
        state[1] = 0x4B;
        state[2] = 0x01;
        state[3] = (st && st->on) ? 0x01 : 0x00;
        state[4] = effective_bri;
        state[5] = state[6] = state[7] = state[8] = 0x00;
        state[9] = 1 + 3 * (n_colors + 1);
        state[10] = n_colors << 4;
        state[11] = data[6];                 /* style */
        state[12] = state[13] = 0x00;
        memcpy(&state[14], &data[9], 3 * n_colors);
        state[14 + 3 * n_colors] = segments;
        state[15 + 3 * n_colors] = offset;
    }

    ESP_LOGI(TAG, "FC03 multiColor: %u colors, style %u, segments %u, offset %u",
             (unsigned)n_colors, (unsigned)data[6], (unsigned)segments, (unsigned)offset);

    emit_gradient_json(endpoint, n_colors, colors, segments, offset);
}

static esp_err_t deferred_driver_init(void)
{
    return ESP_OK;
}

#if HUE_ZDO_DESCRIPTOR_OVERRIDE
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
        HUE_ZDO_SUCCESS_STATUS,
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
    return send_zdo_response(ind->src_short_addr, HUE_ZDO_NODE_DESC_RSP_CLUSTER_ID,
                             payload, sizeof(payload)) == ESP_OK;
}

static bool send_active_ep_response(const esp_zb_apsde_data_ind_t *ind, uint8_t tsn, uint16_t nwk_addr)
{
    uint8_t payload[] = {
        tsn,
        HUE_ZDO_SUCCESS_STATUS,
        0x00, 0x00, /* nwk_addr */
        0x02,
        HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
        HUE_GP_ENDPOINT,
    };
    put_le16(&payload[2], nwk_addr);

    ESP_LOGI(TAG, "ZDO override: Active_EP_rsp nwk=0x%04x eps=[%u,%u] to=0x%04x",
             (unsigned)nwk_addr,
             (unsigned)HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
             (unsigned)HUE_GP_ENDPOINT,
             (unsigned)ind->src_short_addr);
    return send_zdo_response(ind->src_short_addr, HUE_ZDO_ACTIVE_EP_RSP_CLUSTER_ID,
                             payload, sizeof(payload)) == ESP_OK;
}

static bool send_simple_desc_response(const esp_zb_apsde_data_ind_t *ind,
                                      uint8_t tsn,
                                      uint16_t nwk_addr,
                                      uint8_t endpoint)
{
    static const uint8_t ep11_simple_desc[] = {
        HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
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
        0x03, 0xfc, /* Philips FC03 */
        0x00, 0x03, /* Color Control */
        0x01, 0xfc, /* Philips FC01 */
        0x04, 0xfc, /* Philips FC04 */
        0x01,
        0x19, 0x00, /* OTA Upgrade client */
    };
    static const uint8_t ep242_simple_desc[] = {
        HUE_GP_ENDPOINT,
        0xe0, 0xa1, /* Green Power profile */
        0x61, 0x00, /* Green Power Proxy Basic */
        0x00,
        0x00,
        0x01,
        0x21, 0x00, /* Green Power client */
    };

    const uint8_t *desc = NULL;
    uint8_t desc_len = 0;
    if (endpoint == HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE) {
        desc = ep11_simple_desc;
        desc_len = sizeof(ep11_simple_desc);
    } else if (endpoint == HUE_GP_ENDPOINT) {
        desc = ep242_simple_desc;
        desc_len = sizeof(ep242_simple_desc);
    } else {
        return false;
    }

    uint8_t payload[4 + 1 + sizeof(ep11_simple_desc)] = {
        tsn,
        HUE_ZDO_SUCCESS_STATUS,
        0x00, 0x00, /* nwk_addr */
        desc_len,
    };
    put_le16(&payload[2], nwk_addr);
    memcpy(&payload[5], desc, desc_len);

    ESP_LOGI(TAG, "ZDO override: Simple_Desc_rsp nwk=0x%04x ep=%u len=%u to=0x%04x",
             (unsigned)nwk_addr, (unsigned)endpoint, (unsigned)desc_len,
             (unsigned)ind->src_short_addr);
    return send_zdo_response(ind->src_short_addr, HUE_ZDO_SIMPLE_DESC_RSP_CLUSTER_ID,
                             payload, (uint8_t)(5 + desc_len)) == ESP_OK;
}

static bool hue_zdo_descriptor_override_cb(esp_zb_apsde_data_ind_t ind)
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

    if (ind.cluster_id == HUE_ZDO_NODE_DESC_REQ_CLUSTER_ID) {
        ESP_LOGI(TAG, "ZDO override: Node_Desc_req nwk=0x%04x from=0x%04x",
                 (unsigned)nwk_addr, (unsigned)ind.src_short_addr);
        return send_node_desc_response(&ind, tsn, nwk_addr);
    }

    if (ind.cluster_id == HUE_ZDO_ACTIVE_EP_REQ_CLUSTER_ID) {
        ESP_LOGI(TAG, "ZDO override: Active_EP_req nwk=0x%04x from=0x%04x",
                 (unsigned)nwk_addr, (unsigned)ind.src_short_addr);
        return send_active_ep_response(&ind, tsn, nwk_addr);
    }

    if (ind.cluster_id == HUE_ZDO_SIMPLE_DESC_REQ_CLUSTER_ID && ind.asdu_length >= 4) {
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
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            /* Always attempt steering/rejoin on startup. When the Hue bridge is
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
        emit_state_json(endpoint);
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
    if (message->info.cluster == HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID) {
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

    if (message->info.cluster == HUE_MANU_SPECIFIC_PHILIPS2_CLUSTER_ID &&
        message->info.command.id == HUE_FC03_MULTICOLOR_CMD_ID &&
        message->info.header.manuf_code == HUE_SIGNIFY_MANUFACTURER_CODE) {
        handle_hue_multicolor_command(message->info.dst_endpoint,
                                      (const uint8_t *)message->data.value,
                                      message->data.size);
    }

    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
        message->info.command.id == 0xC0 &&
        message->info.header.manuf_code == HUE_SIGNIFY_MANUFACTURER_CODE) {
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
            .manuf_code = HUE_SIGNIFY_MANUFACTURER_CODE,
            .custom_cmd_id = 0xC1,
            .data = {
                .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
                .size = sizeof(HUE_BASIC_CMD_C1_RESPONSE),
                .value = (void *)HUE_BASIC_CMD_C1_RESPONSE,
            },
        };
        ESP_LOGI(TAG, "BASIC_CMD_C0_RESP_C1: tsn=0x%02x size=%u",
                 (unsigned)message->info.header.tsn,
                 (unsigned)sizeof(HUE_BASIC_CMD_C1_RESPONSE));
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
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
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

static void print_hex_bytes(const char *prefix, const uint8_t *payload, uint16_t len)
{
    uint16_t shown = len < 48 ? len : 48;
    printf("%s", prefix);
    for (uint16_t i = 0; i < shown; i++) {
        printf("%02x", payload[i]);
    }
    if (shown < len) {
        printf("...");
    }
    printf("\n");
}

static void log_zcl_payload_summary(const zb_zcl_parsed_hdr_t *hdr, const uint8_t *payload, uint16_t len)
{
    if (!hdr || !payload || len == 0) {
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
 * handler and lets us see every ZCL command the Hue bridge sends during
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
                 zcl_global_cmd_name(hdr->cmd_id),
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
                 zcl_global_cmd_name(hdr->cmd_id),
                 hdr->cmd_direction ? "to_cli" : "to_srv",
                 hdr->is_manuf_specific ? hdr->manuf_specific : 0,
                 hdr->is_common_command ? 1 : 0,
                 (unsigned)payload_len);
        log_zcl_payload_summary(hdr, payload, payload_len);
#if HUE_FC01_EXPLICIT_DEFAULT_RESPONSE
        if (hdr->cluster_id == HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID &&
            hdr->cmd_id == 0x03 &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == HUE_SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV) {
            ESP_LOGI(TAG, "FC01_EXPLICIT_DEFAULT_RESP: cmd=0x03 tsn=0x%02x status=0x00",
                     (unsigned)zb_zcl_get_tsn_from_packet(bufid));
            (void)zb_zcl_send_default_handler(bufid, hdr, ZB_ZCL_STATUS_SUCCESS);
            return true;
        }
#endif
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == HUE_SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_basic_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE &&
            hdr->cmd_id == HUE_OTA_IMAGE_NOTIFY_CMD_ID &&
            !hdr->is_common_command &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI) {
            send_ota_query_next_image_raw(hdr);
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == HUE_SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_identify_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == HUE_MANU_SPECIFIC_PHILIPS2_CLUSTER_ID &&
            hdr->cmd_id == ZB_ZCL_CMD_READ_ATTRIB &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == HUE_SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_fc03_read_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
            hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == HUE_SIGNIFY_MANUFACTURER_CODE &&
            hdr->cmd_direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV &&
            handle_basic_write_attrs_raw(hdr, payload, payload_len)) {
            zb_buf_free(bufid);
            return true;
        }
        if (hdr->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
            hdr->cmd_id == 0xC0 &&
            hdr->is_manuf_specific &&
            hdr->manuf_specific == HUE_SIGNIFY_MANUFACTURER_CODE &&
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

static void add_hue_proprietary_clusters(esp_zb_cluster_list_t *cluster_list, uint8_t idx)
{
    /* manuSpecificPhilips2 (0xFC03): the gradient cluster. */
    esp_zb_attribute_list_t *philips2_attr_list =
        esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS2_CLUSTER_ID);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0002,
                                          ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          s_hue_state[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0001,
                                          ESP_ZB_ZCL_ATTR_TYPE_32BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr1[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0010,
                                          ESP_ZB_ZCL_ATTR_TYPE_16BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr10[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0011,
                                          ESP_ZB_ZCL_ATTR_TYPE_64BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr11[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0012,
                                          ESP_ZB_ZCL_ATTR_TYPE_32BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr12[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0013,
                                          ESP_ZB_ZCL_ATTR_TYPE_16BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr13[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0032,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                          &s_fc03_attr32[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0033,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                          &s_fc03_attr33[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0034,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                          &s_fc03_attr34[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0038,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc03_attr38[idx]);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips2_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* manuSpecificPhilips3 (0xFC01): Signify certification/proprietary cluster.
     * A real Hue gradient lightstrip returns:
     *   0x0000 - 8-bit bitmap = 0x0B
     *   0x0001 - 8-bit enum  = 0x00
     */
    esp_zb_attribute_list_t *philips3_attr_list =
        esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID);
    esp_zb_custom_cluster_add_custom_attr(philips3_attr_list, 0x0000,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr0[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips3_attr_list, 0x0001,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr1[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips3_attr_list, 0x0002,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr2[idx]);
    esp_zb_custom_cluster_add_custom_attr(philips3_attr_list, 0x0003,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc01_attr3[idx]);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips3_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* manuSpecificPhilips4 (0xFC04): present on real devices, purpose unknown. */
    esp_zb_attribute_list_t *philips4_attr_list =
        esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS4_CLUSTER_ID);
    esp_zb_custom_cluster_add_custom_attr(philips4_attr_list, 0x0000,
                                          ESP_ZB_ZCL_ATTR_TYPE_16BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_fc04_attr0[idx]);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips4_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Real gradient endpoints also expose the Touchlink/ZLL commissioning
     * cluster as a server cluster in their simple descriptor. */
    esp_zb_attribute_list_t *touchlink_attr_list =
        esp_zb_zcl_attr_list_create(HUE_TOUCHLINK_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, touchlink_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void log_fake_simple_descriptor(uint8_t endpoint)
{
    printf("FAKE_DESCRIPTOR: endpoint=%u profile=0x%04x device=0x%04x version=%u "
           "server_clusters=[0x0000,0x0003,0x0004,0x0005,0x0006,0x0008,0x1000,0xfc03,0x0300,0xfc01,0xfc04] "
           "client_clusters=[0x0019]\n",
           (unsigned)endpoint,
           (unsigned)ESP_ZB_AF_HA_PROFILE_ID,
           0x010D,
           1);
}

#if HUE_GP_ENDPOINT_MODE == 1
static void add_green_power_endpoint(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *gp_attr_list = esp_zb_zcl_attr_list_create(HUE_GP_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, gp_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HUE_GP_ENDPOINT,
        .app_profile_id = HUE_GP_PROFILE_ID,
        .app_device_id = HUE_GP_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_gateway_ep(ep_list, cluster_list, endpoint_config));

    printf("FAKE_DESCRIPTOR: endpoint=%u profile=0x%04x device=0x%04x version=%u "
           "server_clusters=[] client_clusters=[0x%04x]\n",
           HUE_GP_ENDPOINT,
           HUE_GP_PROFILE_ID,
           HUE_GP_DEVICE_ID,
           0,
           HUE_GP_CLUSTER_ID);
}
#elif HUE_GP_ENDPOINT_MODE == 2
static zb_uint8_t s_gp_shared_security_key_type;
static zb_uint8_t s_gp_shared_security_key[16];
static zb_uint8_t s_gp_link_key[16];
static zb_uint8_t s_gp_max_proxy_table_entries;
static zb_uint32_t s_gp_functionality = 0x0009ac2f;
static zb_uint32_t s_gp_active_functionality = 0x0009ac2f;
static zb_uint8_t s_gp_proxy_table[1] = { 0 };

ZB_ZCL_DECLARE_GPPB_ATTRIB_LIST_CLI(s_hue_gp_attr_list_cli,
                                    &s_gp_shared_security_key_type,
                                    s_gp_shared_security_key,
                                    s_gp_link_key,
                                    &s_gp_max_proxy_table_entries,
                                    &s_gp_functionality,
                                    &s_gp_active_functionality,
                                    s_gp_proxy_table);

ZB_ZCL_START_DECLARE_CLUSTER_LIST(s_gp_cluster_list)
    ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_GREEN_POWER,
                        ZB_ZCL_ARRAY_SIZE(s_hue_gp_attr_list_cli, zb_zcl_attr_t),
                        s_hue_gp_attr_list_cli,
                        ZB_ZCL_CLUSTER_CLIENT_ROLE,
                        ZB_ZCL_MANUF_CODE_INVALID)
ZB_ZCL_FINISH_DECLARE_CLUSTER_LIST;

ZB_ZCL_DECLARE_GPPB_EP(s_gp_endpoint_desc, HUE_GP_ENDPOINT, s_gp_cluster_list);

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

    printf("FAKE_DESCRIPTOR: endpoint=%u profile=0x%04x device=0x%04x version=%u "
           "server_clusters=[] client_clusters=[0x%04x] source=zboss_gppb\n",
           HUE_GP_ENDPOINT,
           HUE_GP_PROFILE_ID,
           HUE_GP_DEVICE_ID,
           0,
           HUE_GP_CLUSTER_ID);
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

    /* Extended color light (0x010D) is what Hue uses for gradient-capable lights. */
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = 0x010D,
        .app_device_version = 1,
    };

    esp_zb_ep_list_add_ep(ep_list, esp_zb_color_dimmable_light_clusters_create(&light_cfg),
                          endpoint_config);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = (char *)HUE_MANUFACTURER_NAME,
        .model_identifier = (char *)HUE_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, endpoint, &info);

    /* Add extra Basic cluster attributes so Hue shows a more complete device
     * record (date code, software build). */
    esp_zb_cluster_list_t *basic_cluster_list = esp_zb_ep_list_get_ep(ep_list, endpoint);
    esp_zb_attribute_list_t *basic_attr_list = esp_zb_cluster_list_get_cluster(
        basic_cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    /* Spoof the firmware identifiers of a real LCX004 so the Hue bridge looks
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
                                          (void *)HUE_BASIC_POWER_ON_CONFIG);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0021,
                                          ESP_ZB_ZCL_ATTR_TYPE_U32,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_basic_attr21[endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0040,
                                          ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          (void *)HUE_BASIC_PRODUCT_LABEL);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0041,
                                          ESP_ZB_ZCL_ATTR_TYPE_U32,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_basic_attr41[endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0050,
                                          ESP_ZB_ZCL_ATTR_TYPE_32BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &s_basic_attr50[endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0051,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_basic_attr51[endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0053,
                                          ESP_ZB_ZCL_ATTR_TYPE_U32,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          &s_basic_attr53[endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE]);
    esp_zb_custom_cluster_add_custom_attr(basic_attr_list, 0x0054,
                                          ESP_ZB_ZCL_ATTR_TYPE_128_BIT_KEY,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          s_basic_attr54[endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE]);

    // Fix "Off with effect" command from Hue bridge.
    // https://github.com/espressif/esp-zigbee-sdk/issues/457#issuecomment-2426128314
    uint16_t on_off_on_time = 0;
    uint16_t on_off_off_wait_time = 0;
    bool on_off_global_scene_control = false;
    uint8_t on_off_startup_on_off = 0xFF;
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
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &on_off_startup_on_off);

    // Restore previous brightness when Hue sends "On" without a level command.
    uint8_t level_on_level = 0xFF; /* 0xFF == use previous level */
    uint16_t level_on_transition_time = 0;
    uint16_t level_off_transition_time = 0;
    uint16_t level_signify_attr3 = 0x000A;
    uint8_t level_startup_current_level = 0xFF;
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
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &level_startup_current_level);

    /* Add the extended Color Control attributes that the default color dimmable
     * light config does not include. ColorCapabilities was already set via
     * light_cfg.color_cfg.color_capabilities above. */
    static uint16_t enhanced_current_hue = 0x0D10;
    static uint8_t color_loop_active = 0;
    static uint8_t current_saturation = 0xFE;
    static uint16_t color_temperature = 500;
    static uint16_t color_temp_min = 153;
    static uint16_t color_temp_max = 500;
    static uint16_t color_attr4010 = 0xFFFF;
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
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_attr4010);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          HUE_COLOR_POINT_RX_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_rx);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          HUE_COLOR_POINT_RY_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_ry);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          HUE_COLOR_POINT_GX_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_gx);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          HUE_COLOR_POINT_GY_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_gy);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          HUE_COLOR_POINT_BX_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_point_bx);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          HUE_COLOR_POINT_BY_ID,
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
        static uint16_t ota_manufacturer = HUE_SIGNIFY_MANUFACTURER_CODE;
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

    /* Add Signify proprietary clusters (FC01/FC03/FC04) so the Hue bridge
     * recognises this as a certified gradient light. */
    add_hue_proprietary_clusters(cluster_list, endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
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

/* Minimal serial CLI for self-testing the gradient parser without re-pairing
 * the device through the Hue app every time.  Typing:
 *   gradient 500104000a20000000ff00000000ff2800
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

        if (strncmp(line, "gradient ", 9) == 0) {
            uint8_t payload[64];
            size_t len = hex_to_bytes(line + 9, payload, sizeof(payload));
            if (len) {
                handle_hue_multicolor_command(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE,
                                              payload, len);
            } else {
                printf("USAGE: gradient <hex payload>\n");
            }
        } else if (strcmp(line, "state") == 0) {
            emit_state_json(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE);
        } else if (strcmp(line, "discover") == 0 || strcmp(line, "reset") == 0) {
            printf("DATA: {\"event\":\"factory_reset\"}\n");
            esp_zb_factory_reset();
        } else if (strcmp(line, "help") == 0) {
            printf("Commands: gradient <hex>, state, discover/reset, help\n");
        }
    }
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    /* Pretend to be a Signify device at the Zigbee MAC and node-descriptor level. */
    ESP_ERROR_CHECK(esp_zb_set_long_address((uint8_t *)HUE_SPOOFED_LONG_ADDR));
    esp_zb_set_node_descriptor_manufacturer_code(HUE_SIGNIFY_MANUFACTURER_CODE);
    ESP_LOGI(TAG, "Node descriptor manufacturer code set to 0x%04x", (unsigned)HUE_SIGNIFY_MANUFACTURER_CODE);

    // Allow joining Philips Hue networks.
    esp_zb_enable_joining_to_distributed(true);
    esp_zb_secur_TC_standard_distributed_key_set(HUE_TRUST_CENTER_KEY);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
        uint8_t endpoint = HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i;
        add_color_dimmable_light_endpoint(ep_list, endpoint);
    }
#if HUE_GP_ENDPOINT_MODE == 1
    add_green_power_endpoint(ep_list);
#endif

    esp_zb_device_register(ep_list);
    for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
        set_real_lcx004_discovery_attrs(HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i);
    }
#if HUE_GP_ENDPOINT_MODE == 2
    register_native_green_power_endpoint();
#endif
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_device_cb_id_handler_register(zb_device_cb_id_handler);
    esp_zb_raw_command_handler_register(zb_raw_command_handler);
#if HUE_ZDO_DESCRIPTOR_OVERRIDE
    esp_zb_aps_data_indication_handler_register(hue_zdo_descriptor_override_cb);
    ESP_LOGI(TAG, "ZDO descriptor override enabled: endpoint list will advertise [%u,%u]",
             (unsigned)HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE, (unsigned)HUE_GP_ENDPOINT);
#endif
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
    xTaskCreate(serial_cmd_task, "serial_cli", 8192, NULL, 1, NULL);
}
