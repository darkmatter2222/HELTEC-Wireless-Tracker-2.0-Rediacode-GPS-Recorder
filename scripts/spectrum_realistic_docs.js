const db = db.getSiblingDB('radiacode');

// Check the 377-channel docs - firmware 1.2.2
print('=== Analyzing 377-channel documents ===');
let count377 = 0;
const cursor = db.tracker_samples.find({spectrumData: {$exists: true, $type: 'array'}}).limit(500);
while (cursor.hasNext() && count377 < 5) {
    const d = cursor.next();
    if (d.spectrumData.length === 377) {
        const unique = [...new Set(d.spectrumData)].sort((a,b) => a - b);
        print('doc channels=377, unique count:', unique.length, 'values:', JSON.stringify(unique));

        // Check position of non-65535 values
        const positions = {};
        for (let idx = 0; idx < d.spectrumData.length; idx++) {
            const v = d.spectrumData[idx];
            if (v !== 65535 && v !== 0) {
                positions[v] = positions[v] || [];
                positions[v].push(idx);
            }
        }
        if (Object.keys(positions).length > 0) {
            print('non-trivial values at positions:', JSON.stringify(positions));
        }
        count377++;
    }
}

// Now check the docs that HAVE real data
print('\n=== Analyzing documents with realistic spectrum counts ===');
let realisticDocs = 0;
const cursor2 = db.tracker_samples.find({spectrumData: {$exists: true, $type: 'array'}});
while (cursor2.hasNext() && realisticDocs < 5) {
    const d = cursor2.next();
    const data = d.spectrumData;
    const unique = [...new Set(data)].sort((a,b) => a - b);

    // Find docs where most values are in a realistic range (0-100 for background radiation)
    const smallCount = data.filter(v => v >= 0 && v <= 100).length;
    if (smallCount > data.length * 0.5 && data.length > 20) {
        print('REALISTIC doc: channels=' + data.length + ', small_values=' + smallCount);
        print('unique values:', JSON.stringify(unique));

        // Show actual spectrum shape
        const nonZero = data.filter(v => v > 0);
        if (nonZero.length < 30) {
            print('non-zero values:', JSON.stringify(nonZero));
        } else {
            print('first 20 non-zero:', JSON.stringify(nonZero.slice(0, 20)));
        }
        realisticDocs++;
    }
}
