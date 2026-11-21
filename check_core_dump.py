import struct
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()
# Check the core dump partition in the ELF
# The core dump is at 0x790000 for V1.2
# Let's look for the core dump header in the ELF
# Look for the core dump magic/header
print('=== Core dump in ELF ===')
# The core dump should be at address 0x790000 in the flash partition
# Let's check if there's a section at that address
shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
for i in range(shnum):
    off = shoff + i * shentsize
    sh_type = struct.unpack_from('<I', d, off + 4)[0]
    sh_addr = struct.unpack_from('<I', d, off + 8)[0]
    sh_off = struct.unpack_from('<I', d, off + 16)[0]
    sh_size = struct.unpack_from('<I', d, off + 20)[0]
    if sh_addr == 0x790000:
        print('COREDUMP section at 0x' + format(sh_addr, '08x') + ' size=' + str(sh_size))
# Now let's read the actual core dump from the ELF data
coredump_start = sh_off
print('Core dump at offset 0x' + format(sh_off, 'x'))
print('Core dump size:', sh_size)
# Try to parse the core dump
if sh_size >= 4:
    magic = struct.unpack_from('<I', d, sh_off)[0]
    print('Magic:', hex(magic))
    if sh_size >= 16:
        flags = struct.unpack_from('<I', d, sh_off + 4)[0]
        print('Flags:', hex(flags))
# Now let's check the RTC memory
print()
print('=== Checking for RTC memory in ELF ===')
# RTC memory is at 0x60000000 in the ESP32-S3
# But in the ELF, we can check for RTC data
for i in range(shnum):
    off = shoff + i * shentsize
    sh_type = struct.unpack_from('<I', d, off + 4)[0]
    sh_addr = struct.unpack_from('<I', d, off + 8)[0]
    sh_off = struct.unpack_from('<I', d, off + 16)[0]
    sh_size = struct.unpack_from('<I', d, off + 20)[0]
    # Look for RTC/PMU sections
    name_end = sh_off
    while name_end < len(d) and d[name_end] != 0:
        name_end += 1
    name = d[sh_off:name_end].decode('ascii', errors='replace')
    if 'rtc' in name.lower() or 'pmem' in name.lower() or 'bss' in name.lower() or 'noinit' in name.lower():
        print('RTC/PMEM section: ' + name + ' at 0x' + format(sh_addr, '08x') + ' size=' + str(sh_size))