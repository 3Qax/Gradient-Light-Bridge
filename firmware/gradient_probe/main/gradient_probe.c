/*
 * Gradient probe: join a Philips Hue network pretending to be a Hue gradient
 * lightstrip (LCX004) and dump every Zigbee command received over USB-CDC.
 *
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "gradient_probe.h"
#include "trust_center_key.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_ota.h"
#include "zcl_utility.h"
#include "zboss_api.h"
#include "zboss_api_buf.h"
#include "zcl/zb_zcl_commands.h"

#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile router source code.
#endif

static const char *TAG = "GRADIENT_PROBE";

/* Hue manufacturer-specific cluster IDs found on real gradient lights. */
#define HUE_MANU_SPECIFIC_PHILIPS2_CLUSTER_ID  0xFC03 /* gradient / multiColor */
#define HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID  0xFC01 /* unknown Philips commands */
#define HUE_MANU_SPECIFIC_PHILIPS4_CLUSTER_ID  0xFC04 /* unknown Philips */
#define HUE_OTA_PRODUCT_METADATA_ATTR_ID       0x6400 /* read by Hue bridge during fake discovery */

/* Manufacturer code for Signify Netherlands B.V. */
#define HUE_SIGNIFY_MANUFACTURER_CODE 0x100B

/* Hue gradient lightstrip identity from the Hue Bridge API.
 * Strings are Zigbee octet strings: first byte is the length. */
static const char *MANUFACTURER_NAME = "\x18" "Signify Netherlands B.V.";
static const char *MODEL_IDENTIFIER  = "\x06" "LCX004";

static void dump_hex(const char *label, const uint8_t *data, uint16_t len)
{
    printf("PROBE: %s (%u bytes): ", label, (unsigned)len);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

static void send_fc01_command(uint16_t nwk_addr, uint8_t endpoint, uint8_t cmd_id,
                              const uint8_t *payload, uint16_t len)
{
    ESP_LOGI(TAG, "FC01 tx cmd 0x%02x -> nwk=0x%04x ep=%u (%u bytes)",
             (unsigned)cmd_id, nwk_addr, (unsigned)endpoint, (unsigned)len);
    static uint8_t tx_buf[64];
    if (len > sizeof(tx_buf)) len = sizeof(tx_buf);
    if (len && payload) memcpy(tx_buf, payload, len);

    esp_zb_zcl_custom_cluster_cmd_req_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = GRADIENT_PROBE_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID,
        .manuf_specific = 1,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 0,
        .manuf_code = HUE_SIGNIFY_MANUFACTURER_CODE,
        .custom_cmd_id = cmd_id,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = len,
            .value = len ? tx_buf : NULL,
        },
    };
    esp_zb_zcl_custom_cluster_cmd_req(&req);
}

static void send_custom_command(uint16_t nwk_addr, uint8_t endpoint, uint16_t cluster_id,
                                uint8_t cmd_id, uint16_t manuf_code,
                                const uint8_t *payload, uint16_t len)
{
    ESP_LOGI(TAG, "tx custom cmd 0x%02x cluster=0x%04x -> nwk=0x%04x ep=%u manuf=0x%04x (%u bytes)",
             (unsigned)cmd_id, cluster_id, nwk_addr, (unsigned)endpoint,
             manuf_code, (unsigned)len);
    static uint8_t tx_buf[64];
    if (len > sizeof(tx_buf)) len = sizeof(tx_buf);
    if (len && payload) memcpy(tx_buf, payload, len);

    esp_zb_zcl_custom_cluster_cmd_req_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = GRADIENT_PROBE_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = cluster_id,
        .manuf_specific = manuf_code ? 1 : 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 0,
        .manuf_code = manuf_code,
        .custom_cmd_id = cmd_id,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = len,
            .value = len ? tx_buf : NULL,
        },
    };
    esp_zb_zcl_custom_cluster_cmd_req(&req);
}

static void send_fc01_read_attr(uint16_t nwk_addr, uint8_t endpoint, uint16_t attr_id)
{
    ESP_LOGI(TAG, "FC01 tx read attr 0x%04x -> nwk=0x%04x ep=%u", attr_id, nwk_addr, (unsigned)endpoint);
    static uint16_t attr_field;
    attr_field = attr_id;
    esp_zb_zcl_read_attr_cmd_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = GRADIENT_PROBE_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID,
        .manuf_specific = 1,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 0,
        .manuf_code = HUE_SIGNIFY_MANUFACTURER_CODE,
        .attr_number = 1,
        .attr_field = &attr_field,
    };
    esp_zb_zcl_read_attr_cmd_req(&req);
}

static void send_read_attr(uint16_t nwk_addr, uint8_t endpoint, uint16_t cluster_id,
                           uint16_t attr_id, uint16_t manuf_code)
{
    ESP_LOGI(TAG, "tx read attr 0x%04x from cluster 0x%04x -> nwk=0x%04x ep=%u (manuf=0x%04x)",
             attr_id, cluster_id, nwk_addr, (unsigned)endpoint, manuf_code);
    static uint16_t attr_field;
    attr_field = attr_id;
    esp_zb_zcl_read_attr_cmd_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = GRADIENT_PROBE_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .manuf_specific = manuf_code ? 1 : 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 0,
        .manuf_code = manuf_code,
        .attr_number = 1,
        .attr_field = &attr_field,
    };
    esp_zb_zcl_read_attr_cmd_req(&req);
}

static void send_discover_attr(uint16_t nwk_addr, uint8_t endpoint, uint16_t cluster_id,
                               uint16_t start_attr_id, uint8_t max_attrs, uint16_t manuf_code)
{
    ESP_LOGI(TAG, "tx discover attr start=0x%04x max=%u cluster 0x%04x -> nwk=0x%04x ep=%u (manuf=0x%04x)",
             start_attr_id, (unsigned)max_attrs, cluster_id, nwk_addr, (unsigned)endpoint, manuf_code);
    esp_zb_zcl_disc_attr_cmd_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = nwk_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = GRADIENT_PROBE_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .cluster_id = cluster_id,
        .manuf_specific = manuf_code ? 1 : 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 0,
        .manuf_code = manuf_code,
        .start_attr_id = start_attr_id,
        .max_attr_number = max_attrs,
    };
    esp_zb_zcl_disc_attr_cmd_req(&req);
}

typedef struct {
    uint16_t nwk_addr;
    uint16_t cluster_id;
    uint16_t start_attr_id;
    uint16_t manuf_code;
    uint8_t endpoint;
    uint8_t max_attrs;
    bool pending;
} discover_ext_ctx_t;

static discover_ext_ctx_t s_discover_ext_ctx;

static void send_discover_attr_ext_scheduled(void *arg)
{
    (void)arg;
    discover_ext_ctx_t ctx = s_discover_ext_ctx;
    s_discover_ext_ctx.pending = false;

    ESP_LOGI(TAG, "tx discover attr EXT start=0x%04x max=%u cluster 0x%04x -> nwk=0x%04x ep=%u (manuf=0x%04x)",
             ctx.start_attr_id, (unsigned)ctx.max_attrs, ctx.cluster_id,
             ctx.nwk_addr, (unsigned)ctx.endpoint, ctx.manuf_code);

    zb_bufid_t buffer = zb_buf_get_out();
    if (!buffer) {
        ESP_LOGE(TAG, "discover attr EXT: no ZBOSS output buffer");
        return;
    }

    ZB_ZCL_GENERAL_DISC_ATTRIBUTE_EXT_REQ(buffer,
                                          ZB_ZCL_FRAME_DIRECTION_TO_SRV,
                                          ZB_ZCL_ENABLE_DEFAULT_RESPONSE,
                                          ctx.nwk_addr,
                                          ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                                          ctx.endpoint,
                                          GRADIENT_PROBE_ENDPOINT,
                                          ESP_ZB_AF_HA_PROFILE_ID,
                                          ctx.cluster_id,
                                          NULL,
                                          ctx.manuf_code ? ZB_TRUE : ZB_FALSE,
                                          ctx.manuf_code,
                                          ctx.start_attr_id,
                                          ctx.max_attrs);
}

