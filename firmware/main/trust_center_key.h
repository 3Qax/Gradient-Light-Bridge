#pragma once

// ZLL Commissioning trust centre link key for Philips Hue bridges.
// Publicly documented in the Zigbee/Philips Hue reverse-engineering community,
// e.g. https://peeveeone.com/2016/11/breakout-breakthrough/
// This file is intentionally gitignored so the key stays local to this machine.

static uint8_t HUE_TRUST_CENTER_KEY[16] = {
    0x81, 0x42, 0x86, 0x86, 0x5D, 0xC1, 0xC8, 0xB2,
    0xC8, 0xCB, 0xC5, 0x2E, 0x5D, 0x65, 0xD1, 0xB8
};
