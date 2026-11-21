import struct
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()
# The crash is at 0x420e4416, near initializer_list
# Let's find all symbols in the range 0x420e4410-0x420e4420
shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
symtab_off = -1
strtab_off = -1
for i in range(shnum):
    off = shoff + i * shentsize
    sh_type = struct.unpack_from('<I', d, off + 4)[0]
    sh_off = struct.unpack_from('<I', d, off + 16)[0]
    sh_size = struct.unpack_from('<I', d, off + 20)[0]
    if sh_type == 2:
        symtab_off = sh_off
        symtab_size = sh_size
    elif sh_type == 1:
        strtab_off = sh_off
# Find all symbols in the target range
print('=== ALL symbols near 0x420e4416 ===')
for i in range(0, symtab_size, 16):
    st_value = struct.unpack_from('<I', d, symtab_off + i + 4)[0]
    st_size = struct.unpack_from('<I', d, symtab_off + i + 8)[0]
    st_name_idx = struct.unpack_from('<I', d, symtab_off + i)[0]
    if st_value <= 0x420e4416 and st_value + st_size > 0x420e4416:
        name_end = strtab_off + st_name_idx
        while name_end < len(d) and d[name_end] != 0:
            name_end += 1
        name = d[strtab_off + st_name_idx:name_end].decode('ascii', errors='replace')
        print('  0x' + format(st_value, '08x') + ' (size ' + str(st_size) + ') ' + name)
# Also search the ELF data for the instruction at that address
print()
print('=== ELF bytes at 0x420e4416 context ===')
# Find all sections and their sizes
for i in range(shnum):
    off = shoff + i * shentsize
    sh_type = struct.unpack_from('<I', d, off + 4)[0]
    sh_addr = struct.unpack_from('<I', d, off + 8)[0]
    sh_off = struct.unpack_from('<I', d, off + 16)[0]
    sh_size = struct.unpack_from('<I', d, off + 20)[0]
    if sh_addr <= 0x420e4416 and sh_addr + sh_size > 0x420e4416:
        # Get section name
        name_end = strtab_off + sh_type
        while name_end < len(d) and d[name_end] != 0:
            name_end += 1
        name = d[sh_off:name_end].decode('ascii', errors='replace')
        print('SECTION: ' + name + ' at 0x' + format(sh_addr, '08x') + ' size=' + str(sh_size))
        if sh_type != 0 and sh_type != 2 and sh_type != 1:
            print('  (type=' + str(sh_type) + ')')