const db = db.getSiblingDB('radiacode');

// Check firmware versions on spectrum-bearing docs
print('=== Firmware version distribution for spectrum docs ===');
const fwPipes = db.tracker_samples.aggregate([
    {$match: {spectrumData: {$exists: true, $type: 'array'}}},
    {$group: {_id: '$firmware', count: {$sum: 1}}},
    {$sort: {count: -1}}
]);
for (const doc of fwPipes) {
    print('fw=' + (doc._id || 'null') + ', docs=' + doc.count);
}

// Check channel count distribution
print('');
print('=== Channel count distribution ===');
const chDist = {};
const cursor = db.tracker_samples.find({spectrumData: {$exists: true, $type: 'array'}}).limit(500);
while (cursor.hasNext()) {
    const d = cursor.next();
    const ch = d.spectrumData.length;
    chDist[ch] = (chDist[ch] || 0) + 1;
}
print('channel counts:', JSON.stringify(Object.entries(chDist).sort((a,b) => a[0]-b[0])));

// Check if ANY doc has spectrum values other than 0 and 65535
print('');
print('=== Hunting for docs with real count data ===');
let found = 0;
const cursor2 = db.tracker_samples.find({spectrumData: {$exists: true, $type: 'array'}});
while (cursor2.hasNext() && found < 5) {
    const d = cursor2.next();
    const unique = [...new Set(d.spectrumData)];
    if (unique.length > 2) {
        print('FOUND doc with', unique.length, 'unique values:', JSON.stringify(unique.slice(0, 30)));
        found++;
    }
}
if (found === 0) {
    const totalDocs = db.tracker_samples.countDocuments({spectrumData: {$exists: true, $type: 'array'}});
    print('CONFIRMED: ALL', totalDocs, 'spectrum docs contain ONLY 0 and 65535');
}

// Timestamp range
print('');
print('=== Spectrum timestamp range ===');
const minTs = db.tracker_samples.findOne({spectrumData: {$exists: true}}, {sort: {timestampMs: 1}, projection: {timestampMs: 1}});
const maxTs = db.tracker_samples.findOne({spectrumData: {$exists: true}}, {sort: {timestampMs: -1}, projection: {timestampMs: 1}});
if (minTs && maxTs) {
    print('earliest:', new Date(minTs.timestampMs).toISOString());
    print('latest:', new Date(maxTs.timestampMs).toISOString());
}
