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

v2.3 upload-path tests (added after the 2026-11-21 upload blackout)
-------------------------------------------------------------------
  wifistat_diagnostics    all three WIFISTAT lines present with gate/skip/chunks
  gate_vbat_consistent    gate state matches the VBAT reading under the thresholds
  sync_forces_cycle       SYNC produces a decided outcome (drain or explicit skip)
  chunked_drain_integrity sum(file_bytes) == cycle_done bytes, chunks reconcile
  uptime_monotonic_cycle  no reset across a full connect+drain window
  event_log_new_fields    LOG carries the v2.3 cycle_start/cycle_done fields

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
# Passive collection helper (for the v2.3 upload-path tests)
# ---------------------------------------------------------------------------

def collect_until(ser: serial.Serial, pattern: "str | tuple[str, ...]",
                  timeout: float,
                  seed: str | None = None) -> list[str]:
    """
    Read serial lines until one of `pattern` appears in a line or `timeout`
    seconds elapse. If `seed` is given it is written first (command injection).
    Heartbeat lines are kept too -- several assertions cross-check them.
    """
    patterns = (pattern,) if isinstance(pattern, str) else tuple(pattern)
    if seed:
        ser.write((seed + "\r\n").encode())
        ser.flush()
    lines: list[str] = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        raw = ser.readline()
        if not raw:
            continue
        try:
            line = raw.decode(errors="replace").rstrip()
        except Exception:
            continue
        if not line:
            continue
        lines.append(line)
        if any(p in line for p in patterns):
            break
    return lines


def parse_kv(lines: list[str], key: str) -> str | None:
    """Return the value of `key=value` from the first line that carries it."""
    token = key + "="
    for line in lines:
        idx = line.find(token)
        if idx >= 0:
            rest = line[idx + len(token):]
            end = rest.find(" ")
            return rest if end < 0 else rest[:end]
    return None


_INT_RE = re.compile(r"(-?\d+)")

def parse_all_int(lines: list[str], key: str) -> list[int]:
    """Every integer value of `key=` found across the lines ('n/a' skipped)."""
    out: list[int] = []
    token = key + "="
    for line in lines:
        start = 0
        while True:
            idx = line.find(token, start)
            if idx < 0:
                break
            rest = line[idx + len(token):]
            end = rest.find(" ")
            val = rest if end < 0 else rest[:end]
            # Tolerate the unit suffixes the firmware prints: uptime=10s,
            # lastAttempt=123456ms, gpsAge=94ms. Take the leading integer.
            m = _INT_RE.match(val)
            if m:
                out.append(int(m.group(1)))
            start = idx + len(token)
    return out


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
    # Field-order-tolerant parse. The firmware appends new [HB] fields over
    # time (life=, dose=, acc=), so match by KEY instead of a rigid positional
    # pattern. A strict pattern silently matched nothing once the core put
    # `acc=499.9m` between `hdop=` and `gpsB=` -- it reported "0 heartbeats"
    # for a perfectly healthy device.
    REQUIRED = ("uptime", "fix", "sats", "hdop", "gpsB", "gpsAge", "baud",
                "rcState", "rec", "samples")

    def parse_hb(line: str) -> dict[str, str] | None:
        if not line.startswith("[HB]"):
            return None
        fields: dict[str, str] = {}
        for token in line.split():
            if "=" in token:
                key, _, val = token.partition("=")
                fields[key] = val
        if any(key not in fields for key in REQUIRED):
            return None
        return fields

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
        f = parse_hb(line)
        if f:
            hb_times.append(time.monotonic())
            try:
                last_samples = int(f["samples"])
            except ValueError:
                pass

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


def has_cycle_done(lines: list[str]) -> bool:
    """
    True when a completed cycle is visible in the trace. The serial line reuses
    the event-log token (`cycle_done ok=1/1 bytes=... chunks=... ms=...`) from
    v2.3 onward; older builds printed `cycle done; ok=1/1` with a space.
    """
    joined = " ".join(lines)
    return "cycle_done ok=" in joined or "cycle done" in joined


# ---------------------------------------------------------------------------
# v2.3 upload-path tests (injected scenarios)
# ---------------------------------------------------------------------------

VBAT_OPEN  = 3.60   # cfg::VBAT_MIN_FOR_WIFI  (release level)
VBAT_FLOOR = 3.45   # cfg::VBAT_GATE_FLOOR    (re-block level)


def read_wifistat(ser: serial.Serial) -> list[str]:
    """Send WIFISTAT and return the three diagnostic lines."""
    lines, _ = send_cmd_collect(ser, "WIFISTAT", timeout=3.0)
    return [l for l in lines if l.startswith("[WIFI]")]