static void send_discover_attr_ext(uint16_t nwk_addr, uint8_t endpoint, uint16_t cluster_id,
                                   uint16_t start_attr_id, uint8_t max_attrs, uint16_t manuf_code)
{
    s_discover_ext_ctx = (discover_ext_ctx_t) {
        .nwk_addr = nwk_addr,
        .cluster_id = cluster_id,
        .start_attr_id = start_attr_id,
        .manuf_code = manuf_code,
        .endpoint = endpoint,
        .max_attrs = max_attrs,
        .pending = true,
    };
    esp_zb_scheduler_alarm((esp_zb_callback_t)send_discover_attr_ext_scheduled, 0, 0);
}

/* --------------------------------------------------------------------------
 * Focused OTA/basic metadata sweep against a real LCX004
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
    uint16_t manuf_code;
    uint8_t direction;
    const char *label;
} metadata_read_t;

static const metadata_read_t METADATA_READS[] = {
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x0004, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.manufacturer_name"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x0005, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.model_identifier"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x000a, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.product_code"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x000c, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.manufacturer_version_details"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x000e, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.product_label"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x0020, HUE_SIGNIFY_MANUFACTURER_CODE, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.signify_attr_0020"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x0021, HUE_SIGNIFY_MANUFACTURER_CODE, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.signify_attr_0021"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x4000, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.sw_build_id"},
    {ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x0007, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "basic.power_source"},
    {ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, 0x0000, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "onoff.on_off"},
    {ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, 0x0000, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "level.current_level"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x400b, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.color_capabilities"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x400c, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.color_temp_startup"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0007, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.color_temperature"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0032, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.point_rx"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0033, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.point_ry"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0036, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.point_gx"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0037, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.point_gy"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x003a, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.point_bx"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x003b, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.point_by"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0003, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.current_x"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0004, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.current_y"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x4000, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.enhanced_current_hue"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0001, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.current_saturation"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0008, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.color_mode"},
    {ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x4002, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "color.enhanced_color_mode"},
    {ESP_ZB_ZCL_CLUSTER_ID_GROUPS, 0xe9d5, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV, "groups.bridge_attr_e9d5"},
    {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_VERSION_ID, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI, "ota.file_version"},
    {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MANUFACTURE_ID, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI, "ota.manufacturer_id"},
    {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_TYPE_ID, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI, "ota.image_type"},
    {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_STAMP_ID, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI, "ota.image_stamp"},
    {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE, HUE_OTA_PRODUCT_METADATA_ATTR_ID, 0, ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI, "ota.hue_attr_6400"},
    {ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE, HUE_OTA_PRODUCT_METADATA_ATTR_ID, HUE_SIGNIFY_MANUFACTURER_CODE, ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI, "ota.hue_attr_6400_signify"},
};

typedef struct {
    uint16_t nwk_addr;
    uint8_t endpoint;
    uint8_t next_idx;
} metadata_sweep_ctx_t;

static metadata_sweep_ctx_t s_metadata_sweep_ctx = {0};

static void metadata_sweep_next(void *arg)
{
    (void)arg;
    metadata_sweep_ctx_t *ctx = &s_metadata_sweep_ctx;
    if (ctx->nwk_addr == 0) {
        return;
    }

    if (ctx->next_idx >= (sizeof(METADATA_READS) / sizeof(METADATA_READS[0]))) {
        ESP_LOGI(TAG, "Metadata sweep done for nwk=0x%04x ep=%u",
                 ctx->nwk_addr, (unsigned)ctx->endpoint);
        ctx->nwk_addr = 0;
        return;
    }

    const metadata_read_t *read = &METADATA_READS[ctx->next_idx++];
    printf("PROBE: metadata_read label=%s nwk=0x%04x ep=%u cluster=0x%04x attr=0x%04x manuf=0x%04x dir=%s\n",
           read->label, ctx->nwk_addr, (unsigned)ctx->endpoint,
           (unsigned)read->cluster_id, (unsigned)read->attr_id, (unsigned)read->manuf_code,
           read->direction == ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI ? "to_cli" : "to_srv");
    static uint16_t attr_field;
    attr_field = read->attr_id;
    esp_zb_zcl_read_attr_cmd_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = ctx->nwk_addr,
            .dst_endpoint = ctx->endpoint,
            .src_endpoint = GRADIENT_PROBE_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = read->cluster_id,
        .manuf_specific = read->manuf_code ? 1 : 0,
        .direction = read->direction,
        .dis_default_resp = 0,
        .manuf_code = read->manuf_code,
        .attr_number = 1,
        .attr_field = &attr_field,
    };
    esp_zb_zcl_read_attr_cmd_req(&req);
    esp_zb_scheduler_alarm((esp_zb_callback_t)metadata_sweep_next, 0, 350);
}

static void start_metadata_sweep(uint16_t nwk_addr, uint8_t endpoint)
{
    s_metadata_sweep_ctx.nwk_addr = nwk_addr;
    s_metadata_sweep_ctx.endpoint = endpoint;
    s_metadata_sweep_ctx.next_idx = 0;
    ESP_LOGI(TAG, "Starting OTA/basic metadata sweep on nwk=0x%04x ep=%u",
             nwk_addr, (unsigned)endpoint);
    metadata_sweep_next(NULL);
}

/* --------------------------------------------------------------------------
 * Automatic FC01 command/attribute sweep against a real gradient device
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t nwk_addr;
    uint8_t endpoint;
    uint8_t step;
} fc01_sweep_ctx_t;

static fc01_sweep_ctx_t s_fc01_sweep_ctx = {0};

/* Observed Hue bridge FC01 cmd 0x03 payload. */
static const uint8_t FC01_CMD3_PAYLOAD[] = {0x00, 0x01, 0xaf, 0xb4, 0x07, 0x00};

#define FC01_SWEEP_STEP_READ_ATTR0   0
#define FC01_SWEEP_STEP_CMD1_EMPTY   1
#define FC01_SWEEP_STEP_CMD2_EMPTY   2
#define FC01_SWEEP_STEP_CMD3_OBS     3
#define FC01_SWEEP_STEP_CMD4_EMPTY   4
#define FC01_SWEEP_STEP_CMD7_EMPTY   5
#define FC01_SWEEP_STEP_READ_ATTR1   6
#define FC01_SWEEP_STEP_READ_ATTR2   7
#define FC01_SWEEP_STEP_READ_ATTR3   8
#define FC01_SWEEP_STEP_DONE         9

static void fc01_sweep_next(void *arg);

static void fc01_sweep_schedule_next(void)
{
    esp_zb_scheduler_alarm((esp_zb_callback_t)fc01_sweep_next, 0, 500);
}

