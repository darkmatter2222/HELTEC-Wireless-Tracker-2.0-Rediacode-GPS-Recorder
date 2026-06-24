print('databases:', JSON.stringify(db.adminCommand({listDatabases: 1}).databases.map(d => d.name)));
db = db.getSiblingDB('radiacode');
print('collections:', JSON.stringify(db.getCollectionNames().sort()));
const counts = {};
db.getCollectionNames().forEach(c => {
  counts[c] = db.getCollection(c).countDocuments({});
});
print('counts:', JSON.stringify(counts));
