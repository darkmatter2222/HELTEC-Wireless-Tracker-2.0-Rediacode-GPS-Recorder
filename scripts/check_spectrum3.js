db = db.getSiblingDB('radiacode');
const doc = db.tracker_samples.findOne({spectrumData: {$exists: true, $ne: null, $ne: []}});
if (doc) {
  print('channels:', doc.spectrumData.length);
  print('first_20:', JSON.stringify(doc.spectrumData.slice(0, 20)));
  var totalSum = 0;
  for (var i = 0; i < doc.spectrumData.length; i++) totalSum += doc.spectrumData[i];
  print('totalSum:', totalSum);
  print('sessionId:', doc.sessionId);
} else {
  print('no spectrum docs found');
  const countWithField = db.tracker_samples.countDocuments({spectrumData: {$exists: true}});
  print('docs with spectrumData field at all:', countWithField);
}
