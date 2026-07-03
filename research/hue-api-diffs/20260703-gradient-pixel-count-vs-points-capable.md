# 2026-07-03 Gradient Pixel Count vs Points Capable

## Conclusion

For Hue v2 gradient resources, the FC03 gradient color array length tracks the
number of gradient points in the command, not `gradient.pixel_count`.

On the current fake LCX004, the bridge reports `pixel_count=10` and
`points_capable=5`, while both the v2 `gradient.points` array and the FC03
payload contain 5 color points.

## Current Fake Device

Live v1 filter command:

```bash
tools/hue_light_cycle.py list --modelid LCX004 --manufacturer 'Signify Netherlands B.V.'
```

Relevant current fake:

```text
59  00:17:88:01:0b:ff:fe:05-0b  Signify Netherlands B.V.  LCX004  certified=True  Hue gradient lightstrip 2
```

Live v2 `/clip/v2/resource/light` excerpt for `/lights/59`:

```json
{
  "id_v1": "/lights/59",
  "name": "Hue gradient lightstrip 2",
  "gradient": {
    "mode": "interpolated_palette",
    "mode_values": [
      "interpolated_palette",
      "interpolated_palette_mirrored",
      "random_pixelated",
      "segmented_palette"
    ],
    "pixel_count": 10,
    "points_capable": 5,
    "points": [
      {"color": {"xy": {"x": 0.6705, "y": 0.3199}}},
      {"color": {"xy": {"x": 0.3626, "y": 0.2755}}},
      {"color": {"xy": {"x": 0.2953, "y": 0.2808}}},
      {"color": {"xy": {"x": 0.237, "y": 0.1653}}},
      {"color": {"xy": {"x": 0.1534, "y": 0.0544}}}
    ]
  }
}
```

This gives:

- `pixel_count = 10`
- `points_capable = 5`
- `gradient.points.length = 5`

## FC03 Payload Evidence

When the Hue app was switched to 3 UI control points, the bridge still sent a
5-point FC03 command:

```text
FC03_MULTICOLOR_HEX: 51010104001350000000992e61e5a7536e46552935325783102800
```

Decoded field evidence:

```text
51 01              flags = 0x0151
01                 on = true
04 00              fade = 4
13                 gradient color block length = 19 = 4 + 3*5
50                 color count nibble = 5 << 4
00                 style = linear
00 00              reserved
99 2e 61 ...       five packed XY color points, 3 bytes each
28 00              gradient params: scale 5, offset 0
```

The command itself therefore says `n=5`. If the payload color array length were
`pixel_count=10`, the gradient color block would need a different length and
count byte (`4 + 3*10 = 34`, count byte `0xA0`). The observed payload is
unambiguously 5 color points.

## Cross-Device Counterexamples

The same live v2 query showed other gradient-capable Hue lights where
`pixel_count` differs from the number of v2 points:

```text
Play gradient tube:     pixel_count=6,   points_capable=5
Hue gradient lightstrip: pixel_count=10,  points_capable=5
Signe/Chair lamp:       pixel_count=4,   points_capable=5, points.length=5
Festavia string lights: pixel_count=500, points_capable=5, points.length=5
```

The `pixel_count=4` and `pixel_count=500` cases are decisive counterexamples to
the theory that Hue v2 `gradient.points.length` or FC03 color count equals
`pixel_count`.

## Practical Rule

For rendering on our side:

- Use FC03 `n` / v2 `gradient.points.length` as the number of received gradient
  color points.
- Treat `gradient.pixel_count` as Hue's product/resource pixel model, not as the
  number of colors in a Zigbee FC03 update.
- Treat `points_capable` as the bridge/app limit for gradient points on this
  product class.
- Map the received gradient points onto the physical OpenRGB LED count in the
  daemon.