static void fc01_sweep_next(void *arg)
{
    (void)arg;
    fc01_sweep_ctx_t *ctx = &s_fc01_sweep_ctx;
    if (ctx->nwk_addr == 0) return;

    switch (ctx->step) {
    case FC01_SWEEP_STEP_READ_ATTR0:
        send_fc01_read_attr(ctx->nwk_addr, ctx->endpoint, 0x0000);
        break;
    case FC01_SWEEP_STEP_CMD1_EMPTY:
        send_fc01_command(ctx->nwk_addr, ctx->endpoint, 0x01, NULL, 0);
        break;
    case FC01_SWEEP_STEP_CMD2_EMPTY:
        send_fc01_command(ctx->nwk_addr, ctx->endpoint, 0x02, NULL, 0);
        break;
    case FC01_SWEEP_STEP_CMD3_OBS:
        send_fc01_command(ctx->nwk_addr, ctx->endpoint, 0x03,
                          FC01_CMD3_PAYLOAD, sizeof(FC01_CMD3_PAYLOAD));
        break;
    case FC01_SWEEP_STEP_CMD4_EMPTY:
        send_fc01_command(ctx->nwk_addr, ctx->endpoint, 0x04, NULL, 0);
        break;
    case FC01_SWEEP_STEP_CMD7_EMPTY:
        send_fc01_command(ctx->nwk_addr, ctx->endpoint, 0x07, NULL, 0);
        break;
    case FC01_SWEEP_STEP_READ_ATTR1:
        send_fc01_read_attr(ctx->nwk_addr, ctx->endpoint, 0x0001);
        break;
    case FC01_SWEEP_STEP_READ_ATTR2:
        send_fc01_read_attr(ctx->nwk_addr, ctx->endpoint, 0x0002);
        break;
    case FC01_SWEEP_STEP_READ_ATTR3:
        send_fc01_read_attr(ctx->nwk_addr, ctx->endpoint, 0x0003);
        break;
    default:
        ESP_LOGI(TAG, "FC01 sweep done for nwk=0x%04x ep=%u", ctx->nwk_addr, ctx->endpoint);
        ctx->nwk_addr = 0;
        return;
    }

    ctx->step++;
    fc01_sweep_schedule_next();
}

static void start_fc01_sweep(uint16_t nwk_addr, uint8_t endpoint)
{
    fc01_sweep_ctx_t *ctx = &s_fc01_sweep_ctx;
    ctx->nwk_addr = nwk_addr;
    ctx->endpoint = endpoint;
    ctx->step = FC01_SWEEP_STEP_READ_ATTR0;
    ESP_LOGI(TAG, "Starting FC01 sweep on nwk=0x%04x ep=%u", nwk_addr, (unsigned)endpoint);
    fc01_sweep_schedule_next();
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
                        , TAG, "Failed to start Zigbee commissioning");
}

static void start_gradient_scan(void *arg);
static void start_metadata_scan(void *arg);
static void start_discover_scan(void *arg);
static void start_match_scan(void *arg);

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
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode",
                     esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
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
            esp_zb_scheduler_alarm((esp_zb_callback_t)start_metadata_scan, 0, 5 * 1000);
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
            }
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* --------------------------------------------------------------------------
 * ZDO scanning of real Hue gradient devices
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    esp_zb_ieee_addr_t ieee;
} target_device_t;

/* IEEE addresses in the byte order used by the Zigbee stack (reverse of the
 * Hue API uniqueid display order). */
static const target_device_t TARGET_DEVICES[] = {
    {"Headboard LCX004",        {0x2f, 0x54, 0x89, 0x0b, 0x01, 0x88, 0x17, 0x00}},
    {"Floor LCX004",            {0x82, 0x38, 0xe4, 0x0b, 0x01, 0x88, 0x17, 0x00}},
    {"Play gradient tube",      {0x3f, 0xde, 0xe4, 0x0b, 0x01, 0x88, 0x17, 0x00}},
    {"Play gradient tube 2",    {0xa7, 0x29, 0xe5, 0x0b, 0x01, 0x88, 0x17, 0x00}},
    {"Signe gradient floor",    {0x80, 0xd3, 0xe5, 0x0b, 0x01, 0x88, 0x17, 0x00}},
    {"Signe gradient table 1",  {0x99, 0x1c, 0xe5, 0x0b, 0x01, 0x88, 0x17, 0x00}},
    {"Signe gradient table 2",  {0xbe, 0x1c, 0xe5, 0x0b, 0x01, 0x88, 0x17, 0x00}},
};
#define TARGET_DEVICE_COUNT (sizeof(TARGET_DEVICES) / sizeof(TARGET_DEVICES[0]))

typedef struct {
    const target_device_t *target;
    uint16_t nwk_addr;
    uint8_t ep_count;
    uint8_t *ep_list;
    uint8_t next_ep_idx;
} scan_context_t;

typedef struct {
    const target_device_t *target;
} metadata_scan_context_t;

typedef struct {
    char label[24];
} metadata_eui_context_t;

static bool parse_ieee_hex(const char *src, esp_zb_ieee_addr_t out)
{
    uint8_t bytes[8];
    uint8_t count = 0;
    while (*src && count < sizeof(bytes)) {
        while (*src == ':' || *src == '-' || *src == ' ' || *src == '\t') {
            src++;
        }
        unsigned value;
        if (sscanf(src, "%2x", &value) != 1) {
            break;
        }
        bytes[count++] = (uint8_t)value;
        src += 2;
    }
    if (count != sizeof(bytes)) {
        return false;
    }
    memcpy(out, bytes, sizeof(bytes));
    return true;
}

static void node_desc_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr,
                         esp_zb_af_node_desc_t *node_desc, void *user_ctx)
{
    (void)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !node_desc) {
        ESP_LOGE(TAG, "NODE_DESC nwk=0x%04x failed status=%u", addr, (unsigned)zdo_status);
        return;
    }

    printf("PROBE: NODE_DESC nwk=0x%04x flags=0x%04x mac=0x%02x manufacturer=0x%04x "
           "max_buf=%u max_in=%u server_mask=0x%04x max_out=%u desc_cap=0x%02x\n",
           addr,
           (unsigned)node_desc->node_desc_flags,
           (unsigned)node_desc->mac_capability_flags,
           (unsigned)node_desc->manufacturer_code,
           (unsigned)node_desc->max_buf_size,
           (unsigned)node_desc->max_incoming_transfer_size,
           (unsigned)node_desc->server_mask,
           (unsigned)node_desc->max_outgoing_transfer_size,
           (unsigned)node_desc->desc_capability_field);
}

static void send_node_desc_req(uint16_t nwk_addr)
{
    ESP_LOGI(TAG, "tx node desc req -> nwk=0x%04x", nwk_addr);
    esp_zb_zdo_node_desc_req_param_t req = {
        .dst_nwk_addr = nwk_addr,
    };
    esp_zb_zdo_node_desc_req(&req, node_desc_cb, NULL);
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    scan_context_t *ctx = (scan_context_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGE(TAG, "SCAN %s: simple_desc failed status=%u", ctx->target->name, (unsigned)zdo_status);
    } else {
        printf("PROBE: SCAN simple_desc name=%s nwk=0x%04x ep=%u profile=0x%04x device=0x%04x ver=%u in=%u out=%u clusters=[",
               ctx->target->name, ctx->nwk_addr,
               (unsigned)simple_desc->endpoint,
               (unsigned)simple_desc->app_profile_id,
               (unsigned)simple_desc->app_device_id,
               (unsigned)simple_desc->app_device_version,
               (unsigned)simple_desc->app_input_cluster_count,
               (unsigned)simple_desc->app_output_cluster_count);
        uint8_t total = simple_desc->app_input_cluster_count + simple_desc->app_output_cluster_count;
        for (uint8_t i = 0; i < total; i++) {
            if (i == simple_desc->app_input_cluster_count) printf(" | ");
            printf("0x%04x", (unsigned)simple_desc->app_cluster_list[i]);
            if (i + 1 < total && !(i + 1 == simple_desc->app_input_cluster_count)) printf(",");
        }
        printf("]\n");
    }

    ctx->next_ep_idx++;
    if (ctx->next_ep_idx < ctx->ep_count && ctx->ep_list) {
        esp_zb_zdo_simple_desc_req_param_t req = {
            .addr_of_interest = ctx->nwk_addr,
            .endpoint = ctx->ep_list[ctx->next_ep_idx],
        };
        esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, ctx);
    } else {
        ESP_LOGI(TAG, "SCAN %s: done", ctx->target->name);
        if (ctx->ep_list) free(ctx->ep_list);
        free(ctx);
    }
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    scan_context_t *ctx = (scan_context_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !ep_count || !ep_id_list) {
        ESP_LOGE(TAG, "SCAN %s: active_ep failed status=%u count=%u", ctx->target->name,
                 (unsigned)zdo_status, (unsigned)ep_count);
        free(ctx);
        return;
    }

    printf("PROBE: SCAN active_ep name=%s nwk=0x%04x count=%u eps=[", ctx->target->name, ctx->nwk_addr, (unsigned)ep_count);
    for (uint8_t i = 0; i < ep_count; i++) {
        printf("%u", (unsigned)ep_id_list[i]);
        if (i + 1 < ep_count) printf(",");
    }
    printf("]\n");

    ctx->ep_count = ep_count;
    ctx->ep_list = (uint8_t *)malloc(ep_count);
    if (ctx->ep_list) memcpy(ctx->ep_list, ep_id_list, ep_count);
    ctx->next_ep_idx = 0;

    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = ctx->nwk_addr,
        .endpoint = ep_id_list[0],
    };
    esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, ctx);
}

