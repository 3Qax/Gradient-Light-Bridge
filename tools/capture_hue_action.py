#!/usr/bin/env python3
"""Capture firmware serial logs while operating an existing Hue light.

This is for post-commissioning behavior such as changing color, gradient, or
effects in the Hue app. It does not delete lights, start search, send discover,
or reset the board.
"""

from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

import serial


DEFAULT_BAUD = 115200
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_SECONDS = 90
DEFAULT_UNIQUEID_PREFIX = "00:17:88:01:0b:ff:fe:05"
DEFAULT_MODELID = "LCX004"
DEFAULT_MANUFACTURER = "Signify Netherlands B.V."


class HueApiError(RuntimeError):
    pass


def load_dotenv(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values

    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if key:
            values[key] = value.strip().strip("'\"")
    return values


@dataclass(frozen=True)
class HueClient:
    host: str
    username: str
    insecure: bool = True
    timeout: int = 30

    @property
    def base_url(self) -> str:
        return f"https://{self.host}/api/{self.username}"

    @property
    def clip_v2_url(self) -> str:
        return f"https://{self.host}/clip/v2/resource"

    def request(self, method: str, path: str, body: Any | None = None) -> Any:
        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        context = ssl._create_unverified_context() if self.insecure else None
        req = urllib.request.Request(
            f"{self.base_url}{path}",
            data=data,
            headers=headers,
            method=method,
        )
        return self._urlopen_json(req, context, f"{method} {path}")

    def v2_request(self, path: str) -> Any:
        context = ssl._create_unverified_context() if self.insecure else None
        req = urllib.request.Request(
            f"{self.clip_v2_url}{path}",
            headers={
                "Accept": "application/json",
                "hue-application-key": self.username,
            },
            method="GET",
        )
        return self._urlopen_json(req, context, f"GET /clip/v2/resource{path}")

    def _urlopen_json(self, req: urllib.request.Request, context: ssl.SSLContext | None, label: str) -> Any:
        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=context) as resp:
                payload = resp.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise HueApiError(f"{label} failed: HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise HueApiError(f"{label} failed: {exc}") from exc

        if not payload:
            return None

        parsed = json.loads(payload)
        if isinstance(parsed, list):
            errors = [item["error"] for item in parsed if isinstance(item, dict) and "error" in item]
            if errors:
                raise HueApiError(f"{label} returned Hue API error(s): {errors}")
        return parsed

    def lights(self) -> dict[str, Any]:
        return self.request("GET", "/lights")

    def v2_devices(self) -> Any:
        return self.v2_request("/device")

    def v2_lights(self) -> Any:
        return self.v2_request("/light")


def default_out_dir() -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return Path("research/hue-api-diffs") / f"action-capture-{stamp}-hue-app"


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def find_matching_lights(lights: dict[str, Any], args: argparse.Namespace) -> list[tuple[str, dict[str, Any]]]:
    matches: list[tuple[str, dict[str, Any]]] = []
    for light_id, light in lights.items():
        if args.id and light_id not in args.id:
            continue
        if args.uniqueid_prefix and not str(light.get("uniqueid", "")).lower().startswith(args.uniqueid_prefix.lower()):
            continue
        if args.modelid and light.get("modelid") != args.modelid:
            continue
        if args.manufacturer and light.get("manufacturername") != args.manufacturer:
            continue
        matches.append((light_id, light))
    return matches


def print_match(label: str, light_id: str, light: dict[str, Any]) -> None:
    certified = light.get("capabilities", {}).get("certified")
    streaming = light.get("capabilities", {}).get("streaming", {})
    print(
        f"{label}: id={light_id} uniqueid={light.get('uniqueid')} "
        f"modelid={light.get('modelid')} certified={certified} "
        f"streaming={streaming} name={light.get('name')!r}"
    )


def capture_serial(args: argparse.Namespace, log_path: Path) -> None:
    print(f"opening serial {args.port} @ {args.baud}; log={log_path}", flush=True)
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    with ser, log_path.open("w", encoding="utf-8", errors="replace", newline="\n") as log:
        time.sleep(0.2)
        if args.reset_input:
            ser.reset_input_buffer()

        ready = (
            f"CAPTURE_READY seconds={args.seconds}; "
            "change the Hue light in the app now.\n"
        )
        print(ready, end="", flush=True)
        log.write(ready)
        log.flush()

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            print(text, flush=True)
            log.write(text + "\n")
            log.flush()


def write_readme(path: Path, args: argparse.Namespace, matches: list[tuple[str, dict[str, Any]]]) -> None:
    lines = [
        "# Hue App Action Capture",
        "",
        f"- port: `{args.port}`",
        f"- baud: `{args.baud}`",
        f"- duration_seconds: `{args.seconds}`",
        "- hue_actions: manual app operation; no delete/search/discover/reset",
    ]
    if args.note:
        lines.append(f"- note: {args.note}")
    if matches:
        lines.append("- matched_lights:")
        for light_id, light in matches:
            lines.append(
                f"  - id `{light_id}`, uniqueid `{light.get('uniqueid')}`, "
                f"name `{light.get('name')}`, model `{light.get('modelid')}`"
            )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def make_parser() -> argparse.ArgumentParser:
    dotenv = load_dotenv(Path(".env"))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.getenv("HUE_BRIDGE_HOST") or dotenv.get("HUE_BRIDGE_HOST"))
    parser.add_argument("--username", default=os.getenv("HUE_API_KEY") or dotenv.get("HUE_API_KEY"))
    parser.add_argument("--verify-tls", action="store_true")
    parser.add_argument("--port", default=os.getenv("PORT", DEFAULT_PORT))
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--id", action="append", help="Specific Hue v1 light id to snapshot.")
    parser.add_argument("--uniqueid-prefix", default=DEFAULT_UNIQUEID_PREFIX)
    parser.add_argument("--modelid", default=DEFAULT_MODELID)
    parser.add_argument("--manufacturer", default=DEFAULT_MANUFACTURER)
    parser.add_argument("--reset-input", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--note", help="Short README note describing the manual action performed.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    if not args.host:
        raise SystemExit("Missing Hue bridge host. Set HUE_BRIDGE_HOST or pass --host.")
    if not args.username:
        raise SystemExit("Missing Hue API key. Set HUE_API_KEY or pass --username.")

    args.out_dir = args.out_dir or default_out_dir()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    client = HueClient(args.host, args.username, insecure=not args.verify_tls)

    before_lights = client.lights()
    matches = find_matching_lights(before_lights, args)
    if not matches:
        print("No matching Hue v1 light found for the capture filter.")
    for light_id, light in matches:
        print_match("matched-before", light_id, light)

    write_json(args.out_dir / "before-v1-lights.json", before_lights)
    write_json(args.out_dir / "before-v2-devices.json", client.v2_devices())
    write_json(args.out_dir / "before-v2-lights.json", client.v2_lights())
    write_readme(args.out_dir / "README.md", args, matches)

    capture_serial(args, args.out_dir / "serial-action.log")

    after_lights = client.lights()
    write_json(args.out_dir / "after-v1-lights.json", after_lights)
    write_json(args.out_dir / "after-v2-devices.json", client.v2_devices())
    write_json(args.out_dir / "after-v2-lights.json", client.v2_lights())

    for light_id, light in find_matching_lights(after_lights, args):
        print_match("matched-after", light_id, light)
    print(f"saved capture artifacts under {args.out_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HueApiError as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1) from exc
