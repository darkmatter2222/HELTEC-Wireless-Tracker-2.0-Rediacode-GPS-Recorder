#!/usr/bin/env python3
"""Quick check of spectrum data in MongoDB for 2026-06-21 session."""
import pymongo, pprint

client = pymongo.MongoClient(
    "mongodb://ryan:Welcome123!@192.168.86.48:27017/?authSource=admin"
)
db = client["radiacode"]

# Count samples for today's session
total = db.tracker_samples.count_documents({"sessionId": "2026-06-21"})
print(f"=== Today's session (2026-06-21) ===")
print(f"Total samples: {total}")

# Check if ANY have spectrumData field
with_spec = db.tracker_samples.count_documents({
    "sessionId": "2026-06-21",
    "spectrumData": {"$exists": True}
})
print(f"With spectrumData field: {with_spec}")

# Get a sample with spectrum if any
sample_with_spec = db.tracker_samples.find_one({
    "sessionId": "2026-06-21",
    "spectrumData": {"$exists": True, "$ne": None, "$ne": ""}
})
if sample_with_spec:
    print("\nSample WITH spectrum data:")
    pprint.pprint({k: v for k, v in sample_with_spec.items() if k != "_id"})

# Get a normal sample for comparison
normal = db.tracker_samples.find_one({"sessionId": "2026-06-21"})
if normal:
    print("\nNormal sample fields:")
    pprint.pprint({k: v for k, v in normal.items() if k != "_id"})

# Storage impact - compare document sizes
spec_docs = list(db.tracker_samples.find({
    "sessionId": "2026-06-21",
    "spectrumData": {"$exists": True}
}).limit(100))

normal_docs = list(db.tracker_samples.find({
    "sessionId": "2026-06-21",
    "spectrumData": {"$exists": False}
}).limit(100))

if spec_docs:
    avg_spec_size = sum(len(str(d).encode()) for d in spec_docs) / len(spec_docs)
    print(f"\nAvg spectrum doc size (sample of {len(spec_docs)}): {avg_spec_size:.0f} bytes")

if normal_docs:
    avg_normal_size = sum(len(str(d).encode()) for d in normal_docs) / len(normal_docs)
    print(f"Avg normal doc size (sample of {len(normal_docs)}): {avg_normal_size:.0f} bytes")

# Check what's in the spectrumData field if it exists
print("\n=== Spectrum data quality ===")
spec_values = list(db.tracker_samples.find(
    {"sessionId": "2026-06-21", "spectrumData": {"$exists": True}},
    {"spectrumData": 1, "_id": 0}
).limit(50))

non_empty = [s for s in spec_values if s.get("spectrumData")]
print(f"Non-empty spectrum values (of {len(spec_values)}): {len(non_empty)}")

if non_empty:
    print("\nFirst 5 spectrum values:")
    for s in non_empty[:5]:
        val = s.get("spectrumData", "")
        channels = val.split("|") if isinstance(val, str) else []
        print(f"  {val[:60]}... ({len(channels)} channels)")

# Compare with yesterday's data (no spectrum support in firmware)
yesterday_total = db.tracker_samples.count_documents({"sessionId": "2026-06-19"})
print(f"\n=== Yesterday's session (2026-06-19) ===")
print(f"Total samples: {yesterday_total}")
