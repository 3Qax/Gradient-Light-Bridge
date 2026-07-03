#!/usr/bin/env python3
"""Bridge argb-to-hue USB-CDC output to OpenRGB.

Developed on macOS, intended to run on modern Linux.
"""

from __future__ import annotations

import argparse
import json
import logging
import re
import sys
import time
from pathlib import Path
from typing import Any

import serial
import serial.tools.list_ports
import yaml
from openrgb import OpenRGBClient
from openrgb.orgb import Device, Zone
from openrgb.utils import RGBColor


logger = logging.getLogger("argb-to-hue")


def load_config(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def find_serial_port(preferred: str | None) -> str:
    if preferred:
        return preferred

    try:
        ports = serial.tools.list_ports.comports()
    except Exception as exc:
        raise RuntimeError("Failed to list serial ports") from exc

    # Prefer USB CDC devices common on ESP32-C6 dev boards.
    for p in ports:
        desc = (p.description or "").lower()
        if "usb" in desc or "acm" in desc or "cdc" in desc or "uart" in desc:
            logger.info("Auto-selected serial port: %s (%s)", p.device, p.description)
            return p.device

    if not ports:
        raise RuntimeError("No serial ports found")

    p = ports[0]
    logger.info("Auto-selected fallback serial port: %s (%s)", p.device, p.description)
    return p.device


def connect_openrgb(host: str, port: int) -> OpenRGBClient:
    client = OpenRGBClient(host, port, name="argb-to-hue")
    logger.info("Connected to OpenRGB SDK at %s:%d", host, port)
    for dev in client.devices:
        logger.info("OpenRGB device: %s (type=%s, zones=%d)", dev.name, dev.type, len(dev.zones))
    return client


def find_device(client: OpenRGBClient, pattern: str) -> Device | None:
    pattern_lower = pattern.lower()
    for dev in client.devices:
        if pattern_lower in dev.name.lower():
            return dev
    return None


# For zone-level mappings we need to remember the parent Device so we can
# switch it into Direct/Custom mode before setting the zone color.
Target = tuple[Device, Zone | None] | None


def ensure_direct_mode(device: Device) -> None:
    try:
        # OpenRGB mode names vary; try the most common Direct/Custom names.
        for mode_name in ("Direct", "Custom", "direct", "custom"):
            for mode in device.modes:
                if mode.name == mode_name:
                    device.set_mode(mode_name)
                    return
    except Exception as exc:
        logger.warning("Could not set Direct/Custom mode on %s: %s", device.name, exc)


def resize_zone(zone: Zone, led_count: int | None) -> None:
    if led_count is None or led_count <= 0:
        return
    try:
        if len(zone.leds) != led_count:
            zone.resize(led_count)
            logger.info("Resized zone %s to %d LEDs", zone.name, led_count)
    except Exception as exc:
        logger.warning("Could not resize zone %s to %d LEDs: %s", zone.name, led_count, exc)


def apply_color(target: Target, color: RGBColor, set_direct_mode: bool) -> None:
    if target is None:
        return
    device, zone = target
    if set_direct_mode:
        ensure_direct_mode(device)
    if zone is None:
        device.set_color(color)
    else:
        zone.set_color(color)


def interpolate_color(a: RGBColor, b: RGBColor, t: float) -> RGBColor:
    t = max(0.0, min(1.0, t))
    return RGBColor(
        int(round(a.r + (b.r - a.r) * t)),
        int(round(a.g + (b.g - a.g) * t)),
        int(round(a.b + (b.b - a.b) * t)),
    )


def apply_gradient(
    target: Target,
    colors: list[dict[str, int]],
    segments: int,
    on: bool,
    set_direct_mode: bool,
) -> None:
    """Apply a Hue FC03 multiColor gradient to the mapped OpenRGB zone/device.

    The firmware emits one RGB sample per gradient point; we linearly
    interpolate between those samples across the LEDs in the target zone.
    """
    if target is None:
        return
    device, zone = target
    if set_direct_mode:
        ensure_direct_mode(device)

    leds = zone.leds if zone else device.leds
    led_count = len(leds)
    if led_count == 0:
        return

    if not on:
        off = [RGBColor(0, 0, 0)] * led_count
        if zone is None:
            device.set_colors(off)
        else:
            zone.set_colors(off)
        return

    sample_colors = [RGBColor(c["r"], c["g"], c["b"]) for c in colors]
    sample_count = len(sample_colors)
    if sample_count == 0:
        return

    if sample_count == 1 or led_count == 1:
        led_colors = [sample_colors[0]] * led_count
    else:
        led_colors = []
        for i in range(led_count):
            pos = (i / (led_count - 1)) * (sample_count - 1)
            idx = int(pos)
            t = pos - idx
            c1 = sample_colors[min(idx, sample_count - 1)]
            c2 = sample_colors[min(idx + 1, sample_count - 1)]
            led_colors.append(interpolate_color(c1, c2, t))

    if zone is None:
        device.set_colors(led_colors)
    else:
        zone.set_colors(led_colors)


def resolve_target(client: OpenRGBClient, mapping: dict[str, Any]) -> Target:
    pattern = mapping.get("device")
    zone_idx = mapping.get("zone")
    led_count = mapping.get("leds")
    if not pattern:
        return None

    dev = find_device(client, pattern)
    if dev is None:
        logger.warning("No OpenRGB device matches pattern: %s", pattern)
        return None

    if zone_idx is None:
        return (dev, None)

    try:
        zone_idx = int(zone_idx)
        if 0 <= zone_idx < len(dev.zones):
            zone = dev.zones[zone_idx]
            resize_zone(zone, led_count)
            return (dev, zone)
        logger.warning("Zone index %d out of range for %s (zones=%d)", zone_idx, dev.name, len(dev.zones))
        return (dev, None)
    except (ValueError, TypeError):
        return (dev, None)


def parse_data_line(line: str, prefix: str) -> dict[str, Any] | None:
    if not line.startswith(prefix):
        return None
    payload = line[len(prefix):].strip()
    if not payload:
        return None
    try:
        return json.loads(payload)
    except json.JSONDecodeError as exc:
        logger.warning("Malformed JSON from firmware: %s (%s)", payload, exc)
        return None


def run(config: dict[str, Any]) -> None:
    or_cfg = config.get("openrgb", {})
    serial_cfg = config.get("serial", {})
    endpoints_cfg = config.get("endpoints", {})
    set_direct_mode = bool(config.get("set_direct_mode", True))

    client = connect_openrgb(or_cfg.get("host", "127.0.0.1"), or_cfg.get("port", 6742))

    port_name = find_serial_port(serial_cfg.get("port"))
    baud = serial_cfg.get("baud", 115200)
    prefix = serial_cfg.get("prefix", "DATA: ")

    # Resolve OpenRGB targets once at startup; refresh on each reconnect if needed.
    targets: dict[int, Target] = {}
    for ep_str, mapping in endpoints_cfg.items():
        try:
            ep = int(ep_str)
        except (ValueError, TypeError):
            logger.warning("Ignoring non-integer endpoint key: %s", ep_str)
            continue
        targets[ep] = resolve_target(client, mapping)
        if targets[ep]:
            logger.info("Endpoint %d -> %s", ep, targets[ep][1].name if targets[ep][1] else targets[ep][0].name)

    logger.info("Opening serial port %s at %d baud", port_name, baud)
    # Open the port without asserting DTR/RTS so the ESP32-C6 doesn't get
    # kicked into firmware download mode on connect.
    ser = serial.Serial()
    ser.port = port_name
    ser.baudrate = baud
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()

    try:
        while True:
            try:
                line = ser.readline().decode("utf-8", errors="replace").strip()
            except serial.SerialException as exc:
                logger.error("Serial error: %s", exc)
                time.sleep(1)
                continue

            if not line:
                continue

            data = parse_data_line(line, prefix)
            if data is None:
                continue

            endpoint = data.get("endpoint")
            on = data.get("on", True)
            target = targets.get(endpoint)
            if target is None:
                logger.debug("No mapping for endpoint %s", endpoint)
                continue

            if data.get("gradient"):
                colors = data.get("colors", [])
                segments = data.get("segments", len(colors))
                logger.debug("RX endpoint=%s gradient n=%s segments=%s", endpoint, len(colors), segments)
                apply_gradient(target, colors, segments, on, set_direct_mode)
            else:
                r = data.get("r", 0)
                g = data.get("g", 0)
                b = data.get("b", 0)
                logger.debug("RX endpoint=%s on=%s rgb=(%s,%s,%s)", endpoint, on, r, g, b)

                if not on:
                    color = RGBColor(0, 0, 0)
                else:
                    color = RGBColor(r, g, b)

                apply_color(target, color, set_direct_mode)
    finally:
        ser.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Bridge argb-to-hue firmware to OpenRGB")
    parser.add_argument("-c", "--config", type=Path, default=Path("config.yaml"),
                        help="Path to config.yaml")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose logging")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    config = load_config(args.config)
    while True:
        try:
            run(config)
        except KeyboardInterrupt:
            logger.info("Interrupted")
            return 0
        except Exception as exc:
            logger.exception("Runtime error: %s", exc)
            logger.info("Restarting in 5 seconds...")
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
