const db = db.getSiblingDB('radiacode');

// First doc - full array analysis
print('=== Full analysis of first spectrum document ===');
let i = 0;
const cursor = db.tracker_samples.find({spectrumData: {$exists: true, $type: 'array'}});
while (cursor.hasNext() && i < 1) {
    const d = cursor.next();
    const data = d.spectrumData;
    print('channels:', data.length);

    const unique = [...new Set(data)].sort((a,b) => a - b);
    print('unique values count:', unique.length);
    print('first 25 unique:', JSON.stringify(unique.slice(0, 25)));

    // Value frequencies
    const counts = {};
    for (const v of data) {
        counts[v] = (counts[v] || 0) + 1;
    }
    print('top 20 value frequencies:', JSON.stringify(Object.entries(counts).sort((a,b) => b[1]-a[1]).slice(0, 20)));

    // Value distribution by range
    const ranges = {'zero': 0, 'one_to_ten': 0, 'eleven_to_hundred': 0, 'hundred_to_thousand': 0, 'above_thousand': 0, 'sixtyfive': 0};
    for (const v of data) {
        if (v === 0) ranges['zero']++;
        else if (v <= 10) ranges['one_to_ten']++;
        else if (v <= 100) ranges['eleven_to_hundred']++;
        else if (v <= 1000) ranges['hundred_to_thousand']++;
        else if (v < 65535) ranges['above_thousand']++;
        else ranges['sixtyfive']++;
    }
    print('distribution by range:', JSON.stringify(ranges));

    i++;
}

// Now search for docs with realistic spectrum data
print('\n=== Scanning first 200 array docs for realistic counts ===');
let foundRealistic = 0;
const cursor2 = db.tracker_samples.find({spectrumData: {$exists: true, $type: 'array'}}).limit(200);
while (cursor2.hasNext()) {
    const d = cursor2.next();
    const data = d.spectrumData;
    if (data.length > 20) {
        const realVals = data.filter(v => v > 0 && v < 65535);
        const maxVal = realVals.length > 0 ? Math.max(...realVals) : 0;
        const totalChannels = data.length;
        const realFraction = realVals.length / totalChannels;

        if (realFraction > 0.1 && maxVal < 200) {
            print('REALISTIC found:', data.length, 'ch', 'non_zero_real=', realVals.length, 'max=', maxVal);
            foundRealistic++;
            if (foundRealistic >= 3) break;
        }
    }
}
if (foundRealistic === 0) print('NO realistic spectrum found in first 200 docs');

