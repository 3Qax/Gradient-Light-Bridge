#!/usr/bin/env python3
"""Convert sniffer_probe serial logs into a pcap file.

The firmware prints lines like:

    MAC_RAW ts_us=123456 len=10 channel=25 rssi=-60 lqi=255 psdu=...

This tool extracts complete MAC_RAW PSDUs and writes a pcap that Wireshark can
open as IEEE 802.15.4 frames. ESP-IDF's direct receive buffer includes the
802.15.4 FCS. By default this tool strips those two trailer bytes and writes a
no-FCS pcap, which is what tshark's Zigbee security dissector expects for
decryption.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
from dataclasses import dataclass


LINKTYPE_IEEE802_15_4_WITHFCS = 195
LINKTYPE_IEEE802_15_4_NOFCS = 230

MAC_RAW_RE = re.compile(
    r"MAC_RAW\s+ts_us=(?P<ts_us>\d+)\s+len=(?P<len>\d+)"
    r"(?:\s+channel=(?P<channel>\d+))?"
    r"(?:\s+rssi=(?P<rssi>-?\d+))?"
    r"(?:\s+lqi=(?P<lqi>\d+))?"
    r"\s+psdu=(?P<psdu>[0-9a-fA-F]+)(?P<truncated>\.\.\.\(\+\d+\))?"
)


@dataclass(frozen=True)
class Frame:
    ts_us: int
    data: bytes
    channel: int | None = None
    rssi: int | None = None
    lqi: int | None = None


def parse_frames(log_path: pathlib.Path, strip_fcs: bool = True) -> tuple[list[Frame], int]:
    frames: list[Frame] = []
    skipped = 0

    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = MAC_RAW_RE.search(line)
        if not match:
            continue
        if match.group("truncated"):
            skipped += 1
            continue

        declared_len = int(match.group("len"))
        psdu = match.group("psdu")
        if len(psdu) % 2:
            skipped += 1
            continue
        try:
            data = bytes.fromhex(psdu)
        except ValueError:
            skipped += 1
            continue
        if len(data) != declared_len:
            skipped += 1
            continue
        lqi = int(match.group("lqi")) if match.group("lqi") else None
        if strip_fcs:
            if len(data) < 2:
                skipped += 1
                continue
            data = data[:-2]

        frames.append(
            Frame(
                ts_us=int(match.group("ts_us")),
                data=data,
                channel=int(match.group("channel")) if match.group("channel") else None,
                rssi=int(match.group("rssi")) if match.group("rssi") else None,
                lqi=lqi,
            )
        )

    return frames, skipped


def write_pcap(path: pathlib.Path, frames: list[Frame], linktype: int) -> None:
    with path.open("wb") as f:
        f.write(
            struct.pack(
                "<IHHIIII",
                0xA1B2C3D4,  # magic, little endian
                2,
                4,
                0,
                0,
                262144,
                linktype,
            )
        )
        for frame in frames:
            sec = frame.ts_us // 1_000_000
            usec = frame.ts_us % 1_000_000
            f.write(struct.pack("<IIII", sec, usec, len(frame.data), len(frame.data)))
            f.write(frame.data)


def write_summary(path: pathlib.Path, frames: list[Frame], skipped: int, linktype: int) -> None:
    channels = sorted({frame.channel for frame in frames if frame.channel is not None})
    with path.open("w", encoding="utf-8") as f:
        f.write("# Sniffer PCAP Summary\n\n")
        f.write(f"- frames: `{len(frames)}`\n")
        f.write(f"- skipped_lines: `{skipped}`\n")
        f.write(f"- linktype: `{linktype}`\n")
        if channels:
            f.write(f"- channels: `{','.join(str(ch) for ch in channels)}`\n")
        if frames:
            f.write(f"- first_ts_us: `{frames[0].ts_us}`\n")
            f.write(f"- last_ts_us: `{frames[-1].ts_us}`\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path, help="serial-sniffer.log from capture_sniffer_probe.py")
    parser.add_argument("--out", type=pathlib.Path, help="output pcap path")
    parser.add_argument(
        "--with-fcs",
        action="store_true",
        help="preserve the 802.15.4 FCS and write pcap link type 195",
    )
    parser.add_argument(
        "--keep-trailing-lqi",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--summary",
        type=pathlib.Path,
        help="optional markdown summary path; defaults next to the pcap",
    )
    args = parser.parse_args()

    out = args.out or args.log.with_suffix(".pcap")
    linktype = LINKTYPE_IEEE802_15_4_WITHFCS if args.with_fcs else LINKTYPE_IEEE802_15_4_NOFCS
    frames, skipped = parse_frames(args.log, strip_fcs=not args.with_fcs)

    write_pcap(out, frames, linktype)
    summary = args.summary or out.with_suffix(".summary.md")
    write_summary(summary, frames, skipped, linktype)

    print(f"wrote {len(frames)} frames to {out}")
    if skipped:
        print(f"skipped {skipped} incomplete or malformed MAC_RAW lines")
    print(f"wrote summary to {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
