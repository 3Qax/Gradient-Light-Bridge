import argparse

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description="Send one line to the firmware serial CLI")
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    cmd = " ".join(args.command).strip() or "help"
    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        ser.write((cmd + "\n").encode())
    print(f"Sent to {args.port}: {cmd}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
