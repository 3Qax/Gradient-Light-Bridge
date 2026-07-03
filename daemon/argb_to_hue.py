#!/usr/bin/env python3
"""Bridge argb-to-hue USB-CDC output to OpenRGB.

Developed on macOS, intended to run on modern Linux.
"""

from __future__ import annotations

import argparse
import json
import logging
import math
import re
import select
import sys
import time
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import serial
import serial.tools.list_ports
import yaml
from openrgb import OpenRGBClient
from openrgb.orgb import Device, Segment, Zone
from openrgb.utils import RGBColor


logger = logging.getLogger("argb-to-hue")

RGBTriplet = tuple[float, float, float]

FASTLED_COLOR_CORRECTIONS: dict[str, tuple[int, int, int]] = {
    "typicalsmd5050": (255, 176, 240),
    "typicalledstrip": (255, 176, 240),
    "typical8mmpixel": (255, 224, 140),
    "typicalpixelstring": (255, 224, 140),
    "uncorrectedcolor": (255, 255, 255),
}

FASTLED_TEMPERATURES: dict[str, tuple[int, int, int]] = {
    "candle": (255, 147, 41),
    "tungsten40w": (255, 197, 143),
    "tungsten100w": (255, 214, 170),
    "halogen": (255, 241, 224),
    "carbonarc": (255, 250, 244),
    "highnoonsun": (255, 255, 251),
    "directsunlight": (255, 255, 255),
    "overcastsky": (201, 226, 255),
    "clearbluesky": (64, 156, 255),
    "warmfluorescent": (255, 244, 229),
    "standardfluorescent": (244, 255, 250),
    "coolwhitefluorescent": (212, 235, 255),
    "fullspectrumfluorescent": (255, 244, 242),
    "growlightfluorescent": (255, 239, 247),
    "blacklightfluorescent": (167, 0, 255),
    "mercuryvapor": (216, 247, 255),
    "sodiumvapor": (255, 209, 178),
    "metalhalide": (242, 252, 255),
    "highpressuresodium": (255, 183, 76),
    "uncorrectedtemperature": (255, 255, 255),
}


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


@dataclass(frozen=True)
class ColorCorrection:
    correction: RGBTriplet = (1.0, 1.0, 1.0)
    temperature: RGBTriplet = (1.0, 1.0, 1.0)
    gain: RGBTriplet = (1.0, 1.0, 1.0)
    gamma: RGBTriplet = (1.0, 1.0, 1.0)
    enabled: bool = True


def normalize_name(value: Any) -> str:
    return re.sub(r"[^a-z0-9]", "", str(value).lower())


