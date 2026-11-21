import struct
elf = r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf'
with open(elf, 'rb') as f:
    data = f.read()
print('Magic:', data[0:4].hex())
print('Class:', data[4])
print('Entry:', hex(struct.unpack_from('<I', data, 0x0c)[0])
print('PH off:', hex(struct.unpack_from('<I', data, 0x10)[0])
print('SH off:', hex(struct.unpack_from('<I', data, 0x14)[0])
print('SH num:', struct.unpack_from('<H', data, 0x28)[0])
print('SH size:', struct.unpack_from('<H', data, 0x26)[0])