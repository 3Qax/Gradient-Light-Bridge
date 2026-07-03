/*
 * ESP32-C6 ZBOSS sniffer probe.
 *
 * This is intentionally separate from the main argb-to-hue firmware. It is a
 * disposable evidence-gathering target for capturing raw frames while a real
 * LCX004 is commissioned to the Hue bridge.
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"
#include "zboss_api.h"
#include "zboss_api_buf.h"
#include "zb_ringbuffer.h"
#include "zb_osif.h"

#define HUE_CHANNEL 25
#define SNIFFER_IO_BUFFER_SIZE 4096
#define MAX_PRINT_BYTES 160

static const char *TAG = "SNIFFER_PROBE";

ZB_RING_BUFFER_DECLARE(sniffer_io_buffer, zb_uint8_t, SNIFFER_IO_BUFFER_SIZE);
static sniffer_io_buffer_t s_sniffer_io_buf;

static void print_hex(const uint8_t *data, uint16_t len)
{
    uint16_t n = len > MAX_PRINT_BYTES ? MAX_PRINT_BYTES : len;
    for (uint16_t i = 0; i < n; i++) {
        printf("%02x", data[i]);
    }
    if (len > n) {
        printf("...(+%u)", (unsigned)(len - n));
    }
}

static bool mac_raw_frame_cb(const uint8_t *frame, const esp_ieee802154_frame_info_t *info)
{
    if (!frame) {
        return false;
    }

    uint8_t psdu_len = frame[0];
    printf("MAC_RAW ts_us=%" PRIi64 " len=%u", esp_timer_get_time(), (unsigned)psdu_len);
    if (info) {
        printf(" channel=%u rssi=%d lqi=%u",
               (unsigned)info->channel,
               (int)info->rssi,
               (unsigned)info->lqi);
    }
    printf(" psdu=");
    print_hex(frame + 1, psdu_len);
    printf("\n");

    return false;
}

static void sniffer_data_ind_cb(zb_uint8_t param)
{
    zb_bufid_t buf = param;
    zb_uint8_t *payload = zb_buf_begin(buf);
    zb_uint16_t len = zb_buf_len(buf);

    printf("ZBOSS_SNIFF ts_us=%" PRIi64 " buf=%u len=%u payload=",
           esp_timer_get_time(), (unsigned)param, (unsigned)len);
    if (payload && len) {
        print_hex(payload, len);
    }
    printf("\n");

    zb_buf_free(buf);
}

static void sniffer_task(void *arg)
{
    (void)arg;

    esp_zb_platform_config_t config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_ERROR_CHECK(esp_zb_mac_raw_frame_handler_register(mac_raw_frame_cb));

    ZB_RING_BUFFER_INIT(&s_sniffer_io_buf);
    zb_osif_set_user_io_buffer((zb_byte_array_t *)&s_sniffer_io_buf, SNIFFER_IO_BUFFER_SIZE);

    esp_zb_set_primary_network_channel_set(1l << HUE_CHANNEL);
    ESP_LOGI(TAG, "starting ZBOSS sniffer mode on Hue channel %u", HUE_CHANNEL);
    zb_ret_t ret = zboss_start_in_sniffer_mode();
    if (ret != RET_OK) {
        ESP_LOGE(TAG, "zboss_start_in_sniffer_mode failed: %d", (int)ret);
        vTaskDelete(NULL);
        return;
    }

    zboss_sniffer_start(sniffer_data_ind_cb);
    ESP_LOGI(TAG, "sniffer active; commission or rejoin the real LCX004 now");

    esp_zb_stack_main_loop();
}

void app_main(void)
{
    xTaskCreate(sniffer_task, "sniffer_task", 8192, NULL, 5, NULL);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    (void)signal_struct;
}
