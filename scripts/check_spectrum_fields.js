const doc = db.tracker_samples.findOne();
if (doc) {
  print('sample keys:', JSON.stringify(Object.keys(doc).sort()));
} else {
  print('no docs');
}

// Check for any field containing "spectrum" in the name
const keys = {};
db.tracker_samples.find({}).limit(100).forEach(d => {
  Object.keys(d).forEach(k => { if (k.toLowerCase().indexOf('spec') >= 0) keys[k] = true; });
});
print('spectrum-related fields:', JSON.stringify(Object.keys(keys)));

// Count docs with spectrumData
const withSpectrum = db.tracker_samples.countDocuments({spectrumData: {$exists: true}});
print('docs with spectrumData field:', withSpectrum);

// Count docs where spectrumData is non-empty array
const withData = db.tracker_samples.countDocuments({spectrumData: {$exists: true, $type: 4, $ne: []}});
print('docs with non-empty spectrumData array:', withData);
