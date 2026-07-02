#include "xy_to_rgb.h"

#include <math.h>

static float gamma_correct(float c)
{
    if (c <= 0.0f) {
        return 0.0f;
    }
    if (c <= 0.0031308f) {
        return 12.92f * c;
    }
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

rgb_t xy_to_rgb(float x, float y, uint8_t bri)
{
    rgb_t result = {0, 0, 0};

    if (bri == 0 || y <= 0.0f || y > 1.0f || x < 0.0f || x > 1.0f) {
        return result;
    }

    float z = 1.0f - x - y;
    float Y = (float)bri / 254.0f;
    float X = (Y / y) * x;
    float Z = (Y / y) * z;

    // sRGB D65 matrix
    float r_lin =  3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g_lin = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float b_lin =  0.0557f * X - 0.2040f * Y + 1.0570f * Z;

    float r = gamma_correct(r_lin);
    float g = gamma_correct(g_lin);
    float b = gamma_correct(b_lin);

    if (r > 1.0f) r = 1.0f;
    if (g > 1.0f) g = 1.0f;
    if (b > 1.0f) b = 1.0f;

    result.r = (uint8_t)(r * 255.0f + 0.5f);
    result.g = (uint8_t)(g * 255.0f + 0.5f);
    result.b = (uint8_t)(b * 255.0f + 0.5f);

    return result;
}
