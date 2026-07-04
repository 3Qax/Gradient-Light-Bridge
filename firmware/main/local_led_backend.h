#pragma once

#include "xy_to_rgb.h"

#include <stddef.h>

#include "esp_err.h"

esp_err_t local_led_backend_init(void);
size_t local_led_backend_pixel_count(void);
esp_err_t local_led_backend_show_solid(rgb_t color);
esp_err_t local_led_backend_show_pixels(const rgb_t *pixels, size_t count);
