import struct

def decode_rv32(word, addr):
    """Decode a 32-bit RISC-V instruction."""
    opcode = word & 0x7f
    rd = (word >> 7) & 0x1f
    rs1 = (word >> 15) & 0x1f
    funct3 = (word >> 12) & 0x7
    funct7 = (word >> 25) & 0x7f
    imm = 0

    # I-type
    if opcode == 0x05 or opcode == 0x13 or opcode == 0x17 or opcode == 0x1b or opcode == 0x23 or opcode == 0x27 or opcode == 0x2b or opcode == 0x2f or opcode == 0x37 or opcode == 0x3b or opcode == 0x3f or opcode == 0x4f or opcode == 0x57 or opcode == 0x5f or opcode == 0x63 or opcode == 0x67 or opcode == 0x6b:
        imm = ((word >> 20) & 0xfff) | ((word & 0x7fffff) >> 3)
        if imm & 0x800: imm -= 0x1000

    # S-type
    if opcode == 0x23:
        imm = ((word >> 25) & 0x3f) << 5 | ((word >> 7) & 0x1f)
        if imm & 0x400000: imm -= 0x800000

    # B-type
    if opcode == 0x63:
        imm = ((word >> 25) & 0x1) << 12 | ((word >> 8) & 0xf) << 8 | ((word >> 9) & 0x1) << 7 | ((word >> 7) & 0x1) << 1 | ((word >> 31) & 0x3ff) << 1
        if imm & 0x1000: imm -= 0x20000

    # U-type
    if opcode == 0x37 or opcode == 0x3f:
        imm = (word >> 12) & 0xfffff

    # J-type
    if opcode == 0x6f:
        imm = ((word >> 25) & 0x1) << 12 | ((word >> 1) & 0xff) << 4 | ((word >> 8) & 0xf) << 8 | ((word >> 24) & 0x1) << 15
        if imm & 0x8000000: imm -= 0x10000000

    # A-type (LR.SC)
    if opcode == 0x0f or opcode == 0x04:
        pass

    # C-extensions and others
    if opcode == 0x2:
        imm = ((word >> 20) & 0xff) | ((word & 0x3) << 8)
        if imm & 0x10000: imm -= 0x20000

    # Check if it's a branch
    if opcode == 0x63:
        print(f'  0x{addr:08x}: 0x{word:08x}  branch 0x{imm:+d}')
    elif opcode == 0x67:
        print(f'  0x{addr:08x}: 0x{word:08x}  jump/call 0x{imm:+d}')
    elif opcode == 0x6b:
        print(f'  0x{addr:08x}: 0x{word:08x}  branch 0x{imm:+d}')
    else:
        print(f'  0x{addr:08x}: 0x{word:08x}')

# Read firmware
f = open(r'C:\Users\ryans\source\repos\HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder\.pio\build\heltec_tracker_v1_2\firmware.elf', 'rb')
d = f.read()
f.close()

# Disassemble around crash
off = 0x155020 + (0x420e4416 - 0x42000020)
print('Disassembly around crash 0x420e4416:')
for i in range(-4, 16, 4):
    word = struct.unpack_from('<I', d, off + i)[0]
    decode_rv32(word, 0x420e4416 + i)

# Also check the section header string table
shoff = struct.unpack_from('<I', d, 0x20)[0]
shnum = struct.unpack_from('<H', d, 0x30)[0]
shentsize = struct.unpack_from('<H', d, 0x2e)[0]
shstrndx = struct.unpack_from('<H', d, 0x32)[0]
secs = []
for i in range(shnum):
    off = shoff + i * shentsize
    s = {}
    s['name_idx'] = struct.unpack_from('<I', d, off)[0]
    s['type'] = struct.unpack_from('<I', d, off + 4)[0]
    s['addr'] = struct.unpack_from('<I', d, off + 12)[0]
    s['off'] = struct.unpack_from('<I', d, off + 16)[0]
    s['size'] = struct.unpack_from('<I', d, off + 20)[0]
    secs.append(s)

# Find all sections
print('\nAll sections in 0x42000000-0x43000000:')
for s in secs:
    if 0x42000000 <= s['addr'] < 0x43000000:
        print(f'  0x{s["addr"]:08x} size={s["size"]} offset=0x{s["off"]:08x}')