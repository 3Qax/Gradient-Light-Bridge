#!/usr/bin/env python3
"""Summarize MAC_RAW frames from sniffer_probe serial logs.

This is intentionally lightweight and stdlib-only. It decodes the IEEE 802.15.4
MAC header enough to show frame type, PAN IDs, source/destination addresses, and
candidate payload markers. Zigbee NWK/APS/ZCL payloads are often encrypted, so
this tool does not pretend to fully decode commissioning traffic.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
from dataclasses import dataclass

from sniffer_log_to_pcap import Frame, parse_frames


FRAME_TYPES = {
    0: "beacon",
    1: "data",
    2: "ack",
    3: "mac_cmd",
    4: "reserved",
    5: "multipurpose",
    6: "fragment",
    7: "extended",
}

ADDR_MODES = {
    0: "none",
    2: "short",
    3: "extended",
}

CANDIDATE_MARKERS = {
    "signify_mfg_100b_le": bytes.fromhex("0b10"),
    "fc01_le": bytes.fromhex("01fc"),
    "fc03_le": bytes.fromhex("03fc"),
    "fc04_le": bytes.fromhex("04fc"),
    "ota_0019_le": bytes.fromhex("1900"),
    "model_lcx004_ascii": b"LCX004",
    "signify_oui": bytes.fromhex("001788"),
    "signify_oui_reversed": bytes.fromhex("881700"),
}


@dataclass(frozen=True)
class MacSummary:
    index: int
    ts_us: int
    length: int
    channel: int | None
    rssi: int | None
    lqi: int | None
    frame_type: str
    seq: int | None
    security: bool
    ack_request: bool
    pan_compression: bool
    dest_pan: str | None
    dest_addr: str | None
    src_pan: str | None
    src_addr: str | None
    payload_len: int
    markers: list[str]
    parse_note: str = ""


def le_hex(data: bytes) -> str:
    return data[::-1].hex()


def parse_addr(data: bytes, offset: int, mode: int) -> tuple[str | None, int, str]:
    if mode == 0:
        return None, offset, ""
    if mode == 2:
        if offset + 2 > len(data):
            return None, offset, "truncated_short_addr"
        return "0x" + le_hex(data[offset:offset + 2]), offset + 2, ""
    if mode == 3:
        if offset + 8 > len(data):
            return None, offset, "truncated_ext_addr"
        return ":".join(f"{b:02x}" for b in data[offset:offset + 8][::-1]), offset + 8, ""
    return None, offset, f"unsupported_addr_mode_{mode}"


def parse_mac(index: int, frame: Frame) -> MacSummary:
    data = frame.data
    if len(data) < 3:
        return MacSummary(
            index=index,
            ts_us=frame.ts_us,
            length=len(data),
            channel=frame.channel,
            rssi=frame.rssi,
            lqi=frame.lqi,
            frame_type="truncated",
            seq=None,
            security=False,
            ack_request=False,
            pan_compression=False,
            dest_pan=None,
            dest_addr=None,
            src_pan=None,
            src_addr=None,
            payload_len=0,
            markers=[],
            parse_note="shorter_than_fcf_seq",
        )

    fcf = data[0] | (data[1] << 8)
    frame_type_id = fcf & 0x7
    security = bool(fcf & (1 << 3))
    ack_request = bool(fcf & (1 << 5))
    pan_compression = bool(fcf & (1 << 6))
    dest_mode = (fcf >> 10) & 0x3
    version = (fcf >> 12) & 0x3
    src_mode = (fcf >> 14) & 0x3

    offset = 3
    dest_pan = None
    src_pan = None
    dest_addr = None
    src_addr = None
    notes: list[str] = []

    if dest_mode:
        if offset + 2 <= len(data):
            dest_pan = "0x" + le_hex(data[offset:offset + 2])
            offset += 2
        else:
            notes.append("truncated_dest_pan")

    dest_addr, offset, note = parse_addr(data, offset, dest_mode)
    if note:
        notes.append(note)

    if src_mode:
        if pan_compression and dest_pan is not None:
            src_pan = dest_pan
        elif offset + 2 <= len(data):
            src_pan = "0x" + le_hex(data[offset:offset + 2])
            offset += 2
        else:
            notes.append("truncated_src_pan")

    src_addr, offset, note = parse_addr(data, offset, src_mode)
    if note:
        notes.append(note)

    # This parser intentionally stops before Aux Security Header / IE parsing.
    payload = data[offset:] if offset <= len(data) else b""
    markers = [name for name, marker in CANDIDATE_MARKERS.items() if marker in data]
    if version:
        notes.append(f"frame_version_{version}")

    return MacSummary(
        index=index,
        ts_us=frame.ts_us,
        length=len(data),
        channel=frame.channel,
        rssi=frame.rssi,
        lqi=frame.lqi,
        frame_type=FRAME_TYPES.get(frame_type_id, f"unknown_{frame_type_id}"),
        seq=data[2],
        security=security,
        ack_request=ack_request,
        pan_compression=pan_compression,
        dest_pan=dest_pan,
        dest_addr=dest_addr,
        src_pan=src_pan,
        src_addr=src_addr,
        payload_len=len(payload),
        markers=markers,
        parse_note=";".join(notes),
    )


def write_csv(path: pathlib.Path, summaries: list[MacSummary]) -> None:
    fieldnames = [
        "index",
        "ts_us",
        "length",
        "channel",
        "rssi",
        "lqi",
        "frame_type",
        "seq",
        "security",
        "ack_request",
        "pan_compression",
        "dest_pan",
        "dest_addr",
        "src_pan",
        "src_addr",
        "payload_len",
        "markers",
        "parse_note",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for summary in summaries:
            row = summary.__dict__.copy()
            row["markers"] = ",".join(summary.markers)
            writer.writerow(row)


def write_markdown(path: pathlib.Path, summaries: list[MacSummary], skipped: int) -> None:
    by_type: dict[str, int] = {}
    by_addr: dict[str, int] = {}
    marker_hits: list[MacSummary] = []
    for summary in summaries:
        by_type[summary.frame_type] = by_type.get(summary.frame_type, 0) + 1
        for addr in (summary.src_addr, summary.dest_addr):
            if addr:
                by_addr[addr] = by_addr.get(addr, 0) + 1
        if summary.markers:
            marker_hits.append(summary)

    with path.open("w", encoding="utf-8") as f:
        f.write("# Sniffer MAC Summary\n\n")
        f.write(f"- frames: `{len(summaries)}`\n")
        f.write(f"- skipped_mac_raw_lines: `{skipped}`\n")
        if summaries:
            f.write(f"- first_ts_us: `{summaries[0].ts_us}`\n")
            f.write(f"- last_ts_us: `{summaries[-1].ts_us}`\n")
        f.write("\n## Frame Types\n\n")
        for frame_type, count in sorted(by_type.items()):
            f.write(f"- `{frame_type}`: `{count}`\n")
        f.write("\n## Top Addresses\n\n")
        for addr, count in sorted(by_addr.items(), key=lambda item: item[1], reverse=True)[:20]:
            f.write(f"- `{addr}`: `{count}`\n")
        f.write("\n## Candidate Marker Hits\n\n")
        if not marker_hits:
            f.write("- none\n")
        for summary in marker_hits[:50]:
            f.write(
                f"- frame `{summary.index}` ts_us=`{summary.ts_us}` "
                f"type=`{summary.frame_type}` src=`{summary.src_addr}` "
                f"dst=`{summary.dest_addr}` markers=`{','.join(summary.markers)}`\n"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path, help="serial-sniffer.log from capture_sniffer_probe.py")
    parser.add_argument("--csv", type=pathlib.Path, help="output CSV path")
    parser.add_argument("--md", type=pathlib.Path, help="output markdown summary path")
    args = parser.parse_args()

    summaries = []
    frames, skipped = parse_frames(args.log)
    for index, frame in enumerate(frames, start=1):
        summaries.append(parse_mac(index, frame))

    csv_path = args.csv or args.log.with_name("mac-summary.csv")
    md_path = args.md or args.log.with_name("mac-summary.md")
    write_csv(csv_path, summaries)
    write_markdown(md_path, summaries, skipped)

    print(f"wrote {len(summaries)} frame summaries to {csv_path}")
    print(f"wrote markdown summary to {md_path}")
    if skipped:
        print(f"skipped {skipped} incomplete or malformed MAC_RAW lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
