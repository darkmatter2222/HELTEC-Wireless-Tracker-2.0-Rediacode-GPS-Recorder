"""
Heltec Tracker — Serial Integration Test Suite
===============================================
Connects to the device over USB-CDC and exercises the full serial REPL.

What is tested
--------------
  heartbeat_format        [HB] lines arrive with the expected fields
  heartbeat_cadence       [HB] fires every ~3 s (within +/-50 % tolerance)
  help_responds           ? lists known commands within 2 s
  ls_responds             LS returns a response within 2 s
  ls_timing_stress        LS sent 5x rapidly; every response within 2 s
  statfs_responds         STATFS returns within 2 s
  sdstat_responds         SDSTAT returns within 2 s
  wifistat_responds       WIFISTAT returns within 2 s
  wifistat_format         WIFISTAT output contains expected keys
  gps_status_responds     g returns within 2 s
  gps_status_format       g output contains satellite / baud info
  sample_count_consistency samples= in [HB] matches LS output
  serial_loop_latency     10 STATFS commands over 30 s all respond < 2 s

Key v0.3.4 regression tests
----------------------------
  ls_timing_stress        LS must be fast even on large sessions (O(1) fix)
  serial_loop_latency     commands must respond even during BLE reconnect storms
                          (non-blocking scan fix)

Usage
-----
  python scripts\\test_device.py                # defaults to COM4
  python scripts\\test_device.py --port COM3
  python scripts\\test_device.py --port COM4 --timeout 3.0

Environment variable override:  TRACKER_PORT=COM5

Exit code: 0 = all tests passed (or skipped), 1 = one or more failures.
"""

import argparse
import os
import re
import sys
import threading
import time

# ---------------------------------------------------------------------------
# Optional dependency check
# ---------------------------------------------------------------------------
try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed.  Run:  pip install pyserial")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DEFAULT_PORT    = os.environ.get("TRACKER_PORT", "COM4")
DEFAULT_BAUD    = 115200
CMD_TIMEOUT_S   = 2.0   # individual command must respond within this many seconds
HB_WINDOW_S     = 12.0  # collect heartbeats for this long
HB_INTERVAL_S   = 3.0   # expected [HB] cadence
HB_TOLERANCE    = 0.50  # fractional tolerance on cadence

PROMPT_PATTERNS = [
    re.compile(r"^\[HB\]"),
    re.compile(r"^>"),
    re.compile(r"^\["),
]

# ---------------------------------------------------------------------------
# Colour helpers (falls back gracefully on Windows without ANSI support)
# ---------------------------------------------------------------------------
try:
    import ctypes
    ctypes.windll.kernel32.SetConsoleMode(
        ctypes.windll.kernel32.GetStdHandle(-11), 7)
except Exception:
    pass

RESET  = "\033[0m"
GREEN  = "\033[32m"
RED    = "\033[31m"
YELLOW = "\033[33m"
BOLD   = "\033[1m"

def _c(color: str, text: str) -> str:
    return f"{color}{text}{RESET}"

# ---------------------------------------------------------------------------
# Serial port helper
# ---------------------------------------------------------------------------

def open_port(port: str, baud: int = DEFAULT_BAUD, timeout: float = 0.05):
    """Open a serial port with no DTR/RTS toggling (avoids ESP32 reset)."""
    s = serial.Serial()
    s.port     = port
    s.baudrate = baud
    s.timeout  = timeout
    s.dtr      = False
    s.rts      = False
    s.open()
    return s


def drain(ser: serial.Serial, seconds: float = 0.3) -> list[str]:
    """Drain whatever is in the RX buffer for `seconds`."""
    deadline = time.monotonic() + seconds
    lines: list[str] = []
    while time.monotonic() < deadline:
        raw = ser.readline()
        if raw:
            try:
                lines.append(raw.decode(errors="replace").rstrip())
            except Exception:
                pass
    return lines


