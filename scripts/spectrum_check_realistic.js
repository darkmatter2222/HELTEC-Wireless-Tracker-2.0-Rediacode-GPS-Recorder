const db = getSiblingDB('radiacode');

// Find spectrum documents and analyze channel count distribution
print('=== Spectrum documents overview ===');
const totalSpectrumDocs = db.tracker_samples.countDocuments({spectrumData: {$exists: true, $ne: []}});
print('total spectrum docs:', totalSpectrumDocs);

// Sample 47-channel doc (first one found)
print('\n=== 47-channel document ===');
const doc47 = db.tracker_samples.findOne({spectrumData: {$exists: true}}, {projection: {spectrumData: 1}});
if (doc47 && doc47.spectrumData.length === 47) {
    print('channels:', doc47.spectrumData.length);
    const unique = [...new Set(doc47.spectrumData)].sort((a,b) => a - b);
    print('unique values:', JSON.stringify(unique));
} else if (doc47) {
    print(`found doc with ${doc47.spectrumData.length} channels instead of 47`);
    const unique = [...new Set(doc47.spectrumData)].sort((a,b) => a - b);
    print('unique values:', JSON.stringify(unique));
}

// Check if ANY spectrum doc has values that look like real radiation data (0-100 range predominantly)
print('\n=== Searching for docs with realistic count range ===');
let foundRealistic = 0;
const cursor = db.tracker_samples.find({spectrumData: {$exists: true, $ne: []}}).limit(500);
while (cursor.hasNext()) {
    const doc = cursor.next();
    const data = doc.spectrumData;
    const maxVal = Math.max(...data);
    const minVal = Math.min(...data);
    // Realistic spectrum counts should be small numbers (background radiation is sparse)
    // If max < 1000 for a live measurement, that looks reasonable
    if (maxVal < 500 && maxVal > 0 && data.length > 20) {
        print(`FOUND realistic doc: ${data.length} ch, min=${minVal}, max=${maxVal}`);
        foundRealistic++;
        if (foundRealistic >= 3) break;
    }
}
if (foundRealistic === 0) {
    print('NONE of first 500 spectrum docs had realistic counts (< 500 max)');
}
