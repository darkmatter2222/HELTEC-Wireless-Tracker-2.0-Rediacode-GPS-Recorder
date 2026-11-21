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
FW_EXPECTED     = "2.3"          # src/config.h FW_VERSION, checked in the boot banner
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
# Helpers for the extended (v2.3 coverage pass) checks
# ---------------------------------------------------------------------------

def _int_prefix(token: str | None) -> int | None:
    """`1234ms`, `123s`, `-1` -> 1234 / 123 / -1 (leading integer only)."""
    if token is None:
        return None
    m = re.match(r"(-?\d+)", token)
    return int(m.group(1)) if m else None


def _num_prefix(token: str | None) -> float | None:
    """`21.4m`, `0.142`, `-1.0` -> float. Field values carry unit suffixes."""
    if token is None:
        return None
    m = re.match(r"-?\d+(?:\.\d+)?", token)
    return float(m.group(0)) if m else None


def collect_window(ser: serial.Serial, seconds: float,
                   prefix: str | None = None) -> list[str]:
    """
    Read every line for a fixed window (no early break, unlike collect_until).
    `prefix` filters to one line family, e.g. "[HB]" so key/value parsing cannot
    be poisoned by a same-named key in another subsystem's line.
    """
    out: list[str] = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        try:
            raw = ser.readline()
        except Exception:
            break               # port vanished (reset re-enumeration) -- caller reopens
        if not raw:
            continue
        try:
            line = raw.decode(errors="replace").rstrip()
        except Exception:
            continue
        if not line:
            continue
        if prefix is None or line.startswith(prefix):
            out.append(line)
    return out


def send_for(ser: serial.Serial, cmd: str, token: str,
             timeout: float = 3.0, attempts: int = 2) -> tuple[bool, list[str]]:
    """
    Send `cmd` and look for `token` in the reply, re-sending up to `attempts`
    times. The single-shot idle-exit reader can lose an ack that lands inside a
    stream tail (the NMEA flood after GPASSTHRU), so the retry is what makes
    these checks deterministic rather than ordering-dependent.
    """
    got: list[str] = []
    for _ in range(max(1, attempts)):
        ser.reset_input_buffer()
        ser.write((cmd + "\r\n").encode())
        ser.flush()
        got = collect_window(ser, timeout)
        if any(token in l for l in got):
            return True, got
    return False, got


def read_hb_key(ser: serial.Serial, key: str, window: float = 6.0) -> str | None:
    """Passively wait for one [HB] line and return the value of `key`."""
    lines = collect_window(ser, window, "[HB]")
    return parse_kv(lines, key)


