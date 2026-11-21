import struct
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()
shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
symtab_off = -1
symtab_size = 0
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
print('SYMTAB:', hex(symtab_off), 'size:', symtab_size, 'STR:', hex(strtab_off))
if symtab_off >= 0 and strtab_off >= 0:
    num_syms = symtab_size // 16
    for i in range(0, symtab_size, 16):
        st_name = struct.unpack_from('<I', d, symtab_off + i)[0]
        st_value = struct.unpack_from('<I', d, symtab_off + i + 4)[0]
        st_size = struct.unpack_from('<I', d, symtab_off + i + 8)[0]
        if st_value <= 0x420e4416 and st_value + st_size > 0x420e4416:
            name_end = strtab_off + st_name
            while name_end < len(d) and d[name_end] != 0:
                name_end += 1
            name = d[strtab_off + st_name:name_end].decode('ascii', errors='replace')
            print('  0x' + format(st_value, '08x') + ' size=' + str(st_size) + ' ' + name)
        if st_value == 0x420e4416:
            print(' *** EX: 0x' + format(st_value, '08x') + ' ' + name)