static void nwk_addr_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_nwk_addr_rsp_t *resp, void *user_ctx)
{
    scan_context_t *ctx = (scan_context_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !resp) {
        ESP_LOGE(TAG, "SCAN %s: nwk_addr failed status=%u", ctx->target->name, (unsigned)zdo_status);
        free(ctx);
        return;
    }

    ctx->nwk_addr = resp->nwk_addr;
    printf("PROBE: SCAN nwk_addr name=%s nwk=0x%04x\n", ctx->target->name, ctx->nwk_addr);

    /* Sweep all known FC01 commands and attributes against this real gradient device. */
    start_fc01_sweep(ctx->nwk_addr, GRADIENT_PROBE_ENDPOINT);

    esp_zb_zdo_active_ep_req_param_t active_req = {
        .addr_of_interest = ctx->nwk_addr,
    };
    esp_zb_zdo_active_ep_req(&active_req, active_ep_cb, ctx);
}

static void metadata_nwk_addr_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_nwk_addr_rsp_t *resp, void *user_ctx)
{
    metadata_scan_context_t *ctx = (metadata_scan_context_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !resp) {
        ESP_LOGE(TAG, "METADATA %s: nwk_addr failed status=%u", ctx->target->name, (unsigned)zdo_status);
        free(ctx);
        return;
    }

    printf("PROBE: METADATA target=%s nwk=0x%04x ep=%u\n",
           ctx->target->name, resp->nwk_addr, (unsigned)GRADIENT_PROBE_ENDPOINT);
    start_metadata_sweep(resp->nwk_addr, GRADIENT_PROBE_ENDPOINT);
    free(ctx);
}

static void metadata_eui_nwk_addr_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_nwk_addr_rsp_t *resp, void *user_ctx)
{
    metadata_eui_context_t *ctx = (metadata_eui_context_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !resp) {
        ESP_LOGE(TAG, "METADATA_EUI %s: nwk_addr failed status=%u",
                 ctx->label, (unsigned)zdo_status);
        free(ctx);
        return;
    }

    printf("PROBE: METADATA_EUI label=%s nwk=0x%04x ep=%u\n",
           ctx->label, resp->nwk_addr, (unsigned)GRADIENT_PROBE_ENDPOINT);
    start_metadata_sweep(resp->nwk_addr, GRADIENT_PROBE_ENDPOINT);
    free(ctx);
}

static void metadata_scan_ieee(const char *label, const esp_zb_ieee_addr_t ieee)
{
    metadata_eui_context_t *ctx = (metadata_eui_context_t *)calloc(1, sizeof(metadata_eui_context_t));
    if (!ctx) return;
    snprintf(ctx->label, sizeof(ctx->label), "%s", label);

    esp_zb_zdo_nwk_addr_req_param_t req = {
        .dst_nwk_addr = 0xFFFD,
        .ieee_addr_of_interest = {0},
        .request_type = 0,
        .start_index = 0,
    };
    memcpy(req.ieee_addr_of_interest, ieee, sizeof(esp_zb_ieee_addr_t));
    ESP_LOGI(TAG, "METADATA_EUI %s: resolving via broadcast", ctx->label);
    esp_zb_zdo_nwk_addr_req(&req, metadata_eui_nwk_addr_cb, ctx);
}

static void metadata_scan_target_device(const target_device_t *target)
{
    metadata_scan_context_t *ctx = (metadata_scan_context_t *)calloc(1, sizeof(metadata_scan_context_t));
    if (!ctx) return;
    ctx->target = target;

    esp_zb_zdo_nwk_addr_req_param_t req = {
        .dst_nwk_addr = 0xFFFD,
        .ieee_addr_of_interest = {0},
        .request_type = 0,
        .start_index = 0,
    };
    memcpy(req.ieee_addr_of_interest, target->ieee, sizeof(esp_zb_ieee_addr_t));
    ESP_LOGI(TAG, "METADATA %s: resolving nwk_addr", target->name);
    esp_zb_zdo_nwk_addr_req(&req, metadata_nwk_addr_cb, ctx);
}

static void scan_target_device(const target_device_t *target)
{
    scan_context_t *ctx = (scan_context_t *)calloc(1, sizeof(scan_context_t));
    if (!ctx) return;
    ctx->target = target;

    esp_zb_zdo_nwk_addr_req_param_t req = {
        .dst_nwk_addr = 0x0000, /* unicast to the Hue coordinator */
        .ieee_addr_of_interest = {0},
        .request_type = 0,
        .start_index = 0,
    };
    memcpy(req.ieee_addr_of_interest, target->ieee, sizeof(esp_zb_ieee_addr_t));
    ESP_LOGI(TAG, "SCAN %s: sending nwk_addr broadcast", target->name);
    esp_zb_zdo_nwk_addr_req(&req, nwk_addr_cb, ctx);
}

static void start_second_lcx004_metadata_scan(void *arg)
{
    (void)arg;
    metadata_scan_target_device(&TARGET_DEVICES[1]);
}

static void start_gradient_scan(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting ZDO scan of %u gradient-capable devices", (unsigned)TARGET_DEVICE_COUNT);
    for (uint8_t i = 0; i < TARGET_DEVICE_COUNT; i++) {
        scan_target_device(&TARGET_DEVICES[i]);
    }
}

static void start_metadata_scan(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting OTA/basic metadata scan of real LCX004 devices");
    metadata_scan_target_device(&TARGET_DEVICES[0]);
    esp_zb_scheduler_alarm((esp_zb_callback_t)start_second_lcx004_metadata_scan, 0, 7000);
}

/* --------------------------------------------------------------------------
 * Broadcast discovery of all color dimmable lights
 * -------------------------------------------------------------------------- */

static void discover_simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx);
static void discover_device_simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx);
static void discover_next_simple_desc(void);

typedef struct {
    uint16_t nwk_addr;
    uint8_t ep;
    bool queried;
} discovered_device_t;

#define MAX_DISCOVERED 64
static discovered_device_t s_discovered[MAX_DISCOVERED];
static uint8_t s_discovered_count = 0;
static uint8_t s_discover_next = 0;

static discovered_device_t *find_discovered(uint16_t nwk_addr, uint8_t ep)
{
    for (uint8_t i = 0; i < s_discovered_count; i++) {
        if (s_discovered[i].nwk_addr == nwk_addr && s_discovered[i].ep == ep) {
            return &s_discovered[i];
        }
    }
    return NULL;
}

