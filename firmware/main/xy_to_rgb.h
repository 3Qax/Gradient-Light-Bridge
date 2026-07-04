#pragma once

#include <stdint.h>

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

/**
 * @brief Convert CIE xy + brightness (0..254) to sRGB.
 *
 * @param x CIE 1931 x coordinate (0..1)
 * @param y CIE 1931 y coordinate (0..1)
 * @param bri Brightness level (0..254)
 * @return rgb_t 8-bit sRGB value.
 */
rgb_t xy_to_rgb(float x, float y, uint8_t bri);
