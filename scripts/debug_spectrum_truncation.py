# Debug spectrum truncation - detailed simulation of what happens when snprintf truncates
import csv
from io import StringIO

def make_spectrum(counts):
    """Create pipe-delimited string from list of int counts."""
    return '|'.join(str(c) for c in counts)

# Simulate the LOW radiation case that the device typically produces indoors
# (mostly 0s with occasional low values)
indoor_spectrum = [0]*950 + [1, 2, 1, 3, 2, 1] * 14 + [0] * 78  # 1024 channels total

row_header = "1782273604090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,"

spec_str = make_spectrum(indoor_spectrum)
row1 = row_header + spec_str + "\n"
print(f"Indoor row length: {len(row1)} bytes (MAX_LINE_BYTES=4096)")
print(f"  Would truncate: {len(row1) > 4096}")

# Now simulate with some counts in the first few channels 
# (this is what actually triggers truncation based on MongoDB evidence)
medium_spectrum = [5]*30 + [2,1,0] * 330 + [0] * 12      # ~1024 channels

spec2 = make_spectrum(medium_spectrum)
row2_before_trunc = row_header + spec2 + "\n"
print(f"\nMedium exposure: {len(row2_before_trunc)} bytes, truncate: {len(row2_before_trunc) > 4096}")

# Now the actual scenario from MongoDB - what produces a value of 5701782273605448?
# This is clearly "5" + "7" + "0" ... + some timestamp digits
# Let's simulate what happens when snprintf truncates exactly after channel 782

print("\n--- Simulating exact truncation point ---")
spec_full = make_spectrum(indoor_spectrum)

row_complete = row_header + spec_full + "\n"
print(f"Complete row is {len(row_complete)} bytes")

# The corruption value "5701782273605448" - break it down:
# It could be: 5 | 7 | 0 | 178227360 | 5448  -> those aren't valid channel counts
# Or: part of spectrum + bleed from next row's timestamp "17822736..."  
corrupt_val = "5701782273605448"

# If we're at exactly byte position X in the spectrum string, what chars could form this?
# The spectrum data is: "0|0|0|...|5|7|0|17822736..." 
# Where 17822736 comes from the NEXT row's timestamp

# Let's find where in a CONCATENATED rows scenario this appears
row_a_data = row_header[:-1] + spec_full   # without \n (truncated)
row_b_complete = "1782273609090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00," + spec_full + "\n"

# csv.reader reads row_a_data as row 1's spectrum column (col 12)
# But WITHOUT the newline, the CSV library will continue reading into row_bComplete!
# This means col 12 of row_a will contain:
# - The pipe-delimited spectrum from row a
# - PLUS everything until it finds 12 more commas
# Which includes part of row b's timestamp as a value in the split()

sim_file = StringIO(row_a_data + row_b_complete)
reader = csv.reader(sim_file)
for parsed_row in reader:
    print(f"\nParsed row has {len(parsed_row)} columns")
    spec_col = parsed_row[12] if len(parsed_row) > 12 else "MISSING"
    channels = spec_col.split('|')
    print(f"Spectrum col string length: {len(spec_col)} chars -> {len(channels)} after pipe-split")

    # Find the corrupt-looking values (anything > 65535 which is uint16 max)
    bad_indices = [i for i, ch in enumerate(channels) if len(ch) > 5]
    print(f"Channels with suspiciously long values: {bad_indices}")
    if bad_indices:
        for idx in bad_indices[:3]:
            # Show context around the bad value
            start = max(0, idx-2)
            end = min(len(channels), idx+3)
            print(f"  Context channels [{start}:{end}]:")
            for ci in range(start, end):
                marker = " <-- BAD" if ci == idx else ""
                print(f"    [{ci}] = {channels[ci]!r}{marker}")

# What if the issue is that TWO short rows concatenate when snprintf returns a value
# > buffer size for the FIRST row in certain conditions?
# Actually, let's check: does the indoor data EVER trigger truncation?
print("\n--- When would 4KB be insufficient? ---")

# Test with varying channel counts but all-low values
for n_ch in [512, 768, 1024]:
    spec = make_spectrum([0]*n_ch)
    row_len = len(row_header) + len(spec) + 1  
    print(f"  {n_ch} channels of zeros -> {row_len} bytes")

# The REAL question: what spectrum distribution produces a row > 4KB?
# Each channel takes "value|", so for values 0-9 that's 2 bytes/channel, 
# for 10-99 that's 3 bytes, for 100-999 that's 4 bytes
# Total budget = 4096 - header - newline = ~4011 bytes

print("\n--- Bytes per channel value ---")
for val in ['0', '50', '200', '1000', '65535']:
    with_pipe = len(val) + 1  # the pipe delimiter
    budget = (4096 - len(row_header) - 1) // with_pipe
    print(f"  value='{val}' -> {with_pipe} bytes/ch, max {budget} channels fit in 4KB")

print("\n--- So truncation only happens when ---")
print("  Many channels HAVE multi-digit values (>1 digit each)")
print("  OR a very high channel count (>1024)")
# The 783-channel results suggest the truncation is happening at exactly  
# the boundary where ~783 single-digit channels still fit but not 784+
spec_783_short = make_spectrum([str(i%10) for i in range(783)])
row_783 = len(row_header) + len(spec_783_short) + 1
print(f"783 single-digit channels: {row_783} bytes")

spec_784_short = make_spectrum([str(i%10) for i in range(784)])
row_784 = len(row_header) + len(spec_784_short) + 1
print(f"784 single-digit channels: {row_784} bytes -> truncates: {row_784 > 4096}")

# FOUND IT! Let's verify - with typical low counts, how many fit?
import random
random.seed(123)
medium_realistic = [str(random.choices([0,1,2,5], weights=[80,10,5,5])[0]) for _ in range(1024)]
total_spectrum_str_len = len('|'.join(medium_realistic))
actual_row = len(row_header) + total_spectrum_str_len + 1
print(f"\n--- Realistic medium row ---")
print(f"1024 channels with [0,1,2,5]: {actual_row} bytes -> truncate: {actual_row > 4096}")

# 783 channels worth of that same distribution
medium_783 = [str(random.choices([0,1,2,5], weights=[80,10,5,5])[0]) for _ in range(783)]
row_783_mixed = len(row_header) + len('|'.join(medium_783)) + 1  
print(f"783 channels same dist: {row_783_mixed} bytes -> under 4KB: {row_783_mixed <= 4096}")

# What about the ACTUAL spectrumReset logic?
print("\n--- Accumulator not resetting would cause values to grow ---")
print("  After N polls without reset, counts accumulate beyond uint16 range")
print("  65535 in one channel = 6 digits + pipe = 7 bytes instead of 2")
print("  With enough channels at high values, row exceeds 4KB -> truncation!")

