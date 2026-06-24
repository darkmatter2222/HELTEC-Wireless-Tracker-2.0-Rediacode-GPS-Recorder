db = db.getSiblingDB('radiacode');
var docs = db.tracker_samples.find({
  sessionId: '2026-06-23',
  spectrumData: { $exists: true, $ne: [] }
}).sort({ timestampMs: -1 }).limit(5);

print('Found: ' + docs.count() + ' docs');
docs.forEach(function(d) {
  var s = d.spectrumData || [];
  var total64 = s.slice(0, 64).reduce(function(a, b) { return a + b; }, 0);
  var nonzero = s.filter(function(x) { return x !== 0; }).length;
  print('ts=' + d.timestampMs + ' len=' + s.length + ' total64=' + total64 + ' nonzero=' + nonzero + ' sample=' + JSON.stringify(s.slice(0, 25)));
});
