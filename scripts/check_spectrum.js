const doc = db.tracker_samples.findOne({spectrumData: {$exists: true, $ne: null, $ne: []}});
if (doc) {
  print('channels:', doc.spectrumData.length);
  print('first_20:', JSON.stringify(doc.spectrumData.slice(0, 20)));
  const totalSum = doc.spectrumData.reduce((a,b) => a + b, 0);
  print('totalSum:', totalSum);
  print('_id:', doc._id);
  print('sessionId:', doc.sessionId);
} else {
  print('no spectrum docs found');
}
