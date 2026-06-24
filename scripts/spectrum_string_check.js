const db = db.getSiblingDB('radiacode');

// Find docs where spectrumData is actually stored as an array (not string)
print('=== Checking actual storage format ===');

let strCount = 0;
let arrCount = 0;
let nullCount = 0;

const cursor = db.tracker_samples.find({spectrumData: {$exists: true}}).limit(100);
while (cursor.hasNext()) {
    const d = cursor.next();
    if (typeof d.spectrumData === 'string') strCount++;
    else if (Array.isArray(d.spectrumData)) arrCount++;
    else nullCount++;
}

print(`string: ${strCount}, array: ${arrCount}, null/other: ${nullCount}`);

// Show raw string value
const strDoc = db.tracker_samples.findOne({spectrumData: {$type: 'string'}});
if (strDoc) {
    print('\n=== Raw spectrumData string (first 500 chars) ===');
    print(strDoc.spectrumData.substring(0, 500));

    // Parse and analyze
    const parts = strDoc.spectrumData.split('|').filter(x => x).map(Number);
    print('\n=== Parsed values ===');
    print(`parsed ${parts.length} channel values`);
    const unique = [...new Set(parts)].sort((a,b) => a - b);
    print(`unique values (${unique.length}):`, JSON.stringify(unique));

    // Value frequency
    const counts = {};
    for (const v of parts) {
        counts[v] = (counts[v] || 0) + 1;
    }
    print('top frequencies:', JSON.stringify(Object.entries(counts).sort((a,b) => b[1]-a[1]).slice(0, 20)));

    // Check for realistic spectrum data in strings
    print('\n=== Scanning for realistic string spectra ===');
    let foundRealistic = 0;
    const cursor2 = db.tracker_samples.find({spectrumData: {$type: 'string'}}).limit(500);
    while (cursor2.hasNext() && foundRealistic < 3) {
        const d = cursor2.next();
        const vals = d.spectrumData.split('|').filter(x => x).map(Number);
        if (vals.length > 20) {
            const maxVal = Math.max(...vals);
            const minVal = Math.min(...vals);
            if (maxVal < 100) {
                print('REALISTIC STRING:', vals.length, 'ch, min=', minVal, 'max=', maxVal);
                foundRealistic++;
            }
        }
    }
    if (foundRealistic === 0) print('NO realistic spectrum data in first 500 string docs');

}
