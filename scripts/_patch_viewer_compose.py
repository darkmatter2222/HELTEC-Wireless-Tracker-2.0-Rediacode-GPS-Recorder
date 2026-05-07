path = '/home/darkmatter2222/vega-tracker-viewer/docker-compose.yml'
txt = open(path).read()
old = '      API_BASE: ${API_BASE:-http://192.168.86.48:8030}'
new = '      API_BASE: ${API_BASE:-http://192.168.86.48:8030}\n      TRACKER_USER: ${TRACKER_USER:-}\n      TRACKER_PASS: ${TRACKER_PASS:-}'
if 'TRACKER_USER' in txt:
    print('Already patched')
else:
    open(path, 'w').write(txt.replace(old, new, 1))
    print('Patched')