def send_cmd_collect(ser: serial.Serial, cmd: str,
                     timeout: float = CMD_TIMEOUT_S,
                     done_pattern: re.Pattern | None = None) -> tuple[list[str], float]:
    """
    Send `cmd`, then collect response lines until:
      - `done_pattern` matches a line, OR
      - `timeout` seconds pass after the last received byte (or from send time).
    Returns (lines, elapsed_seconds).
    """
    ser.reset_input_buffer()
    t0 = time.monotonic()
    ser.write((cmd + "\r\n").encode())
    ser.flush()

    lines: list[str] = []
    last_rx = time.monotonic()
    while True:
        raw = ser.readline()
        if raw:
            try:
                line = raw.decode(errors="replace").rstrip()
            except Exception:
                line = ""
            lines.append(line)
            last_rx = time.monotonic()
            if done_pattern and done_pattern.search(line):
                break
        # Idle exit: no data for 0.4 s after receiving something, or timeout.
        since_last_rx = time.monotonic() - last_rx
        if lines and since_last_rx > 0.4:
            break
        if time.monotonic() - t0 > timeout:
            break
    return lines, time.monotonic() - t0


# ---------------------------------------------------------------------------
# Test result tracking
# ---------------------------------------------------------------------------

class TestResult:
    def __init__(self, name: str):
        self.name    = name
        self.passed  = False
        self.skipped = False
        self.message = ""
        self.elapsed = 0.0

    def __str__(self):
        if self.skipped:
            status = _c(YELLOW, "[ SKIP ]")
        elif self.passed:
            status = _c(GREEN,  "[ PASS ]")
        else:
            status = _c(RED,    "[ FAIL ]")
        t = f"({self.elapsed:.2f}s)"
        msg = f"  -- {self.message}" if self.message else ""
        return f"{status} {self.name:<40} {t}{msg}"


class Suite:
    def __init__(self):
        self.results: list[TestResult] = []

    def record(self, name: str, passed: bool, elapsed: float = 0.0,
               message: str = "", skipped: bool = False) -> TestResult:
        r = TestResult(name)
        r.passed  = passed
        r.skipped = skipped
        r.elapsed = elapsed
        r.message = message
        self.results.append(r)
        print(r)
        return r

    def summary(self) -> tuple[int, int, int]:
        passed  = sum(1 for r in self.results if r.passed and not r.skipped)
        failed  = sum(1 for r in self.results if not r.passed and not r.skipped)
        skipped = sum(1 for r in self.results if r.skipped)
        return passed, failed, skipped


# ---------------------------------------------------------------------------
# Individual tests
# ---------------------------------------------------------------------------

def test_help(ser: serial.Serial, suite: Suite) -> None:
    lines, elapsed = send_cmd_collect(ser, "?")
    passed = any("LS" in l or "STATFS" in l for l in lines)
    suite.record("help_responds", passed, elapsed,
                 "" if passed else f"no command list in response ({len(lines)} lines)")


def test_ls(ser: serial.Serial, suite: Suite) -> None:
    lines, elapsed = send_cmd_collect(ser, "LS")
    # LS outputs "Sessions: N" or a list of session entries, or "no sessions"
    passed = elapsed < CMD_TIMEOUT_S and len(lines) > 0
    suite.record("ls_responds", passed, elapsed,
                 "" if passed else f"no response or timed out (elapsed {elapsed:.2f}s)")


def test_ls_timing_stress(ser: serial.Serial, suite: Suite) -> None:
    """
    Send LS 5 times rapidly.  Each must respond within CMD_TIMEOUT_S.
    This is the direct regression test for the O(1) line-counting fix:
    before v0.3.4 a 20k-row session could hold the LittleFS mutex for
    hundreds of ms.
    """
    max_elapsed = 0.0
    all_pass = True
    for _ in range(5):
        lines, elapsed = send_cmd_collect(ser, "LS", timeout=CMD_TIMEOUT_S + 0.5)
        if elapsed >= CMD_TIMEOUT_S or not lines:
            all_pass = False
        if elapsed > max_elapsed:
            max_elapsed = elapsed
        time.sleep(0.1)
    suite.record("ls_timing_stress", all_pass, max_elapsed,
                 "" if all_pass else f"at least one LS took >= {CMD_TIMEOUT_S}s")


def test_statfs(ser: serial.Serial, suite: Suite) -> None:
    lines, elapsed = send_cmd_collect(ser, "STATFS")
    passed = elapsed < CMD_TIMEOUT_S and any(
        "session" in l.lower() or "bytes" in l.lower() or "/" in l
        for l in lines
    )
    suite.record("statfs_responds", passed, elapsed,
                 "" if passed else f"unexpected output: {lines[:2]}")


def test_sdstat(ser: serial.Serial, suite: Suite) -> None:
    lines, elapsed = send_cmd_collect(ser, "SDSTAT")
    passed = elapsed < CMD_TIMEOUT_S and len(lines) > 0
    suite.record("sdstat_responds", passed, elapsed,
                 "" if passed else "no response")


