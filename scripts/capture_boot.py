"""Trigger an ESP32 hard-reset via the USB-CDC DTR/RTS lines and capture
the first ~12 s of boot output.

Default port is COM4 (this dev machine). Override with --port or the
TRACKER_PORT env var.
"""
import argparse, os, serial, sys, time

DEFAULT_PORT = os.environ.get("TRACKER_PORT", "COM4")

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--port", default=DEFAULT_PORT, help=f"serial port (default {DEFAULT_PORT})")
ap.add_argument("--seconds", type=float, default=12.0, help="capture window (default 12s)")
ap.add_argument("--baud", type=int, default=115200)
args = ap.parse_args()

s = serial.Serial(args.port, args.baud, timeout=0.5)
# ESP32 hard-reset via DTR/RTS pulse (esptool's classic reset).
# DTR=EN(reset), RTS=IO0(boot). Drive EN low, leave IO0 high (run mode).
s.setDTR(False); s.setRTS(False); time.sleep(0.1)
s.setDTR(True);  s.setRTS(False); time.sleep(0.1)   # EN low -> reset
s.setDTR(False); s.setRTS(False); time.sleep(0.05)  # release reset, run mode
end = time.time() + args.seconds
out = b''
while time.time() < end:
    d = s.read(4096)
    if d:
        out += d
sys.stdout.buffer.write(out)
s.close()
