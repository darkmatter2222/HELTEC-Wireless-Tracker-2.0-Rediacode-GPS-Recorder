"""Find and inspect samples with astronomically large values."""
from pymongo import MongoClient
import struct

uri = "mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin"
client = MongoClient(uri)
db = client.radiacode

# Find samples where some value exceeds uint32 range (clear indicator of decoder bug)
samples = list(db.tracker_samples.find(
    {"spectrumData": {"$exists": True}, "spectrumData.0": {"$gte": 1}},
    sort=[("timestampMs", -1)],
    limit=50
))

for s in samples:
    spec = s["spectrumData"]
    mx = max(spec)
    mn = min(spec)
    if mx > 2**32:
        print(f"\n=== timestampMs={s['timestampMs']} len={len(spec)} ===")
        print(f"Min: {mn}, Max: {mx}")
        print(f"First 20: {spec[:20]}")
        # Check if these look like float32 bytes misinterpreted as int64
        print("As hex (first 10):")
        for v in spec[:10]:
            h = f"{v:x}"
            if v < 2**32:
                try:
                    as_float = struct.unpack('<f', v.to_bytes(4, 'little'))[0]
                    print(f"  0x{v:08x} ({v}) as float32={as_float:.6f}")
                except:
                    print(f"  0x{v:08x} ({v})")
            else:
                # Might be float64 or int32 that got sign-extended weirdly
                bytes_repr = v.to_bytes(8, 'little')
                as_float64 = struct.unpack('<d', bytes_repr)[0]
                print(f"  0x{v:016x} ({v}) as float64={as_float64:.6f}")
        break

# Also count by channel length to understand format splits
print("\n\n=== Distribution by channel count ===")
from collections import Counter
lens = []
for s in db.tracker_samples.find(
    {"spectrumData": {"$exists": True, "$ne": []}},
):
    lens.append(len(s["spectrumData"]))
for length, count in sorted(Counter(lens).items()):
    print(f"  {length} channels: {count} samples")
