db = db.getSiblingDB('radiacode');

// Find a document with some non-65535, non-zero real values
const docs = [];
const cursor = db.tracker_samples.find({spectrumData: {$exists: true, $ne: null, $ne: []}}).limit(100);
while (cursor.hasNext()) {
  const d = cursor.next();
  var non65k = d.spectrumData.filter(v => v !== 65535);
  if (non65k.length > 2) {
    docs.push({
      channels: d.spectrumData.length,
      non65kCount: non65k.length,
      non65kVals: non65k.slice(0, 20),
      sessionId: d.sessionId
    });
  }
}
print('docs with >2 real values:', JSON.stringify(docs));
