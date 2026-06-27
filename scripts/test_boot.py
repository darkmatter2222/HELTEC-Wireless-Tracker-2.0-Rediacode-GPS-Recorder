#!/usr/bin/env python3
"""Test if ESP32-S3 is actually running firmware after flash.
Does NOT use DTR/RTS reset, which causes USB_JTAG_CHIP_RESET."""

import serial
import time
import sys

PORT = "COM4"
BAUD = 115200
WAIT_SECONDS = 45

print(f"Waiting {WAIT_SECONDS}s for device to boot...")
time.sleep(WAIT_SECONDS)

print(f"Connecting to {PORT} @ {BAUD} (DTR/RTS disabled)...")
try:
    ser = serial.Serial(PORT, BAUD, timeout=2, dsrdtr=False, rtscts=False)
    ser.dtr = False
    ser.rts = False
    print(f"Connected. Reading {30}s of output...")
    
    heartbeats = 0
    lines_received = 0
    started_at = time.time()
    
    while time.time() - started_at < 30:
        line = ser.readline().decode('utf-8', errors='replace').rstrip()
        if line:
            lines_received += 1
            print(line)
            if '[HB]' in line:
                heartbeats += 1
    
    ser.close()
    
    print(f"\n--- RESULTS ---")
    print(f"Lines received: {lines_received}")
    print(f"Heartbeats ([HB]): {heartbeats}")
    
    if heartbeats >= 3:
        print("✅ DEVICE IS RUNNING! Firmware is working.")
        sys.exit(0)
    elif lines_received > 0:
        print("⚠️ Device is sending output but no heartbeats yet")
        sys.exit(1)
    else:
        print("❌ No serial output - device may be boot looping or panicked")
        sys.exit(2)

except Exception as e:
    print(f"Error: {e}")
    sys.exit(3)
