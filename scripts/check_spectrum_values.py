"""Quick spectrum data diagnostic from MongoDB."""
from pymongo import MongoClient

uri = "mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin"
client = MongoClient(uri)
db = client.radiacode

# Find most recent sample with spectrum data
sample = db.tracker_samples.find_one(
    {"spectrumData": {"$exists": True, "$ne": []}},
    sort=[("timestampMs", -1)]
)

if not sample:
    print("No spectrum samples found")
    exit()

spec = sample["spectrumData"]
print(f"Channels: {len(spec)}")
print(f"First 50: {spec[:50]}")

nz = [v for v in spec if v != 0 and v != 65535]
print(f"Non-trivial values (not 0, not 65535): {len(nz)}")
if nz:
    print(f"Examples: {nz[:20]}")
else:
    zeros = sum(1 for v in spec if v == 0)
    maxs = sum(1 for v in spec if v == 65535)
    print(f"Zeros: {zeros}, 65535s: {maxs}")

# Check a few more recent sessions
print("\n--- Checking value distribution across recent samples ---")
samples = list(db.tracker_samples.find(
    {"spectrumData": {"$exists": True, "$ne": []}},
    sort=[("timestampMs", -1)],
    limit=100
))
for s in samples[:10]:
    spec_data = s["spectrumData"]
    min_v = min(spec_data)
    max_v = max(spec_data)
    nz_count = sum(1 for v in spec_data if v != 0 and v != 65535)
    print(f"  timestampMs={s['timestampMs']} len={len(spec_data)} min={min_v} max={max_v} non_trivial={nz_count}")