def test_wifistat(ser: serial.Serial, suite: Suite) -> None:
    lines, elapsed = send_cmd_collect(ser, "WIFISTAT")
    passed_time = elapsed < CMD_TIMEOUT_S
    # WIFISTAT output should contain at least "enabled" or "busy" or "ssid"
    joined = " ".join(lines).lower()
    passed_fmt = any(kw in joined for kw in ("enabled", "busy", "upload", "ssid", "wifi"))
    passed = passed_time and len(lines) > 0
    suite.record("wifistat_responds", passed, elapsed,
                 "" if passed else "no response")
    suite.record("wifistat_format", passed_fmt, 0.0,
                 "" if passed_fmt else f"missing expected keywords: {lines[:3]}")


def test_gps(ser: serial.Serial, suite: Suite) -> None:
    lines, elapsed = send_cmd_collect(ser, "g")
    passed_time = elapsed < CMD_TIMEOUT_S
    joined = " ".join(lines).lower()
    # g output should reference baud rate and satellite count
    passed_fmt = any(kw in joined for kw in ("baud", "sat", "fix", "gnss", "gps", "nmea"))
    suite.record("gps_status_responds", passed_time and len(lines) > 0, elapsed,
                 "" if passed_time else "no response")
    suite.record("gps_status_format", passed_fmt, 0.0,
                 "" if passed_fmt else f"missing GPS keywords: {lines[:3]}")


def test_heartbeat(ser: serial.Serial, suite: Suite) -> None:
    """
    Passively collect [HB] lines for HB_WINDOW_S seconds, then check:
      - At least 2 heartbeats arrived (format test).
      - Average interval is close to HB_INTERVAL_S (cadence test).
    """
    HB_RE = re.compile(
        r"\[HB\]\s+uptime=(\d+)s\s+fix=(\d+)\s+sats=(\d+)\s+hdop=([\d.]+)"
        r"\s+gpsB=(\d+)\s+gpsAge=(\d+)ms\s+baud=(\d+)\s+rcState=(\d+)"
        r"\s+rec=(\d+)\s+samples=(\d+)"
    )
    hb_times: list[float] = []
    last_samples: int | None = None

    drain(ser, 0.2)
    t0 = time.monotonic()
    print(f"  (collecting heartbeats for {HB_WINDOW_S:.0f} s ...)", flush=True)
    while time.monotonic() - t0 < HB_WINDOW_S:
        raw = ser.readline()
        if not raw:
            continue
        try:
            line = raw.decode(errors="replace").rstrip()
        except Exception:
            continue
        m = HB_RE.match(line)
        if m:
            hb_times.append(time.monotonic())
            last_samples = int(m.group(10))

    elapsed = time.monotonic() - t0

    fmt_pass = len(hb_times) >= 2
    suite.record("heartbeat_format", fmt_pass, elapsed,
                 "" if fmt_pass
                 else f"only {len(hb_times)} heartbeat(s) received in {elapsed:.1f}s")

    if len(hb_times) >= 2:
        intervals = [hb_times[i+1] - hb_times[i] for i in range(len(hb_times)-1)]
        avg_interval = sum(intervals) / len(intervals)
        tolerance   = HB_INTERVAL_S * HB_TOLERANCE
        cad_pass = abs(avg_interval - HB_INTERVAL_S) <= tolerance
        suite.record("heartbeat_cadence", cad_pass, elapsed,
                     "" if cad_pass
                     else f"avg interval {avg_interval:.2f}s, expected ~{HB_INTERVAL_S}s")
    else:
        suite.record("heartbeat_cadence", False, elapsed, "insufficient heartbeats")


