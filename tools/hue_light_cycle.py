#!/usr/bin/env python3
"""Remove and rediscover Hue lights via the local Hue v1 API.

Credentials are read from HUE_BRIDGE_HOST and HUE_API_KEY unless supplied on
the command line. This script intentionally uses only the Python standard
library so it can run from a fresh checkout.
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


DEFAULT_TIMEOUT_SECONDS = 30
DEFAULT_SEARCH_POLL_SECONDS = 90


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
        value = value.strip().strip("'\"")
        if key:
            values[key] = value
    return values


@dataclass(frozen=True)
class HueClient:
    host: str
    username: str
    insecure: bool = True
    timeout: int = DEFAULT_TIMEOUT_SECONDS

    @property
    def base_url(self) -> str:
        return f"https://{self.host}/api/{self.username}"

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

        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=context) as resp:
                payload = resp.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise HueApiError(f"{method} {path} failed: HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise HueApiError(f"{method} {path} failed: {exc}") from exc

        if not payload:
            return None

        parsed = json.loads(payload)
        errors = [item["error"] for item in parsed if isinstance(item, dict) and "error" in item] \
            if isinstance(parsed, list) else []
        if errors:
            raise HueApiError(f"{method} {path} returned Hue API error(s): {errors}")
        return parsed

    def lights(self) -> dict[str, Any]:
        return self.request("GET", "/lights")

    def light(self, light_id: str) -> dict[str, Any]:
        return self.request("GET", f"/lights/{light_id}")

    def config(self) -> dict[str, Any]:
        return self.request("GET", "/config")

    def set_light_state(self, light_id: str, state: dict[str, Any]) -> Any:
        return self.request("PUT", f"/lights/{light_id}/state", state)

    def delete_light(self, light_id: str) -> Any:
        return self.request("DELETE", f"/lights/{light_id}")

    def start_search(self) -> Any:
        return self.request("POST", "/lights", {})

    def new_lights(self) -> dict[str, Any]:
        return self.request("GET", "/lights/new")


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def match_lights(lights: dict[str, Any], args: argparse.Namespace) -> list[tuple[str, dict[str, Any]]]:
    matches: list[tuple[str, dict[str, Any]]] = []
    for light_id, light in lights.items():
        uniqueid = str(light.get("uniqueid", ""))
        name = str(light.get("name", ""))
        modelid = str(light.get("modelid", ""))
        manufacturer = str(light.get("manufacturername", ""))

        if args.id and light_id not in args.id:
            continue
        if args.uniqueid_prefix and not uniqueid.lower().startswith(args.uniqueid_prefix.lower()):
            continue
        if args.name_contains and args.name_contains.lower() not in name.lower():
            continue
        if args.modelid and modelid != args.modelid:
            continue
        if args.manufacturer and manufacturer != args.manufacturer:
            continue
        if args.uncertified_only and light.get("capabilities", {}).get("certified") is not False:
            continue

        if any([args.id, args.uniqueid_prefix, args.name_contains, args.modelid,
                args.manufacturer, args.uncertified_only]):
            matches.append((light_id, light))

    return matches


def print_lights(lights: list[tuple[str, dict[str, Any]]]) -> None:
    for light_id, light in lights:
        certified = light.get("capabilities", {}).get("certified")
        print(
            f"{light_id:>3}  "
            f"{light.get('uniqueid', '-'):<26}  "
            f"{light.get('manufacturername', '-'):<24}  "
            f"{light.get('modelid', '-'):<14}  "
            f"certified={certified!s:<5}  "
            f"{light.get('name', '-')}"
        )


def confirm_or_die(args: argparse.Namespace, matched: list[tuple[str, dict[str, Any]]]) -> None:
    if args.yes:
        return
    print_lights(matched)
    reply = input("Delete these Hue light(s)? Type DELETE to continue: ")
    if reply != "DELETE":
        raise SystemExit("Aborted")


def poll_search(client: HueClient, seconds: int) -> dict[str, Any]:
    deadline = time.monotonic() + seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.new_lights()
        print(json.dumps(last, indent=2, sort_keys=True))
        if any(key != "lastscan" for key in last):
            break
        time.sleep(5)
    return last


def add_common_match_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--id", action="append", help="Hue v1 light id to match. Repeatable.")
    parser.add_argument("--uniqueid-prefix", help="Match Hue uniqueid prefix, e.g. 00:17:88:01:0b")
    parser.add_argument("--name-contains", help="Match lights by case-insensitive name substring.")
    parser.add_argument("--modelid", help="Match exact Hue modelid.")
    parser.add_argument("--manufacturer", help="Match exact manufacturername.")
    parser.add_argument("--uncertified-only", action="store_true", help="Only match certified=false lights.")


def make_parser() -> argparse.ArgumentParser:
    dotenv = load_dotenv(Path(".env"))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default=os.getenv("HUE_BRIDGE_HOST") or dotenv.get("HUE_BRIDGE_HOST"),
        help="Hue bridge host/IP.",
    )
    parser.add_argument(
        "--username",
        default=os.getenv("HUE_API_KEY") or dotenv.get("HUE_API_KEY"),
        help="Hue v1 API username/key.",
    )
    parser.add_argument("--verify-tls", action="store_true", help="Verify bridge TLS certificate.")
    parser.add_argument("--snapshot-dir", type=Path, help="Directory for before/after JSON snapshots.")

    sub = parser.add_subparsers(dest="command", required=True)

    list_parser = sub.add_parser("list", help="List matched lights.")
    add_common_match_args(list_parser)

    get_parser = sub.add_parser("get", help="Print one light's full v1 API JSON.")
    get_parser.add_argument("id", help="Hue v1 light id.")

    config_parser = sub.add_parser("config", help="Print selected bridge config fields.")
    config_parser.add_argument("--json", action="store_true", help="Print full bridge config JSON.")

    state_parser = sub.add_parser("set-state", help="PUT a JSON state object to one light.")
    state_parser.add_argument("id", help="Hue v1 light id.")
    state_parser.add_argument("json", help='State JSON, e.g. \'{"on":true,"bri":254}\'.')

    delete_parser = sub.add_parser("delete", help="Delete matched lights.")
    add_common_match_args(delete_parser)
    delete_parser.add_argument("-y", "--yes", action="store_true", help="Delete without interactive prompt.")

    search_parser = sub.add_parser("search", help="Start Hue bridge light search and poll results.")
    search_parser.add_argument("--poll-seconds", type=int, default=DEFAULT_SEARCH_POLL_SECONDS)

    cycle_parser = sub.add_parser("cycle", help="Delete matched lights, then start/poll rediscovery.")
    add_common_match_args(cycle_parser)
    cycle_parser.add_argument("-y", "--yes", action="store_true", help="Delete without interactive prompt.")
    cycle_parser.add_argument("--poll-seconds", type=int, default=DEFAULT_SEARCH_POLL_SECONDS)
    cycle_parser.add_argument("--settle-seconds", type=int, default=2, help="Delay after delete before search.")

    return parser


def require_credentials(args: argparse.Namespace) -> None:
    if not args.host:
        raise SystemExit("Missing Hue bridge host. Set HUE_BRIDGE_HOST or pass --host.")
    if not args.username:
        raise SystemExit("Missing Hue API key. Set HUE_API_KEY or pass --username.")


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    require_credentials(args)

    client = HueClient(args.host, args.username, insecure=not args.verify_tls)

    try:
        if args.command == "get":
            print(json.dumps(client.light(args.id), indent=2, sort_keys=True))
            return 0

        if args.command == "config":
            config = client.config()
            if args.json:
                print(json.dumps(config, indent=2, sort_keys=True))
            else:
                for key in ("name", "bridgeid", "mac", "ipaddress", "zigbeechannel", "panid"):
                    print(f"{key}: {config.get(key)}")
            return 0

        if args.command == "set-state":
            state = json.loads(args.json)
            if not isinstance(state, dict):
                raise SystemExit("State JSON must be an object.")
            print(json.dumps(client.set_light_state(args.id, state), indent=2, sort_keys=True))
            return 0

        if args.command == "search":
            result = client.start_search()
            print(json.dumps(result, indent=2, sort_keys=True))
            new_lights = poll_search(client, args.poll_seconds)
            if args.snapshot_dir:
                write_json(args.snapshot_dir / "new-lights.json", new_lights)
            return 0

        lights = client.lights()
        if args.snapshot_dir:
            write_json(args.snapshot_dir / "lights-before.json", lights)

        matched = match_lights(lights, args)
        if args.command == "list":
            print_lights(matched)
            return 0

        if not matched:
            print("No matching lights.")
            return 0

        confirm_or_die(args, matched)
        for light_id, light in matched:
            print(f"Deleting light {light_id}: {light.get('name')} ({light.get('uniqueid')})")
            print(json.dumps(client.delete_light(light_id), indent=2, sort_keys=True))

        if args.command == "delete":
            return 0

        time.sleep(args.settle_seconds)
        print("Starting Hue light search...")
        print(json.dumps(client.start_search(), indent=2, sort_keys=True))
        new_lights = poll_search(client, args.poll_seconds)

        if args.snapshot_dir:
            write_json(args.snapshot_dir / "new-lights.json", new_lights)
            write_json(args.snapshot_dir / "lights-after.json", client.lights())
        return 0
    except HueApiError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
