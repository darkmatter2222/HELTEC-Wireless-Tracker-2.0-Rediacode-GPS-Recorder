# Verify the accumulator overflow theory
import random
random.seed(42)

row_header = "1782273604090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,"

print("=== Accumulator overflow theory verification ===\n")

# Scenario: spectrum reset fails every N polls, causing counts to accumulate
# After 5-10 polls WITHOUT reset, typical background radiation gives ~100-500 counts/ch
# That's 4-6 digits per channel instead of 1-2 digits

def sim_spectrum(num_polls_without_reset=1):
    """Simulate spectrum after N accumulated polls without reset."""
    # Background rate: ~10-50 counts/channel/poll integrated over ~5s windows
    base_rate = random.uniform(10, 50)
    
    channels = []
    for i in range(1024):
        # CsI(Tl) has ~64 energy bins typically, but RC-110 reports 1024 channels
        # First few channels get the most counts, higher channels are near zero
        channel_weight = max(0.01, 1.0 / (1 + i * 0.1))
        count = int(base_rate * channel_weight * num_polls_without_reset)
        channels.append(str(count))
    return '|'.join(channels)

print("Row sizes with different accumulation levels:")
for n_polls in [1, 2, 5, 10, 20, 50]:
    spec = sim_spectrum(n_polls)
    row_len = len(row_header) + len(spec) + 1
    truncates = row_len > 4096
    print(f"  {n_polls:3d} polls accumulated -> {row_len:5d} bytes (truncate: {truncates})")
    if truncates and n_polls <= 20:
        # Show what corrupt values look like
        channels = spec.split('|')
        large_vals = [(i, v) for i, v in enumerate(channels) if len(v) >= 5]
        if large_vals:
            print(f"         First large values at indices {large_vals[0][0]}-{large_vals[-1][0]}")
            print(f"         Sample values: {[v for _, v in large_vals[:3]]}")

print("\n=== When truncation DOES happen ===")

# Exactly simulate the snprintf truncation with high counts
spec_high = sim_spectrum(20)  # ~6KB row
row_truncated = (row_header + spec_high)[:4095]  # snprintf writes up to buffer size

# What channels does this produce?
parts = row_truncated.split(',')
spec_part = parts[12] if len(parts) > 12 else ""
channels = spec_part.split('|')

# The last element will be truncated mid-value or mid-comma
print(f"\nTruncated spectrum has {len(channels)} channels")
print(f"Last 3 channel strings: {channels[-3:]!r}")

# Now what happens when csv.reader sees this? The row has no \n, so the NEXT row
# gets concatenated into the same CSV line. This means column 12 (spectrum) receives
# part of row A's spectrum string AND the beginning of row B's header + spectrum!

print("\n=== Two-row concatenation with actual truncation ===")

spec_a = sim_spectrum(20)
rowA = row_header + spec_a  # No \n (truncated)

spec_b = sim_spectrum(1)
rowB = "1782273611590,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00," + spec_b + "\n"

# Row A gets truncated at 4KB
trunc_point = 4095
rowA_trunc = rowA[:trunc_point] 

concat = rowA_trunc + rowB

import csv
from io import StringIO

reader = csv.reader(StringIO(concat))
for idx, parsed in enumerate(reader):
    if len(parsed) < 13:
        continue
    spec_col = parsed[12]
    channels = spec_col.split('|')
    
    # Find timestamps bleeding into spectrum data
    sus_values = [(i, v) for i, v in enumerate(channels) if len(v) > 5 and int(v) > 65535]
    print(f"\nParsed row #{idx}: {len(channels)} channels")
    if sus_values:
        print(f"  Suspicious values (corruption):")
        for ch_idx, ch_val in sus_values[:3]:
            print(f"    [{ch_idx}] = {ch_val} ({len(ch_val)} digits)")

print("\n=== What ACTUALLY caused the MongoDB corruption? ===")
print("The channel values don't need to be huge. Let's check the WDT chunked upload.")

# The wifi_uploader.cpp reads with readStringUntil('\\n') - it's line-boundary aware
# So if a line IS truncated in the FILE, the uploader will keep reading until \\n
# Which could span THOUSANDS of bytes across multiple physical rows

# Actually, let's check: did the corruption happen on-device (file written wrong)
# or during upload (HTTP POST went wrong)?
# The API logs show successful uploads with correct row counts. So the FILE itself
# is corrupted when written by the firmware.

print("\n=== Realistic scenario that causes truncation ===")
# What if NOT all channels are near-zero? Near a radioactive source, 
# MANY channels will have 4-6 digit values
spec_source = []
for i in range(1024):
    # Cherenkov/source spectrum: most channels have significant counts
    if i < 32:
        spec_source.append(str(random.randint(500, 5000)))
    elif i < 128:
        spec_source.append(str(random.randint(50, 500)))  
    elif i < 512:
        spec_source.append(str(random.randint(5, 100)))
    else:
        spec_source.append(str(random.randint(0, 20)))

spec_str = '|'.join(spec_source)
row_len = len(row_header) + len(spec_str) + 1
print(f"Near source: {row_len} bytes -> truncate: {row_len > 4096}")

# More importantly - what if the BUG is that specCount sometimes reports wrong?
# If specCount=2048 (double buffer) instead of 1024, the row doubles in size
print("\n=== Wrong channel count theory ===")
for n_ch in [512, 1024, 1536, 2048]:
    spec = '|'.join([str(random.randint(0,5)) for _ in range(n_ch)])
    row_len = len(row_header) + len(spec) + 1
    print(f"  {n_ch} channels: {row_len} bytes -> truncate: {row_len > 4096}")

print("\n=== The actual fix needed ===")
print("1. Increase MAX_LINE_BYTES to at least 8192 (handles worst case)")
print("2. Add truncation detection to skip corrupted rows")
print("3. Clean corrupted spectrumData from MongoDB")
print("4. Option: limit spectrum channels written to first 64 (matches CsI bins)")
