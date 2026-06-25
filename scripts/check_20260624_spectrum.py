"""Detailed analysis of 2026-06-24 spectrum distribution."""
import pymongo
import math

client = pymongo.MongoClient("mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin")
db = client.radiacode

# Get ALL spectrum samples from today's session
print(f"=== 2026-06-24 Spectrum Analysis ===\n")

cursor = db.tracker_samples.find({"sessionId": "2026-06-24", "spectrumData.0": {"$exists": True}})

length_dist = {}
zero_count = 0
nonzero_patterns = []

for doc in cursor:
    spec = doc.get("spectrumData", [])
    length = len(spec)
    
    length_dist[length] = length_dist.get(length, 0) + 1
    
    mn = min(spec)
    mx = max(spec)
    mean_v = sum(spec) / len(spec) if len(spec) > 0 else 0
    
    non_zeros = sum(1 for x in spec if x > 0)
    
    if non_zeros == 0:
        zero_count += 1
        continue
    
    is_garbage = mx > 65535
    if not is_garbage:
        # Check if it looks physically reasonable
        variance = sum((x - mean_v)**2 for x in spec) / len(spec)
        std_dev = math.sqrt(variance)
        cv = std_dev / mean_v if mean_v > 0 else 0
        
        pattern_type = "REASONABLE" if (0.1 < cv < 5 and mean_v < 50000) else "WEIRD_GOOD_RANGE"
        nonzero_patterns.append({
            'len': length, 'min': mn, 'max': mx, 'mean': mean_v,
            'cv': cv, 'type': pattern_type,
            'first32': spec[:32]
        })

print(f"Total spectrum samples: {sum(length_dist.values())}")
print(f"All-zero spectra: {zero_count}")
print(f"\nChannel count distribution:")
for length in sorted(length_dist.keys()):
    print(f"  {length} channels: {length_dist[length]} samples")

print(f"\nNon-garbage patterns ({len(nonzero_patterns)}):")
for p in nonzero_patterns:
    print(f"  {p['type']}: len={p['len']} min={p['min']} max={p['max']} mean={p['mean']:.0f}")
    print(f"    first32: {p['first32']}")
