# FC03 Brightness and Dynamic Scene Updates

Observed from Hue app action logs against the ESP fake LCX004 on 2026-07-03.

## Dynamic Scene

Payload:

```text
d30101ea040013500000002f2a86b6db7adc4c70fecd6412e4279a2800
```

Decoded flags:

```text
0x01d3 = on/off + brightness + fade + gradient colors + effect speed + gradient params
```

Important details:

- This is a dynamic scene update, not a named Hue effect selection.
- Brightness byte is `0xea` = 234, matching a Hue app brightness of about 91.8%.
- `effect_speed` was present as `0x9a` = 154. Treat this as dynamic-scene speed metadata.

## Dynamic Start and Stop

Start/autoplay-on payload at speed 12/extreme:

```text
d30101cb04001350000000a7eb54673d55944e6265fd6e26bb84e02820
```

Decoded:

```text
0x01d3 = on/off + brightness + fade + gradient colors + effect speed + gradient params
effect_speed = 0xe0 = 224
offset_raw = 0x20 = 32
```

Stop/autoplay-off payload:

```text
530101cb0400135000000026bb8465fd6e944e62673d55a7eb542800
```

Decoded:

```text
0x0153 = on/off + brightness + fade + gradient colors + gradient params
no effect_speed field
offset_raw = 0x00
```

The stop command is not a separate effect-stop command. It is a normal static
gradient update with no `effect_speed` field. The palette appears in the reverse
order of the start payload. Daemon rule:

- `gradient=true` and `effect_speed` present: start/replace dynamic animation.
- `gradient=true` and `effect_speed` absent: stop dynamic animation and render static gradient.
- FC03 xy color update such as `0x001b`: stop dynamic animation and render solid color.
- FC03 brightness-only update such as `0x0012`: preserve current static/dynamic state and update brightness.
- FC03 on/off update such as `0x0011`: toggle output without discarding the cached static/dynamic scene.

## Dynamic Speed Editing

When dragging the dynamic-scene speed control in the app, the bridge sends
speed-only FC03 command `0x00` payloads:

```text
9000040080
90000400c0
90000400e0
900004007c
90000400d0
```

Decoded:

```text
0x0090 = fade + effect speed
fade = 0x0004
effect_speed = final byte
```

After pressing Save or pressing Play, the bridge also writes the selected speed
into the trailing byte of the compact manufacturer-specific Scenes command
`0x00`:

```text
d5e92a0c1350000000dbfd59866c6387cc6c49bc765c0a82d0
```

Here `d5 e9` is group `0xe9d5`, `2a` is scene id `0x2a`, the compact body
starts with `0c`, the following `13 50 ...` block is the gradient color block,
and the final byte `d0` matches the last speed-only FC03 update.

The command path carries the app's intent:

- Scenes Recall `cmd=0x05`: select/stop scene statically; apply cached scene
  without `effect_speed`.
- Manufacturer-specific Scenes `cmd=0x00`: play/update dynamic scene now; apply
  a live FC03 payload with flags `0x01d0` (`fade + gradient colors + effect
  speed + gradient params`).

Do not persist the dynamic `0x01d0` form as the static recall cache. Cache the
same compact colors without `FC03_FLAG_EFFECT_SPEED`, otherwise pressing Stop in
the app recalls a dynamic cached payload and restarts autoplay.

## Standalone Brightness

Payload shape:

```text
1200fe0400
1200d70400
1200010400
```

Decoded flags:

```text
0x0012 = brightness + fade
```

The third byte is the Hue brightness value, e.g. `0xfe` = 254, `0xd7` = 215, `0x01` = 1. These updates do not include gradient colors or xy color changes. If the daemon is currently rendering a gradient, it should preserve the last gradient points and re-render them at the new brightness instead of falling back to a solid color.

## Relax / Color Temperature Scene

Hue's Relax scene used this FC03 payload:

```text
1700018fbf010400
```

Decoded flags:

```text
0x0017 = on/off + brightness + color mirek + fade
on = 0x01
bri = 0x8f = 143
mirek = 0x01bf = 447
fade = 0x0004
```

Before the 2026-07-03 Mirek fix, firmware consumed the two color-temperature
bytes but did not update `st->x`/`st->y`, so the emitted DATA line reused the
previous xy/rgb. The daemon also treated `0x0017` as a gradient brightness/on
metadata update when a cached gradient existed, because only the xy flag counted
as a color change.

Post-fix serial self-test:

```text
g 1700018fbf010400
FC03 update: flags=0x0017 on=true bri=set mirek=set xy=- fade=set gradient=0 effect=-1 speed=-1 params=- scale=0 offset=0
DATA: {"endpoint":11,"source":"fc03","fc03_flags":23,"fade":4,"on":true,"bri":143,"x":0.5016,"y":0.4153,"r":255,"g":170,"b":58}
```

Daemon rule: FC03 `COLOR_MIREK` (`0x0004`) is a real color change, same as
`COLOR_XY` (`0x0008`) for clearing cached gradient/dynamic state and rendering a
solid color.
