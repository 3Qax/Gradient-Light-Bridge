/*
 * ESP32-C6 IEEE 802.15.4 sniffer probe.
 *
 * This is intentionally separate from the main argb-to-hue firmware. It is a
 * disposable evidence-gathering target for capturing raw frames while a real
 * LCX004 is commissioned to the Hue bridge.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_ieee802154.h"
#include "esp_ieee802154_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define HUE_CHANNEL 25
#define MAX_PRINT_BYTES 160
#define RX_QUEUE_DEPTH 32

static const char *TAG = "SNIFFER_PROBE";

typedef struct {
    int64_t ts_us;
    uint8_t len;
    uint8_t channel;
    int8_t rssi;
    uint8_t lqi;
    uint8_t psdu[MAX_PRINT_BYTES];
} sniffed_frame_t;

static QueueHandle_t s_rx_queue;

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

void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    if (frame && s_rx_queue) {
        sniffed_frame_t item = {
            .ts_us = esp_timer_get_time(),
            .len = frame[0],
            .channel = frame_info ? frame_info->channel : HUE_CHANNEL,
            .rssi = frame_info ? frame_info->rssi : 0,
            .lqi = frame_info ? frame_info->lqi : 0,
        };
        uint8_t copy_len = item.len > MAX_PRINT_BYTES ? MAX_PRINT_BYTES : item.len;
        memcpy(item.psdu, frame + 1, copy_len);

        BaseType_t task_woken = pdFALSE;
        (void)xQueueSendFromISR(s_rx_queue, &item, &task_woken);
        if (task_woken) {
            portYIELD_FROM_ISR();
        }
    }

    if (frame) {
        (void)esp_ieee802154_receive_handle_done(frame);
    }
    (void)esp_ieee802154_receive();
}

void esp_ieee802154_receive_sfd_done(void)
{
}

void esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info)
{
    (void)frame;
    (void)ack;
    (void)ack_frame_info;
}

void esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    (void)frame;
    (void)error;
}

void esp_ieee802154_transmit_sfd_done(uint8_t *frame)
{
    (void)frame;
}

static void sniffer_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(nvs_flash_init());

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(sniffed_frame_t));
    ESP_ERROR_CHECK(s_rx_queue ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_ieee802154_enable());
    ESP_ERROR_CHECK(esp_ieee802154_set_channel(HUE_CHANNEL));
    ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
    ESP_ERROR_CHECK(esp_ieee802154_receive());

    ESP_LOGI(TAG, "direct IEEE 802.15.4 sniffer active on Hue channel %u", HUE_CHANNEL);
    ESP_LOGI(TAG, "commission or rejoin the real LCX004 now");

    sniffed_frame_t item;
    while (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
        printf("MAC_RAW ts_us=%" PRIi64 " len=%u channel=%u rssi=%d lqi=%u psdu=",
               item.ts_us,
               (unsigned)item.len,
               (unsigned)item.channel,
               (int)item.rssi,
               (unsigned)item.lqi);
        print_hex(item.psdu, item.len);
        printf("\n");
    }
}

void app_main(void)
{
    xTaskCreate(sniffer_task, "sniffer_task", 8192, NULL, 5, NULL);
}
