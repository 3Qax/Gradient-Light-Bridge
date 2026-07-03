#!/usr/bin/env python3
"""Capture a Hue bridge rediscovery attempt for the ESP32 fake light.

The script coordinates the three things that are otherwise easy to get out of
order while debugging product classification:

1. remove the current fake Hue light from the bridge,
2. start a Hue bridge light search,
3. send ``discover`` to the ESP32 serial CLI and capture firmware logs.

Credentials are read from ``HUE_BRIDGE_HOST`` and ``HUE_API_KEY`` or from the
repo-local ``.env`` file. The API key is never printed.
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
from pathlib import Path
from typing import Any

import serial


DEFAULT_BAUD = 115200
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_SECONDS = 90
DEFAULT_UNIQUEID_PREFIX = "00:17:88:01:0b"
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
        values[key.strip()] = value.strip().strip("'\"")
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

    def delete_light(self, light_id: str) -> Any:
        return self.request("DELETE", f"/lights/{light_id}")

    def start_search(self) -> Any:
        return self.request("POST", "/lights", {})

    def new_lights(self) -> dict[str, Any]:
        return self.request("GET", "/lights/new")

    def v2_devices(self) -> Any:
        return self.v2_request("/device")

    def v2_lights(self) -> Any:
        return self.v2_request("/light")


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def find_fake_lights(lights: dict[str, Any], args: argparse.Namespace) -> list[tuple[str, dict[str, Any]]]:
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
        if args.uncertified_only and light.get("capabilities", {}).get("certified") is not False:
            continue
        matches.append((light_id, light))
    return matches


def print_match(label: str, light_id: str, light: dict[str, Any]) -> None:
    certified = light.get("capabilities", {}).get("certified")
    print(
        f"{label}: id={light_id} uniqueid={light.get('uniqueid')} "
        f"modelid={light.get('modelid')} certified={certified} "
        f"productname={light.get('productname')}"
    )


def read_serial_until(ser: serial.Serial, log_path: Path, deadline: float) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            log.write(text)
            log.flush()


def main(argv: list[str] | None = None) -> int:
    dotenv = load_dotenv(Path(".env"))

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.getenv("HUE_BRIDGE_HOST") or dotenv.get("HUE_BRIDGE_HOST"))
    parser.add_argument("--username", default=os.getenv("HUE_API_KEY") or dotenv.get("HUE_API_KEY"))
    parser.add_argument("--verify-tls", action="store_true")
    parser.add_argument("--port", default=os.getenv("PORT", DEFAULT_PORT))
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    parser.add_argument("--out-dir", type=Path, default=Path("research/hue-api-diffs/discovery-capture"))
    parser.add_argument("--id", action="append", help="Specific v1 light id to delete before capture.")
    parser.add_argument("--uniqueid-prefix", default=DEFAULT_UNIQUEID_PREFIX)
    parser.add_argument("--modelid", default=DEFAULT_MODELID)
    parser.add_argument("--manufacturer", default=DEFAULT_MANUFACTURER)
    parser.add_argument("--uncertified-only", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--keep-existing", action="store_true", help="Do not delete matched lights before search.")
    args = parser.parse_args(argv)

    if not args.host:
        raise SystemExit("Missing Hue bridge host. Set HUE_BRIDGE_HOST or pass --host.")
    if not args.username:
        raise SystemExit("Missing Hue API key. Set HUE_API_KEY or pass --username.")

    client = HueClient(args.host, args.username, insecure=not args.verify_tls)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    before_lights = client.lights()
    before_matches = find_fake_lights(before_lights, args)
    write_json(args.out_dir / "before-v1-lights.json", before_lights)
    write_json(args.out_dir / "before-v2-devices.json", client.v2_devices())
    write_json(args.out_dir / "before-v2-lights.json", client.v2_lights())

    if before_matches:
        for light_id, light in before_matches:
            print_match("matched-before", light_id, light)
    else:
        print("No existing fake light matched the delete filter.")

    if not args.keep_existing:
        for light_id, _light in before_matches:
            client.delete_light(light_id)
            print(f"deleted-before: id={light_id}")
        time.sleep(2)

    print("starting Hue light search")
    client.start_search()

    log_path = args.out_dir / "serial-discovery.log"
    print(f"opening serial {args.port} @ {args.baud}; log={log_path}")
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.write(b"discover\n")
        ser.flush()
        print("sent serial command: discover")
        read_serial_until(ser, log_path, time.monotonic() + args.seconds)

    after_lights = client.lights()
    new_lights = client.new_lights()
    after_matches = find_fake_lights(after_lights, args)
    write_json(args.out_dir / "after-v1-lights.json", after_lights)
    write_json(args.out_dir / "after-v1-new-lights.json", new_lights)
    write_json(args.out_dir / "after-v2-devices.json", client.v2_devices())
    write_json(args.out_dir / "after-v2-lights.json", client.v2_lights())

    for light_id, light in after_matches:
        print_match("matched-after", light_id, light)
    print(f"saved capture artifacts under {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
