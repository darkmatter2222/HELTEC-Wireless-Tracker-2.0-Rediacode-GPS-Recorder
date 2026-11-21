import struct
elf = r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf'
with open(elf, 'rb') as f:
    data = f.read()
print('File size:', len(data))
# ELF header (32-bit):
# 0x10: e_type (2 bytes), 0x12: e_machine (2 bytes)
# 0x14: e_version (4 bytes), 0x18: e_entry (4 bytes)
# 0x1c: e_phoff (4 bytes), 0x20: e_shoff (4 bytes)
# 0x24: e_flags (4 bytes), 0x28: e_ehsize (2 bytes)
# 0x2a: e_phentsize (2 bytes), 0x2c: e_phnum (2 bytes)
# 0x2e: e_shentsize (2 bytes), 0x30: e_shnum (2 bytes)
print('e_type:', hex(struct.unpack_from('<H', data, 0x10)[0])
print('e_machine:', hex(struct.unpack_from('<H', data, 0x12)[0])
print('e_version:', struct.unpack_from('<I', data, 0x14)[0])
print('e_entry:', hex(struct.unpack_from('<I', data, 0x18)[0])
print('e_phoff:', hex(struct.unpack_from('<I', data, 0x1c)[0])
print('e_shoff:', hex(struct.unpack_from('<I', data, 0x20)[0])
print('e_flags:', hex(struct.unpack_from('<I', data, 0x24)[0])
print('e_ehsize:', struct.unpack_from('<H', data, 0x28)[0])
print('e_phentsize:', struct.unpack_from('<H', data, 0x2a)[0])
print('e_phnum:', struct.unpack_from('<H', data, 0x2c)[0])
print('e_shentsize:', struct.unpack_from('<H', data, 0x2e)[0]
print('e_shnum:', struct.unpack_from('<H', data, 0x30)[0])
print('e_shstrndx:', struct.unpack_from('<H', data, 0x32)[0])