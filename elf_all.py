import struct
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()
print('File size:', len(d))
print('Magic:', d[0:4].hex())
print('e_type:', hex(struct.unpack_from('<H', d, 0x10)[0])
print('e_machine:', hex(struct.unpack_from('<H', d, 0x12)[0])
print('e_version:', struct.unpack_from('<I', d, 0x14)[0])
print('e_entry:', hex(struct.unpack_from('<I', d, 0x18)[0])
print('e_phoff:', hex(struct.unpack_from('<I', d, 0x1c)[0])
print('e_shoff:', hex(struct.unpack_from('<I', d, 0x20)[0])
print('e_flags:', hex(struct.unpack_from('<I', d, 0x24)[0])
print('e_ehsize:', struct.unpack_from('<H', d, 0x28)[0])
print('e_phentsize:', struct.unpack_from('<H', d, 0x2a)[0])
print('e_phnum:', struct.unpack_from('<H', d, 0x2c)[0])
print('e_shentsize:', struct.unpack_from('<H', d, 0x2e)[0])
print('e_shnum:', struct.unpack_from('<H', d, 0x30)[0])
print('e_shstrndx:', struct.unpack_from('<H', d, 0x32)[0])
shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
print('shoff:', hex(shoff), 'shnum:', shnum, 'shentsize:', shentsize)
for i in range(shnum):
    off = shoff + i * shentsize
    sh_addr = struct.unpack_from('<I', d, off + 8)[0]
    sh_size = struct.unpack_from('<I', d, off + 20)[0]
    if sh_addr >= 0x42000000 and sh_addr < 0x43000000:
        name_end = off + 4
        while name_end < len(d) and d[name_end] != 0:
            name_end += 1
        name = d[off:name_end].decode('ascii', errors='replace')
        sh_off = struct.unpack_from('<I', d, off + 16)[0]
        print('  0x' + format(sh_addr, '08x') + ' size=' + str(sh_size) + ' ' + name)
print('--- Near 0x420e4416 ---')
found = False
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
        print('  0x' + format(sh_addr, '08x') + ' size=' + str(sh_size) + ' ' + name)
        print('    file_offset:', hex(sh_off))
        found = True
if not found:
    print('No section found')