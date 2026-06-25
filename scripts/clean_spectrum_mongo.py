#!/usr/bin/env python3
"""Cleanup corrupted spectrumData from MongoDB tracker_samples collection.

Corruption symptoms (due to firmware snprintf buffer truncation pre-v1.2.6):
- spectrumData arrays with wrong element counts (783-784 instead of 1024)
- Elements containing values > 65535 (uint16 max), clearly timestamp bleed

Usage:
    python clean_spectrum_mongo.py --dry-run     # Preview what would be cleaned
    python clean_spectrum_mongo.py --execute     # Actually clean the data
"""
import argparse
from pymongo import MongoClient
from datetime import datetime

MONGO_URI = "mongodb://ryan:Welcome123%21@localhost:27017/?authSource=admin"
DB_NAME = "radiacode"
COLLECTION = "tracker_samples"

def main():
    parser = argparse.ArgumentParser(description="Clean corrupted spectrumData from MongoDB")
    parser.add_argument("--dry-run", action="store_true", help="Preview only, don't modify data")
    parser.add_argument("--execute", action="store_true", help="Actually clean the data")
    args = parser.parse_args()

    if not args.dry_run and not args.execute:
        print("Use --dry-run to preview or --execute to clean")
        return

    client = MongoClient(MONGO_URI)
    db = client[DB_NAME]
    coll = db[COLLECTION]

    print(f"Connected to {DB_NAME}.{COLLECTION}")

    # Stats: total docs with spectrumData
    total_with_spectrum = coll.count_documents({"spectrumData": {"$exists": True, "$ne": None}})
    print(f"Total docs with spectrumData: {total_with_spectrum}\n")

    # Find corrupted docs - values > 65535 or wrong array length
    corrupt_filter = {
        "spectrumData": {"$exists": True, "$ne": None},
        "$or": [
            {"spectrumData.1024": {"$exists": False}},  # Not exactly 1024 elements
            {"spectrumData": {"$elemMatch": {"$gt": 65535}}},  # Value exceeds uint16 max
        ]
    }
    corrupt_count = coll.count_documents(corrupt_filter)
    print(f"Corrupted docs found: {corrupt_count}")

    # Group by session for visibility
    sessions_corr = list(coll.distinct("sessionId", corrupt_filter))
    print(f"Affecting {len(sessions_corr)} sessions:\n")
    for sid in sorted(sessions_corr):
        count = coll.count_documents({"sessionId": sid, **corrupt_filter})
        print(f"  {sid}: {count} corrupted rows")

    if args.dry_run:
        print("\n[Dry run] No changes made. Use --execute to clean.")
        return

    # Clean: unset spectrumData on corrupted docs
    result = coll.update_many(
        corrupt_filter,
        [{"$set": {"spectrumData": None}}]
    )
    print(f"\nCleaned {result.modified_count} docs")

    # Verify stats
    after_total = coll.count_documents({"spectrumData": {"$exists": True, "$ne": None}})
    after_corrupt = coll.count_documents(corrupt_filter)
    print(f"After cleanup: {after_total} valid spectrum docs, {after_corrupt} still corrupted")

if __name__ == "__main__":
    main()