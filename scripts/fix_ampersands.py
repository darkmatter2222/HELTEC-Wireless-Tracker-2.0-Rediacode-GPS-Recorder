import sys
f = '/home/darkmatter2222/vega-tracker-viewer/src/App.jsx'
with open(f) as fh:
    content = fh.read()
print(f"Found {content.count('\\\\u0026\\\\u0026')} escaped && sequences")
content = content.replace('\\u0026\\u0026', '&&')
with open(f, 'w') as fh:
    fh.write(content)
print("Fixed")
