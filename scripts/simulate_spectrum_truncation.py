import csv
import io
import random

random.seed(42)

def make_spectrum_str(num_channels=1024):
    """Generate pipe-delimited spectrum data similar to what the device produces."""
    # Low-radiation environment: mostly zeros with occasional low counts
    channels = [str(random.choices([0, 1, 2, 3, 5, 10], weights=[75, 10, 5, 4, 3, 3])[0]) for _ in range(num_channels)]
    return '|'.join(channels)

def make_row(ts_extra=0):
    """Generate a realistic CSV row with spectrum data."""
    ts = 1782273604090 + ts_extra
    spec = make_spectrum_str(1024)
    row = f"{ts},0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,{spec}\n"
    return row

# Test basic row size
row = make_row(0)
print(f"Single row length: {len(row)} bytes")
print(f"Under 4KB? {len(row) <= 4096}")

# Simulate the truncation scenario
# When snprintf with a 4096-byte buffer writes a 4200+ byte row,
# it truncates. The \n at the end gets cut off.
print("\n--- Simulating snprintf truncation ---")
buffer_size = 4096

# Row 1 - will be truncated (no newline)
row1_full = make_row(0)
print(f"Row 1 length: {len(row1_full)} bytes")
row1_truncated = row1_full[:buffer_size]  # snprintf writes this many bytes
print(f"Row 1 truncated to: {len(row1_replaced := row1_truncated)} bytes (missing newline)")

# Row 2 - written normally, but starts right where row 1 was cut off
row2_full = make_row(7500)  # simulate ~7.5s later
print(f"\nRow 2 length: {len(row2_full)} bytes")

# What's actually in the file (two rows concatenated without newline separator)
file_content = row1_truncated + row2_full

# Parse with csv.reader - what does the API see?
lines = file_content.split('\n')
print(f"\nNumber of 'rows' csv.reader sees: {len([l for l in lines if l])}")
if len(lines) >= 1:
    first_line = lines[0]
    # This line contains row1's data + row2's data up to where row2's newline is
    reader = csv.reader(io.StringIO(first_line))
    for parsed_row in reader:
        if len(parsed_row) > 12:
            spec_str = parsed_row[12]
            channels = spec_str.split('|')
            print(f"\nChannels parsed in 'first row': {len(channels)}")
            if len(channels) > 780:
                print(f"Last 5 channels (should show timestamp bleed):")
                for i, ch in enumerate(channels[-5:], len(channels)-5):
                    print(f"  [{i}] = {ch}")

# Test with higher radiation values that would actually exceed 4KB
print("\n--- Testing high-radiation scenarios ---")

# Medium counts (10-200 range) per channel
def make_med_spectrum():
    channels = [str(random.randint(10, 200)) for _ in range(1024)]
    return '|'.join(channels)

row_med = f"1782273604090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,{make_med_spectrum()}\n"
print(f"Medium radiation row: {len(row_med)} bytes -> truncates: {len(row_med) > 4096}")

# High counts (up to 65535)
def make_high_spectrum():
    channels = [str(random.randint(100, 65535)) for _ in range(1024)]
    return '|'.join(channels)

row_high = f"1782273604090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,{make_high_spectrum()}\n"
print(f"High radiation row: {len(row_high)} bytes -> truncates: {len(row_high) > 4096}")

# Mixed realistic outdoor - most zeros with some spikes in first few channels
def make_outdoor_spectrum():
    ch = ['0'] * 1024
    for i in range(min(32, len(ch))):
        ch[i] = str(random.randint(1, 500))
    return '|'.join(ch)

row_outdoor = f"1782273604090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,{make_outdoor_spectrum()}\n"
print(f"Outdoor spectrum row: {len(row_outdoor)} bytes -> truncates: {len(row_outdoor) > 4096}")

# What if the issue is not a single row but TWO rows concatenated?
# When row 1 is truncated (no \n), row 2 starts immediately after
# The csv.reader will see ONE long "row" containing parts of both
print("\n--- Two-row concatenation scenario ---")
row_a = make_row(0)         # ~2170 bytes
row_b = make_row(5000)      # ~2170 bytes
combined = row_a[:4096] + row_b  # row_a truncated + row_b full - ~4300+ bytes total before row_b's \n
# csv.reader reads until \n which is only at the end of row_b

from io import StringIO, TextIOWrapper
import csv as csv_module
reader = csv_module.reader(StringIO(combined))
for parsed in reader:
    if len(parsed) > 12 and len(parsed[12].split('|')) == 2048:
        print(f"Two rows merged -> {len(parsed[12].split('|'))} channels (1024x2 concatenated!)")
    elif len(parsed) > 12:
        n = len(parsed[12].split('|'))
        print(f"Parsed row has {n} channels")

print("\n--- What byte range cuts off at channel 783? ---")
# Find exactly where channel 782 ends in the file
prefix = "1782273604090,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20,,6.00,"
spec_only = make_spectrum_str(1024)
full_row = prefix + spec_only + '\n'
# Find position of | that starts channel 783 (index 782 is the 783rd)
pipe_positions = [i for i, c in enumerate(spec_only) if c == '|']
print(f"Position in full row where channel 783 starts: {len(prefix) + pipe_positions[782] + 1} bytes")
print(f"That's within spec string at offset {pipe_positions[782]} from spec start")
