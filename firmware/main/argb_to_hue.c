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

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl_utility.h"

#if !defined CONFIG_ZB_ZCZR
#error Define ZB_ZCZR in idf.py menuconfig to compile light (Router) source code.
#endif

static const char *TAG = "ARGB_TO_HUE";

/* Zigbee strings are octet strings: first byte is the length.
 * The Hue bridge reads the Basic cluster manufacturer/model and is picky
 * about the format, so these literals include the length prefix. */
static const char *ENDPOINT_NAMES[ARGB_ENDPOINT_COUNT] = {
    "\x07" "PC Case",
    "\x07" "PC Fans",
    "\x06" "PC RAM",
    "\x0e" "PC Motherboard",
};

light_state_t g_light_state[ARGB_ENDPOINT_COUNT] = {0};

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
    uint8_t bri = st->on ? st->bri : 0;

    rgb_t rgb = xy_to_rgb(xf, yf, bri);

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

static esp_err_t deferred_driver_init(void)
{
    return ESP_OK;
}

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
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

static void refresh_state_from_cluster(uint8_t endpoint)
{
    uint8_t idx = endpoint - HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE;
    if (idx >= ARGB_ENDPOINT_COUNT) {
        return;
    }

    light_state_t *st = &g_light_state[idx];

    const esp_zb_zcl_attr_t *on_off_attr = esp_zb_zcl_get_attribute(
        endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (on_off_attr && on_off_attr->data_p) {
        st->on = *(bool *)on_off_attr->data_p;
    }

    const esp_zb_zcl_attr_t *level_attr = esp_zb_zcl_get_attribute(
        endpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
    if (level_attr && level_attr->data_p) {
        st->bri = *(uint8_t *)level_attr->data_p;
    }

    const esp_zb_zcl_attr_t *x_attr = esp_zb_zcl_get_attribute(
        endpoint, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID);
    if (x_attr && x_attr->data_p) {
        st->x = *(uint16_t *)x_attr->data_p;
    }

    const esp_zb_zcl_attr_t *y_attr = esp_zb_zcl_get_attribute(
        endpoint, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID);
    if (y_attr && y_attr->data_p) {
        st->y = *(uint16_t *)y_attr->data_p;
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

    refresh_state_from_cluster(endpoint);

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

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void add_color_dimmable_light_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint,
                                              const char *model_name)
{
    esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = 0x0102,
        .app_device_version = 1,
    };

    esp_zb_ep_list_add_ep(ep_list, esp_zb_color_dimmable_light_clusters_create(&light_cfg),
                          endpoint_config);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = (char *)"\x0a" "argb-to-hue",
        .model_identifier = (char *)model_name,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, endpoint, &info);

    // Fix "Off with effect" command from Hue bridge.
    // https://github.com/espressif/esp-zigbee-sdk/issues/457#issuecomment-2426128314
    uint16_t on_off_on_time = 0;
    bool on_off_global_scene_control = false;
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, endpoint);
    esp_zb_attribute_list_t *onoff_attr_list = esp_zb_cluster_list_get_cluster(
        cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME,
                                   &on_off_on_time);
    esp_zb_on_off_cluster_add_attr(onoff_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL,
                                   &on_off_global_scene_control);
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    // Allow joining Philips Hue networks.
    esp_zb_enable_joining_to_distributed(true);
    esp_zb_secur_TC_standard_distributed_key_set(HUE_TRUST_CENTER_KEY);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    for (uint8_t i = 0; i < ARGB_ENDPOINT_COUNT; i++) {
        uint8_t endpoint = HA_COLOR_DIMMABLE_LIGHT_ENDPOINT_BASE + i;
        add_color_dimmable_light_endpoint(ep_list, endpoint, ENDPOINT_NAMES[i]);
    }

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
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
}
