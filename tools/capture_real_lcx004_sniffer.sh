#!/usr/bin/env bash
# Flash sniffer_probe, capture a real LCX004 bridge rejoin, then restore the
# main fake-light firmware.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-/dev/ttyACM0}"
SECONDS=300
LIGHT_ID=""
OUT_DIR=""
RESTORE=1
CONFIRM=0
NEEDS_RESTORE=0

usage() {
    cat <<'USAGE'
Usage:
  tools/capture_real_lcx004_sniffer.sh --light-id <v1-id> --i-understand-delete-real-device [options]

Options:
  --light-id <id>                    Real LCX004 Hue v1 light id to delete/rejoin.
  --port <path>                      Serial device, default /dev/ttyACM0 or $PORT.
  --seconds <n>                      Capture duration, default 300.
  --out-dir <path>                   Capture artifact directory.
  --no-restore                       Leave sniffer firmware on the ESP32-C6.
  --i-understand-delete-real-device  Required. The script deletes the selected Hue light.
  -h, --help                         Show this help.

Before running, inspect candidates with:
  python3 tools/capture_sniffer_probe.py --list-candidates

The script uses HUE_BRIDGE_HOST and HUE_API_KEY from the environment or .env via
tools/capture_sniffer_probe.py. It flashes firmware, so do not run it while the
current fake-light firmware must remain on the board.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --light-id)
            LIGHT_ID="${2:-}"
            shift 2
            ;;
        --port)
            PORT="${2:-}"
            shift 2
            ;;
        --seconds)
            SECONDS="${2:-}"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="${2:-}"
            shift 2
            ;;
        --no-restore)
            RESTORE=0
            shift
            ;;
        --i-understand-delete-real-device)
            CONFIRM=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$LIGHT_ID" ]]; then
    echo "Missing --light-id." >&2
    usage >&2
    exit 2
fi

if [[ "$CONFIRM" -ne 1 ]]; then
    echo "--light-id requires --i-understand-delete-real-device." >&2
    exit 2
fi

if ! [[ "$SECONDS" =~ ^[0-9]+$ ]]; then
    echo "--seconds must be an integer." >&2
    exit 2
fi

restore_main_firmware() {
    if [[ "$RESTORE" -ne 1 || "$NEEDS_RESTORE" -ne 1 ]]; then
        return
    fi
    echo "Restoring main fake-light firmware on $PORT"
    (
        cd "$ROOT_DIR/firmware"
        sg docker -c "PORT=$PORT ./in-docker.sh idf.py -p $PORT flash"
    )
}

trap restore_main_firmware EXIT

echo "Building sniffer_probe"
(
    cd "$ROOT_DIR/firmware/sniffer_probe"
    sg docker -c "../in-docker.sh idf.py build"
)

echo "Flashing sniffer_probe on $PORT"
(
    cd "$ROOT_DIR/firmware/sniffer_probe"
    sg docker -c "PORT=$PORT ../in-docker.sh idf.py -p $PORT flash"
)
NEEDS_RESTORE=1

capture_cmd=(
    python3 "$ROOT_DIR/tools/capture_sniffer_probe.py"
    --port "$PORT"
    --seconds "$SECONDS"
    --snapshot-hue
    --delete-light-id "$LIGHT_ID"
    --i-understand-delete-real-device
    --start-search
)

if [[ -n "$OUT_DIR" ]]; then
    capture_cmd+=(--out-dir "$OUT_DIR")
fi

echo "Starting coordinated real LCX004 capture for Hue v1 light id $LIGHT_ID"
"${capture_cmd[@]}"
