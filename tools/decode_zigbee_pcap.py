#!/usr/bin/env python3
"""Decode a sniffer pcap with tshark into Zigbee transcript artifacts.

The sniffer pcap contains raw IEEE 802.15.4 frames. tshark can decode MAC/NWK
metadata without keys, but APS/ZCL read/response details require a usable
Zigbee network key or a decryptable key-transport event. This tool keeps key
bytes out of generated artifacts and writes only source labels.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable


DEFAULT_KEY_ENVS = (
    "HUE_ZIGBEE_NWK_KEY",
    "HUE_ZIGBEE_TC_LINK_KEY",
    "HUE_ZLL_MASTER_KEY",
    "HUE_ZLL_LINK_KEY",
)

FIELDS: tuple[tuple[str, str], ...] = (
    ("frame", "frame.number"),
    ("time_s", "frame.time_relative"),
    ("protocol", "_ws.col.Protocol"),
    ("info", "_ws.col.Info"),
    ("wpan_cmd", "wpan.cmd"),
    ("wpan_src64", "wpan.src64"),
    ("wpan_dst64", "wpan.dst64"),
    ("wpan_src16", "wpan.src16"),
    ("wpan_dst16", "wpan.dst16"),
    ("wpan_association_short", "wpan.asoc.addr"),
    ("nwk_src", "zbee_nwk.src"),
    ("nwk_dst", "zbee_nwk.dst"),
    ("nwk_src64", "zbee_nwk.src64"),
    ("nwk_dst64", "zbee_nwk.dst64"),
    ("nwk_security", "zbee_nwk.security"),
    ("security_key_id", "zbee.sec.key_id"),
    ("security_src64", "zbee.sec.src64"),
    ("decryption_key_label", "zbee.sec.decryption_key"),
    ("aps_type", "zbee_aps.type"),
    ("aps_src_ep", "zbee_aps.src"),
    ("aps_dst_ep", "zbee_aps.dst"),
    ("aps_cluster", "zbee_aps.cluster"),
    ("aps_profile", "zbee_aps.profile"),
    ("aps_cmd_id", "zbee_aps.cmd.id"),
    ("aps_cmd_key_type", "zbee_aps.cmd.key_type"),
    ("aps_cmd_device", "zbee_aps.cmd.device"),
    ("zcl_mfg_code", "zbee_zcl.cmd.mc"),
    ("zcl_tsn", "zbee_zcl.cmd.tsn"),
    ("zcl_cmd_id", "zbee_zcl.cmd.id"),
    ("zcl_rsp_to_cmd", "zbee_zcl.cmd.id.rsp"),
    ("zcl_attr_id", "zbee_zcl.attr.id"),
    ("zcl_attr_status", "zbee_zcl.attr.status"),
    ("zcl_attr_type", "zbee_zcl.attr.data.type"),
    ("zcl_attr_uint8", "zbee_zcl.attr.uint8"),
    ("zcl_attr_uint16", "zbee_zcl.attr.uint16"),
    ("zcl_attr_uint24", "zbee_zcl.attr.uint24"),
    ("zcl_attr_uint32", "zbee_zcl.attr.uint32"),
    ("zcl_attr_bitmap8", "zbee_zcl.attr.bitmap8"),
    ("zcl_attr_bitmap16", "zbee_zcl.attr.bitmap16"),
    ("zcl_attr_bitmap32", "zbee_zcl.attr.bitmap32"),
    ("zcl_attr_str", "zbee_zcl.attr.str"),
    ("zcl_attr_ostr", "zbee_zcl.attr.ostr"),
    ("zcl_attr_bytes", "zbee_zcl.attr.bytes"),
)

FIELD_BY_TSHARK = {field: alias for alias, field in FIELDS}
TSHARK_FIELDS = [field for _, field in FIELDS]
CSV_FIELDS = [alias for alias, _ in FIELDS]
ZCL_VALUE_FIELDS = (
    "zcl_attr_uint8",
    "zcl_attr_uint16",
    "zcl_attr_uint24",
    "zcl_attr_uint32",
    "zcl_attr_bitmap8",
    "zcl_attr_bitmap16",
    "zcl_attr_bitmap32",
    "zcl_attr_str",
    "zcl_attr_ostr",
    "zcl_attr_bytes",
)
KEYLIKE_ZCL_REDACTION = "[redacted key-like ZCL value]"
KEYLIKE_ZCL_ATTR_IDS = {"0x0054"}
KEYLIKE_ZCL_ATTR_TYPES = {"0xf1"}
MAC_COMMANDS = {
    "0x01": "association request",
    "0x02": "association response",
    "0x04": "data request",
}


@dataclass(frozen=True)
class KeySource:
    label: str
    key_hex: str


def normalize_key(text: str) -> str:
    byte_literals = re.findall(r"0x([0-9a-fA-F]{2})", text)
    if byte_literals:
        compact = "".join(byte_literals)
    else:
        compact = re.sub(r"[^0-9a-fA-F]", "", text)
    if len(compact) != 32:
        raise ValueError("expected exactly 16 key bytes")
    return compact.upper()


def reverse_key_bytes(key_hex: str) -> str:
    return "".join(reversed([key_hex[i : i + 2] for i in range(0, len(key_hex), 2)]))


def wireshark_key(key_hex: str) -> str:
    return ":".join(key_hex[i : i + 2] for i in range(0, len(key_hex), 2))


def safe_label(label: str) -> str:
    label = re.sub(r"[^A-Za-z0-9_.:/+-]+", "_", label.strip())
    return label[:80] or "key"


def load_keys(args: argparse.Namespace) -> tuple[list[KeySource], list[str]]:
    keys: list[KeySource] = []
    notes: list[str] = []
    env_names = [] if args.no_default_key_env else list(DEFAULT_KEY_ENVS)
    env_names.extend(args.key_env or [])

    for env_name in env_names:
        raw = os.environ.get(env_name)
        if not raw:
            continue
        try:
            keys.append(KeySource(env_name, normalize_key(raw)))
        except ValueError as exc:
            notes.append(f"ignored `{env_name}`: {exc}")

    for path in args.key_file or []:
        try:
            keys.append(KeySource(str(path), normalize_key(path.read_text(encoding="utf-8"))))
        except OSError as exc:
            notes.append(f"ignored `{path}`: {exc}")
        except ValueError as exc:
            notes.append(f"ignored `{path}`: {exc}")

    if args.include_local_trust_center_key:
        for path in (
            pathlib.Path("firmware/main/trust_center_key.h"),
            pathlib.Path("firmware/gradient_probe/main/trust_center_key.h"),
        ):
            if not path.exists():
                continue
            try:
                keys.append(KeySource(str(path), normalize_key(path.read_text(encoding="utf-8"))))
            except (OSError, ValueError) as exc:
                notes.append(f"ignored `{path}`: {exc}")

    deduped: list[KeySource] = []
    seen: set[str] = set()
    for key in keys:
        if key.key_hex in seen:
            continue
        seen.add(key.key_hex)
        deduped.append(KeySource(safe_label(key.label), key.key_hex))

    if args.try_reversed_keys:
        for key in list(deduped):
            reversed_key = reverse_key_bytes(key.key_hex)
            if reversed_key in seen:
                continue
            seen.add(reversed_key)
            deduped.append(KeySource(safe_label(f"{key.label}:reversed"), reversed_key))

    return deduped, notes


def tshark_base_cmd(pcap: pathlib.Path, keys: Iterable[KeySource]) -> list[str]:
    cmd = ["tshark", "-r", str(pcap)]
    for key in keys:
        # Wireshark's zigbee_pc_keys UAT is: key, byte order, label. Do not log
        # this command line; key bytes are intentionally not written to outputs.
        cmd.extend(["-o", f'uat:zigbee_pc_keys:"{wireshark_key(key.key_hex)}","Normal","{key.label}"'])
    return cmd


def run_tshark_fields(
    pcap: pathlib.Path,
    keys: Iterable[KeySource],
    *,
    display_filter: str | None = None,
    fields: Iterable[str] = TSHARK_FIELDS,
) -> str:
    cmd = tshark_base_cmd(pcap, keys)
    if display_filter:
        cmd.extend(["-Y", display_filter])
    cmd.extend(
        [
            "-T",
            "fields",
            "-E",
            "header=y",
            "-E",
            "separator=\t",
            "-E",
            "quote=n",
            "-E",
            "occurrence=a",
        ]
    )
    for field in fields:
        cmd.extend(["-e", field])
    proc = subprocess.run(cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "tshark failed")
    return proc.stdout


def parse_tshark_table(text: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    reader = csv.DictReader(text.splitlines(), delimiter="\t")
    for raw_row in reader:
        row = {alias: "" for alias in CSV_FIELDS}
        for tshark_name, value in raw_row.items():
            alias = FIELD_BY_TSHARK.get(tshark_name) or FIELD_BY_TSHARK.get(tshark_name.lower())
            if alias:
                row[alias] = value or ""
        redact_transport_key_info(row)
        redact_keylike_zcl_values(row)
        rows.append(row)
    return rows


def redact_transport_key_info(row: dict[str, str]) -> None:
    is_transport_key = row.get("aps_cmd_id") == "0x05" or "Transport Key" in row.get("info", "")
    if is_transport_key:
        row["info"] = "[redacted APS transport-key frame]"


def split_multi_value(value: str) -> set[str]:
    return {part.strip().lower() for part in value.split(",") if part.strip()}


def redact_keylike_zcl_values(row: dict[str, str]) -> None:
    attr_ids = split_multi_value(row.get("zcl_attr_id", ""))
    attr_types = split_multi_value(row.get("zcl_attr_type", ""))
    if not (attr_ids & KEYLIKE_ZCL_ATTR_IDS or attr_types & KEYLIKE_ZCL_ATTR_TYPES):
        return
    for field in ZCL_VALUE_FIELDS:
        row[field] = ""
    row["zcl_attr_bytes"] = KEYLIKE_ZCL_REDACTION


def write_csv(path: pathlib.Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def nonempty(row: dict[str, str], names: Iterable[str]) -> bool:
    return any(row.get(name) for name in names)


def is_truthy_field(value: str) -> bool:
    return value not in ("", "0", "0x00", "False", "false", "FALSE")


def normalize_short(value: str | None) -> str | None:
    if not value:
        return None
    value = value.strip().lower()
    if value.startswith("0x"):
        return f"0x{int(value, 16):04x}"
    return f"0x{int(value, 16):04x}"


def normalize_eui(value: str | None) -> str | None:
    if not value:
        return None
    compact = re.sub(r"[^0-9a-fA-F]", "", value)
    if len(compact) != 16:
        return None
    return ":".join(compact[i : i + 2] for i in range(0, 16, 2)).lower()


def row_matches_target(row: dict[str, str], target_eui: str | None, target_short: str | None) -> bool:
    eui_fields = (
        "wpan_src64",
        "wpan_dst64",
        "nwk_src64",
        "nwk_dst64",
        "security_src64",
        "aps_cmd_device",
    )
    short_fields = (
        "wpan_src16",
        "wpan_dst16",
        "wpan_association_short",
        "nwk_src",
        "nwk_dst",
    )
    if target_eui:
        for field in eui_fields:
            for value in row.get(field, "").split(","):
                if normalize_eui(value) == target_eui:
                    return True
    if target_short:
        for field in short_fields:
            for value in row.get(field, "").split(","):
                try:
                    if normalize_short(value) == target_short:
                        return True
                except ValueError:
                    continue
    return False


def zcl_value(row: dict[str, str]) -> str:
    values = [row[name] for name in ZCL_VALUE_FIELDS if row.get(name)]
    return "; ".join(values)


def escape_cell(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ").strip()


def markdown_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    out = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    for row in rows:
        out.append("| " + " | ".join(escape_cell(value) for value in row) + " |")
    return out


def count_filter_rows(pcap: pathlib.Path, keys: list[KeySource], display_filter: str) -> int:
    text = run_tshark_fields(pcap, keys, display_filter=display_filter, fields=("frame.number",))
    return max(0, len(text.splitlines()) - 1)


def write_markdown(
    path: pathlib.Path,
    *,
    pcap: pathlib.Path,
    rows: list[dict[str, str]],
    keys: list[KeySource],
    notes: list[str],
    target_eui: str | None,
    target_short: str | None,
    transport_key_rows: int,
    max_zcl_rows: int,
) -> None:
    nwk_rows = [row for row in rows if nonempty(row, ("nwk_src", "nwk_dst", "security_key_id"))]
    encrypted_rows = [row for row in rows if is_truthy_field(row.get("nwk_security", ""))]
    decrypted_rows = [row for row in rows if row.get("decryption_key_label")]
    aps_rows = [
        row
        for row in rows
        if nonempty(row, ("aps_type", "aps_src_ep", "aps_dst_ep", "aps_cluster", "aps_cmd_id", "aps_profile"))
    ]
    zcl_rows = [
        row
        for row in rows
        if nonempty(row, ("zcl_cmd_id", "zcl_attr_id", "zcl_attr_status", "zcl_attr_type"))
    ]
    target_rows = [row for row in rows if row_matches_target(row, target_eui, target_short)]
    join_rows = [
        row
        for row in target_rows
        if row.get("wpan_cmd") in ("0x01", "0x02")
    ]
    assigned_shorts = sorted(
        {
            row["wpan_association_short"]
            for row in rows
            if row.get("wpan_cmd") == "0x02" and row.get("wpan_association_short")
        }
    )

    lines: list[str] = [
        "# Zigbee PCAP Decode Summary",
        "",
        "## Inputs",
        "",
        f"- pcap: `{pcap}`",
        f"- key sources used: `{len(keys)}`",
    ]
    if keys:
        labels = ", ".join(f"`{key.label}`" for key in keys)
        lines.append(f"- key source labels: {labels}")
    else:
        lines.append("- key source labels: none")
    if notes:
        for note in notes:
            lines.append(f"- note: {note}")
    if target_eui:
        lines.append(f"- target_eui: `{target_eui}`")
    if target_short:
        lines.append(f"- target_short: `{target_short}`")

    lines.extend(
        [
            "",
            "## Decode Counts",
            "",
            f"- total rows: `{len(rows)}`",
            f"- NWK rows: `{len(nwk_rows)}`",
            f"- encrypted NWK rows: `{len(encrypted_rows)}`",
            f"- rows with decryption-key label: `{len(decrypted_rows)}`",
            f"- APS rows: `{len(aps_rows)}`",
            f"- ZCL rows: `{len(zcl_rows)}`",
            f"- APS transport-key rows with decoded key field: `{transport_key_rows}`",
            f"- target rows: `{len(target_rows)}`",
        ]
    )
    if assigned_shorts:
        lines.append(f"- association-response short addresses seen: `{', '.join(assigned_shorts)}`")

    if join_rows:
        lines.extend(["", "## Target Join Frames", ""])
        table_rows = [
            [
                row["frame"],
                row["time_s"],
                MAC_COMMANDS.get(row["wpan_cmd"], row["wpan_cmd"]),
                row["wpan_src64"] or row["wpan_src16"],
                row["wpan_dst64"] or row["wpan_dst16"],
                row["wpan_association_short"],
                row["info"],
            ]
            for row in join_rows[:20]
        ]
        lines.extend(
            markdown_table(
                ["frame", "time_s", "cmd", "src", "dst", "assigned_short", "info"],
                table_rows,
            )
        )

    if zcl_rows:
        lines.extend(["", "## ZCL Rows", ""])
        table_rows = [
            [
                row["frame"],
                row["time_s"],
                f"{row['nwk_src']}->{row['nwk_dst']}",
                f"{row['aps_src_ep']}->{row['aps_dst_ep']}",
                row["aps_cluster"],
                row["zcl_mfg_code"],
                row["zcl_cmd_id"],
                row["zcl_attr_id"],
                row["zcl_attr_status"],
                row["zcl_attr_type"],
                zcl_value(row),
                row["info"],
            ]
            for row in zcl_rows[:max_zcl_rows]
        ]
        lines.extend(
            markdown_table(
                [
                    "frame",
                    "time_s",
                    "nwk",
                    "ep",
                    "cluster",
                    "mfg",
                    "cmd",
                    "attr",
                    "status",
                    "type",
                    "value",
                    "info",
                ],
                table_rows,
            )
        )
        if len(zcl_rows) > max_zcl_rows:
            lines.append(f"\nOnly the first `{max_zcl_rows}` ZCL rows are shown here.")
    else:
        lines.extend(
            [
                "",
                "## Interpretation",
                "",
                "No decoded ZCL rows were found. This pcap still shows the MAC join and",
                "encrypted Zigbee NWK traffic, but not the bridge's exact ZCL",
                "read/response transcript.",
                "",
                "For a rejoin capture, a ZLL/Touchlink link key alone is not enough if the",
                "device reused an existing network key and no decryptable network-key",
                "transport appears in the capture. Use one of these inputs next:",
                "",
                "- `HUE_ZIGBEE_NWK_KEY` containing the actual network key.",
                "- a factory-reset join capture where the network key transport is visible",
                "  and decryptable with the correct link key.",
            ]
        )

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcap", type=pathlib.Path, help="IEEE 802.15.4 pcap from sniffer_log_to_pcap.py")
    parser.add_argument("--out-dir", type=pathlib.Path, help="output directory; defaults next to the pcap")
    parser.add_argument("--target-eui", help="target IEEE address, e.g. 00:17:88:01:0b:89:54:2f")
    parser.add_argument("--target-short", help="target short address, e.g. 0xadeb")
    parser.add_argument(
        "--key-env",
        action="append",
        help="environment variable containing a 16-byte key; can be repeated",
    )
    parser.add_argument(
        "--no-default-key-env",
        action="store_true",
        help=f"do not read default key envs: {', '.join(DEFAULT_KEY_ENVS)}",
    )
    parser.add_argument(
        "--key-file",
        type=pathlib.Path,
        action="append",
        help="file containing a 16-byte key as hex or a C 0xNN byte array; can be repeated",
    )
    parser.add_argument(
        "--include-local-trust-center-key",
        action="store_true",
        help="also read gitignored firmware/*/trust_center_key.h files when present",
    )
    parser.add_argument(
        "--try-reversed-keys",
        action="store_true",
        help="also try each input key with byte order reversed",
    )
    parser.add_argument("--max-zcl-rows", type=int, default=200)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.pcap.exists():
        print(f"pcap not found: {args.pcap}", file=sys.stderr)
        return 2
    if not shutil.which("tshark"):
        print("tshark not found; install the tshark package first", file=sys.stderr)
        return 2

    out_dir = args.out_dir or args.pcap.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    transcript_csv = out_dir / "zigbee-transcript.csv"
    summary_md = out_dir / "zigbee-decode-summary.md"

    target_eui = normalize_eui(args.target_eui) if args.target_eui else None
    if args.target_eui and not target_eui:
        print(f"invalid --target-eui: {args.target_eui}", file=sys.stderr)
        return 2
    try:
        target_short = normalize_short(args.target_short) if args.target_short else None
    except ValueError:
        print(f"invalid --target-short: {args.target_short}", file=sys.stderr)
        return 2

    keys, notes = load_keys(args)
    try:
        rows = parse_tshark_table(run_tshark_fields(args.pcap, keys))
        transport_key_rows = count_filter_rows(args.pcap, keys, "zbee_aps.cmd.key")
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    write_csv(transcript_csv, rows)
    write_markdown(
        summary_md,
        pcap=args.pcap,
        rows=rows,
        keys=keys,
        notes=notes,
        target_eui=target_eui,
        target_short=target_short,
        transport_key_rows=transport_key_rows,
        max_zcl_rows=args.max_zcl_rows,
    )

    zcl_count = sum(
        1
        for row in rows
        if nonempty(row, ("zcl_cmd_id", "zcl_attr_id", "zcl_attr_status", "zcl_attr_type"))
    )
    print(f"wrote {transcript_csv}")
    print(f"wrote {summary_md}")
    print(f"decoded_zcl_rows={zcl_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
