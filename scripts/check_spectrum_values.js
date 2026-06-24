db = db.getSiblingDB('radiacode');

// Check a few random documents for value distribution
const doc1 = db.tracker_samples.findOne({spectrumData: {$exists: true, $ne: null}});
if (doc1) {
  var valueCounts = {};
  doc1.spectrumData.forEach(v => { valueCounts[v] = (valueCounts[v] || 0) + 1; });
  print('doc1 channels:', doc1.spectrumData.length);
  print('doc1 unique values:', JSON.stringify(valueCounts));
  
  // Show non-zero, non-65535 values and their indices
  var realIndices = [];
  for (var i = 0; i < doc1.spectrumData.length; i++) {
    if (doc1.spectrumData[i] !== 0 && doc1.spectrumData[i] !== 65535) {
      realIndices.push({ch: i, val: doc1.spectrumData[i]});
    }
  }
  print('real non-zero non-65535 values:', JSON.stringify(realIndices.slice(0, 20)));
  
  // Show first and last 10 channels
  print('first_10:', JSON.stringify(doc1.spectrumData.slice(0, 10)));
  print('last_10:', JSON.stringify(doc1.spectrumData.slice(-10)));
}
