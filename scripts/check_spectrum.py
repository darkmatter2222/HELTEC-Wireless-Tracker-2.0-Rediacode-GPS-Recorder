import pymongo

client = pymongo.MongoClient("mongodb://ryan:Welcome123!@192.168.86.48:27017/?authSource=admin")
db = client["radiacode"]
coll = db.tracker_samples

# Count total samples vs samples with spectrum data
total = coll.count_documents({})
with_spectrum = coll.count_documents({"spectrumData": {"$exists": True, "$ne": "", "$ne": None}})

print(f"Total samples: {total}")
print(f"Samples with spectrumData: {with_spectrum}")
print()

if with_spectrum > 0:
    # Get latest few samples with spectrum
    for doc in coll.find({"spectrumData": {"$exists": True, "$ne": "", "$ne": None}}).sort("timestampMs", -1).limit(3):
        spec = doc.get("spectrumData", "")
        # spectrumData might be stored as string or list
        if isinstance(spec, list):
            channels = spec
        else:
            channels = spec.split("|") if spec else []
        from datetime import datetime
        dt = datetime.fromtimestamp(doc['timestampMs']/1000)
        print(f"  ts={doc['timestampMs']} ({dt}) cps={doc.get('cps')} uvh={doc.get('uSvPerHour')} lat={doc.get('lat')} lng={doc.get('lng')} sessionId={doc.get('sessionId')}")
        print(f"    type={type(spec).__name__}, channels: {len(channels)}, preview: {str(spec)[:150]}...")
        print()

    # Stats across spectrum samples
    print("--- Stats across spectrum samples ---")
    sample_docs = list(coll.find({"spectrumData": {"$exists": True, "$ne": "", "$ne": None}}).limit(100))
    channel_counts = []
    for d in sample_docs:
        s = d.get("spectrumData", "")
        if isinstance(s, list):
            channel_counts.append(len(s))
        elif s:
            channel_counts.append(len(s.split("|")))
    
    if channel_counts:
        print(f"  sampled docs: {len(channel_counts)}")
        print(f"  avgChannels: {sum(channel_counts)/len(channel_counts):.0f}")
        print(f"  min: {min(channel_counts)}, max: {max(channel_counts)}")
        
        # Check value ranges in first doc
        if isinstance(sample_docs[0].get("spectrumData"), list):
            first = sample_docs[0]["spectrumData"]
            print(f"  first doc values range: min={min(first)}, max={max(first)}, sum={sum(first)}")

    # Check for non-saturated data (values that aren't all 65535)
    print("\n--- Non-saturated spectrum samples ---")
    real_data_docs = []
    for d in sample_docs:
        s = d.get("spectrumData", "")
        if isinstance(s, list):
            non_sat = sum(1 for v in s if 0 < v < 65535)
            if non_sat > len(s) * 0.1:  # More than 10% real data
                real_data_docs.append((d, non_sat, len(s)))
    print(f"  Of {len(sample_docs)} sampled docs, {len(real_data_docs)} have >10% non-saturated channels")
    
    if real_data_docs:
        d, non_sat, total = real_data_docs[0]
        from datetime import datetime
        dt = datetime.fromtimestamp(d['timestampMs']/1000)
        spec = d["spectrumData"]
        real_vals = [v for v in spec if 0 < v < 65535]
        print(f"  Example ({dt}): {non_sat}/{total} channels have real data")
        print(f"    Real values: min={min(real_vals)}, max={max(real_vals)}, mean={sum(real_vals)/len(real_vals):.1f}")

else:
    print("No spectrum data found yet.")
    # Check when the last upload was
    last = coll.find().sort("timestampMs", -1).limit(1)
    for doc in last:
        from datetime import datetime
        dt = datetime.fromtimestamp(doc['timestampMs']/1000)
        print(f"Latest sample: ts={doc['timestampMs']} ({dt}) sessionId={doc.get('sessionId')} hasSpecField={'spectrumData' in doc}")
