#include "local_led_backend.h"

#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ARGB_COLOR_ORDER_RGB 0
#define ARGB_COLOR_ORDER_GRB 1
#define ARGB_COLOR_ORDER_BRG 2
#define ARGB_COLOR_ORDER_RBG 3
#define ARGB_COLOR_ORDER_GBR 4
#define ARGB_COLOR_ORDER_BGR 5

#ifndef ARGB_LED_GPIO
#define ARGB_LED_GPIO -1
#endif

#ifndef ARGB_LED_COUNT
#define ARGB_LED_COUNT 12
#endif

#ifndef ARGB_COLOR_ORDER
#define ARGB_COLOR_ORDER ARGB_COLOR_ORDER_GRB
#endif

#ifndef ARGB_COLOR_CORRECTION_ENABLED
#define ARGB_COLOR_CORRECTION_ENABLED 1
#endif

#ifndef ARGB_COLOR_CORRECTION_R
#define ARGB_COLOR_CORRECTION_R 255
#endif
#ifndef ARGB_COLOR_CORRECTION_G
#define ARGB_COLOR_CORRECTION_G 176
#endif
#ifndef ARGB_COLOR_CORRECTION_B
#define ARGB_COLOR_CORRECTION_B 240
#endif

#ifndef ARGB_COLOR_TEMPERATURE_R
#define ARGB_COLOR_TEMPERATURE_R 255
#endif
#ifndef ARGB_COLOR_TEMPERATURE_G
#define ARGB_COLOR_TEMPERATURE_G 147
#endif
#ifndef ARGB_COLOR_TEMPERATURE_B
#define ARGB_COLOR_TEMPERATURE_B 41
#endif

#ifndef ARGB_COLOR_GAIN_R
#define ARGB_COLOR_GAIN_R 1.0
#endif
#ifndef ARGB_COLOR_GAIN_G
#define ARGB_COLOR_GAIN_G 1.0
#endif
#ifndef ARGB_COLOR_GAIN_B
#define ARGB_COLOR_GAIN_B 0.7
#endif

#ifndef ARGB_COLOR_GAMMA_R
#define ARGB_COLOR_GAMMA_R 1.0
#endif
#ifndef ARGB_COLOR_GAMMA_G
#define ARGB_COLOR_GAMMA_G 1.0
#endif
#ifndef ARGB_COLOR_GAMMA_B
#define ARGB_COLOR_GAMMA_B 1.0
#endif

#ifndef ARGB_LED_RESOLUTION_HZ
#define ARGB_LED_RESOLUTION_HZ 10000000
#endif

#if ARGB_LED_GPIO < 0
#error ARGB_BACKEND_LOCAL_LED requires ARGB_LED_GPIO
#endif

#if ARGB_LED_COUNT < 1
#error ARGB_BACKEND_LOCAL_LED requires ARGB_LED_COUNT >= 1
#endif

static const char *TAG = "ARGB_LOCAL_LED";

static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static SemaphoreHandle_t s_led_lock;
static bool s_ready;
static uint8_t s_led_bytes[ARGB_LED_COUNT * 3];

#define RMT_TICKS_FROM_NS(ns) ((uint32_t)(((uint64_t)ARGB_LED_RESOLUTION_HZ * (ns)) / 1000000000ULL))
#define RMT_TICKS_FROM_US(us) ((uint32_t)(((uint64_t)ARGB_LED_RESOLUTION_HZ * (us)) / 1000000ULL))

static const rmt_symbol_word_t WS2812_ZERO = {
    .level0 = 1,
    .duration0 = RMT_TICKS_FROM_NS(300),
    .level1 = 0,
    .duration1 = RMT_TICKS_FROM_NS(900),
};

static const rmt_symbol_word_t WS2812_ONE = {
    .level0 = 1,
    .duration0 = RMT_TICKS_FROM_NS(900),
    .level1 = 0,
    .duration1 = RMT_TICKS_FROM_NS(300),
};

static const rmt_symbol_word_t WS2812_RESET = {
    .level0 = 0,
    .duration0 = RMT_TICKS_FROM_US(25),
    .level1 = 0,
    .duration1 = RMT_TICKS_FROM_US(25),
};

static const char *color_order_name(void)
{
#if ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_RGB
    return "RGB";
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_GRB
    return "GRB";
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_BRG
    return "BRG";
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_RBG
    return "RBG";
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_GBR
    return "GBR";
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_BGR
    return "BGR";
#else
    return "unknown";
#endif
}

#if ARGB_COLOR_CORRECTION_ENABLED
static uint8_t corrected_channel(uint8_t channel, uint8_t correction, uint8_t temperature, float gain, float gamma)
{
    float value = (float)channel / 255.0f;
    if (gamma != 1.0f) {
        value = powf(value, gamma);
    }
    value *= ((float)correction / 255.0f) * ((float)temperature / 255.0f) * gain;
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return (uint8_t)(value * 255.0f + 0.5f);
}

