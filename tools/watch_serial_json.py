#!/usr/bin/env python3
"""Watch ESP32 serial logs and print parsed DATA JSON events.

The firmware emits machine-readable lines as ``DATA: {...}``. This script keeps
the terminal output focused on those parsed JSON objects while optionally saving
the same JSONL stream for later labeling.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

import serial
from serial import SerialException


DEFAULT_BAUD = 115200
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_OUT_DIR = Path("research/hue-api-diffs/live-labels")
ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def default_out_path() -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return DEFAULT_OUT_DIR / f"{stamp}-data.jsonl"


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_data_line(line: str) -> dict[str, Any] | None:
    clean = strip_ansi(line).strip()
    if "DATA:" not in clean:
        return None

    payload = clean.split("DATA:", 1)[1].strip()
    if not payload:
        return None

    parsed = json.loads(payload)
    if not isinstance(parsed, dict):
        return {"value": parsed}
    return parsed


def systemctl_user(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["systemctl", "--user", *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def service_is_active(service: str) -> bool:
    result = systemctl_user("is-active", service)
    return result.returncode == 0 and result.stdout.strip() == "active"


def stop_daemon_for_capture(service: str, enabled: bool) -> bool:
    if not enabled or not service_is_active(service):
        return False

    print(f"stopping {service} while serial capture owns the port", file=sys.stderr, flush=True)
    result = systemctl_user("stop", service)
    if result.returncode != 0:
        raise RuntimeError(f"failed to stop {service}: {result.stderr.strip()}")
    return True


def restart_daemon_after_capture(service: str, should_restart: bool) -> None:
    if not should_restart:
        return

    print(f"restarting {service}", file=sys.stderr, flush=True)
    result = systemctl_user("start", service)
    if result.returncode != 0:
        print(f"failed to restart {service}: {result.stderr.strip()}", file=sys.stderr, flush=True)


def open_serial(port: str, baud: int, timeout: float, exclusive: bool) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = timeout
    ser.exclusive = exclusive
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=os.getenv("PORT", DEFAULT_PORT))
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument(
        "--out",
        type=Path,
        default=default_out_path(),
        help="JSONL output path. Use --no-save to disable file output.",
    )
    parser.add_argument("--no-save", action="store_true", help="Only print to stdout.")
    parser.add_argument("--duration", type=float, default=0, help="Seconds to run; 0 means until Ctrl-C.")
    parser.add_argument("--timeout", type=float, default=0.2, help="Serial read timeout in seconds.")
    parser.add_argument("--reset-input", action="store_true", help="Drop buffered serial input after opening.")
    parser.add_argument("--raw", action="store_true", help="Echo all serial lines to stderr for debugging.")
    parser.add_argument("--no-time", action="store_true", help="Do not add elapsed _t seconds to output JSON.")
    parser.add_argument(
        "--no-manage-daemon",
        action="store_true",
        help="Do not stop/restart argb-to-hue.service around the serial capture.",
    )
    parser.add_argument(
        "--no-restart-daemon",
        action="store_true",
        help="If this script stops the daemon, leave it stopped on exit.",
    )
    parser.add_argument("--daemon-service", default="argb-to-hue.service", help="systemd --user service owning the serial port.")
    parser.add_argument("--no-exclusive", action="store_true", help="Do not request exclusive serial access.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)

    out_file = None
    if not args.no_save:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        out_file = args.out.open("w", encoding="utf-8", newline="\n")

    start = time.monotonic()
    deadline = start + args.duration if args.duration > 0 else None
    stopped_daemon = False

    try:
        stopped_daemon = stop_daemon_for_capture(args.daemon_service, not args.no_manage_daemon)

        with open_serial(args.port, args.baud, args.timeout, not args.no_exclusive) as ser:
            if args.reset_input:
                ser.reset_input_buffer()

            print(
                f"watching {args.port} @ {args.baud}; "
                f"jsonl={args.out if out_file else 'disabled'}",
                file=sys.stderr,
                flush=True,
            )

            while deadline is None or time.monotonic() < deadline:
                try:
                    raw = ser.readline()
                except SerialException as exc:
                    print(
                        f"serial read failed: {exc}. "
                        "If another process owns the port, stop it or rerun without --no-manage-daemon.",
                        file=sys.stderr,
                        flush=True,
                    )
                    return 2
                if not raw:
                    continue

                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if args.raw:
                    print(line, file=sys.stderr, flush=True)

                try:
                    event = parse_data_line(line)
                except json.JSONDecodeError as exc:
                    print(f"skipping malformed DATA line: {exc}: {line}", file=sys.stderr, flush=True)
                    continue

                if event is None:
                    continue

                if not args.no_time:
                    event["_t"] = round(time.monotonic() - start, 3)

                encoded = json.dumps(event, separators=(",", ":"))
                print(encoded, flush=True)
                if out_file:
                    out_file.write(encoded + "\n")
                    out_file.flush()
    except KeyboardInterrupt:
        return 130
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr, flush=True)
        return 2
    finally:
        if out_file:
            out_file.close()
        restart_daemon_after_capture(args.daemon_service, stopped_daemon and not args.no_restart_daemon)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
