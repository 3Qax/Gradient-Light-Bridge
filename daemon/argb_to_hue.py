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
@dataclass
class OpenRGBTarget:
    device: Device
    zone: Zone | None = None
    segment: Segment | None = None
    start: int = 0
    length: int | None = None
    segment_name: str | None = None

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


Target = OpenRGBTarget | None

HUE_STYLE_LINEAR = 0x00
HUE_STYLE_SCATTERED = 0x02
HUE_STYLE_MIRRORED = 0x04
HUE_STYLE_SEGMENTED = 0x06

HUE_FC03_FLAG_ON_OFF = 0x0001
HUE_FC03_FLAG_COLOR_MIREK = 0x0004
HUE_FC03_FLAG_COLOR_XY = 0x0008


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


def rgb_channel(color: RGBColor, short_name: str, long_name: str) -> int:
    return int(getattr(color, short_name, getattr(color, long_name, 0)))


def copy_rgb_color(color: RGBColor) -> RGBColor:
    return RGBColor(
        rgb_channel(color, "r", "red"),
        rgb_channel(color, "g", "green"),
        rgb_channel(color, "b", "blue"),
    )


def current_zone_colors(zone: Zone) -> list[RGBColor]:
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


def apply_colors(target: Target, colors: list[RGBColor], set_direct_mode: bool) -> None:
    if target is None:
        return
    if set_direct_mode:
        ensure_direct_mode(target.device)

    if target.segment is not None:
        target.segment.set_colors(colors)
        return

    if target.zone is None:
        target.device.set_colors(colors)
        return

    if target.start == 0 and target.led_count == len(target.zone.leds):
        target.zone.set_colors(colors)
        return

    zone_colors = current_zone_colors(target.zone)

    length = min(target.led_count, len(colors), len(zone_colors) - target.start)
    if length <= 0:
        return

    zone_colors[target.start:target.start + length] = colors[:length]
    target.zone.set_colors(zone_colors)


def apply_color(target: Target, color: RGBColor, set_direct_mode: bool) -> None:
    if target is None:
        return
    apply_colors(target, [color] * target.led_count, set_direct_mode)


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
    return max(min_seconds, max_seconds - ((max_seconds - min_seconds) * speed_ratio))


def dynamic_offset(state: GradientState, now: float, min_seconds: float, max_seconds: float) -> float:
    cycle_seconds = dynamic_cycle_seconds(state.effect_speed, min_seconds, max_seconds)
    if cycle_seconds <= 0.0 or not state.colors:
        return state.offset
    elapsed = max(0.0, now - state.started_at)
    palette_span = max(1.0, float(len(state.colors)))
    return state.offset + ((elapsed / cycle_seconds) * palette_span)


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
    """Apply a Hue FC03 multiColor gradient to the mapped OpenRGB zone/device.

    The firmware emits one RGB sample per received Hue gradient point. The
    daemon renders those points across the actual OpenRGB LED count.
    """
    if target is None:
        return

    led_count = target.led_count
    if led_count == 0:
        return

    if not on:
        off = [RGBColor(0, 0, 0)] * led_count
        apply_colors(target, off, set_direct_mode)
        return

    sample_colors = [coerce_gradient_color(c, bri) for c in colors]
    sample_count = len(sample_colors)
    if sample_count == 0:
        return

    led_colors = render_gradient(sample_colors, led_count, style, scale, offset, wrap)

    apply_colors(target, led_colors, set_direct_mode)