static rgb_t apply_color_correction(rgb_t color)
{
    rgb_t corrected = {
        .r = corrected_channel(color.r,
                               ARGB_COLOR_CORRECTION_R,
                               ARGB_COLOR_TEMPERATURE_R,
                               (float)ARGB_COLOR_GAIN_R,
                               (float)ARGB_COLOR_GAMMA_R),
        .g = corrected_channel(color.g,
                               ARGB_COLOR_CORRECTION_G,
                               ARGB_COLOR_TEMPERATURE_G,
                               (float)ARGB_COLOR_GAIN_G,
                               (float)ARGB_COLOR_GAMMA_G),
        .b = corrected_channel(color.b,
                               ARGB_COLOR_CORRECTION_B,
                               ARGB_COLOR_TEMPERATURE_B,
                               (float)ARGB_COLOR_GAIN_B,
                               (float)ARGB_COLOR_GAMMA_B),
    };
    return corrected;
}
#else
static rgb_t apply_color_correction(rgb_t color)
{
    return color;
}
#endif

static void encode_color(size_t led, rgb_t color)
{
    color = apply_color_correction(color);

    uint8_t *out = &s_led_bytes[led * 3];
#if ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_RGB
    out[0] = color.r;
    out[1] = color.g;
    out[2] = color.b;
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_GRB
    out[0] = color.g;
    out[1] = color.r;
    out[2] = color.b;
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_BRG
    out[0] = color.b;
    out[1] = color.r;
    out[2] = color.g;
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_RBG
    out[0] = color.r;
    out[1] = color.b;
    out[2] = color.g;
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_GBR
    out[0] = color.g;
    out[1] = color.b;
    out[2] = color.r;
#elif ARGB_COLOR_ORDER == ARGB_COLOR_ORDER_BGR
    out[0] = color.b;
    out[1] = color.g;
    out[2] = color.r;
#else
    out[0] = color.g;
    out[1] = color.r;
    out[2] = color.b;
#endif
}

static size_t ws2812_encoder_callback(const void *data,
                                      size_t data_size,
                                      size_t symbols_written,
                                      size_t symbols_free,
                                      rmt_symbol_word_t *symbols,
                                      bool *done,
                                      void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (uint8_t bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & bitmask) ? WS2812_ONE : WS2812_ZERO;
        }
        return symbol_pos;
    }

    symbols[0] = WS2812_RESET;
    *done = true;
    return 1;
}

static esp_err_t flush_pixels(void)
{
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_channel,
                                     s_led_encoder,
                                     s_led_bytes,
                                     sizeof(s_led_bytes),
                                     &tx_config),
                        TAG,
                        "RMT transmit failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_led_channel, 20), TAG, "RMT transmit timeout");
    return ESP_OK;
}

esp_err_t local_led_backend_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "Initializing local LED backend: gpio=%d pixels=%d order=%s",
             ARGB_LED_GPIO,
             ARGB_LED_COUNT,
             color_order_name());
#if ARGB_COLOR_CORRECTION_ENABLED
    ESP_LOGI(TAG,
             "Local LED color correction: correction=%u/%u/%u temperature=%u/%u/%u gain=%.3f/%.3f/%.3f gamma=%.3f/%.3f/%.3f",
             (unsigned)ARGB_COLOR_CORRECTION_R,
             (unsigned)ARGB_COLOR_CORRECTION_G,
             (unsigned)ARGB_COLOR_CORRECTION_B,
             (unsigned)ARGB_COLOR_TEMPERATURE_R,
             (unsigned)ARGB_COLOR_TEMPERATURE_G,
             (unsigned)ARGB_COLOR_TEMPERATURE_B,
             (double)ARGB_COLOR_GAIN_R,
             (double)ARGB_COLOR_GAIN_G,
             (double)ARGB_COLOR_GAIN_B,
             (double)ARGB_COLOR_GAMMA_R,
             (double)ARGB_COLOR_GAMMA_G,
             (double)ARGB_COLOR_GAMMA_B);
#else
    ESP_LOGI(TAG, "Local LED color correction disabled");
#endif

    s_led_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_led_lock, ESP_ERR_NO_MEM, TAG, "create LED mutex failed");

    rmt_tx_channel_config_t tx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = ARGB_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = ARGB_LED_RESOLUTION_HZ,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_config, &s_led_channel),
                        TAG,
                        "create RMT TX channel failed");

    const rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encoder_callback,
        .min_chunk_size = 8,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&encoder_config, &s_led_encoder),
                        TAG,
                        "create RMT encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "enable RMT TX channel failed");

    s_ready = true;
    memset(s_led_bytes, 0, sizeof(s_led_bytes));
    return flush_pixels();
}

size_t local_led_backend_pixel_count(void)
{
    return ARGB_LED_COUNT;
}

esp_err_t local_led_backend_show_solid(rgb_t color)
{
    ESP_RETURN_ON_ERROR(local_led_backend_init(), TAG, "local LED backend init failed");

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    for (size_t i = 0; i < ARGB_LED_COUNT; i++) {
        encode_color(i, color);
    }
    esp_err_t err = flush_pixels();
    xSemaphoreGive(s_led_lock);
    return err;
}

esp_err_t local_led_backend_show_pixels(const rgb_t *pixels, size_t count)
{
    if (pixels == NULL && count > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(local_led_backend_init(), TAG, "local LED backend init failed");

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    rgb_t off = {0, 0, 0};
    for (size_t i = 0; i < ARGB_LED_COUNT; i++) {
        encode_color(i, i < count ? pixels[i] : off);
    }
    esp_err_t err = flush_pixels();
    xSemaphoreGive(s_led_lock);
    return err;
}
