import struct

elf = r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf'
with open(elf, 'rb') as f:
    data = f.read()

# 32-bit ELF header layout:
# 0x00-0x03: magic
# 0x04: class
# 0x10-0x13: entry
# 0x14-0x17: phoff
# 0x18-0x1b: shoff
# 0x1c-0x1f: flags
# 0x20-0x21: ehsize
# 0x22-0x23: phentsize
# 0x24-0x25: phnum
# 0x26-0x27: shentsize
# 0x28-0x29: shnum
# 0x2a-0x2b: shstrndx

print('Class:', data[4])
print('Entry:', hex(struct.unpack_from('<I', data, 0x10)[0])
print('Phoff:', hex(struct.unpack_from('<I', data, 0x14)[0])
print('Shoff:', hex(struct.unpack_from('<I', data, 0x18)[0])
print('Shnum:', struct.unpack_from('<H', data, 0x28)[0]
print('Shentsize:', struct.unpack_from('<H', data, 0x26)[0]