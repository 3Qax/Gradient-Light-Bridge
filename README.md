# argb-to-hue

Expose your PC’s motherboard-controlled ARGB as Philips Hue lights.

- **ESP32-C6** joins your existing Hue Zigbee network as one or more color lights.
- It forwards every on/off, brightness, and color change to a tiny daemon on the PC.
- The daemon drives **OpenRGB**, which controls the MSI motherboard, RAM, case fans, etc.

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

The firmware registers four endpoints by default:

1. `PC Case`
2. `PC Fans`
3. `PC RAM`
4. `PC Motherboard`

You can change the names/count in `firmware/main/argb_to_hue.h`.

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
3. The app should discover four new lights: `PC Case`, `PC Fans`, `PC RAM`, `PC Motherboard`.
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

## References

### Papers

- [`papers/morgner-2016-all-your-bulbs-are-belong-to-us-arxiv-1608.03732.pdf`](https://arxiv.org/pdf/1608.03732)
  - Relevance: ZLL security, touchlink attacks, Philips Hue/Osram/GE connected
    lighting behavior. Useful background for commissioning assumptions.

- `papers/morgner-2017-insecure-to-the-touch-wisec.pdf`
  - Source: https://doi.org/10.1145/3098243.3098254
  - Relevance: Zigbee 3.0 touchlink commissioning security. Useful for
    understanding why touchlink and classical commissioning need to be kept
    separate when reasoning about Hue pairing.

- `papers/morgner-2018-security-economics-iot-standardization-arxiv-1810.12035.pdf`
  - Source: https://arxiv.org/abs/1810.12035
  - PDF: https://arxiv.org/pdf/1810.12035
  - Relevance: background on Zigbee/ZLL standardization incentives and security
    economics. Useful for understanding why deprecated ZLL decisions still
    affect Zigbee 3.0 ecosystems.

- `papers/oflynn-2016-a-lightbulb-worm-whitepaper.pdf`
  - Source:
    https://colinoflynn.com/2016/08/philips-hue-r-e-whitepaper-from-black-hat-2016/
  - PDF:
    https://blackhat.com/docs/us-16/materials/us-16-OFlynn-A-Lightbulb-Worm-wp.pdf
  - Relevance: Hue bridge/bulb hardware reverse engineering, security model,
    firmware update encryption/signing, and attack-surface notes.

- `papers/ronen-2017-iot-goes-nuclear-ieee-sp.pdf`
  - Source: https://www.eyalro.net/project/iotworm.html
  - Local file source:
    https://www.eyalro.net/project/iotworm/IotGoesNuclearIEEESP17.pdf
  - Publication DOI: https://doi.org/10.1109/SP.2017.14
  - Relevance: Philips Hue/ZLL chain-reaction work by Ronen, O'Flynn, Shamir,
    and Weingarten. Useful for OTA/key/security background.

- `papers/shafqat-2021-zleaks-zigbee-smart-homes-arxiv-2107.10830.pdf`
  - Source: https://arxiv.org/abs/2107.10830
  - PDF: https://arxiv.org/pdf/2107.10830
  - Relevance: background on passive inference from Zigbee smart-home traffic.
    Not Hue-certification specific, but useful for thinking about probe logs and
    recognizable command/reporting patterns.

- `papers/rohilla-2024-security-concerns-iot-light-bulbs-arxiv-2408.14613.pdf`
  - Source: https://arxiv.org/abs/2408.14613
  - PDF: https://arxiv.org/pdf/2408.14613
  - Relevance: background smart-light security/covert-channel paper. Lower
    direct implementation value, but related to Hue-light security literature.

### Slides

- `slides/oflynn-2016-a-lightbulb-worm-slides.pdf`
  - Source:
    https://blackhat.com/docs/us-16/materials/us-16-OFlynn-A-Lightbulb-Worm.pdf
  - Relevance: presentation version of Colin O'Flynn's Hue teardown.

### Article PDFs

These are local browser-saved PDFs moved from the NAS into
`references/articles/`. They are ignored by git.

- `articles/colin-oflynn-getting-root-on-philips-hue-bridge-2.0.pdf`
  - Source:
    https://colinoflynn.com/2016/07/getting-root-on-philips-hue-bridge-2-0/
  - Relevance: bridge 2.0 UART/root access context and bridge firmware
    inspection starting point.

- `articles/checkpoint-dont-be-silly-its-only-a-lightbulb.pdf`
  - Source:
    https://research.checkpoint.com/2020/dont-be-silly-its-only-a-lightbulb/
  - Relevance: Hue bridge `ipbridge` reverse engineering and Zigbee-to-main-CPU
    message processing.

- `articles/synacktiv-make-it-blink-philips-hue-bridge.pdf`
  - Source:
    https://www.synacktiv.com/en/publications/make-it-blink-over-the-air-exploitation-of-the-philips-hue-bridge
  - Relevance: recent Hue bridge RE and Zigbee frame processing context.

- `aricles/hal9k-snigging-philips-hue-zigbee-traffic-with-wireshark`
    - Source: https://www.hal9k.dk/sniffing-philips-hue-zigbee-traffic-with-wireshark/
    - Relevance: how to decrypt sniffed Zigbee traffic via wireshark
### Specs And Code Notes

- `specs/bifrost-hue-zigbee-format-fc03.md`
  - Source:
    https://github.com/chrivers/bifrost/blob/master/doc/hue-zigbee-format.md
  - Relevance: best current public documentation found for Hue
    manufacturer-specific cluster `0xFC03`, including gradient colors, effects,
    effect speed, scale, offset, and style fields.

- `specs/hue-gradient-command-wizard-custom-gradient-utils.tsx`
  - Source:
    https://github.com/kjagiello/hue-gradient-command-wizard/blob/main/src/modes/CustomGradient/utils.tsx
  - Relevance: implementation reference for the public Hue Gradient command
    wizard. Useful for comparing command payload generation against Bifrost and
    probe captures.

## Linked-Only Sources

These were not mirrored locally, either because they may include sensitive key
material or because direct automated download was blocked.

- PeeVeeOne, "Breakout breakthrough"
  - https://peeveeone.com/2016/11/breakout-breakthrough/
  - Useful for the classical commissioning vs touchlink distinction. Do not
    mirror this page into the repo because it publishes key material.

- PeeVeeOne, "Custom firmware Hue lights"
  - https://peeveeone.com/2016/11/custom-firmware-hue-lights/
  - Useful for historical custom ZLL/Hue-compatible light firmware behavior.

- Wejn.org, "Project intro: Reversing Philips Hue light driver"
  - https://wejn.org/2024/12/reversing-philips-hue-light-driver/
  - Useful for modern Hue light controller hardware context.

- Wejn.org, "Zigbee: Hue-llo world!"
  - https://wejn.org/2025/01/zigbee-hue-llo-world/
  - Useful because it overlaps with ESP32-C6 Hue-compatible device work.

- Hue Gradient command generator
  - https://kjagiello.github.io/hue-gradient-command-wizard/
  - Interactive UI for building Hue gradient payloads.

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

## Integrity

`SHA256SUMS` contains checksums for the downloaded local files.


## License

MIT - 0 warranty, 0 liability, 0 resposibility for the damages. Use at your own risk.
