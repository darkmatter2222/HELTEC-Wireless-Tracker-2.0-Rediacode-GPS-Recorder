const db = mongo.getDB('radiacode');

// Find a spectrum document and inspect values
const doc = db.tracker_samples.findOne({spectrumData: {$exists: true}});
if (!doc) { print('no spectrum docs found'); process.exit(1); }

print('=== Single document analysis ===');
print('channels:', doc.spectrumData.length);
print('unique values count:', new Set(doc.spectrumData).size);

const vals = [...new Set(doc.spectrumData)].sort((a,b) => a - b);
print('sorted unique values (first 30):', JSON.stringify(vals.slice(0, 30)));

// Check value distribution
const counts = {};
for (const v of doc.spectrumData) {
    counts[v] = (counts[v] || 0) + 1;
}
print('value frequencies (sorted by count desc):', JSON.stringify(Object.entries(counts).sort((a,b) => b[1]-a[1]).slice(0, 15)));

// Check first and last few channels specifically
print('first 10 channels:', JSON.stringify(doc.spectrumData.slice(0, 10)));
print('last 10 channels:', JSON.stringify(doc.spectrumData.slice(-10)));
