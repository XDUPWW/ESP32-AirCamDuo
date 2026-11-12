"""Minimal serial monitor for the ESP32-CAM.

Usage: python tools/monitor.py [COM5] [--seconds N]

Reads at 115200 baud and prints lines with a timestamp. Without --seconds it
runs until Ctrl-C.
"""

import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?", default="COM5")
    parser.add_argument("--seconds", type=float, default=None)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--reset", action="store_true", help="pulse EN before reading")
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        if args.reset:
            # DTR low keeps GPIO0 high; RTS drives EN, so this is a plain reset.
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.15)
            ser.setRTS(False)
            time.sleep(0.1)
            ser.reset_input_buffer()
        print(f"--- listening on {args.port} @ {args.baud} ---", flush=True)
        start = time.monotonic()
        pending = b""
        try:
            while args.seconds is None or time.monotonic() - start < args.seconds:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    print(f"[{time.monotonic() - start:7.2f}] {text}", flush=True)
        except KeyboardInterrupt:
            pass

        if pending:
            print(pending.decode("utf-8", "replace"), flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
