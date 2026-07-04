# Power-On Startup Settings

Captured from Hue app "Power On" fixture settings on 2026-07-04.

The bridge encodes the four app modes as standard ZCL startup attributes plus
two manufacturer-specific Color Control XY attributes:

| App setting | On/Off `0x0006/0x4003` | Level `0x0008/0x4000` | Color temp `0x0300/0x4010` | Mfg Color X/Y `0x0300/0x0003,0x0004` |
| --- | --- | --- | --- | --- |
| Recovery | `0xff` | `0xff` | `0xffff` | `0xffff`, `0xffff` |
| Last On | `0x01` | `0xff` | `0xffff` | `0xffff`, `0xffff` |
| Default | `0x01` | `0xfe` | `0x016f` | `0xffff`, `0xffff` |
| Custom | `0x01` | `0xfe` | `0x016f` | explicit saved X/Y |

Observed write payload examples:

```text
Recovery:
0006: 034030ff
0008: 004020ff
0300: 104021ffff
0300 mfg 100b: 030021ffff, 040021ffff

Last On:
0006: 03403001
0008: 004020ff
0300: 104021ffff
0300 mfg 100b: 030021ffff, 040021ffff

Default:
0006: 03403001
0008: 004020fe
0300: 1040216f01
0300 mfg 100b: 030021ffff, 040021ffff

Custom example:
0006: 03403001
0008: 004020fe
0300: 1040216f01
0300 mfg 100b: 0300213f5a, 0400218f75
```

Implementation rule:

- Persist both current scalar lamp state and these startup attributes in NVS.
- Treat `0xff` and `0xffff` startup values as "previous".
- On boot, explicit startup XY wins over startup color temperature.
- App-side custom color previews arrive as normal FC03 state updates; the
  startup attribute writes are the durable power-recovery configuration.
- Hue app solid-color changes for this gradient-capable endpoint can arrive as
  FC03 gradient payloads with repeated points. Recovery must therefore persist
  the representative FC03 gradient color, not only explicit scalar XY fields.
