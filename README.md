# argb-to-hue

Expose PC ARGB fans/strips as Hue-compatible gradient lights.

- **ESP32-C6** joins your existing Hue Zigbee network as one or more gradient lights.
- It can forward on/off, brightness, color, scene, and gradient changes to a PC daemon.
- It can also drive 5 V 3-pin addressable fans/strips directly from a GPIO/RMT backend.

Developed on macOS, deployed on Linux.

## Hardware

- [M5Stack Nano C6](https://docs.m5stack.com/en/core/M5NanoC6) or another ESP32-C6 dev board.
- PC running OpenRGB with your ARGB devices detected (e.g. MSI B850M GAMING PLUS WIFI6E).
- Existing Philips Hue bridge + iOS Hue app.

## Architecture

```
iOS Hue App → Hue Bridge → Zigbee 3.0 → ESP32-C6
                                              │
                                              │ USB-CDC (JSON lines)
                                              ▼
                                        argb_to_hue.py
                                              │
                                              │ OpenRGB SDK
                                              ▼
                                        OpenRGB → ARGB devices
```

The firmware registers one light endpoint by default. Build with
`ARGB_ENDPOINT_COUNT=<n>` to expose multiple logical Hue lights from the same
ESP32-C6 Zigbee node. In the local LED backend, each endpoint owns a contiguous
slice of the physical LED chain:

```text
endpoint 11 -> pixels 0..11
endpoint 12 -> pixels 12..23
endpoint 13 -> pixels 24..35
...
```

For five 12-LED fans on one ARGB data line, use `ARGB_ENDPOINT_COUNT=5`,
`ARGB_ENDPOINT_LED_COUNT=12`, and `ARGB_LED_COUNT=60`. A clean 2026-07-04
Hue Bridge test accepted five LCX004 endpoints from one ESP, while six endpoints
were read through endpoint 16 and then the bridge sent a ZDO leave/reset.

## Important: Hue Trust Center Link Key

To join a Philips Hue bridge, the ESP32 must present the Hue Zigbee trust-center link key. This key is widely documented in the Hue reverse-engineering community but is not included in this repository for legal reasons.

Background references for Hue commissioning and gradient behavior are collected
in [`research/hue-references.md`](research/hue-references.md).

1. Copy the placeholder header:

   ```bash
   cp firmware/main/trust_center_key.h.in firmware/main/trust_center_key.h
   ```

2. Replace the placeholder bytes in `firmware/main/trust_center_key.h` with the real 16-byte Hue trust center link key.

   The key is used by projects such as [`esp32-huello-world`](https://github.com/wejn/esp32-huello-world) and [`e32wamb`](https://github.com/wejn/e32wamb).

## Build the firmware (Docker, no host ESP-IDF install)

The build uses `firmware/in-docker.sh`, which runs the official ESP-IDF Docker
image. You do not need to install ESP-IDF on the host.

```bash
cd firmware
chmod +x in-docker.sh
./in-docker.sh idf.py set-target esp32c6
./in-docker.sh idf.py build
```

The firmware binary is written to `firmware/build/argb_to_hue.bin`.

Useful Docker helper details:

- Run `./in-docker.sh ...` from `firmware/`; it mounts the current directory as
  `/project` inside the container.
- The default image is `docker.io/espressif/idf:v5.3.2`. Override it with
  `IDFVER`, for example:

  ```bash
  IDFVER=v5.3.2 ./in-docker.sh idf.py build
  ```

- If your user is not in the `docker` group, run through `sg docker`:

  ```bash
  sg docker -c './in-docker.sh idf.py build'
  ```

- To build the gradient probe subproject, run the helper from that directory and
  point it at the parent script:

  ```bash
  cd firmware/gradient_probe
  ../in-docker.sh idf.py set-target esp32c6
  ../in-docker.sh idf.py build
  ```

### Direct fan/strip driving backend

The default build emits serial JSON for the daemon/OpenRGB path. To drive a
5 V 3-pin addressable fan or strip directly from an M5NanoC6 Grove pin, build
the local LED backend instead:

```bash
cd firmware
sg docker -c 'ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED ARGB_EUI_SUFFIX=auto ARGB_LED_GPIO=1 ARGB_LED_COUNT=12 ARGB_COLOR_ORDER=GRB ./in-docker.sh idf.py build'
```

Use `ARGB_LED_GPIO=1` for Grove `G1` or `ARGB_LED_GPIO=2` for Grove `G2`.
Power the LEDs from the motherboard ARGB header `5V`/`GND`, share that ground
with the NanoC6, and leave the motherboard ARGB data pin disconnected.

For one ESP exposing five independently controlled Hue lightstrips on one
60-pixel ARGB chain:

```bash
cd firmware
sg docker -c 'ARGB_BACKEND=ARGB_BACKEND_LOCAL_LED ARGB_EUI_SUFFIX=auto ARGB_LED_GPIO=2 ARGB_ENDPOINT_COUNT=5 ARGB_ENDPOINT_LED_COUNT=12 ARGB_LED_COUNT=60 ARGB_COLOR_ORDER=GRB ./in-docker.sh idf.py build'
```

This is one Zigbee device with multiple light endpoints, not five separate
IEEE/EUI-64 radios. The bridge should discover one Hue light per endpoint.

Local LED builds apply the same FastLED-style correction defaults as the
OpenRGB daemon config: `ARGB_COLOR_CORRECTION=TypicalLEDStrip`,
`ARGB_COLOR_TEMPERATURE=Candle`, `ARGB_COLOR_GAIN_R=1.0`,
`ARGB_COLOR_GAIN_G=1.0`, `ARGB_COLOR_GAIN_B=0.7`, and per-channel gamma of
`1.0`. Disable it with `ARGB_COLOR_CORRECTION_ENABLED=0`, or override preset,
temperature, per-channel correction bytes, gain, and gamma at build time.

After flashing a local LED build, use the serial CLI smoke tests to confirm the
data pin and color order before pairing or assigning scenes:

```bash
python3 firmware/send_cmd.py --port /dev/ttyACM0 led off
python3 firmware/send_cmd.py --port /dev/ttyACM0 led solid ff0000
python3 firmware/send_cmd.py --port /dev/ttyACM0 led solid 00ff00
python3 firmware/send_cmd.py --port /dev/ttyACM0 led solid 0000ff
python3 firmware/send_cmd.py --port /dev/ttyACM0 led gradient
python3 firmware/send_cmd.py --port /dev/ttyACM0 led chase
```

## Flash the firmware

Docker Desktop on macOS cannot pass USB serial ports into containers, so flashing is done with `esptool.py` on the host:

```bash
pipx install esptool
# or: python3 -m pip install esptool

# Find the port (macOS example)
ls /dev/tty.usbmodem*

esptool.py -p /dev/tty.usbmodem* --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash "@firmware/build/flash_args"
```

On Linux you can flash directly with the Docker helper if you prefer:

```bash
cd firmware
PORT=/dev/ttyACM0 ./in-docker.sh idf.py -p /dev/ttyACM0 flash
```

To erase Zigbee pairing state without wiping the full chip, erase the
`zb_storage` partition from Linux:

```bash
cd firmware
PORT=/dev/ttyACM0 ./in-docker.sh python3 -m esptool --chip esp32c6 \
  -p /dev/ttyACM0 -b 460800 erase_region 0xf1000 0x4000
```

To watch the device logs:

```bash
esptool.py --chip esp32c6 -p /dev/tty.usbmodem* monitor
# or, on Linux:
PORT=/dev/ttyACM0 ./in-docker.sh idf.py -p /dev/ttyACM0 monitor
```

Leave monitoring running until you see the device join the network (next step).

## Pair with the Hue bridge

1. Open the Philips Hue app → **Settings** → **Light setup** → **Add light**.
2. Power-cycle or reset the ESP32 if needed. It will start Zigbee network steering automatically.
3. The app should discover one light per advertised endpoint.
4. Add them to the room/automation of your choice.

For fast firmware-identity iteration, use
[`docs/hue-api-device-cycle.md`](docs/hue-api-device-cycle.md) to remove the
bridge's stale light record and trigger rediscovery through the local Hue API.

If pairing fails, erase the ESP32 and try again:

```bash
esptool.py -p /dev/tty.usbmodem* --chip esp32c6 erase_flash
esptool.py -p /dev/tty.usbmodem* --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash "@firmware/build/flash_args"
```

## Install the PC daemon

Create a venv and install dependencies (macOS or Linux):

```bash
cd daemon
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Edit `config.yaml` to match your OpenRGB device names. The default config assumes:

- Fractal case → pattern `Fractal`
- MSI motherboard/fans → pattern `Mystic Light`
- GOODRAM RAM → pattern `GOODRAM`

Start OpenRGB and enable the **SDK Server** (Settings → SDK Server → Start Server). Then run:

```bash
python argb_to_hue.py -c config.yaml
```

Add `-v` for debug output.

## Run automatically on Linux

```bash
sudo mkdir -p /opt/argb-to-hue
sudo cp -r daemon /opt/argb-to-hue/
sudo cp daemon/argb-to-hue.service /etc/systemd/system/argb-to-hue.service
sudo systemctl daemon-reload
sudo systemctl enable --now argb-to-hue.service
```

## Run automatically on macOS

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

## Known Gaps

The current firmware supports the useful Hue-gradient-light workflow: pairing,
certified gradient classification, scenes, dynamic scenes, play/stop, fades,
power recovery, and direct local LED rendering. It is not a complete clone of
every behavior in a Signify light. Known remaining gaps:

- **Hue Entertainment / streaming**: not implemented. The research notes
  separated gradient support from `capabilities.streaming`, and firmware does
  not implement the entertainment streaming path.
- **Named/timed Hue effects**: dynamic scenes are supported, but dedicated
  effects like candle/fireplace are not mapped. Firmware parses and logs effect
  fields, but does not implement a library of named effects.
- **OTA updates**: firmware exposes enough OTA-like identity/static attributes
  to satisfy the bridge, but does not implement a real firmware update flow.
- **Identify/alert behavior**: discovery/read support exists, but "blink this
  light so I can identify it" behavior is not implemented as a visible feature.
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

## Practical Takeaways For This Repo

- Joining Hue is a classical commissioning problem for this firmware, not a
  touchlink problem.
- `0xFC03` is the main public target for gradient payload correctness.
- `0xFC01` remains important because Hue bridges have been observed sending
  manufacturer-specific command `0x03` there after device announcements.
- The Hue API `capabilities.certified` flag is probably bridge-side product
  classification logic, not something explained by the public ZLL papers.
- If exact certification behavior becomes necessary, the most relevant RE path
  is bridge firmware / `ipbridge` discovery and product-mapping logic.


## License

MIT. 0 warranty, 0 liability, 0 resposibility for the damages. Use at your own risk.
