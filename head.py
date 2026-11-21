import struct
f=open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf','rb')
d=f.read()
f.close()
print('File size:', len(d))
# Print all header bytes in hex
print(d[:64].hex())
# Manual parse
e_entry=struct.unpack_from('<I',d,0x0c)[0]
e_phoff=struct.unpack_from('<I',d,0x10)[0]
e_shoff=struct.unpack_from('<I',d,0x14)[0]
print('entry=0x',hex(e_entry))
print('phoff=',e_phoff)
print('shoff=',e_shoff)