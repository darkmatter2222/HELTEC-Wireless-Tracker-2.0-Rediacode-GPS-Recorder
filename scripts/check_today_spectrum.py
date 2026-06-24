#!/usr/bin/env python3
import sys
from pymongo import MongoClient

client = MongoClient("mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin")
db = client["radiacode"]

docs = list(
    db.tracker_samples.find(
        {"sessionId": "2026-06-23", "spectrumData": {"$exists": True, "$ne": []}}
    )
    .sort("timestampMs", -1)
    .limit(5)
)

print(f"Found {len(docs)} spectrum docs for today")
for d in docs:
    spec = d.get("spectrumData", [])
    total = sum(spec[:64]) if spec else 0
    nonzero = sum(1 for x in spec if x != 0) if spec else 0
    maxv = max(spec[:64]) if spec else 0
    print(f"  ts={d['timestampMs']} len={len(spec)} total64={total} nonzero={nonzero} max64={maxv}")
    print(f"  first30={spec[:30]}")
