#!/usr/bin/env python3
"""Query last 30 days of data from MongoDB for dose rate vs count rate analysis."""
import pymongo
from datetime import datetime, timezone, timedelta

client = pymongo.MongoClient(
    "mongodb://ryan:Welcome123%21@localhost:27017/?authSource=admin",
    serverSelectionTimeoutMS=5000
)
db = client["radiacode"]

# Show recent sessions
print("=" * 60)
print("RECENT SESSIONS")
print("=" * 60)
sessions = list(db.tracker_sessions.find({}, {"sessionId":1,"samples":1,"firstTsMs":1,"lastTsMs":1}).sort("firstTsMs",-1).limit(15))
for s in sessions:
    first_dt = datetime.fromtimestamp(s["firstTsMs"]/1000, tz=timezone.utc) if "firstTsMs" in s else "?"
    print(f"  {s.get('sessionId','?'):20s}  samples={s.get('samples',0):>6}  first={first_dt}")

# Last 30 days from now
now = datetime.now(timezone.utc)
start = (now - timedelta(days=30)).replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000
end = now.timestamp() * 1000

print(f"\n{'=' * 60}")
print(f"LAST 30 DAYS ({(now - timedelta(days=30)).strftime('%Y-%m-%d')} to {now.strftime('%Y-%m-%d')})")
print("=" * 60)

total = db.tracker_samples.count_documents({"timestampMs": {"$gte": start, "$lt": end}})
print(f"Total samples: {total:,}")

# Breakdown by session
weekend_sessions = list(db.tracker_sessions.aggregate([
    {"$match": {"firstTsMs": {"$gte": start, "$lt": end}}},
    {"$project": {"sessionId":1,"samples":1}},
    {"$sort": {"sessionId": 1}}
]))
for s in weekend_sessions:
    print(f"  {s['sessionId']}: {s['samples']:,} samples")

# Dose rate vs CPS statistics
print(f"\n{'=' * 60}")
print("DOSE RATE vs CPS STATISTICS (last 30 days)")
print("=" * 60)

stats = list(db.tracker_samples.aggregate([
    {"$match": {
        "timestampMs": {"$gte": start, "$lt": end},
        "uSvPerHour": {"$ne": None},
        "cps": {"$ne": None},
        "uSvPerHour": {"$gt": 0},
        "cps": {"$gt": 0}
    }},
    {
        "$group": {
            "_id": None,
            "avg_uSv": {"$avg": "$uSvPerHour"},
            "avg_cps": {"$avg": "$cps"},
            "min_uSv": {"$min": "$uSvPerHour"},
            "max_uSv": {"$max": "$uSvPerHour"},
            "min_cps": {"$min": "$cps"},
            "max_cps": {"$max": "$cps"},
            "count": {"$sum": 1},
            # Compute dose/cps ratio
            "avg_ratio": {
                "$avg": {
                    "$cond": [
                        {"$gt": ["$cps", 0]},
                        {"$divide": ["$uSvPerHour", "$cps"]},
                        None
                    ]
                }
            },
            "min_ratio": {
                "$min": {
                    "$cond": [
                        {"$gt": ["$cps", 0]},
                        {"$divide": ["$uSvPerHour", "$cps"]},
                        None
                    ]
                }
            },
            "max_ratio": {
                "$max": {
                    "$cond": [
                        {"$gt": ["$cps", 0]},
                        {"$divide": ["$uSvPerHour", "$cps"]},
                        None
                    ]
                }
            }
        }
    }
]))

if stats:
    s = stats[0]
    print(f"Count:              {s['count']:,}")
    print(f"Avg uSv/hr:         {s['avg_uSv']:.4f}")
    print(f"Avg CPS:            {s['avg_cps']:.2f}")
    print(f"Ratio uSv/hr / CPS avg: {s['avg_ratio']:.6f}")
    print(f"Ratio range:        {s['min_ratio']:.6f} - {s['max_ratio']:.6f}")
    print(f"\nuSv/hr range:     {s['min_uSv']:.4f} - {s['max_uSv']:.4f}")
    print(f"CPS range:         {s['min_cps']:.2f} - {s['max_cps']:.2f}")