static void discover_next_simple_desc(void)
{
    while (s_discover_next < s_discovered_count && s_discovered[s_discover_next].queried) {
        s_discover_next++;
    }
    if (s_discover_next >= s_discovered_count) {
        ESP_LOGI(TAG, "DISCOVER complete: %u endpoints", (unsigned)s_discovered_count);
        return;
    }
    discovered_device_t *dev = &s_discovered[s_discover_next];
    dev->queried = true;
    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = dev->nwk_addr,
        .endpoint = dev->ep,
    };
    esp_zb_zdo_simple_desc_req(&req, discover_simple_desc_cb, NULL);
}

static void discover_simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    (void)user_ctx;
    if (s_discover_next >= s_discovered_count) return;

    discovered_device_t *dev = &s_discovered[s_discover_next];
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGE(TAG, "DISCOVER simple_desc nwk=0x%04x ep=%u failed status=%u",
                 dev->nwk_addr, (unsigned)dev->ep, (unsigned)zdo_status);
    } else {
        printf("PROBE: DISCOVER simple_desc nwk=0x%04x ep=%u profile=0x%04x device=0x%04x ver=%u in=%u out=%u clusters=[",
               dev->nwk_addr,
               (unsigned)simple_desc->endpoint,
               (unsigned)simple_desc->app_profile_id,
               (unsigned)simple_desc->app_device_id,
               (unsigned)simple_desc->app_device_version,
               (unsigned)simple_desc->app_input_cluster_count,
               (unsigned)simple_desc->app_output_cluster_count);
        uint8_t total = simple_desc->app_input_cluster_count + simple_desc->app_output_cluster_count;
        for (uint8_t i = 0; i < total; i++) {
            if (i == simple_desc->app_input_cluster_count) printf(" | ");
            printf("0x%04x", (unsigned)simple_desc->app_cluster_list[i]);
            if (i + 1 < total && !(i + 1 == simple_desc->app_input_cluster_count)) printf(",");
        }
        printf("]\n");
    }

    s_discover_next++;
    discover_next_simple_desc();
}

typedef struct {
    uint16_t nwk_addr;
    uint8_t ep_count;
    uint8_t *ep_list;
    uint8_t next_idx;
} discover_device_ctx_t;

static void discover_device_simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    discover_device_ctx_t *ctx = (discover_device_ctx_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGE(TAG, "DISCOVER device simple_desc nwk=0x%04x ep=%u failed status=%u",
                 ctx->nwk_addr, ctx->next_idx < ctx->ep_count ? (unsigned)ctx->ep_list[ctx->next_idx] : 0, (unsigned)zdo_status);
    } else {
        printf("PROBE: DISCOVER simple_desc nwk=0x%04x ep=%u profile=0x%04x device=0x%04x ver=%u in=%u out=%u clusters=[",
               ctx->nwk_addr,
               (unsigned)simple_desc->endpoint,
               (unsigned)simple_desc->app_profile_id,
               (unsigned)simple_desc->app_device_id,
               (unsigned)simple_desc->app_device_version,
               (unsigned)simple_desc->app_input_cluster_count,
               (unsigned)simple_desc->app_output_cluster_count);
        uint8_t total = simple_desc->app_input_cluster_count + simple_desc->app_output_cluster_count;
        for (uint8_t i = 0; i < total; i++) {
            if (i == simple_desc->app_input_cluster_count) printf(" | ");
            printf("0x%04x", (unsigned)simple_desc->app_cluster_list[i]);
            if (i + 1 < total && !(i + 1 == simple_desc->app_input_cluster_count)) printf(",");
        }
        printf("]\n");
    }

    ctx->next_idx++;
    if (ctx->next_idx < ctx->ep_count && ctx->ep_list) {
        esp_zb_zdo_simple_desc_req_param_t req = {
            .addr_of_interest = ctx->nwk_addr,
            .endpoint = ctx->ep_list[ctx->next_idx],
        };
        esp_zb_zdo_simple_desc_req(&req, discover_device_simple_desc_cb, ctx);
    } else {
        free(ctx->ep_list);
        free(ctx);
    }
}

static void discover_active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    discover_device_ctx_t *ctx = (discover_device_ctx_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !ep_count || !ep_id_list) {
        ESP_LOGE(TAG, "DISCOVER active_ep nwk=0x%04x failed status=%u", ctx->nwk_addr, (unsigned)zdo_status);
        free(ctx);
        return;
    }

    printf("PROBE: DISCOVER active_ep nwk=0x%04x count=%u eps=[", ctx->nwk_addr, (unsigned)ep_count);
    for (uint8_t i = 0; i < ep_count; i++) {
        printf("%u", (unsigned)ep_id_list[i]);
        if (i + 1 < ep_count) printf(",");
        if (s_discovered_count < MAX_DISCOVERED) {
            s_discovered[s_discovered_count].nwk_addr = ctx->nwk_addr;
            s_discovered[s_discovered_count].ep = ep_id_list[i];
            s_discovered[s_discovered_count].queried = false;
            s_discovered_count++;
        }
    }
    printf("]\n");

    ctx->ep_count = ep_count;
    ctx->ep_list = (uint8_t *)malloc(ep_count);
    if (ctx->ep_list) memcpy(ctx->ep_list, ep_id_list, ep_count);
    ctx->next_idx = 0;

    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = ctx->nwk_addr,
        .endpoint = ep_id_list[0],
    };
    esp_zb_zdo_simple_desc_req(&req, discover_device_simple_desc_cb, ctx);
}

static void discover_match_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, uint8_t endpoint, void *user_ctx)
{
    (void)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "DISCOVER match failed status=%u", (unsigned)zdo_status);
        return;
    }
    if (find_discovered(addr, endpoint)) return; /* already seen */

    printf("PROBE: DISCOVER match nwk=0x%04x ep=%u\n", addr, (unsigned)endpoint);

    if (s_discovered_count < MAX_DISCOVERED) {
        s_discovered[s_discovered_count].nwk_addr = addr;
        s_discovered[s_discovered_count].ep = endpoint;
        s_discovered[s_discovered_count].queried = false;
        s_discovered_count++;
    }

    /* Query active endpoints of this device (only once per address). */
    static uint16_t s_active_ep_queried[MAX_DISCOVERED];
    static uint8_t s_active_ep_queried_count = 0;
    bool already = false;
    for (uint8_t i = 0; i < s_active_ep_queried_count; i++) {
        if (s_active_ep_queried[i] == addr) { already = true; break; }
    }
    if (!already && s_active_ep_queried_count < MAX_DISCOVERED) {
        s_active_ep_queried[s_active_ep_queried_count++] = addr;
        discover_device_ctx_t *ctx = (discover_device_ctx_t *)calloc(1, sizeof(discover_device_ctx_t));
        if (ctx) {
            ctx->nwk_addr = addr;
            esp_zb_zdo_active_ep_req_param_t req = { .addr_of_interest = addr };
            esp_zb_zdo_active_ep_req(&req, discover_active_ep_cb, ctx);
        }
    }
}

static void discover_delayed_simple_desc_cb(void *arg)
{
    (void)arg;
    s_discover_next = 0;
    discover_next_simple_desc();
}

static void start_discover_scan(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting broadcast discovery of color dimmable lights");
    s_discovered_count = 0;
    s_discover_next = 0;
    memset(s_discovered, 0, sizeof(s_discovered));

    esp_zb_zdo_match_desc_req_param_t req = {
        .dst_nwk_addr = 0xFFFD, /* broadcast to all routers */
        .addr_of_interest = 0xFFFD,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .num_in_clusters = 0,
        .num_out_clusters = 0,
        .cluster_list = NULL,
    };
    esp_zb_zdo_find_color_dimmable_light(&req, discover_match_cb, NULL);

    /* After responses settle, also query simple descriptors for every matched endpoint. */
    esp_zb_scheduler_alarm((esp_zb_callback_t)discover_delayed_simple_desc_cb, 0, 6 * 1000);
}

static uint16_t s_fc01_cluster = 0xFC01;

