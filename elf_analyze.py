import struct
import sys

f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()

shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
shstrndx = struct.unpack_from('<H', d, 0x32)[0]
print('shoff:', hex(shoff), 'shnum:', shnum, 'shentsize:', shentsize)

# Parse all sections
secs = []
for i in range(shnum):
    off = shoff + i * shentsize
    s = {}
    s['name_idx'] = struct.unpack_from('<I', d, off)[0]
    s['type'] = struct.unpack_from('<I', d, off + 4)[0]
    s['flags'] = struct.unpack_from('<I', d, off + 8)[0]
    s['addr'] = struct.unpack_from('<I', d, off + 12)[0]
    s['off'] = struct.unpack_from('<I', d, off + 16)[0]
    s['size'] = struct.unpack_from('<I', d, off + 20)[0]
    s['link'] = struct.unpack_from('<I', d, off + 24)[0]
    s['info'] = struct.unpack_from('<I', d, off + 28)[0]
    s['addralign'] = struct.unpack_from('<I', d, off + 32)[0]
    s['entsize'] = struct.unpack_from('<I', d, off + 36)[0]
    secs.append(s)

# Read string table
shdr_strtab = d[secs[shstrndx]['off']:secs[shstrndx]['off'] + secs[shstrndx]['size']]
for s in secs:
    ns = s['name_idx']
    ne = ns
    while ne < len(shdr_strtab) and shdr_strtab[ne] != 0:
        ne += 1
    s['name'] = shdr_strtab[ns:ne].decode('ascii', errors='replace')

# Crash address
crash = 0x420e4416
print(f'\n=== SECTION AT: Crash address 0x{crash:08x} ===')
for s in secs:
    if s['addr'] <= crash < s['addr'] + s['size']:
        print(f'  0x{s["addr"]:08x} size={s["size"]} name="{s["name"]}"')
        print(f'    file_offset=0x{s["off"]:08x}')

# All sections in 0x42000000 range
print('\n=== ALL sections in 0x42000000-0x43000000 ===')
for s in secs:
    if 0x42000000 <= s['addr'] < 0x43000000:
        print(f'  0x{s["addr"]:08x} size={s["size"]} "{s["name"]}"')

# Find symbols near crash
print('\n=== SYMBOLS NEAR 0x420e4416 ===')
symtab_off = -1
strtab_off = -1
for s in secs:
    if s['type'] == 2:
        symtab_off = s['off']
    elif s['type'] == 1:
        strtab_off = s['off']

if symtab_off >= 0 and strtab_off >= 0:
    symtab_size = 0
    for s in secs:
        if s['name'] == '.symtab':
            symtab_size = s['size']
            break
    for i in range(0, symtab_size, 16):
        st_value = struct.unpack_from('<I', d, symtab_off + i + 4)[0]
        st_size = struct.unpack_from('<I', d, symtab_off + i + 8)[0]
        st_name_idx = struct.unpack_from('<I', d, symtab_off + i)[0]
        if st_value <= crash and st_value + st_size > crash:
            name_end = strtab_off + st_name_idx
            while name_end < len(d) and d[name_end] != 0:
                name_end += 1
            name = d[strtab_off + st_name_idx:name_end].decode('ascii', errors='replace')
            print(f'  0x{st_value:08x} (size {st_size}) "{name}"')

# Disassembly around crash address
print('\n=== Fallback: nearest symbol ===')
# Show all symbols in range
for i in range(0, symtab_size, 16):
    st_value = struct.unpack_from('<I', d, symtab_off + i + 4)[0]
    st_size = struct.unpack_from('<I', d, symtab_off + i + 8)[0]
    st_name_idx = struct.unpack_from('<I', d, symtab_off + i)[0]
    if 0x420e0000 <= st_value < 0x420f0000:
        name_end = strtab_off + st_name_idx
        while name_end < len(d) and d[name_end] != 0:
            name_end += 1
        name = d[strtab_off + st_name_idx:name_end].decode('ascii', errors='replace')
        print(f'  0x{st_value:08x} (size {st_size}) "{name}"')