def color_tuple_from_hex(value: int) -> tuple[int, int, int]:
    return (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF


def parse_rgb_bytes(value: Any, presets: Mapping[str, tuple[int, int, int]], default: tuple[int, int, int]) -> tuple[int, int, int]:
    if value is None:
        return default

    if isinstance(value, str):
        preset = presets.get(normalize_name(value))
        if preset is not None:
            return preset
        text = value.strip().removeprefix("#").removeprefix("0x")
        if len(text) == 6:
            try:
                return color_tuple_from_hex(int(text, 16))
            except ValueError:
                pass

    if isinstance(value, int):
        return color_tuple_from_hex(max(0, min(0xFFFFFF, value)))

    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return tuple(max(0, min(255, coerce_int(v, 255))) for v in value[:3])  # type: ignore[return-value]

    if isinstance(value, Mapping):
        return (
            max(0, min(255, coerce_int(value.get("r", value.get("red")), default[0]))),
            max(0, min(255, coerce_int(value.get("g", value.get("green")), default[1]))),
            max(0, min(255, coerce_int(value.get("b", value.get("blue")), default[2]))),
        )

    logger.warning("Ignoring invalid RGB correction value: %s", value)
    return default


def parse_float_triplet(value: Any, default: RGBTriplet) -> RGBTriplet:
    if value is None:
        return default
    if isinstance(value, (int, float, str)):
        parsed = coerce_float(value, default[0])
        return (parsed, parsed, parsed)
    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return (
            coerce_float(value[0], default[0]),
            coerce_float(value[1], default[1]),
            coerce_float(value[2], default[2]),
        )
    if isinstance(value, Mapping):
        return (
            coerce_float(value.get("r", value.get("red")), default[0]),
            coerce_float(value.get("g", value.get("green")), default[1]),
            coerce_float(value.get("b", value.get("blue")), default[2]),
        )
    return default


def merge_color_correction_configs(*configs: Any) -> dict[str, Any]:
    merged: dict[str, Any] = {}
    for cfg in configs:
        if isinstance(cfg, Mapping):
            merged.update(dict(cfg))
    return merged


def parse_color_correction(cfg: Mapping[str, Any] | None) -> ColorCorrection:
    cfg = cfg or {}
    enabled = bool(cfg.get("enabled", True))

    correction_value = (
        cfg.get("rgb")
        or cfg.get("correction_rgb")
        or cfg.get("correction")
        or cfg.get("preset")
        or cfg.get("profile")
        or "UncorrectedColor"
    )
    correction_bytes = parse_rgb_bytes(
        correction_value,
        FASTLED_COLOR_CORRECTIONS,
        FASTLED_COLOR_CORRECTIONS["uncorrectedcolor"],
    )

    temperature_value = cfg.get("temperature_rgb") or cfg.get("temperature") or "UncorrectedTemperature"
    temperature_bytes = parse_rgb_bytes(
        temperature_value,
        FASTLED_TEMPERATURES,
        FASTLED_TEMPERATURES["uncorrectedtemperature"],
    )

    gain = parse_float_triplet(cfg.get("gain"), (1.0, 1.0, 1.0))
    gain = (
        coerce_float(cfg.get("red_gain"), gain[0]),
        coerce_float(cfg.get("green_gain"), gain[1]),
        coerce_float(cfg.get("blue_gain"), gain[2]),
    )

    gamma = parse_float_triplet(cfg.get("gamma"), (1.0, 1.0, 1.0))
    gamma = (
        max(0.01, coerce_float(cfg.get("red_gamma"), gamma[0])),
        max(0.01, coerce_float(cfg.get("green_gamma"), gamma[1])),
        max(0.01, coerce_float(cfg.get("blue_gamma"), gamma[2])),
    )

    return ColorCorrection(
        correction=tuple(v / 255.0 for v in correction_bytes),  # type: ignore[arg-type]
        temperature=tuple(v / 255.0 for v in temperature_bytes),  # type: ignore[arg-type]
        gain=gain,
        gamma=gamma,
        enabled=enabled,
    )


UNCORRECTED_COLOR = ColorCorrection()


# For zone-level mappings we need to remember the parent Device so we can
# switch it into Direct/Custom mode before setting the zone color.
@dataclass
class OpenRGBTarget:
    device: Device
    zone: Zone | None = None
    segment: Segment | None = None
    start: int = 0
    length: int | None = None
    segment_name: str | None = None
    source_name: str = ""
    endpoint: int = 0
    target_name: str = ""
    target_index: int = 0
    correction: ColorCorrection = UNCORRECTED_COLOR

    @property
    def led_count(self) -> int:
        if self.segment is not None:
            return self.segment.leds_count
        if self.zone is None:
            total = len(self.device.leds)
        else:
            total = len(self.zone.leds)
        if self.length is None:
            return max(0, total - self.start)
        return self.length

    @property
    def description(self) -> str:
        if self.segment is not None:
            return f"{self.zone.name}/{self.segment.name}[{self.segment.start_idx}:{self.segment.start_idx + self.segment.leds_count}]"
        if self.zone is None:
            return self.device.name
        if self.segment_name:
            return f"{self.zone.name}/{self.segment_name}[{self.start}:{self.start + self.led_count}]"
        if self.start or self.length not in (None, len(self.zone.leds)):
            return f"{self.zone.name}[{self.start}:{self.start + self.led_count}]"
        return self.zone.name

    @property
    def cache_key(self) -> str:
        return f"{self.source_name}:{self.endpoint}:{self.target_index}:{self.target_name or self.description}"


Target = OpenRGBTarget | None
TargetGroup = list[OpenRGBTarget]
TargetColorSet = tuple[OpenRGBTarget, list[RGBColor]]
QueuedColorSet = tuple[OpenRGBTarget, list[RGBColor], bool]

HUE_STYLE_LINEAR = 0x00
HUE_STYLE_SCATTERED = 0x02
HUE_STYLE_MIRRORED = 0x04
HUE_STYLE_SEGMENTED = 0x06

HUE_FC03_FLAG_ON_OFF = 0x0001
HUE_FC03_FLAG_COLOR_MIREK = 0x0004
HUE_FC03_FLAG_COLOR_XY = 0x0008

ZONE_COLOR_CACHE: dict[int, list[RGBColor]] = {}
TARGET_COLOR_CACHE: dict[int, list[RGBColor]] = {}
TARGET_COLOR_CACHE_BY_KEY: dict[str, list[RGBColor]] = {}
PENDING_COLOR_SETS: list[QueuedColorSet] = []
COLOR_CONFIG_MTIME_NS: int | None = None
COLOR_STATE_CACHE_PATH: Path | None = None
COLOR_STATE_CACHE_DIRTY = False
COLOR_STATE_CACHE_LAST_SAVE_AT = 0.0


@dataclass
class GradientState:
    colors: list[dict[str, Any]]
    style: int
    scale: float
    offset: float
    bri: int
    on: bool
    dynamic: bool = False
    effect_speed: int | None = None
    started_at: float = 0.0
    last_frame_at: float = 0.0


@dataclass
class SerialSource:
    name: str
    port_name: str
    baud: int
    prefix: str
    ser: serial.Serial
    targets: dict[int, TargetGroup]
    gradient_states: dict[int, GradientState]


def ensure_direct_mode(device: Device) -> None:
    try:
        # OpenRGB mode names vary; try the most common Direct/Custom names.
        try:
            active_mode = device.modes[device.active_mode]
            if active_mode.name.lower() in ("direct", "custom"):
                return
        except (AttributeError, IndexError, TypeError):
            pass

        for mode_name in ("Direct", "Custom", "direct", "custom"):
            for mode in device.modes:
                if mode.name.lower() == mode_name.lower():
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


def rgb_channel(color: RGBColor, short_name: str, long_name: str) -> int:
    return int(getattr(color, short_name, getattr(color, long_name, 0)))


def copy_rgb_color(color: RGBColor) -> RGBColor:
    return RGBColor(
        rgb_channel(color, "r", "red"),
        rgb_channel(color, "g", "green"),
        rgb_channel(color, "b", "blue"),
    )


def color_to_json(color: RGBColor) -> list[int]:
    return [
        rgb_channel(color, "r", "red"),
        rgb_channel(color, "g", "green"),
        rgb_channel(color, "b", "blue"),
    ]


def colors_from_json(value: Any) -> list[RGBColor] | None:
    if not isinstance(value, list):
        return None

    colors: list[RGBColor] = []
    for item in value:
        if not isinstance(item, list) or len(item) < 3:
            return None
        colors.append(
            RGBColor(
                max(0, min(255, coerce_int(item[0], 0))),
                max(0, min(255, coerce_int(item[1], 0))),
                max(0, min(255, coerce_int(item[2], 0))),
            )
        )
    return colors


def clamp_byte(value: float) -> int:
    return max(0, min(255, int(round(value))))


def correct_color(color: RGBColor, correction: ColorCorrection) -> RGBColor:
    if not correction.enabled:
        return copy_rgb_color(color)

    channels = (
        rgb_channel(color, "r", "red"),
        rgb_channel(color, "g", "green"),
        rgb_channel(color, "b", "blue"),
    )
    corrected: list[int] = []
    for idx, channel in enumerate(channels):
        value = max(0.0, min(1.0, channel / 255.0))
        gamma = correction.gamma[idx]
        if gamma != 1.0:
            value = pow(value, gamma)
        value *= correction.correction[idx] * correction.temperature[idx] * correction.gain[idx]
        corrected.append(clamp_byte(value * 255.0))
    return RGBColor(corrected[0], corrected[1], corrected[2])


def uncorrect_color(color: RGBColor, correction: ColorCorrection) -> RGBColor:
    if not correction.enabled:
        return copy_rgb_color(color)

    channels = (
        rgb_channel(color, "r", "red"),
        rgb_channel(color, "g", "green"),
        rgb_channel(color, "b", "blue"),
    )
    raw: list[int] = []
    for idx, channel in enumerate(channels):
        factor = correction.correction[idx] * correction.temperature[idx] * correction.gain[idx]
        value = max(0.0, min(1.0, channel / 255.0))
        if factor > 0.0:
            value = min(1.0, value / factor)
        gamma = correction.gamma[idx]
        if gamma != 1.0:
            value = pow(value, 1.0 / gamma)
        raw.append(clamp_byte(value * 255.0))
    return RGBColor(raw[0], raw[1], raw[2])


def correct_colors(colors: list[RGBColor], correction: ColorCorrection) -> list[RGBColor]:
    return [correct_color(color, correction) for color in colors]


def uncorrect_colors(colors: list[RGBColor], correction: ColorCorrection) -> list[RGBColor]:
    return [uncorrect_color(color, correction) for color in colors]


def current_zone_colors(zone: Zone) -> list[RGBColor]:
    cached = ZONE_COLOR_CACHE.get(id(zone))
    if cached is not None and len(cached) == len(zone.leds):
        return [copy_rgb_color(c) for c in cached]

    colors = getattr(zone, "colors", None)
    if isinstance(colors, list) and len(colors) == len(zone.leds):
        return [copy_rgb_color(c) for c in colors]
    return [RGBColor(0, 0, 0) for _ in zone.leds]


def find_segment(zone: Zone, value: Any) -> Segment | None:
    segments = getattr(zone, "segments", None) or []
    if value is None:
        return None
    try:
        idx = int(value)
    except (TypeError, ValueError):
        idx = None
    if idx is not None and 0 <= idx < len(segments):
        return segments[idx]

    wanted = str(value).lower()
    for segment in segments:
        if segment.name.lower() == wanted:
            return segment
    return None


def cache_zone_colors(zone: Zone, colors: list[RGBColor]) -> None:
    copied = [copy_rgb_color(c) for c in colors]
    ZONE_COLOR_CACHE[id(zone)] = copied
    try:
        zone.colors = copied
    except Exception:
        pass


def set_zone_colors_fast(zone: Zone, colors: list[RGBColor]) -> None:
    start = time.monotonic()
    zone.set_colors(colors, fast=True)
    cache_zone_colors(zone, colors)
    elapsed_ms = (time.monotonic() - start) * 1000.0
    if elapsed_ms > 25.0:
        logger.debug("OpenRGB write zone %s took %.1f ms", zone.name, elapsed_ms)


def set_device_colors_fast(device: Device, colors: list[RGBColor]) -> None:
    start = time.monotonic()
    device.set_colors(colors, fast=True)
    elapsed_ms = (time.monotonic() - start) * 1000.0
    if elapsed_ms > 25.0:
        logger.debug("OpenRGB write device %s took %.1f ms", device.name, elapsed_ms)


def target_slice(target: OpenRGBTarget) -> tuple[Zone, int, int] | None:
    if target.segment is not None:
        return target.segment.parent_zone, target.segment.start_idx, target.segment.leds_count
    if target.zone is None:
        return None
    return target.zone, target.start, target.led_count


def set_cached_target_colors(target: OpenRGBTarget, colors: list[RGBColor]) -> None:
    global COLOR_STATE_CACHE_DIRTY

    copied = [copy_rgb_color(c) for c in colors]
    TARGET_COLOR_CACHE[id(target)] = copied
    TARGET_COLOR_CACHE_BY_KEY[target.cache_key] = copied
    COLOR_STATE_CACHE_DIRTY = True


def apply_color_sets(updates: list[TargetColorSet], set_direct_mode: bool) -> None:
    for target, colors in updates:
        set_cached_target_colors(target, colors)
        PENDING_COLOR_SETS.append((target, colors, set_direct_mode))


def flush_color_sets() -> None:
    if not PENDING_COLOR_SETS:
        return

    updates = list(PENDING_COLOR_SETS)
    PENDING_COLOR_SETS.clear()

    seen_devices: set[int] = set()
    for target, _colors, set_direct_mode in updates:
        if not set_direct_mode:
            continue
        device_id = id(target.device)
        if device_id in seen_devices:
            continue
        seen_devices.add(device_id)
        ensure_direct_mode(target.device)

    zone_updates: dict[int, tuple[Zone, list[RGBColor]]] = {}
    start_time = time.monotonic()
    device_write_count = 0
    zone_write_count = 0

    for target, colors, _set_direct_mode in updates:
        colors = correct_colors(colors, target.correction)
        zone_slice = target_slice(target)
        if zone_slice is None:
            set_device_colors_fast(target.device, colors)
            device_write_count += 1
            continue

        zone, start, led_count = zone_slice
        if id(zone) not in zone_updates:
            zone_updates[id(zone)] = (zone, current_zone_colors(zone))

        _zone, zone_colors = zone_updates[id(zone)]
        length = min(led_count, len(colors), len(zone_colors) - start)
        if length <= 0:
            continue
        zone_colors[start:start + length] = colors[:length]

    for zone, zone_colors in zone_updates.values():
        set_zone_colors_fast(zone, zone_colors)
        zone_write_count += 1

    elapsed_ms = (time.monotonic() - start_time) * 1000.0
    if elapsed_ms > 35.0 or len(updates) > zone_write_count + device_write_count:
        logger.debug(
            "OpenRGB flush updates=%d zone_writes=%d device_writes=%d took %.1f ms",
            len(updates),
            zone_write_count,
            device_write_count,
            elapsed_ms,
        )


def apply_colors(target: Target, colors: list[RGBColor], set_direct_mode: bool) -> None:
    if target is None:
        return
    apply_color_sets([(target, colors)], set_direct_mode)


def apply_color(target: Target, color: RGBColor, set_direct_mode: bool) -> None:
    if target is None:
        return
    apply_colors(target, [color] * target.led_count, set_direct_mode)


def apply_color_group(targets: TargetGroup, color: RGBColor, set_direct_mode: bool) -> None:
    apply_color_sets(
        [(target, [color] * target.led_count) for target in targets if target is not None],
        set_direct_mode,
    )


def interpolate_color(a: RGBColor, b: RGBColor, t: float) -> RGBColor:
    t = max(0.0, min(1.0, t))
    ar, ag, ab = rgb_channel(a, "r", "red"), rgb_channel(a, "g", "green"), rgb_channel(a, "b", "blue")
    br, bg, bb = rgb_channel(b, "r", "red"), rgb_channel(b, "g", "green"), rgb_channel(b, "b", "blue")
    return RGBColor(
        int(round(ar + (br - ar) * t)),
        int(round(ag + (bg - ag) * t)),
        int(round(ab + (bb - ab) * t)),
    )


def color_at_position(colors: list[RGBColor], position: float, wrap: bool = False) -> RGBColor:
    if not colors:
        return RGBColor(0, 0, 0)
    if len(colors) == 1:
        return colors[0]

    if wrap:
        position = position % len(colors)
        idx = int(position)
        next_idx = (idx + 1) % len(colors)
        return interpolate_color(colors[idx], colors[next_idx], position - idx)

    position = max(0.0, min(float(len(colors) - 1), position))
    idx = int(position)
    if idx >= len(colors) - 1:
        return colors[-1]
    return interpolate_color(colors[idx], colors[idx + 1], position - idx)


def coerce_rgb_color(data: dict[str, Any]) -> RGBColor:
    if not isinstance(data, dict):
        return RGBColor(0, 0, 0)

    def channel(name: str) -> int:
        try:
            value = int(data.get(name, 0))
        except (TypeError, ValueError):
            value = 0
        return max(0, min(255, value))

    return RGBColor(channel("r"), channel("g"), channel("b"))


def gamma_correct(c: float) -> float:
    if c <= 0.0:
        return 0.0
    if c <= 0.0031308:
        return 12.92 * c
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055


def xy_to_rgb(x: float, y: float, bri: int) -> RGBColor:
    if bri <= 0 or y <= 0.0 or y > 1.0 or x < 0.0 or x > 1.0:
        return RGBColor(0, 0, 0)

    z = 1.0 - x - y
    yy = bri / 254.0
    xx = (yy / y) * x
    zz = (yy / y) * z

    r_lin = 3.2406 * xx - 1.5372 * yy - 0.4986 * zz
    g_lin = -0.9689 * xx + 1.8758 * yy + 0.0415 * zz
    b_lin = 0.0557 * xx - 0.2040 * yy + 1.0570 * zz

    def channel(value: float) -> int:
        corrected = min(1.0, gamma_correct(value))
        return max(0, min(255, int(round(corrected * 255.0))))

    return RGBColor(channel(r_lin), channel(g_lin), channel(b_lin))


def coerce_gradient_color(data: dict[str, Any], bri: int) -> RGBColor:
    if isinstance(data, dict) and "x" in data and "y" in data:
        try:
            return xy_to_rgb(float(data["x"]), float(data["y"]), bri)
        except (TypeError, ValueError):
            pass
    return coerce_rgb_color(data)


def fixed_eighths_to_float(value: Any, default: float) -> float:
    if value is None:
        return default
    try:
        return int(value) / 8.0
    except (TypeError, ValueError):
        return default


def coerce_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def coerce_hue_bri(value: Any, default: int) -> int:
    return max(0, min(254, coerce_int(value, default)))


def coerce_float(value: Any, default: float) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def render_linear_gradient(
    colors: list[RGBColor],
    led_count: int,
    scale: float,
    offset: float,
    wrap: bool = False,
) -> list[RGBColor]:
    if led_count <= 1:
        return [color_at_position(colors, offset, wrap)] * max(0, led_count)

    span = max(0.0, scale - 1.0)
    return [
        color_at_position(colors, offset + (i / (led_count - 1)) * span, wrap)
        for i in range(led_count)
    ]


def render_mirrored_gradient(
    colors: list[RGBColor],
    led_count: int,
    scale: float,
    offset: float,
    wrap: bool = False,
) -> list[RGBColor]:
    if led_count <= 1:
        return [color_at_position(colors, offset, wrap)] * max(0, led_count)

    center = (led_count - 1) / 2.0
    max_distance = max(center, 1.0)
    span = max(0.0, scale - 1.0)
    return [
        color_at_position(colors, offset + (abs(i - center) / max_distance) * span, wrap)
        for i in range(led_count)
    ]


def coprime_palette_step(color_count: int) -> int:
    if color_count <= 1:
        return 1

    step = max(1, round(color_count * 0.61803398875))
    while math.gcd(step, color_count) != 1:
        step += 1
    return step


def render_scattered_gradient(colors: list[RGBColor], led_count: int, offset: float) -> list[RGBColor]:
    if not colors:
        return []
    if len(colors) == 1:
        return [colors[0]] * led_count

    phase = int(offset) % len(colors)
    step = coprime_palette_step(len(colors))
    return [colors[(phase + i * step) % len(colors)] for i in range(led_count)]


def render_segmented_gradient(
    colors: list[RGBColor],
    led_count: int,
    scale: float,
    offset: float,
    wrap: bool = False,
) -> list[RGBColor]:
    if not colors:
        return []

    visible_segments = max(1.0, scale)
    rendered: list[RGBColor] = []
    for i in range(led_count):
        position = offset + (i / max(1, led_count)) * visible_segments
        idx = int(position) % len(colors) if wrap else max(0, min(len(colors) - 1, int(position)))
        rendered.append(colors[idx])
    return rendered


def render_gradient(
    colors: list[RGBColor],
    led_count: int,
    style: int,
    scale: float,
    offset: float,
    wrap: bool = False,
) -> list[RGBColor]:
    if led_count <= 0 or not colors:
        return []

    if scale <= 0.0:
        scale = float(len(colors))

    if style == HUE_STYLE_MIRRORED:
        return render_mirrored_gradient(colors, led_count, scale, offset, wrap)
    if style == HUE_STYLE_SCATTERED:
        return render_scattered_gradient(colors, led_count, offset)
    if style == HUE_STYLE_SEGMENTED:
        return render_segmented_gradient(colors, led_count, scale, offset, wrap)

    if style != HUE_STYLE_LINEAR:
        logger.debug("Unknown Hue gradient style %s; rendering as linear", style)
    return render_linear_gradient(colors, led_count, scale, offset, wrap)


def dynamic_cycle_seconds(effect_speed: int | None, min_seconds: float, max_seconds: float) -> float:
    speed = max(1, min(254, coerce_int(effect_speed, 1)))
    speed_ratio = speed / 254.0
    # Hue's "extreme" dynamic-scene speed has been observed as 224 rather
    # than 254. Use an eased curve so high app speeds feel fast instead of
    # landing near the middle of a long linear cycle.
    remaining_ratio = 1.0 - speed_ratio
    return max(min_seconds, min_seconds + ((max_seconds - min_seconds) * remaining_ratio * remaining_ratio))


def dynamic_offset(state: GradientState, now: float, min_seconds: float, max_seconds: float) -> float:
    cycle_seconds = dynamic_cycle_seconds(state.effect_speed, min_seconds, max_seconds)
    if cycle_seconds <= 0.0 or not state.colors:
        return state.offset
    elapsed = max(0.0, now - state.started_at)
    palette_span = max(1.0, float(len(state.colors)))
    return state.offset + ((elapsed / cycle_seconds) * palette_span)


def render_gradient_for_target(
    target: Target,
    colors: list[dict[str, Any]],
    style: int,
    scale: float,
    offset: float,
    on: bool,
    bri: int = 254,
    wrap: bool = False,
) -> list[RGBColor] | None:
    """Render a Hue FC03 multiColor gradient for one mapped OpenRGB target.

    The firmware emits one RGB sample per received Hue gradient point. The
    daemon renders those points across the actual OpenRGB LED count.
    """
    if target is None:
        return None

    led_count = target.led_count
    if led_count == 0:
        return None

    if not on:
        return [RGBColor(0, 0, 0)] * led_count

    sample_colors = [coerce_gradient_color(c, bri) for c in colors]
    sample_count = len(sample_colors)
    if sample_count == 0:
        return None

    return render_gradient(sample_colors, led_count, style, scale, offset, wrap)


def apply_gradient(
    target: Target,
    colors: list[dict[str, Any]],
    style: int,
    scale: float,
    offset: float,
    on: bool,
    set_direct_mode: bool,
    bri: int = 254,
    wrap: bool = False,
) -> None:
    led_colors = render_gradient_for_target(target, colors, style, scale, offset, on, bri, wrap)
    if target is None or led_colors is None:
        return

    apply_colors(target, led_colors, set_direct_mode)


def apply_gradient_state(
    target: Target,
    state: GradientState,
    set_direct_mode: bool,
    now: float | None = None,
    min_cycle_seconds: float = 1.5,
    max_cycle_seconds: float = 90.0,
) -> None:
    offset = state.offset
    wrap = False
    if state.dynamic:
        offset = dynamic_offset(state, now or time.monotonic(), min_cycle_seconds, max_cycle_seconds)
        wrap = True
    apply_gradient(
        target,
        state.colors,
        state.style,
        state.scale,
        offset,
        state.on,
        set_direct_mode,
        state.bri,
        wrap,
    )


def apply_gradient_state_group(
    targets: TargetGroup,
    state: GradientState,
    set_direct_mode: bool,
    now: float | None = None,
    min_cycle_seconds: float = 1.5,
    max_cycle_seconds: float = 90.0,
) -> None:
    offset = state.offset
    wrap = False
    if state.dynamic:
        offset = dynamic_offset(state, now or time.monotonic(), min_cycle_seconds, max_cycle_seconds)
        wrap = True

    updates: list[TargetColorSet] = []
    for target in targets:
        led_colors = render_gradient_for_target(
            target,
            state.colors,
            state.style,
            state.scale,
            offset,
            state.on,
            state.bri,
            wrap,
        )
        if target is not None and led_colors is not None:
            updates.append((target, led_colors))
    apply_color_sets(updates, set_direct_mode)


def maybe_render_dynamic_frames(
    targets: Mapping[int, TargetGroup],
    gradient_states: dict[int, GradientState],
    frame_interval_seconds: float,
    min_cycle_seconds: float,
    max_cycle_seconds: float,
) -> None:
    now = time.monotonic()
    for endpoint, state in gradient_states.items():
        if not state.dynamic or not state.on:
            continue
        if now - state.last_frame_at < frame_interval_seconds:
            continue
        target_group = targets.get(endpoint)
        if not target_group:
            continue
        apply_gradient_state_group(
            target_group,
            state,
            set_direct_mode=False,
            now=now,
            min_cycle_seconds=min_cycle_seconds,
            max_cycle_seconds=max_cycle_seconds,
        )
        state.last_frame_at = now


def resolve_target(
    client: OpenRGBClient,
    mapping: dict[str, Any],
    source_name: str,
    endpoint: int,
    target_index: int,
) -> Target:
    pattern = mapping.get("device")
    zone_idx = mapping.get("zone")
    led_count = mapping.get("leds")
    segment_start = mapping.get("segment_start", mapping.get("start", 0))
    segment_name = mapping.get("segment")
    if not pattern:
        return None

    dev = find_device(client, pattern)
    if dev is None:
        logger.warning("No OpenRGB device matches pattern: %s", pattern)
        return None

    if zone_idx is None:
        return OpenRGBTarget(dev, None, length=coerce_int(led_count, len(dev.leds)) if led_count else None)

    try:
        zone_idx = int(zone_idx)
        if 0 <= zone_idx < len(dev.zones):
            zone = dev.zones[zone_idx]
            segment = find_segment(zone, segment_name)
            if segment is not None:
                if led_count and coerce_int(led_count, segment.leds_count) != segment.leds_count:
                    logger.warning(
                        "Configured leds=%s does not match segment %s length %d; using segment length",
                        led_count,
                        segment.name,
                        segment.leds_count,
                    )
                return OpenRGBTarget(
                    device=dev,
                    zone=zone,
                    segment=segment,
                    start=segment.start_idx,
                    length=segment.leds_count,
                    segment_name=segment.name,
                    source_name=source_name,
                    endpoint=endpoint,
                    target_name=str(mapping.get("name", segment.name)),
                    target_index=target_index,
                )
            if segment_name:
                logger.warning("Segment %s not found in zone %s; falling back to segment_start/leds", segment_name, zone.name)
            start = max(0, coerce_int(segment_start, 0))
            length = coerce_int(led_count, len(zone.leds) - start) if led_count else len(zone.leds) - start
            if start >= len(zone.leds):
                logger.warning("Segment start %d out of range for %s (leds=%d)", start, zone.name, len(zone.leds))
                return None
            if start + length > len(zone.leds):
                logger.warning(
                    "Segment %s[%d:%d] exceeds zone length %d; truncating",
                    zone.name,
                    start,
                    start + length,
                    len(zone.leds),
                )
                length = len(zone.leds) - start
            if start == 0 and length == len(zone.leds):
                resize_zone(zone, led_count)
            return OpenRGBTarget(
                device=dev,
                zone=zone,
                start=start,
                length=length,
                segment_name=str(segment_name) if segment_name else None,
                source_name=source_name,
                endpoint=endpoint,
                target_name=str(mapping.get("name", segment_name or zone.name)),
                target_index=target_index,
            )
        logger.warning("Zone index %d out of range for %s (zones=%d)", zone_idx, dev.name, len(dev.zones))
        return OpenRGBTarget(dev, None, source_name=source_name, endpoint=endpoint, target_name=str(mapping.get("name", dev.name)), target_index=target_index)
    except (ValueError, TypeError):
        return OpenRGBTarget(dev, None, source_name=source_name, endpoint=endpoint, target_name=str(mapping.get("name", dev.name)), target_index=target_index)


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


def resolve_target_group(client: OpenRGBClient, mapping: Any, source_name: str, endpoint: int) -> TargetGroup:
    mappings = mapping if isinstance(mapping, list) else [mapping]
    targets: TargetGroup = []
    for idx, item in enumerate(mappings):
        if not isinstance(item, Mapping):
            logger.warning("Ignoring non-object OpenRGB target mapping: %s", item)
            continue
        target = resolve_target(client, dict(item), source_name, endpoint, idx)
        if target is not None:
            targets.append(target)
    return targets


def describe_target_group(targets: TargetGroup) -> str:
    return ", ".join(target.description for target in targets)


def resolve_targets(client: OpenRGBClient, endpoints_cfg: Mapping[str, Any], source_name: str) -> dict[int, TargetGroup]:
    targets: dict[int, TargetGroup] = {}
    for ep_str, mapping in endpoints_cfg.items():
        try:
            ep = int(ep_str)
        except (ValueError, TypeError):
            logger.warning("Ignoring non-integer endpoint key for %s: %s", source_name, ep_str)
            continue
        targets[ep] = resolve_target_group(client, mapping, source_name, ep)
        if targets[ep]:
            logger.info("%s endpoint %d -> %s", source_name, ep, describe_target_group(targets[ep]))
    return targets


def open_serial_port(port_name: str, baud: int, timeout: float) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port_name
    ser.baudrate = baud
    ser.timeout = timeout
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def input_config_items(config: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    inputs_cfg = config.get("inputs")
    if inputs_cfg is None:
        return [
            (
                "default",
                {
                    "serial": config.get("serial", {}),
                    "endpoints": config.get("endpoints", {}),
                },
            )
        ]

    if isinstance(inputs_cfg, Mapping):
        return [(str(name), dict(input_cfg or {})) for name, input_cfg in inputs_cfg.items()]

    if isinstance(inputs_cfg, list):
        return [
            (str(input_cfg.get("name", f"input-{idx + 1}")), dict(input_cfg or {}))
            for idx, input_cfg in enumerate(inputs_cfg)
            if isinstance(input_cfg, Mapping)
        ]

    raise RuntimeError("Config key `inputs` must be a mapping or list")


def endpoint_mapping_for_input(input_cfg: Mapping[str, Any], endpoint: int) -> Any:
    endpoints_cfg = input_cfg.get("endpoints", {})
    if not isinstance(endpoints_cfg, Mapping):
        return None
    if endpoint in endpoints_cfg:
        return endpoints_cfg[endpoint]
    return endpoints_cfg.get(str(endpoint))


def target_mapping_for_target(endpoint_mapping: Any, target: OpenRGBTarget) -> Mapping[str, Any]:
    if isinstance(endpoint_mapping, list):
        if 0 <= target.target_index < len(endpoint_mapping):
            item = endpoint_mapping[target.target_index]
            if isinstance(item, Mapping):
                return item
        for item in endpoint_mapping:
            if isinstance(item, Mapping) and str(item.get("name", "")) == target.target_name:
                return item
        return {}
    if isinstance(endpoint_mapping, Mapping):
        return endpoint_mapping
    return {}


def color_correction_for_target(
    config: dict[str, Any],
    input_cfg_by_name: Mapping[str, dict[str, Any]],
    target: OpenRGBTarget,
) -> ColorCorrection:
    input_cfg = input_cfg_by_name.get(target.source_name, {})
    endpoint_mapping = endpoint_mapping_for_input(input_cfg, target.endpoint)
    target_mapping = target_mapping_for_target(endpoint_mapping, target)
    correction_cfg = merge_color_correction_configs(
        config.get("color_correction", {}),
        input_cfg.get("color_correction", {}),
        target_mapping.get("color_correction", {}) if isinstance(target_mapping, Mapping) else {},
    )
    return parse_color_correction(correction_cfg)


def iter_targets(sources: list[SerialSource]) -> list[OpenRGBTarget]:
    targets: list[OpenRGBTarget] = []
    for source in sources:
        for target_group in source.targets.values():
            targets.extend(target_group)
    return targets


def color_state_cache_path(config: Mapping[str, Any]) -> Path | None:
    value = config.get("color_state_cache", config.get("state_cache"))
    if value is False or value == "false" or value == "disabled":
        return None
    if value:
        return Path(str(value)).expanduser()
    return Path.home() / ".cache" / "argb-to-hue" / "last-colors.json"


def set_color_state_cache_path(path: Path | None) -> None:
    global COLOR_STATE_CACHE_PATH
    COLOR_STATE_CACHE_PATH = path


def load_target_color_cache(path: Path | None, sources: list[SerialSource]) -> int:
    if path is None or not path.exists():
        return 0

    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        logger.warning("Could not load color state cache %s: %s", path, exc)
        return 0

    raw_targets = payload.get("targets") if isinstance(payload, Mapping) else None
    if not isinstance(raw_targets, Mapping):
        return 0

    loaded = 0
    for target in iter_targets(sources):
        colors = colors_from_json(raw_targets.get(target.cache_key))
        if not colors:
            continue
        TARGET_COLOR_CACHE[id(target)] = colors
        TARGET_COLOR_CACHE_BY_KEY[target.cache_key] = colors
        loaded += 1

    if loaded:
        logger.info("Loaded %d cached target color set(s) from %s", loaded, path)
    return loaded


def current_target_colors(target: OpenRGBTarget) -> list[RGBColor] | None:
    zone_slice = target_slice(target)
    if zone_slice is not None:
        zone, start, led_count = zone_slice
        zone_colors = current_zone_colors(zone)
        if start >= len(zone_colors):
            return None
        return [copy_rgb_color(c) for c in zone_colors[start:start + led_count]]

    colors = getattr(target.device, "colors", None)
    if isinstance(colors, list):
        return [copy_rgb_color(c) for c in colors[:target.led_count]]
    return None


def seed_missing_target_color_cache_from_openrgb(sources: list[SerialSource]) -> int:
    seeded = 0
    for target in iter_targets(sources):
        if id(target) in TARGET_COLOR_CACHE:
            continue
        colors = current_target_colors(target)
        if not colors:
            continue
        set_cached_target_colors(target, uncorrect_colors(colors, target.correction))
        seeded += 1
    if seeded:
        logger.info("Seeded %d target color set(s) from current OpenRGB state", seeded)
    return seeded


def maybe_save_target_color_cache(force: bool = False) -> None:
    global COLOR_STATE_CACHE_DIRTY, COLOR_STATE_CACHE_LAST_SAVE_AT

    if COLOR_STATE_CACHE_PATH is None or not COLOR_STATE_CACHE_DIRTY:
        return

    now = time.monotonic()
    if not force and now - COLOR_STATE_CACHE_LAST_SAVE_AT < 2.0:
        return

    payload = {
        "version": 1,
        "targets": {
            key: [color_to_json(color) for color in colors]
            for key, colors in sorted(TARGET_COLOR_CACHE_BY_KEY.items())
        },
    }

    try:
        COLOR_STATE_CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = COLOR_STATE_CACHE_PATH.with_suffix(COLOR_STATE_CACHE_PATH.suffix + ".tmp")
        tmp_path.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="utf-8")
        tmp_path.replace(COLOR_STATE_CACHE_PATH)
        COLOR_STATE_CACHE_DIRTY = False
        COLOR_STATE_CACHE_LAST_SAVE_AT = now
    except Exception as exc:
        logger.warning("Could not save color state cache %s: %s", COLOR_STATE_CACHE_PATH, exc)


def requeue_cached_target_colors(targets: list[OpenRGBTarget]) -> int:
    count = 0
    for target in targets:
        colors = TARGET_COLOR_CACHE.get(id(target))
        if colors:
            apply_color_sets([(target, [copy_rgb_color(c) for c in colors])], set_direct_mode=False)
            count += 1
    return count


def reload_color_corrections_if_needed(config_path: Path, sources: list[SerialSource], force: bool = False) -> None:
    global COLOR_CONFIG_MTIME_NS

    try:
        mtime_ns = config_path.stat().st_mtime_ns
    except OSError as exc:
        logger.warning("Could not stat config for color correction reload: %s", exc)
        return

    if not force and COLOR_CONFIG_MTIME_NS == mtime_ns:
        return

    try:
        config = load_config(config_path)
    except Exception as exc:
        logger.warning("Could not reload color correction config: %s", exc)
        return

    COLOR_CONFIG_MTIME_NS = mtime_ns
    input_cfg_by_name = dict(input_config_items(config))
    changed = False
    targets = iter_targets(sources)
    for target in targets:
        correction = color_correction_for_target(config, input_cfg_by_name, target)
        if target.correction != correction:
            target.correction = correction
            changed = True

    if force:
        logger.info("Loaded color correction from %s", config_path)
    elif changed:
        logger.info("Reloaded color correction from %s", config_path)
        requeued = requeue_cached_target_colors(targets)
        logger.info("Reapplied color correction to %d cached target(s)", requeued)


def serial_cfg_for_input(global_serial_cfg: Mapping[str, Any], input_cfg: Mapping[str, Any]) -> dict[str, Any]:
    serial_cfg = dict(global_serial_cfg)
    nested_serial = input_cfg.get("serial", {})
    if isinstance(nested_serial, Mapping):
        serial_cfg.update(nested_serial)
    for key in ("port", "baud", "prefix", "timeout"):
        if key in input_cfg:
            serial_cfg[key] = input_cfg[key]
    return serial_cfg


def create_serial_sources(
    client: OpenRGBClient,
    config: dict[str, Any],
    serial_timeout: float,
) -> list[SerialSource]:
    global_serial_cfg = config.get("serial", {})
    items = input_config_items(config)
    multiple_inputs = len(items) > 1
    sources: list[SerialSource] = []

    for name, input_cfg in items:
        if input_cfg.get("enabled", True) is False:
            logger.info("Input %s is disabled; skipping", name)
            continue

        endpoints_cfg = input_cfg.get("endpoints", {})
        if not isinstance(endpoints_cfg, Mapping) or not endpoints_cfg:
            logger.warning("Input %s has no endpoint mappings; skipping", name)
            continue

        serial_cfg = serial_cfg_for_input(global_serial_cfg, input_cfg)
        preferred_port = serial_cfg.get("port")
        if multiple_inputs and not preferred_port:
            raise RuntimeError(f"Input {name} must set a serial port when multiple inputs are configured")

        port_name = find_serial_port(preferred_port)
        baud = coerce_int(serial_cfg.get("baud", 115200), 115200)
        prefix = str(serial_cfg.get("prefix", "DATA: "))
        input_timeout = max(0.02, coerce_float(serial_cfg.get("timeout"), serial_timeout))

        targets = resolve_targets(client, endpoints_cfg, name)
        logger.info("Opening %s serial port %s at %d baud", name, port_name, baud)
        ser = open_serial_port(port_name, baud, input_timeout)
        sources.append(
            SerialSource(
                name=name,
                port_name=port_name,
                baud=baud,
                prefix=prefix,
                ser=ser,
                targets=targets,
                gradient_states={},
            )
        )

    if not sources:
        raise RuntimeError("No serial inputs configured")
    return sources


def handle_data_event(
    source: SerialSource,
    data: dict[str, Any],
    set_direct_mode: bool,
    dynamic_min_cycle: float,
    dynamic_max_cycle: float,
) -> None:
    endpoint = data.get("endpoint")
    on = data.get("on", True)
    fc03_flags = data.get("fc03_flags")
    if fc03_flags is not None:
        fc03_flags = coerce_int(fc03_flags, 0)
    target_group = source.targets.get(endpoint)
    if not target_group:
        logger.debug("No mapping for %s endpoint %s", source.name, endpoint)
        return

    if data.get("gradient"):
        colors = data.get("colors", [])
        if not isinstance(colors, list):
            colors = []
        previous = source.gradient_states.get(endpoint)
        default_bri = previous.bri if previous else 254
        bri = coerce_hue_bri(data.get("bri"), default_bri)
        style = coerce_int(data.get("style"), HUE_STYLE_LINEAR)
        scale = fixed_eighths_to_float(
            data.get("scale_raw"),
            coerce_float(data.get("scale", data.get("segments")), float(len(colors))),
        )
        offset = fixed_eighths_to_float(data.get("offset_raw"), coerce_float(data.get("offset"), 0.0))
        effect_speed = data.get("effect_speed")
        dynamic = effect_speed is not None
        now = time.monotonic()
        source.gradient_states[endpoint] = GradientState(
            colors=[c for c in colors if isinstance(c, dict)],
            style=style,
            scale=scale,
            offset=offset,
            bri=bri,
            on=bool(on),
            dynamic=dynamic,
            effect_speed=coerce_int(effect_speed, 0) if dynamic else None,
            started_at=now,
            last_frame_at=now,
        )
        logger.debug(
            "RX %s endpoint=%s gradient n=%s style=%s scale=%.3f offset=%.3f bri=%s dynamic=%s speed=%s",
            source.name,
            endpoint,
            len(colors),
            style,
            scale,
            offset,
            bri,
            dynamic,
            effect_speed,
        )
        apply_gradient_state_group(
            target_group,
            source.gradient_states[endpoint],
            set_direct_mode,
            now=now,
            min_cycle_seconds=dynamic_min_cycle,
            max_cycle_seconds=dynamic_max_cycle,
        )
        return

    stored_gradient = source.gradient_states.get(endpoint)
    fc03_changed_color = bool(
        fc03_flags is not None
        and (fc03_flags & (HUE_FC03_FLAG_COLOR_MIREK | HUE_FC03_FLAG_COLOR_XY))
    )
    fc03_changed_on = fc03_flags is None or bool(fc03_flags & HUE_FC03_FLAG_ON_OFF)
    if (
        data.get("source") == "fc03"
        and stored_gradient is not None
        and not fc03_changed_color
        and ("bri" in data or "on" in data)
    ):
        if "bri" in data:
            stored_gradient.bri = coerce_hue_bri(data.get("bri"), stored_gradient.bri)
        if "on" in data and fc03_changed_on:
            stored_gradient.on = bool(data.get("on"))
        logger.debug(
            "RX %s endpoint=%s gradient state update on=%s bri=%s dynamic=%s",
            source.name,
            endpoint,
            stored_gradient.on,
            stored_gradient.bri,
            stored_gradient.dynamic,
        )
        now = time.monotonic()
        apply_gradient_state_group(
            target_group,
            stored_gradient,
            set_direct_mode,
            now=now,
            min_cycle_seconds=dynamic_min_cycle,
            max_cycle_seconds=dynamic_max_cycle,
        )
        stored_gradient.last_frame_at = now
        return

    if stored_gradient is not None:
        source.gradient_states.pop(endpoint, None)

    r = data.get("r", 0)
    g = data.get("g", 0)
    b = data.get("b", 0)
    logger.debug("RX %s endpoint=%s on=%s rgb=(%s,%s,%s)", source.name, endpoint, on, r, g, b)

    if not on:
        color = RGBColor(0, 0, 0)
    else:
        color = RGBColor(r, g, b)

    apply_color_group(target_group, color, set_direct_mode)


def run(config_path: Path, config: dict[str, Any]) -> None:
    or_cfg = config.get("openrgb", {})
    serial_cfg = config.get("serial", {})
    dynamic_cfg = config.get("dynamic", {})
    set_direct_mode = bool(config.get("set_direct_mode", True))
    set_color_state_cache_path(color_state_cache_path(config))

    client = connect_openrgb(or_cfg.get("host", "127.0.0.1"), or_cfg.get("port", 6742))

    dynamic_frame_interval = max(0.02, coerce_float(dynamic_cfg.get("frame_interval_seconds"), 0.05))
    dynamic_min_cycle = max(0.5, coerce_float(dynamic_cfg.get("min_cycle_seconds"), 1.5))
    dynamic_max_cycle = max(dynamic_min_cycle, coerce_float(dynamic_cfg.get("max_cycle_seconds"), 90.0))
    serial_timeout = max(0.02, min(coerce_float(serial_cfg.get("timeout"), dynamic_frame_interval), dynamic_frame_interval))

    sources = create_serial_sources(client, config, serial_timeout)
    reload_color_corrections_if_needed(config_path, sources, force=True)
    loaded_cached_colors = load_target_color_cache(COLOR_STATE_CACHE_PATH, sources)
    seeded_cached_colors = seed_missing_target_color_cache_from_openrgb(sources)
    if loaded_cached_colors or seeded_cached_colors:
        restored = requeue_cached_target_colors(iter_targets(sources))
        logger.info("Restored %d cached target color set(s) through current correction", restored)
        flush_color_sets()

    try:
        while True:
            reload_color_corrections_if_needed(config_path, sources)

            for source in sources:
                maybe_render_dynamic_frames(
                    source.targets,
                    source.gradient_states,
                    dynamic_frame_interval,
                    dynamic_min_cycle,
                    dynamic_max_cycle,
                )
            flush_color_sets()
            maybe_save_target_color_cache()

            readable_sources = [source for source in sources if source.ser.is_open]
            if not readable_sources:
                raise RuntimeError("All serial inputs are closed")

            ser_to_source = {source.ser: source for source in readable_sources}
            try:
                ready, _, _ = select.select(list(ser_to_source.keys()), [], [], dynamic_frame_interval)
            except (OSError, ValueError) as exc:
                logger.error("Serial select error: %s", exc)
                time.sleep(1)
                continue

            for ser in ready:
                source = ser_to_source[ser]
                for _ in range(25):
                    try:
                        line = ser.readline().decode("utf-8", errors="replace").strip()
                    except serial.SerialException as exc:
                        logger.error("Serial error on %s (%s): %s", source.name, source.port_name, exc)
                        time.sleep(1)
                        break

                    if not line:
                        break

                    data = parse_data_line(line, source.prefix)
                    if data is not None:
                        handle_data_event(source, data, set_direct_mode, dynamic_min_cycle, dynamic_max_cycle)

                    if ser.in_waiting <= 0:
                        break

            flush_color_sets()
            maybe_save_target_color_cache()
    finally:
        flush_color_sets()
        maybe_save_target_color_cache(force=True)
        for source in sources:
            source.ser.close()


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

    while True:
        try:
            config = load_config(args.config)
            run(args.config, config)
        except KeyboardInterrupt:
            logger.info("Interrupted")
            return 0
        except Exception as exc:
            logger.exception("Runtime error: %s", exc)
            logger.info("Restarting in 5 seconds...")
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
