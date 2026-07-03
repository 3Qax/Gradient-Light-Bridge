# Philips Hue Reverse-Engineering References

These notes capture the public references behind the Hue joining and gradient
emulation work in this repo. Do not copy the Hue trust-center link key into
tracked files; keep it only in local `trust_center_key.h` files.

## Joining a Hue Bridge

- Colin O'Flynn, "Getting Root on Philips Hue Bridge 2.0"
  https://colinoflynn.com/2016/07/getting-root-on-philips-hue-bridge-2-0/
  - Useful bridge 2.0 hardware context and UART access notes.
  - Confirms the square bridge is a normal embedded Linux target, but this
    project does not require bridge rooting.

- PeeVeeOne, "Breakout breakthrough"
  https://peeveeone.com/2016/11/breakout-breakthrough/
  - Important distinction: the ZLL master key is for Touchlink, while Hue
    bridge light pairing uses classical commissioning with the Hue/ZLL trust
    center link key.
  - This is the historical source for why the ESP32 firmware must configure a
    Hue-compatible trust-center link key before network steering.

- PeeVeeOne, "Custom firmware Hue lights"
  https://peeveeone.com/2016/11/custom-firmware-hue-lights/
  - Shows a custom Zigbee Light Link endpoint joining Hue as a dimmable or color
    light once flashed with firmware that supports the expected commissioning
    path.
  - Practical behavior matches this project: start "Add light" in the Hue app,
    reset/power-cycle the device, and let the bridge discover it.

## Hue Gradient Emulation

- Colin O'Flynn, "Philips Hue - R.E. Whitepaper from Black Hat 2016"
  https://colinoflynn.com/2016/08/philips-hue-r-e-whitepaper-from-black-hat-2016/
  - Broad background on Hue reverse engineering, update encryption, and Zigbee
    Light Link security assumptions.
  - Useful as security context, not as an implementation recipe for the ESP32
    firmware.

- Hue Gradient command generator
  https://kjagiello.github.io/hue-gradient-command-wizard/
  - Interactive helper for generating Hue gradient commands.
  - Use this as a sanity check for command payload shapes while comparing
    observed `0xFC03` gradient traffic from real Hue gradient products.

## Repo Mapping

- `firmware/main/trust_center_key.h.in` documents where to place the local
  Hue-compatible trust-center link key.
- `firmware/main/argb_to_hue.c` configures network steering and currently
  spoofs a Signify/LCX004 identity to expose Hue gradient UI behavior.
- `firmware/gradient_probe/main/gradient_probe.c` is the exploratory firmware
  for observing real gradient device behavior, especially the Signify
  manufacturer-specific clusters `0xFC01`, `0xFC03`, and `0xFC04`.
- `research/hue_bridge_lights.json` contains Hue bridge API observations from
  the local network, including model identifiers, product IDs, uniqueid suffixes,
  and gradient-capable product records.
