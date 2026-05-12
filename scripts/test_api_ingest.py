"""Synthetic ingest test for v0.5.1 deviceId derivation.

Posts a fake CSV with deviceId in the row data (firmware never sends
the X-Device-Id header), then verifies the session metadata gets the
derived deviceId. Cleans up after itself with a soft-delete + purge.
"""
import json, urllib.request, urllib.parse, time, sys

API = "http://192.168.86.48:8030"
SESSION = f"SYNTH-{int(time.time())}"
DEVICE_ID = "AABBCCDD0011"

# 10-column CSV matching firmware 0.3.0+ schema. Timestamps in 2026.
header = "timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,speedKph,bearingDeg,altitudeM,hdop\n"
rows = []
base = int(time.time() * 1000)
for i in range(10):
    rows.append(f"{base+i*1000},0.1{i},5.{i},47.6062,-122.3321,{DEVICE_ID},0.0,,12.4,1.5")
body = header + "\n".join(rows) + "\n"

# POST without X-Device-Id header (exactly like firmware does)
req = urllib.request.Request(
    f"{API}/ingest/csv",
    data=body.encode("utf-8"),
    method="POST",
    headers={
        "Content-Type": "text/csv",
        "X-Session-Id": SESSION,
        "X-Tracker-Id": "esp32-synth-test",
        "X-Firmware":   "0.4.8-test",
    },
)
with urllib.request.urlopen(req) as r:
    print(f"POST status: {r.status}")
    print(f"POST body: {r.read().decode()}")

# Check the resulting session metadata
with urllib.request.urlopen(f"{API}/sessions") as r:
    sessions = json.loads(r.read())
match = next((s for s in sessions if s["sessionId"] == SESSION), None)
assert match is not None, f"Session {SESSION} not found"
print(f"\nResulting session metadata:")
for k in ("sessionId", "deviceId", "trackerId", "firmware", "samples"):
    print(f"  {k}: {match.get(k)!r}")

# Assertions
assert match["deviceId"]  == DEVICE_ID, f"deviceId mismatch: got {match['deviceId']!r}"
assert match["trackerId"] == "esp32-synth-test"
assert match["firmware"]  == "0.4.8-test"
assert match["samples"]   == 10
print("\nAll assertions PASSED.")

# Cleanup: soft-delete then purge
req = urllib.request.Request(f"{API}/sessions/{SESSION}?confirm=DELETE_CONFIRMED", method="DELETE")
urllib.request.urlopen(req).read()
req = urllib.request.Request(f"{API}/admin/purge/{SESSION}?confirm=PURGE_CONFIRMED", method="POST")
urllib.request.urlopen(req).read()
print(f"Cleaned up synthetic session {SESSION}")
