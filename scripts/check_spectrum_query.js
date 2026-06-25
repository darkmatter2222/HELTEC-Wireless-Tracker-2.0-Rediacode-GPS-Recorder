const s = db.tracker_samples.findOne(
  { spectrumData: { $exists: true, $ne: [] } },
  null,
  { sort: { timestampMs: -1 } }
);
if (!s) { print("no spectrum samples found"); mongosh quit(); }
print("channels: " + s.spectrumData.length);
print("first 50: " + JSON.stringify(s.spectrumData.slice(0, 50)));
const nz = s.spectrumData.filter(v => v !== 0 && v !== 65535);
print("non-trivial values (not 0, not 65535): " + nz.length);
if (nz.length > 0) {
  print("examples: " + JSON.stringify(nz.slice(0, 20)));
} else {
  const zeros = s.spectrumData.filter(v => v === 0);
  const maxs = s.spectrumData.filter(v => v === 65535);
  print("zeros: " + zeros.length + ", 65535s: " + maxs.length);
}
