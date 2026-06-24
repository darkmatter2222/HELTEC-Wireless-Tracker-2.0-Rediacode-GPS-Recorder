const db = db.getSiblingDB('radiacode');

// Check what type spectrumData actually is in MongoDB
print('=== Checking spectrumData types ===');
let i = 0;
const cursor = db.tracker_samples.find({spectrumData: {$exists: true}}).limit(20);
while (cursor.hasNext() && i < 5) {
    const d = cursor.next();
    print(`doc ${i}: type=${typeof d.spectrumData}, isArray=${Array.isArray(d.spectrumData)}, length=${d.spectrumData?.length || 'n/a'}`);
    if (Array.isArray(d.spectrumData)) {
        const unique = [...new Set(d.spectrumData)].sort((a,b) => a - b);
        print(`  unique vals (${unique.length}):`, JSON.stringify(unique.slice(0, 25)));
        i++;
    }
}
