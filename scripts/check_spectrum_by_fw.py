"""Check what spectrum data looks like in the most recent session."""
import pymongo

client = pymongo.MongoClient("mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin")
db = client.radiacode

# Get most recent sample with spectrum from today's session
sample = db.tracker_samples.find_one(
    {"sessionId": "2026-06-24", "spectrumData": {"$exists": True, "$ne": []}},
)
if sample:
    spec = sample["spectrumData"]
    if spec:
        print(f"SAMPLE DETAILS:")
        print(f"  Length: {len(spec)}")
        print(f"  Min: {min(spec)}, Max: {max(spec)}")
        print(f"  Mean: {sum(spec)/len(spec):.1f}")
        # Check for all zeros
        non_zeros = sum(1 for x in spec if x > 0)
        print(f"  Non-zero channels: {non_zeros}/{len(spec)}")
        variance = sum((x - sum(spec)/len(spec))**2 for x in spec) / len(spec)
        import math
        std_dev = math.sqrt(variance)
        mean_v = sum(spec)/len(spec)
        cv = std_dev / mean_v if mean_v > 0 else 0
        print(f"  Std dev: {std_dev:.1f}, CV: {cv:.2f}")
        # Print first 64 channels as raw values
        print(f"  Channels 0-63: {spec[:64]}")
    else:
        print("spectrumData exists but is empty/null")
else:
    print("No spectrum data in 2026-06-24 session")

# Also check what previous sessions look like between 1.2.3 and 1.2.2 transitions
print(f"\n{'='*60}")
print(f"SPECTRUM ACROSS FIRMWARE VERSIONS:")
print(f"{'='*60}")

for fw, session_id in [("1.2.4", "2026-06-24"), ("1.2.3", "2026-06-23"), ("1.2.2", "2026-06-22"), ("1.2.0", "2026-06-21")]:
    samples = list(db.tracker_samples.find(
        {"sessionId": session_id, "spectrumData": {"$exists": True, "$ne": []}}
    ).limit(1))
    
    if samples:
        spec = samples[0]["spectrumData"]
        print(f"\nFW {fw} ({session_id}):")
        print(f"  length={len(spec):4d}")
        print(f"  min={min(spec):>12d}, max={max(spec):>12d}")
        mean_v = sum(spec) / len(spec)
        variance = sum((x - mean_v)**2 for x in spec) / len(spec)
        std_dev = math.sqrt(variance)
        print(f"  mean={mean_v:>8.1f}, std={std_dev:>8.1f}")
    else:
        print(f"\nFW {fw} ({session_id}): NO SPECTRUM DATA")

# Check if we can find the vsAddr that was read for CONFIGUREMENT - look at logs on device
print(f"\n{'='*60}")
print(f"Checking system logs...")
print(f"{'='*60}")

# The device stores boot logs in /system.log via event_log subsystem
# Let's query the upload history to see if we get any useful info
uploads = list(db.tracker_uploads.find(
    {"sessionId": "2026-06-24"},
    sort=[("receivedAt", -1)]
).limit(3))

if uploads:
    for u in uploads:
        print(f"Upload at {u['receivedAt']}: firmware={u.get('firmware')}, rows Accepted={u.get('rowsAccepted')}")