static void start_match_scan(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting match-desc scan for FC01 cluster");
    s_discovered_count = 0;
    s_discover_next = 0;
    memset(s_discovered, 0, sizeof(s_discovered));

    esp_zb_zdo_match_desc_req_param_t req = {
        .dst_nwk_addr = 0xFFFD, /* broadcast to all routers */
        .addr_of_interest = 0xFFFD,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .num_in_clusters = 1,
        .num_out_clusters = 0,
        .cluster_list = &s_fc01_cluster,
    };
    esp_err_t err = esp_zb_zdo_match_cluster(&req, discover_match_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "match_cluster failed: %s", esp_err_to_name(err));
    }
}

static void leave_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "Leave request status=%u", (unsigned)zdo_status);
}

static void serial_input_task(void *arg)
{
    (void)arg;
    char buf[128];
    while (1) {
        if (fgets(buf, sizeof(buf), stdin)) {
            if (strncmp(buf, "metadataeui ", 12) == 0) {
                esp_zb_ieee_addr_t ieee;
                char *arg = buf + 12;
                if (!parse_ieee_hex(arg, ieee)) {
                    ESP_LOGE(TAG, "Serial command metadataeui: expected 8 hex bytes");
                    continue;
                }
                ESP_LOGI(TAG, "Serial command: metadata by IEEE");
                metadata_scan_ieee("serial", ieee);
            } else if (strncmp(buf, "scan", 4) == 0) {
                ESP_LOGI(TAG, "Serial command: scan");
                start_gradient_scan(NULL);
            } else if (strncmp(buf, "metadata", 8) == 0) {
                ESP_LOGI(TAG, "Serial command: metadata scan");
                start_metadata_scan(NULL);
            } else if (strncmp(buf, "discover", 8) == 0) {
                ESP_LOGI(TAG, "Serial command: discover");
                start_discover_scan(NULL);
            } else if (strncmp(buf, "nodedesc ", 9) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 9, NULL, 16);
                ESP_LOGI(TAG, "Serial command: node desc on 0x%04x", target);
                send_node_desc_req(target);
            } else if (strncmp(buf, "leave", 5) == 0) {
                ESP_LOGI(TAG, "Serial command: leave network");
                esp_zb_ieee_addr_t self;
                esp_zb_get_long_address(self);
                esp_zb_zdo_mgmt_leave_req_param_t req = {
                    .dst_nwk_addr = esp_zb_get_short_address(),
                    .rejoin = 1,
                    .remove_children = 0,
                };
                memcpy(req.device_address, self, sizeof(esp_zb_ieee_addr_t));
                esp_zb_zdo_device_leave_req(&req, leave_cb, NULL);
            } else if (strncmp(buf, "join", 4) == 0) {
                ESP_LOGI(TAG, "Serial command: rejoin");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else if (strncmp(buf, "fc01sweep ", 10) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 10, NULL, 16);
                char *ep_ptr = strchr(buf + 10, ' ');
                uint8_t ep = GRADIENT_PROBE_ENDPOINT;
                if (ep_ptr) ep = (uint8_t)strtol(ep_ptr + 1, NULL, 16);
                ESP_LOGI(TAG, "Serial command: FC01 sweep on 0x%04x ep=%u", target, (unsigned)ep);
                start_fc01_sweep(target, ep);
            } else if (strncmp(buf, "fc01cmd ", 8) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 8, NULL, 16);
                char *cmd_ptr = strchr(buf + 8, ' ');
                if (!cmd_ptr) continue;
                uint8_t cmd_id = (uint8_t)strtol(cmd_ptr + 1, NULL, 16);
                char *hex_ptr = strchr(cmd_ptr + 1, ' ');
                uint8_t payload[64];
                uint16_t len = 0;
                if (hex_ptr) {
                    hex_ptr++;
                    while (len < sizeof(payload)) {
                        unsigned byte;
                        if (sscanf(hex_ptr, "%2x", &byte) != 1) break;
                        payload[len++] = (uint8_t)byte;
                        hex_ptr += 2;
                        while (*hex_ptr == ' ') hex_ptr++;
                    }
                }
                ESP_LOGI(TAG, "Serial command: FC01 cmd 0x%02x to 0x%04x (%u bytes)",
                         (unsigned)cmd_id, target, (unsigned)len);
                send_fc01_command(target, GRADIENT_PROBE_ENDPOINT, cmd_id, payload, len);
            } else if (strncmp(buf, "customcmd ", 10) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 10, NULL, 16);
                char *cluster_ptr = strchr(buf + 10, ' ');
                if (!cluster_ptr) continue;
                uint16_t cluster_id = (uint16_t)strtol(cluster_ptr + 1, NULL, 16);
                char *cmd_ptr = strchr(cluster_ptr + 1, ' ');
                if (!cmd_ptr) continue;
                uint8_t cmd_id = (uint8_t)strtol(cmd_ptr + 1, NULL, 16);
                char *manuf_ptr = strchr(cmd_ptr + 1, ' ');
                if (!manuf_ptr) continue;
                uint16_t manuf_code = (uint16_t)strtol(manuf_ptr + 1, NULL, 16);
                char *payload_ptr = strchr(manuf_ptr + 1, ' ');
                uint8_t payload[64];
                uint16_t len = 0;
                if (payload_ptr) {
                    payload_ptr++;
                    while (len < sizeof(payload)) {
                        unsigned byte;
                        if (sscanf(payload_ptr, "%2x", &byte) != 1) break;
                        payload[len++] = (uint8_t)byte;
                        payload_ptr += 2;
                        while (*payload_ptr == ' ') payload_ptr++;
                    }
                }
                ESP_LOGI(TAG, "Serial command: custom cmd 0x%02x cluster=0x%04x to 0x%04x manuf=0x%04x (%u bytes)",
                         (unsigned)cmd_id, cluster_id, target, manuf_code, (unsigned)len);
                send_custom_command(target, GRADIENT_PROBE_ENDPOINT, cluster_id, cmd_id, manuf_code, payload, len);
            } else if (strncmp(buf, "fc01read ", 9) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 9, NULL, 16);
                char *attr_ptr = strchr(buf + 9, ' ');
                if (!attr_ptr) continue;
                uint16_t attr_id = (uint16_t)strtol(attr_ptr + 1, NULL, 16);
                ESP_LOGI(TAG, "Serial command: FC01 read attr 0x%04x from 0x%04x", attr_id, target);
                send_fc01_read_attr(target, GRADIENT_PROBE_ENDPOINT, attr_id);
            } else if (strncmp(buf, "fc01 ", 5) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 5, NULL, 16);
                ESP_LOGI(TAG, "Serial command: send FC01 cmd3 to 0x%04x", target);
                send_fc01_command(target, GRADIENT_PROBE_ENDPOINT, 0x03,
                                  FC01_CMD3_PAYLOAD, sizeof(FC01_CMD3_PAYLOAD));
            } else if (strncmp(buf, "readattr ", 9) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 9, NULL, 16);
                char *cluster_ptr = strchr(buf + 9, ' ');
                if (!cluster_ptr) continue;
                uint16_t cluster_id = (uint16_t)strtol(cluster_ptr + 1, NULL, 16);
                char *attr_ptr = strchr(cluster_ptr + 1, ' ');
                if (!attr_ptr) continue;
                uint16_t attr_id = (uint16_t)strtol(attr_ptr + 1, NULL, 16);
                char *manuf_ptr = strchr(attr_ptr + 1, ' ');
                uint16_t manuf_code = manuf_ptr ? (uint16_t)strtol(manuf_ptr + 1, NULL, 16) : 0;
                ESP_LOGI(TAG, "Serial command: read attr 0x%04x from cluster 0x%04x on 0x%04x",
                         attr_id, cluster_id, target);
                send_read_attr(target, GRADIENT_PROBE_ENDPOINT, cluster_id, attr_id, manuf_code);
            } else if (strncmp(buf, "discattr ", 9) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 9, NULL, 16);
                char *cluster_ptr = strchr(buf + 9, ' ');
                if (!cluster_ptr) continue;
                uint16_t cluster_id = (uint16_t)strtol(cluster_ptr + 1, NULL, 16);
                char *start_ptr = strchr(cluster_ptr + 1, ' ');
                if (!start_ptr) continue;
                uint16_t start_attr = (uint16_t)strtol(start_ptr + 1, NULL, 16);
                char *max_ptr = strchr(start_ptr + 1, ' ');
                uint8_t max_attrs = max_ptr ? (uint8_t)strtol(max_ptr + 1, NULL, 16) : 3;
                char *manuf_ptr = max_ptr ? strchr(max_ptr + 1, ' ') : NULL;
                uint16_t manuf_code = manuf_ptr ? (uint16_t)strtol(manuf_ptr + 1, NULL, 16) : 0;
                ESP_LOGI(TAG, "Serial command: discover attrs cluster 0x%04x start 0x%04x on 0x%04x",
                         cluster_id, start_attr, target);
                send_discover_attr(target, GRADIENT_PROBE_ENDPOINT, cluster_id, start_attr, max_attrs, manuf_code);
            } else if (strncmp(buf, "discext ", 8) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 8, NULL, 16);
                char *cluster_ptr = strchr(buf + 8, ' ');
                if (!cluster_ptr) continue;
                uint16_t cluster_id = (uint16_t)strtol(cluster_ptr + 1, NULL, 16);
                char *start_ptr = strchr(cluster_ptr + 1, ' ');
                if (!start_ptr) continue;
                uint16_t start_attr = (uint16_t)strtol(start_ptr + 1, NULL, 16);
                char *max_ptr = strchr(start_ptr + 1, ' ');
                uint8_t max_attrs = max_ptr ? (uint8_t)strtol(max_ptr + 1, NULL, 16) : 3;
                char *manuf_ptr = max_ptr ? strchr(max_ptr + 1, ' ') : NULL;
                uint16_t manuf_code = manuf_ptr ? (uint16_t)strtol(manuf_ptr + 1, NULL, 16) : 0;
                char *ep_ptr = manuf_ptr ? strchr(manuf_ptr + 1, ' ') : NULL;
                uint8_t ep = ep_ptr ? (uint8_t)strtol(ep_ptr + 1, NULL, 16) : GRADIENT_PROBE_ENDPOINT;
                ESP_LOGI(TAG, "Serial command: discover attrs EXT cluster 0x%04x start 0x%04x on 0x%04x ep=%u",
                         cluster_id, start_attr, target, (unsigned)ep);
                send_discover_attr_ext(target, ep, cluster_id, start_attr, max_attrs, manuf_code);
            } else if (strncmp(buf, "otasweep ", 9) == 0) {
                uint16_t target = (uint16_t)strtol(buf + 9, NULL, 16);
                char *ep_ptr = strchr(buf + 9, ' ');
                uint8_t ep = GRADIENT_PROBE_ENDPOINT;
                if (ep_ptr) ep = (uint8_t)strtol(ep_ptr + 1, NULL, 16);
                ESP_LOGI(TAG, "Serial command: metadata sweep on 0x%04x ep=%u", target, (unsigned)ep);
                start_metadata_sweep(target, ep);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");

    printf("PROBE: attr_set endpoint=%u cluster=0x%04x attr=0x%04x status=%u size=%u data=",
           (unsigned)message->info.dst_endpoint,
           (unsigned)message->info.cluster,
           (unsigned)message->attribute.id,
           (unsigned)message->info.status,
           (unsigned)message->attribute.data.size);
    if (message->attribute.data.value && message->attribute.data.size) {
        dump_hex("value", message->attribute.data.value, message->attribute.data.size);
    } else {
        printf("(nil)\n");
    }
    return ESP_OK;
}

