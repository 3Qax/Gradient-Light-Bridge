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


def ensure_direct_mode(target: Device | Zone) -> None:
    device = target if isinstance(target, Device) else target.device
    try:
        # OpenRGB mode names vary; try the most common Direct/Custom names.
        for mode_name in ("Direct", "Custom", "direct", "custom"):
            for mode in device.modes:
                if mode.name == mode_name:
                    device.set_mode(mode_name)
                    return
    except Exception as exc:
        logger.warning("Could not set Direct/Custom mode on %s: %s", device.name, exc)


def apply_color(target: Device | Zone | None, color: RGBColor, set_direct_mode: bool) -> None:
    if target is None:
        return
    if set_direct_mode:
        ensure_direct_mode(target)
    target.set_color(color)


def resolve_target(client: OpenRGBClient, mapping: dict[str, Any]) -> Device | Zone | None:
    pattern = mapping.get("device")
    zone_idx = mapping.get("zone")
    if not pattern:
        return None

    dev = find_device(client, pattern)
    if dev is None:
        logger.warning("No OpenRGB device matches pattern: %s", pattern)
        return None

    if zone_idx is None:
        return dev

    try:
        zone_idx = int(zone_idx)
        if 0 <= zone_idx < len(dev.zones):
            return dev.zones[zone_idx]
        logger.warning("Zone index %d out of range for %s (zones=%d)", zone_idx, dev.name, len(dev.zones))
        return dev
    except (ValueError, TypeError):
        return dev


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
    targets: dict[int, Device | Zone | None] = {}
    for ep_str, mapping in endpoints_cfg.items():
        try:
            ep = int(ep_str)
        except (ValueError, TypeError):
            logger.warning("Ignoring non-integer endpoint key: %s", ep_str)
            continue
        targets[ep] = resolve_target(client, mapping)
        if targets[ep]:
            logger.info("Endpoint %d -> %s", ep, targets[ep].name)

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
            r = data.get("r", 0)
            g = data.get("g", 0)
            b = data.get("b", 0)

            logger.debug("RX endpoint=%s on=%s rgb=(%s,%s,%s)", endpoint, on, r, g, b)

            target = targets.get(endpoint)
            if target is None:
                logger.debug("No mapping for endpoint %s", endpoint)
                continue

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
