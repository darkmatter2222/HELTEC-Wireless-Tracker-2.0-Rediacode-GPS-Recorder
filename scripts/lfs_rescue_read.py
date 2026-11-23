#!/usr/bin/env python
"""Chunked raw flash reader for the Heltec Tracker.

esptool's stub read_flash corrupts the data stream after ~15-16 consecutive
4 KB pages on the ESP32-S3's GigaDevice QSPI flash (USB-CDC path overflow:
"Corrupt data, expected 0x1000 bytes but received 0xbda bytes").  Single or
short page runs per connection always succeed, so this driver re-connects
esptool for every chunk and concatenates the results.

Default region is the littlefs partition (0x210000, 0x5E0000) from
hardware/partitions_tracker_v2.csv.

Usage:
    python scripts/lfs_rescue_read.py [--port COM3] [--chunk 0x8000]
        [--start 0x210000] [--size 0x5E0000] [--out data/littlefs_dump.bin]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

ESPTOOL = os.path.join(
    os.path.expanduser("~"),
    ".platformio", "packages", "tool-esptoolpy", "esptool.py")


def read_chunk(port, addr, length, attempts=4, timeout=180, baud="921600"):
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        for attempt in range(1, attempts + 1):
            r = None
            try:
                r = subprocess.run(
                    [sys.executable, ESPTOOL, "--chip", "esp32s3",
                     "--port", port, "--baud", baud,
                     "read_flash", hex(addr), hex(length), tmp,
                     "--flash_size", "8MB", "--no-progress"],
                    capture_output=True, text=True, timeout=timeout)
            except subprocess.TimeoutExpired:
                # chip can reset mid-read (USB-JTAG); the stub then never
                # answers and pyserial blocks forever. Kill it and retry.
                pass
            err = ("timeout" if r is None
                   else (r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "bad size"))
            if r is not None and r.returncode == 0:
                with open(tmp, "rb") as f:
                    data = f.read()
                if len(data) == length:
                    return data
                err = f"bad size ({len(data)}/{length})"
            if attempt < attempts:
                print(f"  chunk {addr:#x}: attempt {attempt} failed ({err}), retrying",
                      flush=True)
        raise RuntimeError(f"chunk at {addr:#x} failed after {attempts} attempts")
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="COM3")
    p.add_argument("--start", type=lambda s: int(s, 16), default=0x210000)
    p.add_argument("--size", type=lambda s: int(s, 16), default=0x5E0000)
    p.add_argument("--chunk", type=lambda s: int(s, 16), default=0x8000)
    p.add_argument("--baud", default="921600")
    p.add_argument("--attempts", type=int, default=6)
    p.add_argument("--append", action="store_true",
                   help="append to --out instead of overwriting (resume)")
    p.add_argument("--out", default=os.path.join("data", "littlefs_dump.bin"))
    a = p.parse_args()

    total = a.size
    chunks = (total + a.chunk - 1) // a.chunk
    print(f"reading {total} bytes ({total / 1e6:.2f} MB) from {a.start:#x} "
          f"in {chunks} chunks of {a.chunk:#x}", flush=True)

    with open(a.out, "ab" if a.append else "wb") as out:
        t0 = time.time()
        for i in range(chunks):
            addr = a.start + i * a.chunk
            length = min(a.chunk, total - i * a.chunk)
            data = read_chunk(a.port, addr, length, attempts=a.attempts,
                              timeout=180, baud=a.baud)
            out.write(data)
            done = min((i + 1) * a.chunk, total)
            if i % 8 == 0 or i == chunks - 1:
                rate = done / max(time.time() - t0, 1e-6)
                eta = (total - done) / max(rate, 1)
                print(f"  {done}/{total} ({done * 100 // total}%) "
                      f"{rate / 1024:.0f} KB/s eta {eta:.0f}s", flush=True)
    print(f"done: {a.out} ({total} bytes, {time.time() - t0:.0f}s)", flush=True)


if __name__ == "__main__":
    main()
