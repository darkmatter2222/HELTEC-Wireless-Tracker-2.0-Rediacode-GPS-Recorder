"""Check channel counts and huge values across all docs"""
import pymongo
from collections import Counter

client = pymongo.MongoClient("mongodb://ryan:Welcome123!@192.168.86.48:27017/?authSource=admin", serverSelectionTimeoutMS=5000)
db = client.radiacode
coll = db.tracker_samples

# Check ALL spectrum docs for values > 65535
huge_docs = []
for doc in coll.find({"sessionId": "2026-06-24", "spectrumData": {"$exists": True, "$ne": "", "$ne": []}}):
    spec = doc["spectrumData"]
    if isinstance(spec, list):
        for i, v in enumerate(spec):
            if isinstance(v, (int, float)) and v > 65535:
                huge_docs.append({"ts": doc["timestampMs"], "idx": i, "val": v})

print(f"Total docs with spectrum: {coll.count_documents({'sessionId': '2026-06-24', 'spectrumData': {'$exists': True, '$ne': '', '$ne': []}})}")
print(f"Out-of-range values found: {len(huge_docs)}")
for d in huge_docs[:30]:
    print(f"  ts={d['ts']}, index={d['idx']}, value={d['val']}")

# Also check channel counts per document - should be ~64 for RC-110
print("\nChannel count distribution:")
counts = []
for doc in coll.find({"sessionId": "2026-06-24", "spectrumData": {"$exists": True, "$ne": "", "$ne": []}}):
    spec = doc["spectrumData"]
    if isinstance(spec, list):
        counts.append(len(spec))

for ch_len, cnt in Counter(counts).most_common():
    print(f"  {ch_len} channels: {cnt} docs")

# Check the session file size / upload info
print("\nUpload records for 2026-06-24:")
uploads = list(db.tracker_uploads.find({"sessionId": "2026-06-24"}).sort("receivedAt", -1))
for u in uploads[:5]:
    print(f"  receivedAt={u.get('receivedAt')}, rowsSeen={u.get('rowsSeen')}, payloadBytes={u.get('payloadBytes')}")
