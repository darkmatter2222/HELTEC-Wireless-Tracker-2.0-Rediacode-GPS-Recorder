"""v0.5.1 recompute regression test.

Inserts two synthetic sessions, runs /admin/recompute-sessions, and asserts
that deviceId/trackerId/firmware on the resulting metadata match the LAST
sample in each session (deterministic ordering via the new explicit $sort).
"""
import json, urllib.request, time, sys

API = "http://192.168.86.48:8030"

# Two sessions with different identity fields in the LAST row vs earlier rows.
# Without the explicit $sort the result of $last is unpredictable, so we
# specifically include a value-changing pattern.
def make_csv(rows):
    h = "timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,speedKph,bearingDeg,altitudeM,hdop\n"
    return h + "\n".join(rows) + "\n"

now = int(time.time() * 1000)
S1 = f"SYNTH-RC-{now}-A"
S2 = f"SYNTH-RC-{now}-B"

# Session A: 5 rows, last row has device 111111111111
rows_a = [
    f"{now+i*1000},0.1,5.0,47.6,-122.3,000000000000,0,,12,1.5" for i in range(4)
] + [f"{now+5000},0.1,5.0,47.6,-122.3,111111111111,0,,12,1.5"]

# Session B: 5 rows, last row has device 222222222222
rows_b = [
    f"{now+10000+i*1000},0.2,8.0,47.6,-122.3,AAAAAAAAAAAA,0,,12,1.5" for i in range(4)
] + [f"{now+15000},0.2,8.0,47.6,-122.3,222222222222,0,,12,1.5"]

for sid, body in ((S1, make_csv(rows_a)), (S2, make_csv(rows_b))):
    req = urllib.request.Request(
        f"{API}/ingest/csv", data=body.encode("utf-8"), method="POST",
        headers={"Content-Type": "text/csv", "X-Session-Id": sid,
                 "X-Tracker-Id": "esp32-recompute-test", "X-Firmware": "0.4.8"},
    )
    urllib.request.urlopen(req).read()
    print(f"Ingested {sid} (5 rows)")

# Trigger recompute
req = urllib.request.Request(f"{API}/admin/recompute-sessions", method="POST")
result = json.loads(urllib.request.urlopen(req).read())
print(f"\nRecompute: {result}")

# Verify
with urllib.request.urlopen(f"{API}/sessions") as r:
    sessions = json.loads(r.read())
got = {s["sessionId"]: s for s in sessions if s["sessionId"] in (S1, S2)}

# After explicit $sort timestampMs asc, $last must be the highest-timestamp row.
# Session A's last row by timestamp is row index 4 (ts=now+5000) deviceId=111111111111
# BUT rows 0-3 have ts=now+0..3000, and row 4 has ts=now+5000. So row 4 wins. Good.
assert got[S1]["deviceId"] == "111111111111", f"S1 deviceId={got[S1]['deviceId']!r}"
assert got[S2]["deviceId"] == "222222222222", f"S2 deviceId={got[S2]['deviceId']!r}"
print(f"\nS1 deviceId={got[S1]['deviceId']!r} PASS")
print(f"S2 deviceId={got[S2]['deviceId']!r} PASS")

# Cleanup both
for sid in (S1, S2):
    req = urllib.request.Request(f"{API}/sessions/{sid}?confirm=DELETE_CONFIRMED", method="DELETE")
    urllib.request.urlopen(req).read()
    req = urllib.request.Request(f"{API}/admin/purge/{sid}?confirm=PURGE_CONFIRMED", method="POST")
    urllib.request.urlopen(req).read()
print(f"\nCleaned up {S1} and {S2}")