def apply_gradient_state(
    target: Target,
    state: GradientState,
    set_direct_mode: bool,
    now: float | None = None,
    min_cycle_seconds: float = 8.0,
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


def maybe_render_dynamic_frames(
    targets: Mapping[int, Target],
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
        target = targets.get(endpoint)
        if target is None:
            continue
        apply_gradient_state(
            target,
            state,
            set_direct_mode=False,
            now=now,
            min_cycle_seconds=min_cycle_seconds,
            max_cycle_seconds=max_cycle_seconds,
        )
        state.last_frame_at = now


def resolve_target(client: OpenRGBClient, mapping: dict[str, Any]) -> Target:
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
            )
        logger.warning("Zone index %d out of range for %s (zones=%d)", zone_idx, dev.name, len(dev.zones))
        return OpenRGBTarget(dev, None)
    except (ValueError, TypeError):
        return OpenRGBTarget(dev, None)


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
    dynamic_cfg = config.get("dynamic", {})
    endpoints_cfg = config.get("endpoints", {})
    set_direct_mode = bool(config.get("set_direct_mode", True))

    client = connect_openrgb(or_cfg.get("host", "127.0.0.1"), or_cfg.get("port", 6742))

    port_name = find_serial_port(serial_cfg.get("port"))
    baud = serial_cfg.get("baud", 115200)
    prefix = serial_cfg.get("prefix", "DATA: ")
    dynamic_frame_interval = max(0.02, coerce_float(dynamic_cfg.get("frame_interval_seconds"), 0.1))
    dynamic_min_cycle = max(1.0, coerce_float(dynamic_cfg.get("min_cycle_seconds"), 8.0))
    dynamic_max_cycle = max(dynamic_min_cycle, coerce_float(dynamic_cfg.get("max_cycle_seconds"), 90.0))
    serial_timeout = max(0.02, min(coerce_float(serial_cfg.get("timeout"), dynamic_frame_interval), dynamic_frame_interval))

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
            logger.info("Endpoint %d -> %s", ep, targets[ep].description)

    gradient_states: dict[int, GradientState] = {}

    logger.info("Opening serial port %s at %d baud", port_name, baud)
    # Open the port without asserting DTR/RTS so the ESP32-C6 doesn't get
    # kicked into firmware download mode on connect.
    ser = serial.Serial()
    ser.port = port_name
    ser.baudrate = baud
    ser.timeout = serial_timeout
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
                maybe_render_dynamic_frames(
                    targets,
                    gradient_states,
                    dynamic_frame_interval,
                    dynamic_min_cycle,
                    dynamic_max_cycle,
                )
                continue

            data = parse_data_line(line, prefix)
            if data is None:
                continue

            endpoint = data.get("endpoint")
            on = data.get("on", True)
            fc03_flags = data.get("fc03_flags")
            if fc03_flags is not None:
                fc03_flags = coerce_int(fc03_flags, 0)
            target = targets.get(endpoint)
            if target is None:
                logger.debug("No mapping for endpoint %s", endpoint)
                continue

            if data.get("gradient"):
                colors = data.get("colors", [])
                if not isinstance(colors, list):
                    colors = []
                previous = gradient_states.get(endpoint)
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
                gradient_states[endpoint] = GradientState(
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
                    "RX endpoint=%s gradient n=%s style=%s scale=%.3f offset=%.3f bri=%s dynamic=%s speed=%s",
                    endpoint,
                    len(colors),
                    style,
                    scale,
                    offset,
                    bri,
                    dynamic,
                    effect_speed,
                )
                apply_gradient_state(
                    target,
                    gradient_states[endpoint],
                    set_direct_mode,
                    now=now,
                    min_cycle_seconds=dynamic_min_cycle,
                    max_cycle_seconds=dynamic_max_cycle,
                )
            else:
                stored_gradient = gradient_states.get(endpoint)
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
                        "RX endpoint=%s gradient state update on=%s bri=%s dynamic=%s",
                        endpoint,
                        stored_gradient.on,
                        stored_gradient.bri,
                        stored_gradient.dynamic,
                    )
                    now = time.monotonic()
                    apply_gradient_state(
                        target,
                        stored_gradient,
                        set_direct_mode,
                        now=now,
                        min_cycle_seconds=dynamic_min_cycle,
                        max_cycle_seconds=dynamic_max_cycle,
                    )
                    stored_gradient.last_frame_at = now
                    continue

                if stored_gradient is not None:
                    gradient_states.pop(endpoint, None)

                r = data.get("r", 0)
                g = data.get("g", 0)
                b = data.get("b", 0)
                logger.debug("RX endpoint=%s on=%s rgb=(%s,%s,%s)", endpoint, on, r, g, b)

                if not on:
                    color = RGBColor(0, 0, 0)
                else:
                    color = RGBColor(r, g, b)

                apply_color(target, color, set_direct_mode)

            maybe_render_dynamic_frames(
                targets,
                gradient_states,
                dynamic_frame_interval,
                dynamic_min_cycle,
                dynamic_max_cycle,
            )
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