static esp_err_t zb_custom_cluster_handler(const esp_zb_zcl_custom_cluster_command_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty custom cluster message");

    printf("PROBE: custom_cluster endpoint=%u cluster=0x%04x cmd=0x%02x manuf=0x%04x fc=0x%02x size=%u data=",
           (unsigned)message->info.dst_endpoint,
           (unsigned)message->info.cluster,
           (unsigned)message->info.command.id,
           (unsigned)message->info.header.manuf_code,
           (unsigned)message->info.header.fc,
           (unsigned)message->data.size);
    if (message->data.value && message->data.size) {
        dump_hex("payload", message->data.value, message->data.size);
    } else {
        printf("(nil)\n");
    }
    return ESP_OK;
}

static esp_err_t zb_default_resp_handler(const esp_zb_zcl_cmd_default_resp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty default response message");
    printf("PROBE: default_resp cluster=0x%04x resp_to_cmd=0x%02x status=0x%02x (%u)\n",
           (unsigned)message->info.cluster,
           (unsigned)message->resp_to_cmd,
           (unsigned)message->status_code, (unsigned)message->status_code);
    return ESP_OK;
}

static esp_err_t zb_read_attr_resp_handler(const esp_zb_zcl_cmd_read_attr_resp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty read attr response message");
    printf("PROBE: read_attr_resp cluster=0x%04x src_ep=%u dst_ep=%u\n",
           (unsigned)message->info.cluster,
           (unsigned)message->info.src_endpoint,
           (unsigned)message->info.dst_endpoint);
    for (esp_zb_zcl_read_attr_resp_variable_t *v = message->variables; v; v = v->next) {
        printf("  attr=0x%04x status=0x%02x type=0x%02x size=%u data=",
               (unsigned)v->attribute.id,
               (unsigned)v->status,
               (unsigned)v->attribute.data.type,
               (unsigned)v->attribute.data.size);
        if (v->attribute.data.value && v->attribute.data.size) {
            dump_hex("value", v->attribute.data.value, v->attribute.data.size);
        } else {
            printf("(nil)\n");
        }
    }
    return ESP_OK;
}

static esp_err_t zb_disc_attr_resp_handler(const esp_zb_zcl_cmd_discover_attributes_resp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty discover attr response message");
    printf("PROBE: disc_attr_resp cluster=0x%04x src_ep=%u dst_ep=%u completed=%u\n",
           (unsigned)message->info.cluster,
           (unsigned)message->info.src_endpoint,
           (unsigned)message->info.dst_endpoint,
           (unsigned)message->is_completed);
    for (esp_zb_zcl_disc_attr_variable_t *v = message->variables; v; v = v->next) {
        printf("  attr=0x%04x type=0x%02x\n",
               (unsigned)v->attr_id,
               (unsigned)v->data_type);
    }
    return ESP_OK;
}

static const char *zcl_global_cmd_name(uint8_t cmd_id)
{
    switch (cmd_id) {
    case ZB_ZCL_CMD_READ_ATTRIB:
        return "read_attr";
    case ZB_ZCL_CMD_READ_ATTRIB_RESP:
        return "read_attr_resp";
    case ZB_ZCL_CMD_DISC_ATTRIB:
        return "discover_attr";
    case ZB_ZCL_CMD_DISC_ATTRIB_RESP:
        return "discover_attr_resp";
    case ZB_ZCL_CMD_DISCOVER_ATTR_EXT:
        return "discover_attr_ext";
    case ZB_ZCL_CMD_DISCOVER_ATTR_EXT_RES:
        return "discover_attr_ext_resp";
    default:
        return "other";
    }
}

