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

The build uses the official ESP-IDF Docker image so you do not need to install the toolchain on your Mac.

```bash
cd firmware
chmod +x in-docker.sh
./in-docker.sh idf.py set-target esp32c6
./in-docker.sh idf.py build
```

The firmware binary is written to `firmware/build/argb_to_hue.bin`.

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

To watch the device logs:

```bash
esptool.py --chip esp32c6 -p /dev/tty.usbmodem* monitor
# or, on Linux:
./in-docker.sh idf.py -p /dev/ttyACM0 monitor
```

Leave monitoring running until you see the device join the network (next step).

## Pair with the Hue bridge

1. Open the Philips Hue app → **Settings** → **Light setup** → **Add light**.
2. Power-cycle or reset the ESP32 if needed. It will start Zigbee network steering automatically.
3. The app should discover four new lights: `PC Case`, `PC Fans`, `PC RAM`, `PC Motherboard`.
4. Add them to the room/automation of your choice.

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

## License

MIT — except for the Hue trust-center key, which you must supply yourself.
