#!/usr/bin/env python
"""
Quick check of a few random spectrum documents from MongoDB to understand the data patterns.
"""
from pymongo import MongoClient
import json
import random

client = MongoClient("mongodb://ryan:Welcome123%21@host.docker.internal:27017/?authSource=admin")
db = client["radiacode"]
samples = db["tracker_samples"]

# Get docs that have spectrum data
specs = list(samples.find({"spectrumData": {"$exists": True, "$ne": None}}, {"spectrumData": 1, "uSvPerHour": 1, "cps": 1}).limit(5))

for idx, doc in enumerate(specs):
    print(f"\n=== Doc {idx} ===")
    data = doc.get("spectrumData", [])
    if isinstance(data, list):
        if len(data) > 0:
            non_empty = [x for x in data if x]
            print(f"  channels: {len(data)}")
            print(f"  non-zero: {len(non_empty)}")
            print(f"  min: {min(data)}, max: {max(data)}")
            non_zero_counts = [x for x in data if x > 0]
            if non_zero_counts:
                avg_sz = sum(non_zero_counts) / len(non_zero_counts) if non_zero_counts else 0
                print(f"  avg of non-zero: {avg_sz:.1f}")
            total_sum = sum(data) if data else 0
            print(f"  sum of all channels: {total_sum}")
            print(f"  distribution breakdown:")
            count_zero = data.count(0) if isinstance(data, list) else 0
            count_65535 = data.count(65535) if isinstance(data, list) else 0
            lt_10 = sum(1 for x in data if 0 < x < 10)
            ge_65535 = sum(1 for x in data if x == 65535)
            gt_1k = sum(1 for x in data if 1000 < x < 65535)
            print(f"    0:          {count_zero}")
            print(f"    1-9:        {lt_10}")
            print(f"    1000-65534: {gt_1k}")
            print(f"    65535:      {ge_65535}")
            # Show first and last values
            print(f"  first 20 channels: {data[:20]}")
        else:
            print("  empty array")
    elif isinstance(data, str):
        parts = data.split("|")
        ints = [int(x) for x in parts if x.strip()]
        print(f"  type: STRING (pipe-delimited)")
        print(f"  channels parsed: {len(ints)}")
        if ints:
            print(f"  min: {min(ints)}, max: {max(ints)}")
            print(f"  first 20: {ints[:20]}")

client.close()