# Ratio distribution — bin the dose/cps ratio
print(f"\n{'=' * 60}")
print("DOSE/CPS RATIO DISTRIBUTION (binned)")
print("=" * 60)

ratio_bins = list(db.tracker_samples.aggregate([
    {"$match": {
        "timestampMs": {"$gte": start, "$lt": end},
        "uSvPerHour": {"$ne": None, "$gt": 0},
        "cps": {"$ne": None, "$gt": 0}
    }},
    {"$addFields": {
        "ratio": {
            "$cond": [
                {"$gt": ["$cps", 0]},
                {"$divide": ["$uSvPerHour", "$cps"]},
                None
            ]
        }
    }},
    {"$bucket": {
        "groupBy": "$ratio",
        "boundaries": [0, 0.005, 0.01, 0.015, 0.02, 0.025, 0.03, 0.035, 0.04, 0.05, 0.06, 0.08, 0.1, 1.0],
        "default": "outliers",
        "output": {
            "count": {"$sum": 1},
            "avg_uSv": {"$avg": "$uSvPerHour"},
            "avg_cps": {"$avg": "$cps"}
        }
    }},
    {"$sort": {"_id.min": 1}}
]))

for b in ratio_bins:
    bid = b["_id"]
    if isinstance(bid, dict) and "min" in bid:
        lo, hi = bid["min"], bid.get("max", "")
        label = f"{lo:.5f} - {hi:.5f}"
    else:
        label = str(bid)
    print(f"  {label:>24s}:  count={b['count']:>6}  avg_uSv={b['avg_uSv']:.4f}  avg_cps={b['avg_cps']:.2f}")

# Check for anomalous samples (high CPS, low dose or vice versa)
print(f"\n{'=' * 60}")
print("TOP 15 HIGHEST DOSE/CPS RATIO (possible hard radiation)")
print("=" * 60)

high_ratio = list(db.tracker_samples.aggregate([
    {"$match": {
        "timestampMs": {"$gte": start, "$lt": end},
        "uSvPerHour": {"$ne": None, "$gt": 0},
        "cps": {"$ne": None, "$gt": 0}
    }},
    {"$addFields": {
        "ratio": {"$divide": ["$uSvPerHour", "$cps"]}
    }},
    {"$sort": {"ratio": -1}},
    {"$limit": 15},
    {"$project": {
        "timestampMs": 1, "uSvPerHour": 1, "cps": 1,
        "ratio": 1, "latitude": 1, "longitude": 1, "sessionId": 1
    }}
]))

for i, row in enumerate(high_ratio):
    ts = datetime.fromtimestamp(row["timestampMs"]/1000, tz=timezone.utc)
    print(f"  {i+1:2d}. ratio={row['ratio']:.6f}  uSv/hr={row['uSvPerHour']:.4f}  cps={row['cps']:.2f}  @ {ts:%H:%M:%S}  session={row.get('sessionId','?')}")

print(f"\nTOP 15 LOWEST DOSE/CPS RATIO (possible soft radiation)")
print("=" * 60)

low_ratio = list(db.tracker_samples.aggregate([
    {"$match": {
        "timestampMs": {"$gte": start, "$lt": end},
        "uSvPerHour": {"$ne": None, "$gt": 0},
        "cps": {"$ne": None, "$gt": 0}
    }},
    {"$addFields": {
        "ratio": {"$divide": ["$uSvPerHour", "$cps"]}
    }},
    {"$sort": {"ratio": 1}},
    {"$limit": 15},
    {"$project": {
        "timestampMs": 1, "uSvPerHour": 1, "cps": 1,
        "ratio": 1, "latitude": 1, "longitude": 1, "sessionId": 1
    }}
]))

for i, row in enumerate(low_ratio):
    ts = datetime.fromtimestamp(row["timestampMs"]/1000, tz=timezone.utc)
    print(f"  {i+1:2d}. ratio={row['ratio']:.6f}  uSv/hr={row['uSvPerHour']:.4f}  cps={row['cps']:.2f}  @ {ts:%H:%M:%S}  session={row.get('sessionId','?')}")

client.close()
