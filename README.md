# Gradient Light Bridge

ESP32-C6 firmware that emulates Hue-compatible Zigbee gradient lights and drives OpenRGB or addressable LEDs. Control your RGB accessories as Hue-compatible gradient lights. Up to 5 light endpoints per ESP.

[![Youtube Video](https://github.com/user-attachments/assets/e8d90265-b60f-47eb-bfa0-9ac36bf647f7)](https://youtu.be/fr-RntL9Mu8?si=YywJU7S6XEgSeV5y)

## Hardware

- [M5Stack Nano C6](https://docs.m5stack.com/en/core/M5NanoC6) or another ESP32-C6 dev board.
- Philips Hue Bridge 2.0, running 1.77.1977138000. Other might work but are untested, please report your finding.

## Architecture

### Output backends

Gradient Light Bridge has two output backends: the [**OpenRGB backend**](#openrgb-backend) (`ARGB_BACKEND=ARGB_BACKEND_SERIAL_JSON`) and the [**addressable LED backend**](#addressable-led-backend) (`ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED`). The backend is selected at firmware build time with `ARGB_BACKEND`.

```
                    Hue app
                       │
                       │ Hue app control
                       ▼
                 Hue Bridge 2.0
                       │
                       │ Zigbee 3.0 / ZCL
                       ▼
                   ESP32-C6
                       │
         ┌─────────────┴──────────────┐
         │                           │
         │ USB-CDC JSON lines        │ GPIO/RMT data signal
         ▼                           ▼
   PC daemon                    5 V ARGB fans/strips
         │
         │ OpenRGB SDK
         ▼
   OpenRGB server
         │
         │ OpenRGB device control
         ▼
  Motherboard / RAM /
  case RGB controllers
```

### OpenRGB backend

`ARGB_BACKEND=ARGB_BACKEND_SERIAL_JSON`

In this mode the ESP32-C6 only behaves as the Zigbee device. It emits one JSON
line for each light update over USB serial. The PC daemon reads those events and
renders them to OpenRGB zones through the OpenRGB SDK.

```text
Hue app -> Hue Bridge -> Zigbee -> ESP32-C6 -> USB serial JSON -> daemon -> OpenRGB
```

Use this backend when the LEDs are already connected to a motherboard, RAM,
case controller, or any other device supported by OpenRGB.

Major firmware options:

- `ARGB_BACKEND=ARGB_BACKEND_SERIAL_JSON`: select the serial JSON backend.
- `ARGB_EUI_SUFFIX=auto`: derive the Zigbee EUI suffix from the ESP factory MAC.
- `ARGB_ENDPOINT_COUNT=<n>`: expose `n` logical gradient lights from one ESP.
- `ARGB_SERIAL_DEBUG=1`: print additional Zigbee/ZCL debug logs.

The mapping from a logical Zigbee endpoint to OpenRGB pixels is configured in
`daemon/config.yaml`, not in firmware. Each endpoint can drive one or more
OpenRGB slices.

Example daemon mapping:

```yaml
inputs:
  pc_fans:
    port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_xxx-if00
    endpoints:
      11:
        name: "Side"
        device: "MSI MYSTIC LIGHT"
        zone: 3
        segment_start: 0
        leds: 6
      12:
        name: "Bottom"
        device: "MSI MYSTIC LIGHT"
        zone: 1
        segment_start: 0
        leds: 6
      13:
        name: "VRM"
        device: "MSI MYSTIC LIGHT"
        zone: 2
        segment_start: 0
        leds: 12
      14:
        - name: "Top Right"
          device: "MSI MYSTIC LIGHT"
          zone: 2
          segment_start: 12
          leds: 12
        - name: "Top Left"
          device: "MSI MYSTIC LIGHT"
          zone: 2
          segment_start: 36
          leds: 12
```

In this example Hue sees endpoint `14` as one gradient light, but the daemon
renders the same light state to two OpenRGB pixel slices.

### Addressable LED backend

`ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED`

In this mode the ESP32-C6 behaves as the Zigbee device and directly drives a
5 V 3-pin addressable LED data line using the ESP RMT peripheral.

```text
Hue app -> Hue Bridge -> Zigbee -> ESP32-C6 -> GPIO data pin -> addressable LEDs
```

Use this backend when the ESP is wired directly to WS2812/SK6812-style fans or
strips.

Major firmware options:

- `ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED`: select the direct LED backend.
- `ARGB_EUI_SUFFIX=auto`: derive the Zigbee EUI suffix from the ESP factory MAC.
- `ARGB_LED_GPIO=<gpio>`: ESP GPIO connected to the LED data input.
- `ARGB_LED_COUNT=<n>`: total number of physical LEDs on the data line.
- `ARGB_ENDPOINT_COUNT=<n>`: number of logical gradient lights exposed to Hue.
- `ARGB_ENDPOINT_LED_COUNT=<n>`: number of physical LEDs assigned to each endpoint.
- `ARGB_COLOR_ORDER=GRB`: channel order for the LED protocol.
- `ARGB_COLOR_CORRECTION_ENABLED=0|1`: enable FastLED-style correction.
- `ARGB_COLOR_CORRECTION=<preset>`: correction preset, for example `TypicalLEDStrip`.
- `ARGB_COLOR_TEMPERATURE=<preset>`: temperature preset, for example `Candle`.
- `ARGB_COLOR_GAIN_R/G/B=<float>`: per-channel gain.
- `ARGB_COLOR_GAMMA_R/G/B=<float>`: per-channel gamma.

Endpoint-to-pixel mapping is contiguous. Hue sees each endpoint as a separate
gradient light; the firmware maps that endpoint to a slice of the LED chain.

```text
endpoint 11 -> pixels 0..11
endpoint 12 -> pixels 12..23
endpoint 13 -> pixels 24..35
endpoint 14 -> pixels 36..47
endpoint 15 -> pixels 48..59
```

For five 12-pixel fans on one 60-pixel LED chain:

```bash
cd firmware
ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED \
ARGB_EUI_SUFFIX=auto \
ARGB_LED_GPIO=2 \
ARGB_ENDPOINT_COUNT=5 \
ARGB_ENDPOINT_LED_COUNT=12 \
ARGB_LED_COUNT=60 \
ARGB_COLOR_ORDER=GRB \
./in-docker.sh idf.py build
```

With that build, the Hue bridge should discover five gradient lights from one
ESP32-C6 Zigbee node. Each Hue light controls one 12-pixel slice of the chain.

Use `ARGB_LED_GPIO=1` for Grove `G1` or `ARGB_LED_GPIO=2` for Grove `G2` on an
M5Stack Nano C6. Power the LEDs from an appropriate 5 V supply, share ground
with the ESP32-C6, and connect only the ESP data pin to the LED data input.

## Hue Trust Center Link Key! Important!

To join a Philips Hue bridge, the ESP32 must present the Hue Zigbee trust-center link key. This key is widely documented in the Hue reverse-engineering community but is not included in this repository for legal reasons.

Background references for Hue commissioning and gradient behavior are collected
in [`research/hue-references.md`](research/hue-references.md).

1. Copy the placeholder header:

   ```bash
   cp firmware/main/trust_center_key.h.in firmware/main/trust_center_key.h
   ```

2. Replace the placeholder bytes in `firmware/main/trust_center_key.h` with the real 16-byte Hue trust center link key.

   The key is used by projects such as [`esp32-huello-world`](https://github.com/wejn/esp32-huello-world) and [`e32wamb`](https://github.com/wejn/e32wamb).

## Color correction

As you're about to find out RGB accessories are rarely color accurate. Different LED chips,
diffusers, controller firmware, and channel ordering can make the same RGB value
look too blue, too green, too dim, or too saturated. Usually too blue and too cold.

Gradient Light Bridge uses a small built-in implementation of FastLED-style
color correction in both output backends. Hat tip to them. The correction model and preset names
are based on FastLED’s color correction and color temperature constants, but
FastLED is not linked into the firmware or daemon.

References:

- [`FastLED color.h`](https://github.com/FastLED/FastLED/blob/08388047ac3e0ebcaafe71563ae2a55421d70c02/src/color.h#L9-L32)
- [`FastLED ColorTemperature example`](https://github.com/FastLED/FastLED/blob/master/examples/ColorTemperature/ColorTemperature.ino)

The default correction is tuned for the my PC fans and me preference for warm tones:

```text
ARGB_COLOR_CORRECTION=TypicalLEDStrip
ARGB_COLOR_TEMPERATURE=Candle
ARGB_COLOR_GAIN_R=1.0
ARGB_COLOR_GAIN_G=1.0
ARGB_COLOR_GAIN_B=0.7
ARGB_COLOR_GAMMA_R=1.0
ARGB_COLOR_GAMMA_G=1.0
ARGB_COLOR_GAMMA_B=1.0
```

For the addressable LED backend, these values are compile-time options passed to
the firmware build. For the OpenRGB backend, the same correction model is
configured in `daemon/config.yaml` and can be changed without reflashing the ESP.

Disable correction entirely with:

```text
ARGB_COLOR_CORRECTION_ENABLED=0
```

Suggested tuning flow:

1. Set the same Hue scene on a real Hue light and on Gradient Light Bridge.
2. Adjust channel gain first, usually reducing the channel that is too strong.
3. Adjust the color temperature preset if whites are too cool or too warm.
4. Adjust gamma only if low brightness or fades look wrong.
5. Reapply the Hue scene after changing correction so cached colors are rendered
   through the new settings.


## Build the firmware

I recommend building with the official ESP-IDF Docker image (`docker.io/espressif/idf:v5.3.2`) through `firmware/in-docker.sh`. You need
Docker, but you do not need to install ESP-IDF on your machine, which in my
experience is much less problematic.

Start from the repository root, then enter the firmware directory and set the
target once:

```bash
cd firmware
chmod +x in-docker.sh
./in-docker.sh idf.py set-target esp32c6
```

Configure the build with environment variables, then build. If you do not set `ARGB_BACKEND`, the firmware builds the OpenRGB backend.

```bash
// Use the backend and color configuration values from the sections above. For
// example, a direct LED build would set `ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED`
// plus LED options such as `ARGB_LED_GPIO`, `ARGB_LED_COUNT`,
// `ARGB_ENDPOINT_COUNT`, `ARGB_ENDPOINT_LED_COUNT`, and `ARGB_COLOR_ORDER`.
export ARGB_EUI_SUFFIX=auto
export ARGB_BACKEND=ARGB_BACKEND_SERIAL_JSON
export ARGB_ENDPOINT_COUNT=2
./in-docker.sh idf.py build
```

The binary is written to `firmware/build/argb_to_hue.bin` and flash layout is written to `firmware/build/flash_args`.

Useful notes:

- If Docker requires group switching on your Linux machine, wrap the command:

  ```bash
  sg docker -c 'ARGB_EUI_SUFFIX=auto ARGB_BACKEND=ARGB_BACKEND_SERIAL_JSON ./in-docker.sh idf.py build'
  ```

- If you switch backend or target and the build looks stale, clean once, then
  rebuild:

  ```bash
  ./in-docker.sh idf.py fullclean
  ```


## Flash the firmware

Now that we have binary we need to flash it onto the ESP. 
Commands below run from the repository root, if you just finished building from `firmware/`, return to the repository root first - `cd ..`.

### Find the ESP serial port

In order to flash we need to know which serial port the ESP is connected to. Plug the ESP into USB, then find the serial device.



```bash
# On Linux:
ls -l /dev/serial/by-id/ 2>/dev/null
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null

# On macOS:
ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null
```



Use the real port shown on your machine.

If you are not sure which entry is the ESP, run the commands once with the ESP
unplugged and once after plugging it in. The new device is the port to use.
Set it for the rest of the commands:

```bash
# Linux:
export PORT=/dev/ttyACM0

# MacOS:
export PORT=/dev/cu.usbmodemXXXX
```

### Flash

Flash from the host with `esptool.py`. This works on both Linux and macOS:

```bash
python3 -m pip install esptool
# or: pipx install esptool

cd firmware/build
esptool.py -p "$PORT" --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash "@flash_args"
cd ../..
```

The command runs `esptool.py` from `firmware/build` so the paths inside `flash_args` resolve correctly.

### Watch logs

After flashing, I recommend you watch the serial log until the device starts Zigbee network
steering or joins the bridge.

```bash
python3 -m serial.tools.miniterm "$PORT" 115200
```

Start the serial monitor and keep the ESP connected until you see lines such as `Start network steering` and, after pairing,
`Joined network successfully`.

## Pair with the Hue bridge

1. Open the Philips Hue app → **Settings** → **Light setup** → **Add light**.
2. The app should discover one light per advertised endpoint (`ARGB_ENDPOINT_COUNT`).
3. Add them to the room/automation of your choice.

If pairing fails, first try clearing [pairing state](#pairing-state-and-reflashing). If
that still fails:

```bash
# erase the whole flash
esptool.py -p "$PORT" --chip esp32c6 erase_flash

# and flash the firmware again
cd firmware/build
esptool.py -p "$PORT" --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash "@flash_args"
cd ../..
```

### Pairing state and reflashing

Normal reflashing does not make Hue forget the ESP. The ESP has a stable factory
MAC address, and with `ARGB_EUI_SUFFIX=auto` the firmware derives a stable
Zigbee EUI from that MAC. The joined Zigbee network state is stored separately
in the `zb_storage` flash partition, and normal `write_flash "@flash_args"` does
not erase that partition.

This is nice during development: you can rebuild and reflash firmware changes,
restart the ESP, and the Hue bridge should usually recognize it again without
removing the light or pairing from scratch.

Clear Zigbee pairing state only when you want the ESP to join as a fresh device,
when pairing gets stuck, or when you intentionally changed identity-related
settings such as the EUI suffix or endpoint count.

```bash
esptool.py --chip esp32c6 -p "$PORT" -b 460800 erase_region 0xf1000 0x4000
```

## Install the PC daemon

This section is only relevant if you want to use OpenRGB backend. If you went with `ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED` this section is irrelevant.

Now that you have an ESP that talks Hue and outputs serial commands, we are missing two parts: 1. internal state machine that for example renders animations or transitions and 2. something that will apply this state to the devices themselves. Because the unfortunate reality is that almost every device only talks to manufacturer proprietary software / specification / language on the OS level, we need to abstract away. To do this I use OpenRGB. Please follow the instructions at one on the links below to get it configured and resume to this document when you have configured zones and segments, and can steer all you devices from OpenRGB. My forever gratiude to [Adam Honse](https://www.patreon.com/cw/CalcProgrammer1)'s, author of OpenRGB. 

https://openrgb.org/
https://gitlab.com/CalcProgrammer1/OpenRGB

This [daemon](https://en.wikipedia.org/wiki/Daemon_(computing)) reads commands, interprets them and renders internal state of your lights and forwards resulting state to openrgb server. Since I wrote it in Python you will need to create a [venv](https://docs.python.org/3/library/venv.html) and install dependencies (macOS or Linux):

```bash
cd daemon
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Edit `config.yaml` to map the firmware endpoints to OpenRGB targets. Start in
OpenRGB and note the device name, zone index, and optionally segment name for
the LEDs you want Gradient Light Bridge to control.

The important fields are:

- `openrgb.host` and `openrgb.port`: where the OpenRGB SDK server is listening.
  Use `127.0.0.1:6742` when OpenRGB and this daemon run on the same machine.
- `inputs`: one entry per ESP connected over serial. The key is your own name
  for that ESP, for example `case_fans` or `desk_strip`; it is used in logs and
  lets one daemon handle multiple ESPs.
- `inputs.<name>.port`: serial port for the ESP running
  `ARGB_BACKEND=ARGB_BACKEND_SERIAL_JSON`.
- `inputs.<name>.endpoints`: endpoint-to-output mapping. Firmware endpoint
  `11` is the first Hue light, `12` is the second Hue light, and so on.
- `device`: substring matched against the OpenRGB device name.
- `zone`: OpenRGB zone index. Omit it only if you want to write the whole
  device.
- `segment`: OpenRGB segment name, if your OpenRGB profile exposes named
  segments.
- `segment_start` and `leds`: explicit pixel range inside the zone. Use these
  when there is no named segment, or when you want only part of a zone.
- `name`: human-friendly label used in daemon logs.

A single Hue light can drive more than one OpenRGB range by making that endpoint
value a list. This is useful when multiple physical fans should mirror the same
Hue light.

Example:

```yaml
openrgb:
  host: 127.0.0.1
  port: 6742

inputs:
  case_fans:
    port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_xxx-if00
    endpoints:
      11:
        name: "Front fans"
        device: "OpenRGB device name"
        zone: 0
        segment_start: 0
        leds: 12
      12:
        - name: "Top left"
          device: "OpenRGB device name"
          zone: 1
          segment: "Left"
        - name: "Top right"
          device: "OpenRGB device name"
          zone: 1
          segment: "Right"

  desk_strip:
    port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_yyy-if00
    endpoints:
      11:
        name: "Desk strip"
        device: "Another OpenRGB device name"
        zone: 0
        leds: 30

set_direct_mode: true
```

Start exactly one OpenRGB SDK server, then point this daemon at that server.
The SDK server can be either:

- the OpenRGB GUI app with **SDK Server** enabled (Settings → SDK Server →
  Start Server), which is convenient for manual use but must stay open; or
- a headless OpenRGB service such as `OpenRGB --noautoconnect --server
  --server-port 6742`, which is better for automatic startup after login.

Do not run two OpenRGB instances that both detect/control the same RGB
hardware. In OpenRGB mode, `argb-to-hue` does not talk to the motherboard
controller directly; it connects to the OpenRGB SDK at `openrgb.host` /
`openrgb.port` in `config.yaml`. If a headless OpenRGB server already owns the
hardware and you want to open the GUI, launch the GUI as an SDK client, for
example:

```bash
OpenRGB --client 127.0.0.1:6742 --nodetect
```

Then run:

```bash
python argb_to_hue.py -c config.yaml
```

Add `-v` for debug output.

The daemon consumes the same immediate `DATA:` events produced by the firmware's
scene cache, so scene recall does not wait for the bridge's later FC03 replay.
It also applies Hue fade timing to solid colors, scene recalls, on/off changes,
brightness changes, and gradient updates before writing frames through the
OpenRGB SDK.

## Start daemon automatically

Linux:
```bash

sudo mkdir -p /opt/argb-to-hue
sudo cp -r daemon /opt/argb-to-hue/
sudo cp daemon/argb-to-hue.service /etc/systemd/system/argb-to-hue.service
sudo systemctl daemon-reload
sudo systemctl enable --now argb-to-hue.service
```

MacOS:
Edit `daemon/argb-to-hue.plist` and fill in the real paths, then:

```bash
cp daemon/argb-to-hue.plist ~/Library/LaunchAgents/com.argb-to-hue.daemon.plist
launchctl load ~/Library/LaunchAgents/com.argb-to-hue.daemon.plist
launchctl start com.argb-to-hue.daemon
```

## Troubleshooting

- **Hue app does not find the lights**: make sure the trust-center key is correct and that the Hue bridge is in pairing mode. Erase the ESP32 and power-cycle it.
- **Daemon cannot open serial port**: ensure no other program (e.g. `idf.py monitor`) is using the port.
- **Colors do not change in OpenRGB**: check that OpenRGB detects your devices and that they support a Direct/Custom mode. Set `set_direct_mode: true` in `config.yaml`.
- **Wrong zone mapping**: use OpenRGB’s GUI to inspect device/zone names and indices, then update `config.yaml`.

### Verify local LED wiring

For addressable LED builds, use the serial smoke tests before pairing or
assigning scenes:

```bash
python3 firmware/send_cmd.py --port "$PORT" led off
python3 firmware/send_cmd.py --port "$PORT" led solid ff0000
python3 firmware/send_cmd.py --port "$PORT" led solid 00ff00
python3 firmware/send_cmd.py --port "$PORT" led solid 0000ff
python3 firmware/send_cmd.py --port "$PORT" led gradient
python3 firmware/send_cmd.py --port "$PORT" led chase
```

If red and green are swapped, rebuild with a different `ARGB_COLOR_ORDER`.

## Known Limitations

The current firmware supports the useful Hue-gradient-light workflow: pairing,
certified gradient classification, scenes, dynamic scenes, play/stop, fades,
power recovery, and direct local LED rendering. It is not a complete clone of
every behavior in a Signify light. Known remaining gaps:

- **Hue Entertainment / streaming**: not implemented. The research notes
  separated gradient support from `capabilities.streaming`, and firmware does
  not implement the entertainment streaming path.
- **Named/timed Hue effects**: dynamic scenes are supported, but dedicated
  effects like candle/fireplace are not mapped. Firmware parses and logs effect
  fields, but does not implement a library of named effects. PRs are welcome.
- **OTA updates**: firmware exposes enough OTA-like identity/static attributes
  to satisfy the bridge, but does not implement a real firmware update flow.
- **Identify/alert behavior**: discovery/read support exists, but "blink this
  light so I can identify it" behavior is not implemented as a visible feature.
- **Hue parent-device fanout sync**: when one ESP exposes multiple light
  endpoints, the Hue app shows them under one parent device. Captures of parent
  device on/off and parent-wide color/brightness changes show the bridge
  unicasting commands to endpoints `11..15` sequentially, so those changes can
  visibly fan out across the PC zones. Whole-room on/off uses the room/group
  path and has been observed to stay in sync. The same non-synchronized color
  fanout is visible when manually dragging multiple authentic Hue lights on the
  Hue color wheel, so this appears to be normal Hue app/bridge behavior rather
  than a firmware-specific issue.
- **Full Zigbee scene/group command matrix**: firmware implements the commands
  the Hue bridge uses in the captured app flows. Other standard edge commands
  may still be missing until the app/bridge needs them.

## Research & References

### Articles

- [Getting Root on Philips Hue Bridge 2.0](https://colinoflynn.com/2016/07/getting-root-on-philips-hue-bridge-2-0/)
  - Bridge 2.0 UART/root access context and bridge firmware
    inspection starting point.

- [Don't be silly - it's only a lightbulb](https://research.checkpoint.com/2020/dont-be-silly-its-only-a-lightbulb/)
  - Hue bridge `ipbridge` reverse engineering and Zigbee-to-main-CPU
    message processing.

- [Make it Blink: Over-the-Air Exploitation of the Philips Hue Bridge](https://www.synacktiv.com/en/publications/make-it-blink-over-the-air-exploitation-of-the-philips-hue-bridge)
  - Recent Hue bridge RE and Zigbee frame processing context.

- [HAL9K, Sniffing Philips Hue Zigbee Traffic With Wireshark](https://www.hal9k.dk/sniffing-philips-hue-zigbee-traffic-with-wireshark/)
  - How to decrypt sniffed Zigbee traffic via wireshark

- [PeeVeeOne, "Breakout breakthrough"](https://peeveeone.com/2016/11/breakout-breakthrough/)
  - Useful for the classical commissioning vs touchlink distinction. Do not
    mirror this page into the repo because it publishes key material.

- [PeeVeeOne, "Custom firmware Hue lights"](https://peeveeone.com/2016/11/custom-firmware-hue-lights/)
  - Useful for historical custom ZLL/Hue-compatible light firmware behavior.

- [Wejn, "Project intro: Reversing Philips Hue light driver"](https://wejn.org/2024/12/reversing-philips-hue-light-driver/)
  - Useful for modern Hue light controller hardware context.

- [Wejn, "Zigbee: Hue-llo world!"](https://wejn.org/2025/01/zigbee-hue-llo-world/)
  - Useful because it overlaps with ESP32-C6 Hue-compatible device work.

### Papers

- [All Your Bulbs Are Belong to Us:
Investigating the Current State of Security in Connected Lighting Systems](https://arxiv.org/pdf/1608.03732)
  - ZLL security, touchlink attacks, Philips Hue/Osram/GE connected
    lighting behavior. Useful background for commissioning assumptions.

- [Insecure to the touch: attacking ZigBee 3.0 via touchlink commissioning
](https://doi.org/10.1145/3098243.3098254)
  - Zigbee 3.0 touchlink commissioning security. Useful for
    understanding why touchlink and classical commissioning need to be kept
    separate when reasoning about Hue pairing.

- [Exploring Security Economics in IoT Standardization Efforts](https://arxiv.org/pdf/1810.12035)
  - Background on Zigbee/ZLL standardization incentives and security
    economics. Useful for understanding why deprecated ZLL decisions still
    affect Zigbee 3.0 ecosystems.

- [A Lightbulb Worm? Details of the Philips Hue Smart Lighting Design](https://blackhat.com/docs/us-16/materials/us-16-OFlynn-A-Lightbulb-Worm-wp.pdf)
  - Hue bridge/bulb hardware reverse engineering, security model,
    firmware update encryption/signing, and attack-surface notes.

- [IoT Goes Nuclear: Creating a ZigBee Chain Reaction](https://doi.org/10.1109/SP.2017.14)
  - Philips Hue/ZLL chain-reaction work by Ronen, O'Flynn, Shamir,
    and Weingarten. Useful for OTA/key/security background.

- [ZLeaks: Passive Inference Attacks on Zigbee based Smart Homes](https://arxiv.org/pdf/2107.10830)
  - Background on passive inference from Zigbee smart-home traffic.
    Not Hue-certification specific, but useful for thinking about probe logs and
    recognizable command/reporting patterns.

- [Security Concerns in IoT Light Bulbs: Investigating Covert Channels](https://arxiv.org/pdf/2408.14613)
  - Background smart-light security/covert-channel paper. Lower
    direct implementation value, but related to Hue-light security literature.

### Slides

- [A Lightbulb Worm? (Black Hat USA 2016 slides)](https://blackhat.com/docs/us-16/materials/us-16-OFlynn-A-Lightbulb-Worm.pdf)
  - Presentation version of Colin O'Flynn's Hue teardown.

### Specs And Code Notes

- [Bifrost, hue-zigbee-format.md](https://github.com/chrivers/bifrost/blob/12d9e37e6ea032fb0708ddcd2faaa6db0133d7c8/doc/hue-zigbee-format.md)
  - Best current public documentation found for Hue
    manufacturer-specific cluster `0xFC03`, including gradient colors, effects,
    effect speed, scale, offset, and style fields.

- [Krzysztof Jagiełło, Hue Gradient command generator](https://github.com/kjagiello/hue-gradient-command-wizard/)
  - Implementation reference for the public Hue Gradient command
    wizard. Useful for comparing command payload generation against Bifrost and
    probe captures.

## License

MIT. 0 warranty, 0 liability, 0 resposibility for any damages. Use at your own risk.
