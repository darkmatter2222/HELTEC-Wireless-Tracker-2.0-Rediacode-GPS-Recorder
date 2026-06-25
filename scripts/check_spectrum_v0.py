"""Inspect the v0-format spectrum samples more closely."""
from pymongo import MongoClient

uri = "mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin"
client = MongoClient(uri)
db = client.radiacode

# Find a 783-channel sample (v0 decoder attempt on v1 data, or actual v0 data?)
sample = db.tracker_samples.find_one(
    {"spectrumData": {"$exists": True}, "spectrumData.0": {"$gte": 1}},
    sort=[("timestampMs", -1)]
)

if not sample:
    print("No samples with non-zero first channel found")
    exit()

spec = sample["spectrumData"]
print(f"Channels: {len(spec)}")
print(f"First 64 values: {spec[:64]}")
print(f"Min: {min(spec)}, Max: {max(spec)}")

# Check for float-like patterns (large ints that look like they could be floats misinterpreted)
import struct
print("\nFirst 10 values as hex:")
for v in spec[:10]:
    if v > 2**32:
        print(f"  {v} (0x{v:x}) - exceeds uint32!")
    elif v > 65535:
        # Show as float bytes
        try:
            h = f"0x{v:08x}"
            as_float = struct.unpack('<f', v.to_bytes(4, 'little'))[0]
            print(f"  {v} ({h}) as float32 = {as_float}")
        except:
            print(f"  {v} (0x{v:x})")
    else:
        print(f"  {v} (0x{v:04x})")

# Distribution analysis
unique_vals = set(spec)
print(f"\nUnique values: {len(unique_vals)}")
if len(unique_vals) <= 100:
    print(f"All unique values: {sorted(unique_vals)[:50]}")