static void dump_discover_attr_ext_resp(uint16_t cluster_id, const uint8_t *payload, uint16_t payload_len)
{
    if (!payload || payload_len < 1) {
        return;
    }

    printf("PROBE: disc_attr_ext_resp cluster=0x%04x completed=%u raw=",
           (unsigned)cluster_id, (unsigned)payload[0]);
    for (uint16_t i = 0; i < payload_len; i++) {
        printf("%02x", payload[i]);
    }
    printf("\n");

    uint16_t off = 1;
    while (off + 3 < payload_len) {
        uint16_t attr_id = payload[off] | ((uint16_t)payload[off + 1] << 8);
        uint8_t data_type = payload[off + 2];
        uint8_t access = payload[off + 3];
        printf("  ext_attr=0x%04x type=0x%02x access=0x%02x\n",
               (unsigned)attr_id, (unsigned)data_type, (unsigned)access);
        off += 4;
    }
    if (off != payload_len) {
        printf("  trailing_bytes=%u\n", (unsigned)(payload_len - off));
    }
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
        if (hdr->cmd_id == ZB_ZCL_CMD_DISCOVER_ATTR_EXT_RES) {
            dump_discover_attr_ext_resp(hdr->cluster_id, payload, payload_len);
        } else if (payload && payload_len) {
            dump_hex("zcl_raw_payload", payload, payload_len);
        }
    }

    return false;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        ret = zb_read_attr_resp_handler((esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_DISC_ATTR_RESP_CB_ID:
        ret = zb_disc_attr_resp_handler((esp_zb_zcl_cmd_discover_attributes_resp_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID:
    case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_RESP_CB_ID:
        ret = zb_custom_cluster_handler((esp_zb_zcl_custom_cluster_command_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        ret = zb_default_resp_handler((esp_zb_zcl_cmd_default_resp_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void add_hue_proprietary_clusters(esp_zb_cluster_list_t *cluster_list)
{
    /* manuSpecificPhilips2 (0xFC03): the gradient cluster.
     * Attribute 0x0002 is an OCTET_STR blob called "state". */
    esp_zb_attribute_list_t *philips2_attr_list = esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS2_CLUSTER_ID);

    /* Minimal initial state: on, 50% brightness, linear gradient with two colors.
     * This is the raw FC03 attribute 0x0002 format used by real gradient lights:
     *   mode (2 LE) | onOff (1) | brightness (1) | unknown (4) |
     *   length (1) | ncolors<<4 | style (1) | reserved (3) | colors (3*N) |
     *   segments (1) | offset (1)
     * mode = 0x014B (gradient), ncolors = 2, segments = 10.
     */
    /* OCTET_STRING attributes are stored with a leading length byte. */
    static uint8_t s_hue_state[] = {
        0x15,                   /* length of payload (21 bytes) */
        0x4B, 0x01,             /* mode = gradient */
        0x01,                   /* onOff = on */
        0x80,                   /* brightness = 128 */
        0x00, 0x00, 0x00, 0x00, /* unknown / placeholder */
        0x0A,                   /* length of color payload */
        0x20,                   /* color count << 4 (2 colors), style linear */
        0x00, 0x00, 0x00,       /* reserved */
        /* red-ish */
        0xFF, 0x00, 0x00,
        /* blue-ish */
        0x00, 0x00, 0xFF,
        0x0A,                   /* segments */
        0x00,                   /* offset */
    };
    esp_zb_custom_cluster_add_custom_attr(philips2_attr_list, 0x0002,
                                          ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                          s_hue_state);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips2_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* manuSpecificPhilips3 (0xFC01): Signify certification/proprietary cluster.
     * The real gradient device on the network returns:
     *   0x0000 - 8-bit bitmap = 0x0B
     *   0x0001 - 8-bit enum  = 0x00
     */
    esp_zb_attribute_list_t *philips3_attr_list = esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID);
    static uint8_t fc01_attr0 = 0x0B;
    static uint8_t fc01_attr1 = 0x00;
    esp_zb_custom_cluster_add_custom_attr(philips3_attr_list, 0x0000,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &fc01_attr0);
    esp_zb_custom_cluster_add_custom_attr(philips3_attr_list, 0x0001,
                                          ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &fc01_attr1);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips3_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Also add FC01 as a client cluster so the probe can receive TO_CLI
     * responses from real gradient devices (e.g. replies to cmd 4/7). */
    esp_zb_attribute_list_t *philips3_client_attr_list = esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS3_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips3_client_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_attribute_list_t *ota_client_attr_list =
        esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, ota_client_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_attribute_list_t *philips4_attr_list = esp_zb_zcl_attr_list_create(HUE_MANU_SPECIFIC_PHILIPS4_CLUSTER_ID);
    esp_zb_cluster_list_add_custom_cluster(cluster_list, philips4_attr_list,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static void add_gradient_endpoint(esp_zb_ep_list_t *ep_list)
{
    esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = GRADIENT_PROBE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = 0x010D, /* Extended color light (Hue uses this, not 0x0102) */
        .app_device_version = 1,
    };

    esp_zb_ep_list_add_ep(ep_list, esp_zb_color_dimmable_light_clusters_create(&light_cfg),
                          endpoint_config);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = (char *)MANUFACTURER_NAME,
        .model_identifier = (char *)MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, GRADIENT_PROBE_ENDPOINT, &info);

    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, GRADIENT_PROBE_ENDPOINT);

    add_hue_proprietary_clusters(cluster_list);

    /* Hue gradient lights expose the full Color Control cluster, including
     * enhanced hue and color capabilities. The bridge uses ColorCapabilities
     * to decide whether to show the gradient UI. */
    static uint16_t color_capabilities = 0x001F; /* hs, enhanced hue, color loop, xy, ct */
    static uint16_t enhanced_current_hue = 0;
    static uint8_t enhanced_color_mode = 0;
    static uint8_t color_loop_active = 0;
    static uint16_t color_temp_min = 153;
    static uint16_t color_temp_max = 500;

    esp_zb_attribute_list_t *color_attr_list = esp_zb_cluster_list_get_cluster(
        cluster_list, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &enhanced_current_hue);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &enhanced_color_mode);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_ACTIVE_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U8,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_loop_active);
    esp_zb_custom_cluster_add_custom_attr(color_attr_list,
                                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID,
                                          ESP_ZB_ZCL_ATTR_TYPE_U16,
                                          ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                          &color_capabilities);
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
}

/* Spoof a Signify Netherlands B.V. EUI-64 so the Hue bridge treats us as a
 * genuine Hue device. Must be unique on the network. */
static const esp_zb_ieee_addr_t HUE_SPOOFED_LONG_ADDR = {
    0x00, 0x17, 0x88, 0x01, 0x0b, 0xff, 0xfe, 0x03
};

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    ESP_ERROR_CHECK(esp_zb_set_long_address((uint8_t *)HUE_SPOOFED_LONG_ADDR));
    esp_zb_ieee_addr_t long_addr;
    esp_zb_get_long_address(long_addr);
    ESP_LOGI(TAG, "Using spoofed long address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             long_addr[7], long_addr[6], long_addr[5], long_addr[4],
             long_addr[3], long_addr[2], long_addr[1], long_addr[0]);

    /* Set Signify manufacturer code in the node descriptor before starting.
     * The docs say after esp_zb_start(), but that broke discovery; try before. */
    esp_zb_set_node_descriptor_manufacturer_code(HUE_SIGNIFY_MANUFACTURER_CODE);
    ESP_LOGI(TAG, "Node descriptor manufacturer code set to 0x%04x", (unsigned)HUE_SIGNIFY_MANUFACTURER_CODE);

    /* Allow joining Philips Hue networks. */
    esp_zb_enable_joining_to_distributed(true);
    esp_zb_secur_TC_standard_distributed_key_set(HUE_TRUST_CENTER_KEY);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    add_gradient_endpoint(ep_list);

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_raw_command_handler_register(zb_raw_command_handler);
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
    xTaskCreate(serial_input_task, "serial_input", 2048, NULL, 5, NULL);
}