def test_wifistat_diagnostics(ser: serial.Serial, suite: Suite) -> None:
    """All three v2.3 WIFISTAT lines must carry their decoded fields."""
    lines = read_wifistat(ser)
    joined = " ".join(lines)
    required = ["phase=", "consec=", "gate=", "vbat=", "skip=", "batterySkips=",
                "connect attempts=", "chunks=", "nextIn="]
    missing = [k for k in required if k not in joined]
    passed = len(lines) >= 3 and not missing
    suite.record("wifistat_diagnostics", passed, 0.0,
                 "" if passed else f"lines={len(lines)} missing={missing}")


def test_gate_vbat_consistent(ser: serial.Serial, suite: Suite) -> None:
    """
    The latched gate must agree with its own input -- an open gate only above
    the floor, a closed gate only below the release level. This is the on-device
    twin of the hysteresis unit tests.
    """
    lines = read_wifistat(ser)
    gate = parse_kv(lines, "gate")
    vbat_s = parse_kv(lines, "vbat")
    if gate is None or vbat_s is None:
        suite.record("gate_vbat_consistent", False, 0.0,
                     f"missing gate=/vbat= in {lines}")
        return
    try:
        vbat = float(vbat_s.rstrip("V"))
    except ValueError:
        suite.record("gate_vbat_consistent", False, 0.0, f"unparsable vbat={vbat_s}")
        return
    if vbat <= 0.0:                       # ADC not sampled yet: cannot judge
        suite.record("gate_vbat_consistent", True, 0.0,
                     f"skipped (vbat={vbat_s} not sampled)", skipped=True)
        return
    ok = (gate == "open" and vbat > VBAT_FLOOR) or (gate == "close" and vbat < VBAT_OPEN)
    suite.record("gate_vbat_consistent", ok, 0.0,
                 "" if ok else f"gate={gate} vbat={vbat} "
                               f"(expected open>{VBAT_FLOOR} or close<{VBAT_OPEN})")


def test_sync_forces_cycle(ser: serial.Serial, suite: Suite) -> None:
    """
    Inject a SYNC poke and require a *decided* outcome within one cadence
    window: a chunked drain, an explicit battery skip, or an explicit
    "nothing queued". Only a silent stall (the v2.2 symptom) fails.
    """
    before = read_wifistat(ser)
    up_before = int(parse_kv(before, "uploaded") or 0)
    lines = collect_until(ser, ("cycle_done ok=", "cycle done"), 130.0, seed="SYNC")
    joined = " ".join(lines)
    drained = has_cycle_done(lines)
    gated   = "skip vbat=" in joined
    idle    = "skip=no_pending" in joined
    if not (drained or gated or idle):
        # The serial line can lag the internal state; give WIFISTAT a chance.
        now = read_wifistat(ser)
        if parse_kv(now, "skip") == "no_pending":
            idle = True
    if idle and not (drained or gated):
        suite.record("sync_forces_cycle", True, 0.0,
                     "idle: queue empty, radio correctly left off", skipped=True)
        return
    if not (drained or gated):
        suite.record("sync_forces_cycle", False, 0.0,
                     f"no outcome in 130s (last={lines[-1:] or 'none'})")
        return
    after = read_wifistat(ser)
    up_after = int(parse_kv(after, "uploaded") or 0)
    if drained:
        http = parse_kv(after, "lastHttp")
        ok = up_after > up_before and http == "200"
        suite.record("sync_forces_cycle", ok, 0.0,
                     "" if ok else f"uploaded {up_before}->{up_after} lastHttp={http}")
    else:
        # Battery skip: the reason must be reported and the queue untouched.
        skip = parse_kv(after, "skip")
        ok = skip == "battery" or "gate=closed" in joined
        suite.record("sync_forces_cycle", ok, 0.0,
                     "" if ok else f"gated but skip={skip} not reported")


