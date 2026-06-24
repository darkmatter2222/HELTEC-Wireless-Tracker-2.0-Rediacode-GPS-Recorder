// Use --eval mode compatible syntax for mongosh 2.6.0
const db = db.getSiblingDB('radiacode');

print('=== Spectrum documents overview ===');
const totalSpectrumDocs = db.tracker_samples.countDocuments({spectrumData: {$exists: true, $ne: []}});
print('total spectrum docs:', totalSpectrumDocs);

// Check what raw spectrumData looks like - string or array?
print('\n=== Raw sample (first doc) ===');
const rawDoc = db.tracker_samples.findOne({spectrumData: {$exists: true}}, {projection: {spectrumData: 1}});
if (rawDoc) {
    print('type:', typeof rawDoc.spectrumData);
    if (typeof rawDoc.spectrumData === 'string') {
        print('first 200 chars:', rawDoc.spectrumData.substring(0, 200));
    } else if (Array.isArray(rawDoc.spectrumData)) {
        print('array length:', rawDoc.spectrumData.length);
        const unique = [...new Set(rawDoc.spectrumData)].sort((a,b) => a - b);
        print('unique values:', JSON.stringify(unique));
    }
}

// If stored as array, do full analysis
print('\n=== Searching for realistic spectrum data ===');
let foundRealistic = 0;
const cursor = db.tracker_samples.find({spectrumData: {$exists: true, $ne: []}}).limit(500);
while (cursor.hasNext()) {
    const d = cursor.next();
    const data = Array.isArray(d.spectrumData) ? d.spectrumData : null;
    if (!data || data.length === 0) continue;
    const maxVal = Math.max(...data);
    const minVal = Math.min(...data);
    if (maxVal < 500 && maxVal > 0 && data.length > 20) {
        print(`REALISTIC: ${data.length} ch, min=${minVal}, max=${maxVal}`);
        foundRealistic++;
        if (foundRealistic >= 3) break;
    }
}
if (foundRealistic === 0) print('NONE of first 500 spectrum docs had realistic counts');

