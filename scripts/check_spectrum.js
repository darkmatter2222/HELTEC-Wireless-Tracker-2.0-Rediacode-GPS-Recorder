db = db.getSiblingDB('radiacode');
print("Total samples: " + db.tracker_samples.countDocuments());
var withSpec = db.tracker_samples.find({spectrumData:{$exists:true}}).count();
print("With spectrum field: " + withSpec);
var withSpecData = db.tracker_samples.find({spectrumData:{$exists:true,$ne:[]}}).count();
print("With non-empty spectrum: " + withSpecData);
var withSpecNull = db.tracker_samples.find({spectrumData:{$exists:true,$eq:[]}}).count();
print("With empty spectrum array: " + withSpecNull);
if (withSpec > 0) {
  print("\nFirst sample with spectrum field:");
  var doc = db.tracker_samples.findOne({spectrumData:{$exists:true}});
  print(JSON.stringify({sessionId:doc.sessionId, ts:doc.timestampMs, spectrumType:typeof doc.spectrumData, spectrumValue:JSON.stringify(doc.spectrumData).substring(0,200)}));
}
if (withSpecData > 0) {
  print("\nSamples with data (limit 3):");
  db.tracker_samples.find({spectrumData:{$exists:true,$ne:[]}}).sort({timestampMs:-1}).limit(3).forEach(function(x) {
    var arr = x.spectrumData;
    print("channels:" + arr.length + " first10=" + JSON.stringify(arr.slice(0,10)));
  });
}
