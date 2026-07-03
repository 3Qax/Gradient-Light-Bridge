#!/usr/bin/env python3
"""Capture ESP32-C6 sniffer_probe serial output into a timestamped artifact."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import re
import subprocess
import sys
import time

import serial

from hue_light_cycle import HueApiError, HueClient, load_dotenv, print_lights, write_json


INTERESTING_PATTERNS = [
    re.compile(rb"MAC_RAW"),
    re.compile(rb"ZBOSS_SNIFF"),
    re.compile(rb"fc01", re.IGNORECASE),
    re.compile(rb"fc03", re.IGNORECASE),
    re.compile(rb"100b", re.IGNORECASE),
]


def default_out_dir() -> pathlib.Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return pathlib.Path("research/hue-api-diffs") / f"sniffer-capture-{stamp}-real-lcx004"


def make_hue_client(args: argparse.Namespace) -> HueClient | None:
    if not args.start_search and not args.delete_light_id and not args.snapshot_hue and not args.list_candidates:
        return None

    if not args.host:
        raise SystemExit("Missing Hue bridge host. Set HUE_BRIDGE_HOST or pass --host.")
    if not args.username:
        raise SystemExit("Missing Hue API key. Set HUE_API_KEY or pass --username.")

    return HueClient(args.host, args.username, insecure=not args.verify_tls)


def write_capture_readme(path: pathlib.Path, args: argparse.Namespace) -> None:
    hue_actions = []
    if args.delete_light_id:
        hue_actions.append(f"delete_light_id={','.join(args.delete_light_id)}")
    if args.start_search:
        hue_actions.append("start_search=true")
    if args.snapshot_hue:
        hue_actions.append("snapshot_hue=true")

    path.write_text(
        "# Sniffer Capture\n\n"
        f"- port: `{args.port}`\n"
        f"- baud: `{args.baud}`\n"
        f"- duration_seconds: `{args.seconds}`\n"
        f"- hue_actions: `{'; '.join(hue_actions) if hue_actions else 'none'}`\n"
        "- expected use: flash `firmware/sniffer_probe`, then commission or rejoin a real LCX004 while this runs.\n"
        "- note: the sniffer is passive; Hue API actions only remove/search from the bridge side.\n",
        encoding="utf-8",
    )


def list_candidates(client: HueClient, args: argparse.Namespace) -> int:
    lights = client.lights()
    matches = []
    for light_id, light in lights.items():
        if args.candidate_modelid and light.get("modelid") != args.candidate_modelid:
            continue
        if args.candidate_manufacturer and light.get("manufacturername") != args.candidate_manufacturer:
            continue
        if args.candidate_certified is not None:
            certified = light.get("capabilities", {}).get("certified")
            if certified is not args.candidate_certified:
                continue
        matches.append((light_id, light))

    if matches:
        print_lights(matches)
    else:
        print("No matching candidate lights.")
    return 0


def main() -> int:
    dotenv = load_dotenv(pathlib.Path(".env"))

    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--out-dir", type=pathlib.Path, default=default_out_dir())
    parser.add_argument("--host", default=os.getenv("HUE_BRIDGE_HOST") or dotenv.get("HUE_BRIDGE_HOST"))
    parser.add_argument("--username", default=os.getenv("HUE_API_KEY") or dotenv.get("HUE_API_KEY"))
    parser.add_argument("--verify-tls", action="store_true")
    parser.add_argument("--snapshot-hue", action="store_true", help="Save before/after Hue v1 snapshots.")
    parser.add_argument("--list-candidates", action="store_true", help="List candidate real LCX004 lights and exit.")
    parser.add_argument("--candidate-modelid", default="LCX004")
    parser.add_argument("--candidate-manufacturer", default="Signify Netherlands B.V.")
    parser.add_argument(
        "--candidate-certified",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Filter candidate list by Hue certified flag.",
    )
    parser.add_argument("--start-search", action="store_true", help="Start Hue bridge light search after serial capture opens.")
    parser.add_argument(
        "--reset-board",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="Reset the ESP32-C6 after opening serial, before Hue API actions.",
    )
    parser.add_argument(
        "--delete-light-id",
        action="append",
        help="Hue v1 light id to delete after serial capture opens. Repeatable.",
    )
    parser.add_argument(
        "--i-understand-delete-real-device",
        action="store_true",
        help="Required with --delete-light-id; prevents accidental deletion of real Hue lights.",
    )
    parser.add_argument("--settle-seconds", type=float, default=2.0, help="Delay between delete and search.")
    args = parser.parse_args()

    if args.delete_light_id and not args.i_understand_delete_real_device:
        raise SystemExit("--delete-light-id requires --i-understand-delete-real-device.")

    client = make_hue_client(args)

    if args.list_candidates:
        if client is None:
            raise SystemExit("Internal error: --list-candidates requires Hue client.")
        return list_candidates(client, args)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.out_dir / "serial-sniffer.log"
    interesting_path = args.out_dir / "interesting-lines.log"
    readme_path = args.out_dir / "README.md"

    write_capture_readme(readme_path, args)
    if client and args.snapshot_hue:
        write_json(args.out_dir / "before-v1-lights.json", client.lights())

    deadline = time.monotonic() + args.seconds
    seen_frames = 0
    seen_interesting = 0

    with serial.Serial(args.port, args.baud, timeout=0.5) as ser, raw_path.open("ab") as raw, interesting_path.open("ab") as interesting:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.5)
        if args.reset_board:
            ser.dtr = True
            ser.rts = True
            time.sleep(0.1)
            ser.dtr = False
            ser.rts = False
            time.sleep(0.5)
        else:
            ser.reset_input_buffer()

        if client and args.delete_light_id:
            for light_id in args.delete_light_id:
                print(f"deleting Hue light id={light_id} after sniffer opened")
                print(json.dumps(client.delete_light(light_id), indent=2, sort_keys=True))
            time.sleep(args.settle_seconds)

        if client and args.start_search:
            print("starting Hue light search after sniffer opened")
            print(json.dumps(client.start_search(), indent=2, sort_keys=True))

        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                continue
            raw.write(line)
            raw.flush()
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()

            if b"MAC_RAW" in line or b"ZBOSS_SNIFF" in line:
                seen_frames += 1
            if any(pattern.search(line) for pattern in INTERESTING_PATTERNS):
                interesting.write(line)
                interesting.flush()
                seen_interesting += 1

    print(f"saved sniffer capture under {args.out_dir}")
    print(f"frames/log lines with raw markers: {seen_frames}")
    print(f"interesting lines: {seen_interesting}")

    if client and args.snapshot_hue:
        write_json(args.out_dir / "after-v1-lights.json", client.lights())
        write_json(args.out_dir / "after-v1-new-lights.json", client.new_lights())

    if seen_frames:
        converter = pathlib.Path(__file__).with_name("sniffer_log_to_pcap.py")
        subprocess.run(
            [
                sys.executable,
                str(converter),
                str(raw_path),
                "--out",
                str(args.out_dir / "mac-raw.pcap"),
            ],
            check=False,
        )
        summarizer = pathlib.Path(__file__).with_name("summarize_sniffer_log.py")
        subprocess.run(
            [
                sys.executable,
                str(summarizer),
                str(raw_path),
                "--csv",
                str(args.out_dir / "mac-summary.csv"),
                "--md",
                str(args.out_dir / "mac-summary.md"),
            ],
            check=False,
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HueApiError as exc:
        raise SystemExit(str(exc)) from exc
