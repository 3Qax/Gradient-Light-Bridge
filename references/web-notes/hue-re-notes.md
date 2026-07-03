# Hue Reverse-Engineering Notes

## Pairing And Commissioning

- Treat Hue bridge pairing for this project as classical Zigbee commissioning.
- Touchlink research is still useful background, but it is not the path the
  ESP32 firmware should rely on for normal Hue app "Add light" discovery.
- Do not commit Hue trust-center key material. Public sources that publish key
  bytes are linked from `references/README.md` but intentionally not mirrored.

## Bridge Internals

- Colin O'Flynn's bridge 2.0 work shows how to get local root access and gives
  the starting point for bridge binary analysis.
- Check Point and Synacktiv both identify `ipbridge` as the central bridge
  process that handles higher-level Zigbee/ZDP/ZCL messages after the radio
  controller has parsed lower layers.
- If we need the exact Hue API `capabilities.certified` decision, `ipbridge`
  discovery/product classification logic is the most plausible target.

## Gradient And Manufacturer-Specific Clusters

- `0xFC03` is the main cluster for Hue gradient/effects updates.
- Bifrost's `hue-zigbee-format.md` is the strongest public reference found for
  `0xFC03` payload structure:
  - 16-bit little-endian flags header.
  - Field order: on/off, brightness, color temperature, XY color, fade speed,
    effect type, gradient colors, effect speed, gradient parameters.
  - Gradient colors use packed 12-bit XY values.
  - Gradient styles include linear, scattered, and mirrored.
  - Gradient params encode scale and offset in fixed-point units.
- `0xFC01` still matters because Hue bridges have been observed sending
  manufacturer-specific command `0x03` there after device announcement.

## Certified Flag Working Hypothesis

- The public RE material does not provide a complete recipe for making the Hue
  API report `capabilities.certified: true`.
- Current hypothesis: the bridge derives it from product identity and bridge-side
  product records, probably involving some combination of manufacturer name,
  model identifier, product ID, software/config IDs, endpoint layout, and
  manufacturer-specific cluster behavior.
- Spoofing a real Signify model can unlock product-specific behavior, but may
  not be sufficient for the API-certified flag if the bridge expects additional
  product metadata or responses.

## Implementation Implications

- Use real Hue bridge API dumps in `research/hue_bridge_lights.json` to choose
  model/product identities.
- Use the gradient probe firmware to compare real device behavior against the
  ESP32 implementation for:
  - Basic cluster attributes.
  - endpoint/device IDs.
  - `0xFC01` command responses.
  - `0xFC03` attribute and command formats.
  - OTA cluster identity fields, if the bridge uses them for product mapping.
