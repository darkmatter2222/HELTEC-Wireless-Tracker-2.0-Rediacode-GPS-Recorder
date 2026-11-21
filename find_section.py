import struct
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()
shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
print('shoff:', hex(shoff), 'shnum:', shnum, 'shentsize:', shentsize)
for i in range(shnum):
    off = shoff + i * shentsize
    sh_addr = struct.unpack_from('<I', d, off + 8)[0]
    sh_size = struct.unpack_from('<I', d, off + 20)[0]
    if sh_addr <= 0x420e4416 and sh_addr + sh_size > 0x420e4416:
        name_end = off + 4
        while name_end < len(d) and d[name_end] != 0:
            name_end += 1
        name = d[off:name_end].decode('ascii', errors='replace')
        sh_off = struct.unpack_from('<I', d, off + 16)[0]
        print('Section:', name, 'addr=0x' + format(sh_addr, '08x') + ', size=' + str(sh_size))
        print('  file_offset:', hex(sh_off))
        print('  first 64 bytes:', d[sh_off:sh_off+64].hex())