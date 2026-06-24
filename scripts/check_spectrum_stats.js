db = db.getSiblingDB('radiacode');
const docs = [];
db.tracker_samples.find({spectrumData: {$exists: true, $ne: null, $ne: []}}).limit(10).forEach(d => {
  docs.push({
    channels: d.spectrumData.length,
    sum: d.spectrumData.reduce((a,b) => a+b, 0),
    first5: d.spectrumData.slice(0, 5),
    maxVal:Math.max(...d.spectrumData),
    sessionId: d.sessionId,
    count65535: d.spectrumData.filter(v => v === 65535).length
  });
});
print(JSON.stringify(docs, null, 2));

// Overall stats
const all = db.tracker_samples.aggregate([
  {$match: {spectrumData: {$exists: true, $ne: null}}},
  {$group: {_id: null, avgChannels: {$avg: {$size: "$spectrumData"}}, totalDocs: {$sum: 1}}}
]).toArray();
print('avg channels across all spectrum docs:', JSON.stringify(all));
