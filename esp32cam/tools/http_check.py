"""End-to-end checks for the ESP32-CAM endpoints.

The device only ships sensor data: JPEG in jpeg mode, raw YUYV422 in gray mode.
Everything that needs decoding or reinterpretation happens here, on the PC.

Usage: python tools/http_check.py [--base http://192.168.3.210] [--out captures]
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

import numpy as np


def request(base, path, timeout=20, method=None):
    req = urllib.request.Request(base + path, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read()


def check(cond, message):
    print(("  ok   " if cond else "  FAIL ") + message)
    return bool(cond)


def set_mode(base, mode, attempts=8):
    for _ in range(attempts):
        status, _, body = request(base, f"/mode?set={mode}")
        if status == 200:
            print(f"/mode?set={mode} -> {body.decode().strip()}")
            return True
        if status != 409:
            print(f"/mode?set={mode} -> {status} {body.decode(errors='replace').strip()}")
            return False
        time.sleep(0.3)
    print(f"/mode?set={mode} -> still busy")
    return False


def read_multipart(base, seconds, on_part):
    """Reads a multipart stream, calling on_part(payload) for each frame."""
    deadline = time.monotonic() + seconds
    buf = bytearray()
    frames = 0

    try:
        resp = urllib.request.urlopen(base + "/stream", timeout=20)
    except urllib.error.HTTPError as exc:
        return exc.code, 0

    try:
        while time.monotonic() < deadline:
            chunk = resp.read(65536)
            if not chunk:
                break
            buf.extend(chunk)

            while True:
                head_end = buf.find(b"\r\n\r\n")
                if head_end < 0:
                    break
                head = bytes(buf[:head_end]).decode("latin-1")
                length = None
                for line in head.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        length = int(line.split(":", 1)[1])
                if length is None:
                    del buf[:head_end + 4]
                    continue
                start = head_end + 4
                if len(buf) < start + length:
                    break
                on_part(bytes(buf[start:start + length]))
                del buf[:start + length]
                frames += 1
    finally:
        resp.close()

    return resp.status, frames


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://192.168.3.210")
    parser.add_argument("--out", default="captures")
    parser.add_argument("--stream-seconds", type=float, default=3.0)
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    results = []

    status, _, body = request(args.base, "/status")
    info = json.loads(body)
    print(f"/status -> {status}")
    print("  " + body.decode().strip())
    results.append(check(status == 200 and info.get("camera") == "ready", "camera ready"))

    results.append(set_mode(args.base, "jpeg"))
    status, headers, body = request(args.base, "/jpg")
    path = os.path.join(args.out, "capture.jpg")
    with open(path, "wb") as fh:
        fh.write(body)
    print(f"/jpg -> {status} {headers.get('Content-Type')}, {len(body)} bytes -> {path}")
    results.append(
        check(status == 200 and body[:2] == b"\xff\xd8" and body[-2:] == b"\xff\xd9",
              "jpeg snapshot has SOI/EOI markers")
    )

    _, jpeg_frames = read_multipart(args.base, args.stream_seconds, lambda _: None)
    print(f"/stream (jpeg) -> {jpeg_frames} frames in {args.stream_seconds}s")
    results.append(check(jpeg_frames > 10, "jpeg stream delivers frames"))

    results.append(set_mode(args.base, "gray"))
    status, headers, body = request(args.base, "/raw")
    width = int(headers.get("X-Frame-Width", 0))
    height = int(headers.get("X-Frame-Height", 0))
    fmt = headers.get("X-Frame-Format", "")
    print(f"/raw -> {status} {headers.get('Content-Type')}, {len(body)} bytes, "
          f"{width}x{height} {fmt}")
    ok = check(status == 200 and fmt == "gray8" and len(body) == width * height,
               "raw frame is one byte per pixel")
    results.append(ok)

    if ok:
        luma = np.frombuffer(body, dtype=np.uint8).reshape(height, width)
        pgm = os.path.join(args.out, "frame.pgm")
        with open(pgm, "wb") as fh:
            fh.write(f"P5\n{width} {height}\n255\n".encode() + luma.tobytes())
        print(f"  luma min={luma.min()} max={luma.max()} mean={luma.mean():.1f} -> {pgm}")
        results.append(check(luma.max() - luma.min() > 10, "luma has real image content"))

    raw_sizes = []

    def on_raw(payload):
        raw_sizes.append(len(payload))

    _, raw_frames = read_multipart(args.base, args.stream_seconds, on_raw)
    print(f"/stream (gray) -> {raw_frames} frames in {args.stream_seconds}s, "
          f"first frame {raw_sizes[0] if raw_sizes else 0} bytes "
          f"({raw_frames / args.stream_seconds:.1f} fps)")
    results.append(check(raw_frames > 5, "raw stream delivers frames"))
    results.append(check(all(s == width * height for s in raw_sizes), "raw frames are full size"))

    results.append(set_mode(args.base, "jpeg"))

    failed = results.count(False)
    print(f"\n{len(results) - failed}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
