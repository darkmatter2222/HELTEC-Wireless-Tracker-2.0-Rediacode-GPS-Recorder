#!/usr/bin/env python3
import re
path = '/home/darkmatter2222/docucraft/nginx/entrypoint.sh'
txt = open(path).read()
txt = re.sub(r'chmod 644 [^\n]+\n', 'chmod 644 "${HTPASSWD_PATH}"\n', txt)
open(path, 'w').write(txt)
for i, line in enumerate(txt.splitlines(), 1):
    if 'chmod' in line or 'HTPASSWD_PATH' in line:
        print(f'{i}: {line}')
print('Done.')
