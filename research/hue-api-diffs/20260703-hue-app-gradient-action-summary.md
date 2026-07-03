# 2026-07-03 Hue App Gradient Action Capture

## Artifacts

- Initial capture before parser fix:
  `research/hue-api-diffs/action-capture-20260703-hue-app-color-change/`
- Validation capture after parser fix:
  `research/hue-api-diffs/action-capture-20260703-hue-app-gradient-parser/`

## What Hue Sends

Once the bridge classifies the fake as a certified LCX004 with v2 gradient
resources, manual Hue app gradient edits are sent to endpoint 11 through the
Signify FC03 cluster:

- cluster: `0xfc03` / `manuSpecificPhilips2`
- manufacturer: `0x100b`
- command id: `0x00`
- command direction: to server
- command kind: cluster-specific, not a global ZCL read

The live Hue app payload shape observed here is:

```text
51 01 01 04 00 13 50 00 00 00 <five 3-byte scaled-XY colors> 28 00
```

This is distinct from the Zigbee2MQTT / Koenkk generated command shape:

```text
50 01 04 00 13 50 00 00 00 <five 3-byte scaled-XY colors> 28 00
```

The scaled-XY color encoding matches Koenkk's `decodeScaledGradientToRGB`
method in `philips.ts`; the new information from this capture is the Hue app's
`0x51` command prefix and one-byte-shifted field offsets.

## Firmware Result

`firmware/main/argb_to_hue.c` now accepts both `0x50` and `0x51` FC03
`multiColor` layouts. The validation capture recorded:

- `15` FC03 command-id-`0x00` packets total;
- `14` complete `0x51` gradient packets with 5 colors;
- `14` parsed `DATA` gradient JSON lines emitted by firmware;
- one short FC03 command payload `1100010400`, retained as unhandled evidence.

First parsed gradient sample:

```json
{"endpoint":11,"gradient":true,"n":5,"segments":5,"offset":0}
```

The final parsed sample and the Hue v2 API after-snapshot agree semantically:
the light is `on=true`, brightness is `100.0`, and the v2 `gradient.points`
contain five distinct XY points.

## Notes

- The same validation window did not show separate standard On/Off (`0x0006`)
  or Level Control (`0x0008`) commands. The before/after API snapshots do show
  the resource changing from `on=false` to `on=true`; capture those paths in a
  longer or more focused action run if standard state handling becomes the next
  target.
- The raw logger now names FC03 cluster-specific command `0x00` as
  `multi_color` and prints `FC03_MULTICOLOR_HEX` instead of mislabeling the
  packet as a global `read_attr`.
