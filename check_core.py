import struct
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()
offset = 0x1840a09
print('Core dump header bytes:', d[offset:offset+32].hex())
magic = struct.unpack_from('<I', d, offset)[0]
flags = struct.unpack_from('<I', d, offset+4)[0]
print('Magic:', hex(magic))
print('Flags:', hex(flags))
# Check if this looks like a core dump header
if magic == 0x79732e00:
    print('Found ESP32 core dump')
else:
    print('Invalid core dump header - likely corrupted or not present')