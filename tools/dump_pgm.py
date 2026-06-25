#!/usr/bin/env python3
"""
Capture PGM image from Sony UP-D898MD emulator over UART.

Usage:
    python3 tools/dump_pgm.py [/dev/cu.xxx] [output.pgm]

Defaults:
    port   = /dev/cu.usbserial-A5069RR4
    output = /tmp/captured.pgm
"""

import sys
import serial
import time

PORT   = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-A5069RR4"
OUTPUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/captured.pgm"
BAUD   = 115200

print(f"Port  : {PORT}")
print(f"Output: {OUTPUT}")

ser = serial.Serial(PORT, BAUD, timeout=2)
time.sleep(0.3)

# Flush stale data
ser.reset_input_buffer()

# Send command
ser.write(b"dump_pgm\r\n")
ser.flush()
print("Sent: dump_pgm")

# Read PGM header line by line until binary payload starts
pgm_lines = []
binary_start = False
header_bytes = b""

line = ser.readline()       # "\r\n" echo
line = ser.readline()       # "P5\r\n"
if not line.strip().startswith(b"P5"):
    # might be "ERROR: no image"
    print(f"Unexpected response: {line!r}")
    # drain a few more lines
    for _ in range(3):
        l = ser.readline()
        print(f"  {l!r}")
    ser.close()
    sys.exit(1)

header_bytes += line
line = ser.readline()       # "<width> <height>\r\n"
header_bytes += line
parts = line.strip().split()
width, height = int(parts[0]), int(parts[1])
line = ser.readline()       # "255\r\n"
header_bytes += line

print(f"Image : {width}x{height}  ({width * height} bytes)")

# Write PGM header (normalise line endings to LF for proper PGM)
pgm_header = f"P5\n{width} {height}\n255\n".encode()

# Read pixel data
pixel_count = width * height
received = 0
chunks = []

ser.timeout = 10           # longer timeout for large payload
while received < pixel_count:
    want = min(4096, pixel_count - received)
    data = ser.read(want)
    if not data:
        print(f"Timeout after {received}/{pixel_count} bytes")
        break
    chunks.append(data)
    received += len(data)
    pct = received * 100 // pixel_count
    print(f"\r  {received}/{pixel_count} bytes ({pct}%)", end="", flush=True)

print()

ser.close()

if received == pixel_count:
    with open(OUTPUT, "wb") as f:
        f.write(pgm_header)
        for chunk in chunks:
            f.write(chunk)
    print(f"Saved {OUTPUT}  ({len(pgm_header) + received} bytes total)")
else:
    print(f"Incomplete capture: {received}/{pixel_count} bytes")
    sys.exit(1)