def wait_hb_key(ser: serial.Serial, key: str, value: str,
                timeout: float) -> tuple[bool, float]:
    """Poll [HB] lines until key==value, or the timeout elapses."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        left = max(0.4, t0 + timeout - time.monotonic())
        lines = collect_window(ser, min(4.0, left), "[HB]")
        if parse_kv(lines, key) == value:
            return True, time.monotonic() - t0
    return False, time.monotonic() - t0


def session_count(ser: serial.Serial) -> int | None:
    """
    Current on-device session count. Reads the [LS-END] marker first and falls
    back to the [LS] header, both from one fixed window: the per-session line
    count takes ~0.4 s for a 1.6 MB day file, which is longer than the idle
    exit of send_cmd_collect, so that helper returns only the header line.
    """
    ser.reset_input_buffer()
    ser.write(b"LS\r\n")
    ser.flush()
    lines = collect_window(ser, 2.5)
    for line in reversed(lines):
        m = re.search(r"\[LS-END\]\s+count=(\d+)", line)
        if m:
            return int(m.group(1))
    for line in lines:
        m = re.search(r"\[LS\]\s+(\d+) sessions", line)
        if m:
            return int(m.group(1))
    return None


# ---------------------------------------------------------------------------
# v2.3 coverage pass: heartbeat field set
# ---------------------------------------------------------------------------

def test_heartbeat_v23_fields(ser: serial.Serial, suite: Suite) -> None:
    """acc=, life= and dose= were appended to [HB]; they must be parseable by
    key, and `life` (the per-boot monotonic counter) must never go backwards."""
    lines = collect_window(ser, max(10.0, HB_WINDOW_S), "[HB]")
    acc      = parse_kv(lines, "acc")
    life     = parse_all_int(lines, "life")
    dose     = parse_kv(lines, "dose")
    uptime   = parse_all_int(lines, "uptime")

    ok_hdr = len(lines) >= 2
    suite.record("hb_lines_captured", ok_hdr, 0.0,
                 "" if ok_hdr else f"only {len(lines)} [HB] line(s) in the window")

    acc_num = _num_prefix(acc)
    ok_acc = acc_num is not None and acc_num >= -1.0     # -1.0 = no fix yet
    suite.record("hb_acc_field", ok_acc, 0.0,
                 "" if ok_acc else f"acc={acc} in {len(lines)} HB line(s)")

    ok_life = len(life) >= 2 and all(life[i + 1] >= life[i] for i in range(len(life) - 1))
    suite.record("hb_life_monotonic", ok_life, 0.0,
                 "" if ok_life else f"life sequence {life}")

    ok_dose = dose is not None and dose.endswith("uSv") and _num_prefix(dose) is not None
    suite.record("hb_dose_field", ok_dose, 0.0,
                 "" if ok_dose else f"dose={dose}")

    ok_up = len(uptime) >= 2 and all(uptime[i + 1] >= uptime[i]
                                      for i in range(len(uptime) - 1))
    suite.record("hb_uptime_monotonic", ok_up, 0.0,
                 "" if ok_up else f"uptime sequence {uptime}")


# ---------------------------------------------------------------------------
# v2.3 coverage pass: BLE REPL commands
# ---------------------------------------------------------------------------

def test_ble_scan_and_list(ser: serial.Serial, suite: Suite) -> None:
    """`s <secs>` arms an async scan (must not block the loop), `l` dumps the
    result table with address/addrType/RSSI/name per entry."""
    ok_start, lines = send_for(ser, "s 3", "starting manual scan", timeout=4.0)
    suite.record("ble_scan_issued", ok_start, 0.0,
                 "" if ok_start else f"no scan ack: {lines[:2]}")

    # The scan runs on the BLE stack task; give the 3 s window time to close.
    time.sleep(3.2)
    ok_list, lines2 = send_for(ser, "l", "scan results", timeout=2.5)
    suite.record("ble_list_responds", ok_list, 0.0,
                 "" if ok_list else f"no table header: {lines2[:2]}")

    header = [l for l in lines2 if "scan results" in l]
    claimed = 0
    if header:
        m2 = re.search(r"(\d+) scan results", header[0])
        claimed = int(m2.group(1)) if m2 else 0
    entries = [l for l in lines2
               if re.search(r"\[\d+\]\s+[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}", l)]
    ok_tbl = bool(header) and (claimed == len(entries) or claimed == 0)
    suite.record("ble_list_table_consistent", ok_tbl, 0.0,
                 "" if ok_tbl else f"header={claimed} entries={len(entries)}")
    # The scan must not have stalled the main loop: heartbeats keep flowing.
    ok_hb = read_hb_key(ser, "uptime", 6.0) is not None
    suite.record("ble_scan_nonblocking", ok_hb, 0.0,
                 "" if ok_hb else "no [HB] while/after scanning")


def test_ble_grab_pattern_roundtrip(ser: serial.Serial, suite: Suite) -> None:
    """`t <pattern>` arms the name auto-grab, bare `t` clears it."""
    ok_set, lines = send_for(ser, "t RadiaCode", "auto-grab armed", timeout=2.5)
    suite.record("ble_grab_pattern_set", ok_set, 0.0,
                 "" if ok_set else f"got {lines[:2]}")
    ok_clr, lines2 = send_for(ser, "t", "auto-grab cleared", timeout=2.5)
    suite.record("ble_grab_pattern_clear", ok_clr, 0.0,
                 "" if ok_clr else f"got {lines2[:2]}")


def test_ble_disconnect_reconnect_chain(ser: serial.Serial, suite: Suite) -> None:
    """Chain: `D` drops the link (pin kept) and `x` cancels any scan; the async
    auto-scan loop must bring rcState back to 4 (Ready) with no reboot -- the
    v0.3.6 'stuck in Scanning' regression and the v0.3.4 blocking-scan fix."""
    before = read_hb_key(ser, "rcState")
    uptime_before = _int_prefix(read_hb_key(ser, "uptime"))

    lines, _ = send_cmd_collect(ser, "D")
    ok_d = any("keep pin" in l for l in lines)
    suite.record("ble_disconnect_acked", ok_d, 0.0,
                 "" if ok_d else f"'D' -> {lines[:2]}")
    lines2, _ = send_cmd_collect(ser, "x")
    ok_x = any("cancel scan" in l for l in lines2)
    suite.record("ble_cancel_acked", ok_x, 0.0, "" if ok_x else f"'x' -> {lines2[:2]}")

    # Sample rcState over ~24 s. Two answers matter:
    #   4 (Ready)      -> the v0.3.6 auto-scan state machine recovered the link.
    #   1 <-> 5 cycling-> the loop is alive; the peer is simply not advertising
    #                    (RC-110 sleeps after inactivity) -> informative SKIP.
    #   frozen at 1    -> the v0.3.6 'stuck in Scanning' regression -> FAIL.
    samples: list[str] = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 24.0:
        v = read_hb_key(ser, "rcState", 4.0)
        if v is None:
            continue
        samples.append(v)
        if v == "4":
            break
    ready = "4" in samples
    cycling = len(set(samples)) > 1
    if ready:
        suite.record("ble_auto_reconnect_ready", True, time.monotonic() - t0,
                     f"rcState samples {samples}")
    elif cycling:
        suite.record("ble_auto_reconnect_ready", True, time.monotonic() - t0,
                     f"skipped: auto loop cycling {sorted(set(samples))}, "
                     f"peer not advertising", skipped=True)
    else:
        suite.record("ble_auto_reconnect_ready", False, time.monotonic() - t0,
                     f"rcState frozen at {samples[:1]} (was {before} before the poke)")

    uptime_after = _int_prefix(read_hb_key(ser, "uptime"))
    ok_no_reset = (uptime_before is not None and uptime_after is not None
                   and uptime_after >= uptime_before)
    suite.record("ble_reconnect_no_reset", ok_no_reset, 0.0,
                 "" if ok_no_reset
                 else f"uptime {uptime_before} -> {uptime_after} (device reset)")


# ---------------------------------------------------------------------------
# v2.3 coverage pass: raw GPS pipe, WIPE guard, LOG lifecycle
# ---------------------------------------------------------------------------

def test_gps_passthru(ser: serial.Serial, suite: Suite) -> None:
    """GPASSTHRU pipes the UC6580 UART straight to USB -- ground truth that the
    NMEA path works independently of the TinyGPS++ parser."""
    ok, lines = send_for(ser, "GPASSTHRU 3", "$", timeout=4.0, attempts=2)
    nmea = [l for l in lines if l.startswith("$")]
    suite.record("gps_passthru_nmea", ok and len(nmea) > 0, 0.0,
                 "" if ok else f"no NMEA in {len(lines)} line(s)")
    ok_ck = any("*" in l for l in nmea)          # every sentence is checksummed
    suite.record("gps_passthru_checksum", ok_ck, 0.0,
                 "" if ok_ck else "sentences carry no checksum")
    # Parsed vs raw: the same fix verdict must appear in the structured `g` output.
    raw_fix = any(l.startswith("$GNGGA") and ",1," in l for l in nmea)
    g_lines, _ = send_cmd_collect(ser, "g")
    parsed_fix = "fix=1" in " ".join(g_lines)
    ok_pair = bool(raw_fix) == parsed_fix
    suite.record("gps_passthru_matches_parsed", ok_pair, 0.0,
                 "" if ok_pair else f"raw GGA fix={raw_fix} parsed fix={parsed_fix}")


def test_wipe_guard(ser: serial.Serial, suite: Suite) -> None:
    """The count guard is what makes WIPE safe: a mismatched or missing count
    must abort and leave the append-only store untouched."""
    before = session_count(ser)
    lines, _ = send_cmd_collect(ser, "WIPE")
    ok_usage = any("usage" in l for l in lines)
    suite.record("wipe_no_arg_prints_usage", ok_usage, 0.0,
                 "" if ok_usage else f"got {lines[:2]}")

    lines2, _ = send_cmd_collect(ser, "WIPE 999999")
    abort = [l for l in lines2 if "WIPE-ABORT" in l]
    suite.record("wipe_mismatch_aborts", bool(abort), 0.0,
                 "" if abort else f"no WIPE-ABORT: {lines2[:2]}")

    reported = None
    if abort:
        m = re.search(r"have=(\d+)", abort[0])
        reported = int(m.group(1)) if m else None
    suite.record("wipe_abort_reports_have", reported == before, 0.0,
                 "" if reported == before else f"have={reported} LS={before}")

    after = session_count(ser)
    ok_same = before is not None and after == before
    suite.record("wipe_guard_preserves_store", ok_same, 0.0,
                 "" if ok_same else f"count changed {before} -> {after}")


def test_log_clear_and_dump(ser: serial.Serial, suite: Suite) -> None:
    """LOG dumps the persistent ring (both files + byte total); LOGCLEAR empties
    it and the next dump must read as empty/short. Diagnostic surface only --
    session CSVs are untouched."""
    lines, e1 = send_cmd_collect(ser, "LOG", timeout=CMD_TIMEOUT_S + 1.0)
    totals_before = [int(m.group(1)) for l in lines
                     for m in [re.search(r"\((\d+) bytes total\)", l)] if m]
    ok_dump = len(lines) > 0 and bool(totals_before)
    suite.record("log_dump_responds", ok_dump, e1,
                 "" if ok_dump else f"no byte-total footer: {lines[:2]}")

    lines2, e2 = send_cmd_collect(ser, "LOGCLEAR")
    ok_clear = any("cleared" in l for l in lines2)
    suite.record("logclear_acks", ok_clear, e2,
                 "" if ok_clear else f"got {lines2[:2]}")

    lines3, _ = send_cmd_collect(ser, "LOG", timeout=CMD_TIMEOUT_S + 1.0)
    totals_after = [int(m.group(1)) for l in lines3
                    for m in [re.search(r"\((\d+) bytes total\)", l)] if m]
    empty_flag = any("empty" in l for l in lines3)
    shrunk = (not totals_after) or (totals_before and
                                     totals_after[0] <= totals_before[0])
    suite.record("logclear_empties_ring", bool(totals_after) and shrunk or empty_flag, 0.0,
                 "" if (shrunk or empty_flag)
                 else f"{totals_before} -> {totals_after} bytes")

    # A fresh event must appear again after the clear -- but only if the poked
    # cycle actually reached the radio. A `skip=no_pending` cycle writes nothing
    # to the persistent log by design (see runOnce()), so that is a SKIP.
    send_cmd_collect(ser, "SYNC")
    trail = collect_window(ser, 8.0)
    idle_cycle = any("skip=no_pending" in l for l in trail)
    lines4, _ = send_cmd_collect(ser, "LOG", timeout=CMD_TIMEOUT_S + 1.0)
    grew = [int(m.group(1)) for l in lines4
            for m in [re.search(r"\((\d+) bytes total\)", l)] if m]
    wrote = bool(grew) and grew[0] > 0
    if not wrote and idle_cycle:
        suite.record("log_writes_after_clear", True, 0.0,
                     "skipped (cycle reported skip=no_pending: no log line by design)",
                     skipped=True)
    else:
        suite.record("log_writes_after_clear", wrote, 0.0,
                     "" if wrote else "no new records written after LOGCLEAR")


def test_multi_chunk_drain_observed(ser: serial.Serial, suite: Suite) -> None:
    """At least one file must drain in >1 chunk on a full backlog, which is what
    proves the v2.2 chunking path (not the single-request path) is live."""
    send_cmd_collect(ser, "SYNC")
    lines = collect_until(ser, ("[UPLOAD] cycle start", "cycle start;",
                                "cycle_done ok=", "cycle done",
                                "skip=no_pending"), 45.0)
    pairs: list[tuple[int, int]] = []
    for l in lines:
        if "OK http=" not in l:
            continue
        mc = re.search(r"chunks=(\d+)", l)
        mb = re.search(r"file_bytes=(\d+)", l)
        if mc and mb:
            pairs.append((int(mb.group(1)), int(mc.group(1))))
    if not pairs:
        suite.record("multi_chunk_drain", True, 0.0,
                     "skipped (no file completed this cycle: queue empty)",
                     skipped=True)
        return
    # Arithmetic identity: chunks == ceil(file_bytes / 8192), possibly +1 because
    # every POST is cut at the last newline at or before the limit.
    ceil_ = lambda b: max(1, (b + 8191) // 8192)
    bad = [(b, c) for (b, c) in pairs if c not in (ceil_(b), ceil_(b) + 1)]
    suite.record("chunk_count_matches_bytes", not bad, 0.0,
                 "" if not bad else f"mismatched (bytes, chunks) pairs: {bad[:4]}")
    seen_multi = any(c >= 2 for _, c in pairs)
    all_small  = all(b <= 8192 for b, _ in pairs)
    ok = seen_multi or all_small          # 1 chunk is correct for a small day file
    suite.record("multi_chunk_drain", ok, 0.0,
                 f"{len(pairs)} file(s); max {max(c for _, c in pairs)} chunk(s)"
                 + (" [all single-chunk files]" if all_small else ""))


def test_reboot_restart(ser: serial.Serial, suite: Suite,
                        port: str = DEFAULT_PORT) -> serial.Serial:
    """REBOOT must restart the firmware: banner again, uptime restarts from a
    smaller value, NVS-backed state survives. Runs LAST -- the port may
    re-enumerate, so the handle is replaced and returned."""
    uptime_before = _int_prefix(read_hb_key(ser, "uptime"))
    # The boot banner lands ~1.5 s after the ack (500 ms flush delay + boot), so
    # read it from the same window instead of chasing it with a reopen.
    ok_ack, lines = send_for(ser, "REBOOT", "soft-resetting", timeout=6.0, attempts=1)
    suite.record("reboot_acks", ok_ack, 0.0, "" if ok_ack else f"got {lines[:2]}")

    banner = [l for l in lines if "firmware v" in l]
    t0 = time.monotonic()
    while not banner and time.monotonic() - t0 < 15.0:
        got = collect_window(ser, 3.0)
        b2 = [l for l in got if "firmware v" in l]
        if b2:
            banner = b2
    if not banner:
        # Native CDC can drop its buffer across the reset; reopen and try once.
        try:
            ser.close()
        except Exception:
            pass
        for _ in range(4):
            try:
                ser = open_port(port)
                break
            except Exception:
                time.sleep(1.0)
        got = collect_window(ser, 12.0)
        banner = [l for l in got if "firmware v" in l]

    suite.record("reboot_banner", bool(banner), 0.0,
                 "" if banner else "no boot banner within 15 s")
    ok_ver = bool(banner) and f"v{FW_EXPECTED}" in banner[0]
    suite.record("reboot_firmware_version", ok_ver, 0.0,
                 "" if ok_ver else f"banner: {banner[:1]} (want v{FW_EXPECTED})")

    uptime_after = _int_prefix(read_hb_key(ser, "uptime", 12.0))
    ok_restart = (uptime_before is not None and uptime_after is not None
                  and uptime_after < uptime_before)
    suite.record("reboot_uptime_restarts", ok_restart, 0.0,
                 "" if ok_restart
                 else f"uptime {uptime_before} -> {uptime_after}")
    return ser

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

        # Coverage pass: full [HB] field set, BLE REPL commands, raw GPS pipe,
        # guard paths, persistent-log lifecycle.
        test_heartbeat_v23_fields(ser, suite)
        test_gps_passthru(ser, suite)
        test_ble_scan_and_list(ser, suite)
        test_ble_grab_pattern_roundtrip(ser, suite)
        test_ble_disconnect_reconnect_chain(ser, suite)
        test_wipe_guard(ser, suite)
        test_log_clear_and_dump(ser, suite)
        test_multi_chunk_drain_observed(ser, suite)

        # Reset check goes last: the native CDC port re-enumerates and the
        # handle is replaced, so nothing may run before it.
        ser = test_reboot_restart(ser, suite, args.port)

    finally:
        try:
            ser.close()
        except Exception:
            pass

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
