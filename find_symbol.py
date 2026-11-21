import struct
elf = r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf'
with open(elf, 'rb') as f:
    data = f.read()
shoff = struct.unpack_from('<I', data, 0x18)[0]
shnum = struct.unpack_from('<H', data, 0x28)[0]
shentsize = struct.unpack_from('<H', data, 0x26)[0]
print(f'shoff={shoff:#x}, shnum={shnum}, shentsize={shentsize}')
symtab_off = -1
strtab_off = -1
for i in range(shnum):
    off = shoff + i * shentsize
    sh_type = struct.unpack_from('<I', data, off + 4)[0]
    sh_addr = struct.unpack_from('<I', data, off + 12)[0]
    sh_off = struct.unpack_from('<I', data, off + 16)[0]
    sh_size = struct.unpack_from('<I', data, off + 20)[0]
    if sh_type == 2:
        symtab_off = sh_off
        print(f'SYMTAB at {sh_off:#x} size={sh_size}')
    elif sh_type == 1:
        strtab_off = sh_off
        print(f'STRTAB at {sh_off:#x} size={sh_size}')
if symtab_off >= 0 and strtab_off >= 0:
    for i in range(0, shnum * shentsize, 16):
        st_name = struct.unpack_from('<I', data, symtab_off + i)[0]
        st_value = struct.unpack_from('<I', data, symtab_off + i + 4)[0]
        st_size = struct.unpack_from('<I', data, symtab_off + i + 8)[0]
        st_info = data[symtab_off + i + 12]
        st_shndx = struct.unpack_from('<H', data, symtab_off + i + 14)[0]
        name_start = strtab_off + st_name
        name_end = name_start
        while name_end < len(data) and data[name_end] != 0:
            name_end += 1
        name = data[name_start:name_end].decode('ascii', errors='replace')
        if st_value == 0x420e4416:
            print(f'*** EXact: 0x{st_value:08x} {name}')
        elif st_value <= 0x420e4416 and st_value + st_size > 0x420e4416:
            print(f'    0x{st_value:08x} {name}')