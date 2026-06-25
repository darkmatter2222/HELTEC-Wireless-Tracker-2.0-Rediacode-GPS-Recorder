"""Check for samples that show REAL spectrum data (not zeros, not garbage)."""
from pymongo import MongoClient
import struct

uri = "mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin"
client = MongoClient(uri)
db = client.radiacode

# Check 377-382 channel range samples - these seem like they might have real data
print("=== Checking 377-382 channel samples ===")
for target_len in range(377, 383):
    s = db.tracker_samples.find_one(
        {"spectrumData": {"$exists": True}},
        sort=[("timestampMs", -1)]
    )
    # Find by manually scanning
    cursor = db.tracker_samples.find(
        {"spectrumData.0": {"$gte": 1}}  # at least first channel non-zero
    ).sort("timestampMs", -1).limit(20)
    
found_nonzero = []
for s in db.tracker_samples.find({"spectrumData.0": {"$gte": 1}}).limit(50):
    spec = s["spectrumData"]
    mn = min(spec)
    mx = max(spec)
    length = len(spec)
    found_nonzero.append((length, mn, mx))

# Deduplicate by looking at unique lengths and their patterns
from collections import defaultdict
patterns = defaultdict(list)
for length, mn, mx in found_nonzero:
    # Bin by 10-count ranges
    bin_len = (length // 10) * 10
    if mx < 10**6:
        patterns[bin_len].append((mn, mx))

for bin_len in sorted(patterns.keys()):
    entries = patterns[bin_len]
    print(f"\nLength bin {bin_len} ({len(entries)} samples):")
    for mn, mx in entries[:3]:
        print(f"  min={mn}, max={mx}")

# Check if ANY sample looks physically reasonable (CsI(Tl) spectrum should have peak ~channel 20-50)
print("\n=== Looking for physically plausible spectra ===")
reasonable = 0
for s in db.tracker_samples.find({"spectrumData": {"$exists": True}}).limit(500):
    spec = s["spectrumData"]
    if len(spec) < 10:
        continue
    # Real spectrum should have variation (std dev relative to mean)
    mean_v = sum(spec) / len(spec)
    if mean_v == 0 or mean_v >= 65534:
        continue
    variance = sum((x - mean_v)**2 for x in spec) / len(spec)
    std_dev = variance ** 0.5
    cv = std_dev / mean_v if mean_v > 0 else 0  # coefficient of variation
    if 0.1 < cv < 5 and mean_v < 1000:  # likely real spectrum shape
        reasonable += 1
        print(f"  FOUND: len={len(spec)} mean={mean_v:.1f} std={std_dev:.1f} cv={cv:.2f}")
        print(f"    first 30: {spec[:30]}")

print(f"\nReasonable spectra found in 500 samples: {reasonable}")