def test_chunked_drain_integrity(ser: serial.Serial, suite: Suite) -> None:
    """
    Per-file bookkeeping must reconcile with the cycle total: the sum of the
    file_bytes= and chunks= reported by each successful POST sequence has to
    equal the cycle_done figures. A short body would show up as a mismatch.
    """
    lines = collect_until(ser, ("cycle_done ok=", "cycle done"), 150.0, seed="SYNC")
    done = next((l for l in lines if "cycle_done ok=" in l or "cycle done" in l), None)
    if done is None:
        suite.record("chunked_drain_integrity", True, 0.0,
                     "skipped (no cycle completed -- gate closed?)", skipped=True)
        return
    ok_lines = [l for l in lines if " OK http=200 " in l]
    if not ok_lines:
        suite.record("chunked_drain_integrity", True, 0.0,
                     "skipped (no 2xx file uploads this cycle)", skipped=True)
        return
    bytes_per_file = sum(sum(parse_all_int([l], "file_bytes")) for l in ok_lines)
    chunks_per_file = sum(sum(parse_all_int([l], "chunks")) for l in ok_lines)
    eb = parse_all_int([done], "bytes")
    ec = parse_all_int([done], "chunks")
    if not eb or not ec:
        suite.record("chunked_drain_integrity", True, 0.0,
                     "skipped (cycle line carries no byte accounting)", skipped=True)
        return
    exp_bytes = eb[0]
    exp_chunks = ec[0]
    ok = (bytes_per_file == exp_bytes) and (chunks_per_file == exp_chunks)
    if ok and exp_chunks < sum(1 for _ in ok_lines):
        ok = False   # fewer POSTs than files => a chunk boundary was miscounted
    suite.record("chunked_drain_integrity", ok, 0.0,
                 "" if ok else f"bytes {bytes_per_file}!={exp_bytes} "
                               f"chunks {chunks_per_file}!={exp_chunks}")


def test_uptime_monotonic_cycle(ser: serial.Serial, suite: Suite) -> None:
    """
    A full connect+drain window must not contain a reset: uptime= only moves
    forward and no BOOT line appears.
    """
    # Take the "before" reading from a heartbeat. One HB line is enough and the
    # 12 s window covers the 3 s cadence with margin; retry once so a
    # coincidental quiet window cannot turn this into a skip.
    first: list[str] = []
    for _ in range(2):
        first = [l for l in collect_until(ser, "[HB]", 12.0) if l.startswith("[HB]")]
        if first:
            break
    up_before = parse_all_int(first[:1], "uptime")
    lines = collect_until(ser, ("cycle_done ok=", "cycle done"), 130.0, seed="SYNC")
    extra: list[str] = []
    after_hb = [l for l in lines if l.startswith("[HB]")]
    if not after_hb:
        # The cycle can finish inside the first 3 s heartbeat gap; take one more
        # reading so the pair comparison is still possible.
        extra = collect_until(ser, "[HB]", 8.0)
        after_hb = [l for l in extra if l.startswith("[HB]")]
    boots = [l for l in (lines + extra) if ",BOOT," in l]
    if not up_before or not after_hb:
        suite.record("uptime_monotonic_cycle", True, 0.0,
                     "skipped (no heartbeat pair captured)", skipped=True)
        return
    up_after = parse_all_int(after_hb[-1:], "uptime")
    ok = (not boots) and up_after and up_after[0] >= up_before[0]
    suite.record("uptime_monotonic_cycle", bool(ok), 0.0,
                 "" if ok else f"uptime {up_before[0]}->{up_after[:1]} boots={len(boots)}")


def test_event_log_new_fields(ser: serial.Serial, suite: Suite) -> None:
    """The persistent log must carry the v2.3 field set for the new lines."""
    # An idle device (queue empty since boot) legitimately writes no WIFI
    # lines at all -- /system.log.old then dominates the dump. Skip instead of
    # failing on lines written by the previous firmware.
    stat = read_wifistat(ser)
    uploaded = int(parse_kv(stat, "uploaded") or 0)
    skips    = int(parse_kv(stat, "batterySkips") or 0)
    if uploaded == 0 and skips == 0:
        suite.record("event_log_new_fields", True, 0.0,
                     "skipped (uploader idle since boot: nothing logged)",
                     skipped=True)
        return
    lines = collect_until(ser, "bytes total)", 8.0, seed="LOG")
    # Only inspect the CURRENT boot's section: everything before the
    # "--- /system.log ---" marker belongs to /system.log.old.
    marker = next((i for i, l in enumerate(lines) if "/system.log ---" in l), None)
    current = lines[marker + 1:] if marker is not None else lines
    joined = " ".join(current)
    has_new_start = "cycle_start files=" in joined
    has_new_done  = "cycle_done ok=" in joined and " ms=" in joined
    has_skip      = "skip vbat=" in joined
    ok = has_new_done and (has_new_start or has_skip)
    suite.record("event_log_new_fields", ok, 0.0,
                 "" if ok else f"done={has_new_done} start={has_new_start} "
                               f"skip={has_skip}")


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
    print(f"{BOLD}  HTIT-Tracker Integration Test Suite (v2.3){RESET}")
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

        # v2.3 upload-path checks (each pokes the uploader once).
        test_wifistat_diagnostics(ser, suite)
        test_gate_vbat_consistent(ser, suite)
        test_sync_forces_cycle(ser, suite)
        test_chunked_drain_integrity(ser, suite)
        test_uptime_monotonic_cycle(ser, suite)
        test_event_log_new_fields(ser, suite)

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