def test_sample_count_consistency(ser: serial.Serial, suite: Suite) -> None:
    """
    Grab the most recent [HB] line's samples= field, then issue LS and parse
    the same session's sample count.  They must agree (or within +/-1 due to
    timing).  This validates that sampleCount_ in memory stays in sync with
    the file.
    """
    HB_RE = re.compile(r"\[HB\].*\bsamples=(\d+)")

    # Wait for a fresh heartbeat (up to 5 s).
    hb_samples: int | None = None
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        try:
            line = raw.decode(errors="replace").rstrip()
        except Exception:
            continue
        m = HB_RE.search(line)
        if m:
            hb_samples = int(m.group(1))
            break

    if hb_samples is None:
        suite.record("sample_count_consistency", False, 0.0, "no [HB] received")
        return

    # Immediately issue LS.
    t0 = time.monotonic()
    lines, _ = send_cmd_collect(ser, "LS")
    elapsed = time.monotonic() - t0

    # If device is recording, LS should show the active session's sample count.
    # Match patterns like "samples=1234", "1234 samples", or "1234 rows".
    ls_samples: int | None = None
    sample_re = re.compile(r"samples?[=:\s]+(\d+)", re.IGNORECASE)
    for line in lines:
        m = sample_re.search(line)
        if m:
            ls_samples = int(m.group(1))
            break

    if ls_samples is None:
        # No active recording — not a failure, just not testable right now.
        suite.record("sample_count_consistency", True, elapsed,
                     f"skipped (device not recording; HB samples={hb_samples})",
                     skipped=True)
        return

    delta = abs(hb_samples - ls_samples)
    passed = delta <= 2  # 1-2 samples can be appended in the time between the two reads
    suite.record("sample_count_consistency", passed, elapsed,
                 "" if passed
                 else f"HB says {hb_samples}, LS says {ls_samples} (delta={delta})")


def test_serial_loop_latency(ser: serial.Serial, suite: Suite) -> None:
    """
    Send STATFS 10 times spread over 30 seconds.  Every command must respond
    within CMD_TIMEOUT_S.  This is the regression test for the non-blocking
    BLE scan fix: before v0.3.4 the main loop was blocked for up to 8 s per
    scan window, making serial commands unresponsive during BLE storms.
    """
    interval = 3.0  # seconds between commands
    total    = 10
    failures = 0
    max_lat  = 0.0

    print(f"  (sending {total} commands over ~{total * interval:.0f} s ...)", flush=True)
    t_suite_start = time.monotonic()
    for i in range(total):
        lines, elapsed = send_cmd_collect(ser, "STATFS", timeout=CMD_TIMEOUT_S + 0.5)
        if elapsed > max_lat:
            max_lat = elapsed
        if elapsed >= CMD_TIMEOUT_S or not lines:
            failures += 1
        # Wait for the next slot, accounting for time already spent.
        next_t = t_suite_start + (i + 1) * interval
        wait = next_t - time.monotonic()
        if wait > 0:
            time.sleep(wait)

    elapsed_total = time.monotonic() - t_suite_start
    passed = failures == 0
    suite.record("serial_loop_latency", passed, elapsed_total,
                 "" if passed
                 else f"{failures}/{total} commands exceeded {CMD_TIMEOUT_S}s (max {max_lat:.2f}s)")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="HTIT-Tracker v0.3.4 integration test suite")
    parser.add_argument("--port",    default=DEFAULT_PORT,
                        help=f"serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--baud",    type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=CMD_TIMEOUT_S,
                        help=f"per-command timeout in seconds (default: {CMD_TIMEOUT_S})")
    args = parser.parse_args()

    WIDTH = 60
    print("=" * WIDTH)
    print(f"{BOLD}  HTIT-Tracker Integration Test Suite (v0.3.4){RESET}")
    print(f"  Port: {args.port}   Baud: {args.baud}   Timeout: {args.timeout}s")
    print("=" * WIDTH)

    try:
        ser = open_port(args.port, args.baud)
    except serial.SerialException as exc:
        print(_c(RED, f"ERROR: Cannot open {args.port}: {exc}"))
        print("  Make sure the device is connected and no other program has the port open.")
        return 1

    suite = Suite()
    try:
        drain(ser, 0.5)   # flush any boot noise

        # Fast REPL tests (order matters -- do quick ones first).
        test_help(ser, suite)
        test_ls(ser, suite)
        test_ls_timing_stress(ser, suite)
        test_statfs(ser, suite)
        test_sdstat(ser, suite)
        test_wifistat(ser, suite)
        test_gps(ser, suite)

        # Passive heartbeat tests (takes HB_WINDOW_S seconds).
        test_heartbeat(ser, suite)

        # Cross-check sample count.
        test_sample_count_consistency(ser, suite)

        # Long-running latency test (~30 s).
        test_serial_loop_latency(ser, suite)

    finally:
        ser.close()

    passed, failed, skipped = suite.summary()
    total = len(suite.results)
    print("=" * WIDTH)
    p = _c(GREEN,  f"Passed:  {passed}")
    f = _c(RED,    f"Failed:  {failed}")
    s = _c(YELLOW, f"Skipped: {skipped}")
    print(f"  {p}   {f}   {s}   (Total: {total})")
    print("=" * WIDTH)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
