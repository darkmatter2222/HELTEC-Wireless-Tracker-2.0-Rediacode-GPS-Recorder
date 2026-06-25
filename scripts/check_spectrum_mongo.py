import pymongo, sys

client = pymongo.MongoClient("mongodb://ryan:Welcome123!@192.168.86.48:27017/?authSource=admin", serverSelectionTimeoutMS=5000)
db = client.radiacode
coll = db.tracker_samples

# Find sessions that have spectrum data
sessions_with_spectrum = coll.distinct("sessionId", {"spectrumData": {"$exists": True, "$ne": ""}})
print(f"Sessions with spectrumData: {len(sessions_with_spectrum)}")
for s in sorted(sessions_with_spectrum)[-5:]:
    print(f"  {s}")

# Check latest session's spectrum values
latest = sorted(sessions_with_spectrum)[-1] if sessions_with_spectrum else None
if not latest:
    print("NO sessions have spectrum data")
    sys.exit(0)

samples = list(coll.find({"sessionId": latest, "spectrumData": {"$exists": True, "$ne": ""}}).limit(5))
print(f"\nLatest session {latest}: {len(samples)} samples with spectrum (first 5)")

for doc in samples:
    spec = doc.get("spectrumData", "")
    # spectrumData can be string (pipe-delimited) or list depending on import path
    if isinstance(spec, list):
        parts = [str(v) for v in spec]
    elif spec:
        parts = spec.split("|")
    else:
        parts = []
    numeric = []
    for p in parts[:10]:
        try:
            numeric.append(int(p))
        except:
            numeric.append(0)
    print(f"  ts={doc.get('timestampMs')}, channels={len(parts)}, first10={numeric}")

# Check value ranges across ALL spectrum data in this session
all_spec = list(coll.find({"sessionId": latest, "spectrumData": {"$exists": True, "$ne": ""}}, {"spectrumData": 1})[:20])
print(f"\nScanning {len(all_spec)} documents for value ranges:")
max_val = 0
min_nonzero = 999999
zeros_count = 0
sat65535 = 0
for doc in all_spec:
    spec = doc["spectrumData"]
    if isinstance(spec, list):
        parts = [str(v) for v in spec]
    else:
        parts = spec.split("|")
    for p in parts:
        try:
            v = int(p)
        except:
            continue
        if v == 0:
            zeros_count += 1
        elif v == 65535:
            sat65535 += 1
        else:
            min_nonzero = min(min_nonzero, v)
        max_val = max(max_val, v)

    total_vals = sum(len(spec if isinstance(spec, list) else spec.split("|")) for d in all_spec if (spec := d.get("spectrumData")))
print(f"  total channel values scanned: {total_vals}")
print(f"  zero count:     {zeros_count} ({100*zeros_count/total_vals:.1f}%)")
print(f"  saturated 65535: {sat65535} ({100*sat65535/total_vals:.1f}%)")
print(f"  max value:      {max_val}")
if min_nonzero < 999999:
    print(f"  min non-zero:     {min_nonzero}")
