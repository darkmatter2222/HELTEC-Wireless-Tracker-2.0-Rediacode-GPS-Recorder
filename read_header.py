import struct
elf = r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf'
with open(elf, 'rb') as f:
    data = f.read()
print('0x10:', hex(struct.unpack_from('<I', data, 0x10)[0])
print('0x14:', hex(struct.unpack_from('<I', data, 0x14)[0])
print('0x18:', hex(struct.unpack_from('<I', data, 0x18)[0])
print('0x20:', hex(struct.unpack_from('<I', data, 0x20)[0])
print('0x24:', struct.unpack_from('<H', data, 0x24)[0])
print('0x26:', struct.unpack_from('<H', data, 0x26)[0])
print('0x28:', struct.unpack_from('<H', data, 0x28)[0])
print('0x2a:', struct.unpack_from('<H', data, 0x2a)[0])