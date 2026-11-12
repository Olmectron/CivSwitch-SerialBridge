#!/usr/bin/env python3
"""Send a JS payload to the Switch, or upload a file to it over FTP,
via the ESP32 bridge over USB serial.

Usage:
  # Run a CivSwitch payload (TCP loader, default port 9026)
  python3 send_serial.py <switch-ip> payload.js
  python3 send_serial.py <switch-ip> <port> payload.js
  python3 send_serial.py --port COM3 <switch-ip> payload.js

  # Upload a file to the console's FTP server (default port 5000)
  python3 send_serial.py --ftp /switch/payload.bin <switch-ip> file.bin
  python3 send_serial.py --ftp /uploads/x.nro <switch-ip> 5000 file.nro

Requires: pip install pyserial
"""
import argparse
import glob
import platform
import sys
import time

import serial


def find_esp32_port():
    system = platform.system()
    if system == "Darwin":
        candidates = (
            glob.glob("/dev/cu.usbserial-*") +
            glob.glob("/dev/cu.wchusbserial-*") +
            glob.glob("/dev/cu.SLAB_USBtoUART*") +
            glob.glob("/dev/cu.usbmodem*")
        )
    elif system == "Linux":
        candidates = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    elif system == "Windows":
        # Can't glob COM ports, user needs to specify --port
        return None
    else:
        return None

    if len(candidates) == 1:
        return candidates[0]
    preferred = [c for c in candidates
                 if "usbserial" in c or "SLAB" in c
                 or "wchusbserial" in c or "ttyUSB" in c]
    if preferred:
        return preferred[0]
    if candidates:
        return candidates[0]
    return None


def send_via_serial(serial_port, header, payload):
    ser = serial.Serial(serial_port, 115200, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    ser.write((header + "\n").encode())
    time.sleep(0.1)

    # Stream the payload in chunks. ser.write() paces itself at the serial
    # line rate, so this both avoids buffering the whole file on the device
    # and drives a progress indicator for large uploads.
    total = len(payload)
    sent = 0
    chunk = 8192
    show_progress = total > chunk
    start = time.time()
    while sent < total:
        n = ser.write(payload[sent:sent + chunk])
        sent += n if n else 0
        if show_progress:
            elapsed = max(time.time() - start, 0.001)
            print(f"\r  sent {sent}/{total} bytes ({sent * 100 // total}%)  "
                  f"{sent / elapsed / 1024:.1f} KB/s", end="", flush=True)
    ser.flush()
    if show_progress:
        print()

    output = []
    # Big uploads take a while at the serial line rate (~11 KB/s @ 115200),
    # so scale the wait to the payload size.
    deadline = time.time() + max(130, total / 8000 + 60)
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").rstrip("\r\n")
        if line:
            print(line)
            output.append(line)
            if line.strip() == "END":
                break
            deadline = time.time() + 10
    ser.close()
    return "\n".join(output)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ESP32 serial port (auto-detected on Mac/Linux)")
    ap.add_argument("--ftp", metavar="REMOTE_PATH",
                    help="FTP mode: upload the file to this path on the console's "
                         "FTP server (e.g. /switch/payload.bin) instead of running "
                         "it through the TCP loader")
    ap.add_argument("switch_ip", help="Switch IP on the bridge network (usually 192.168.4.2)")
    ap.add_argument("args", nargs="+", help="[tcp_port] file")
    args = ap.parse_args()

    serial_port = args.port or find_esp32_port()
    if not serial_port:
        print("ERROR: Can't find the ESP32. Use --port to specify it.")
        print("  Mac:     --port /dev/cu.usbserial-XXXX")
        print("  Linux:   --port /dev/ttyUSB0")
        print("  Windows: --port COM3")
        sys.exit(1)

    # Default TCP port: Switch homebrew FTP servers usually listen on 5000;
    # the CivSwitch TCP loader listens on 9026.
    tcp_port = 5000 if args.ftp else 9026
    rest = args.args
    if rest and rest[0].isdigit():
        tcp_port = int(rest[0])
        rest = rest[1:]
    if not rest:
        print("ERROR: No file specified")
        sys.exit(1)

    # Read as raw bytes — works for JS payloads and arbitrary FTP uploads alike.
    with open(rest[0], "rb") as f:
        data = f.read()

    if args.ftp:
        header = f"FTP {args.switch_ip} {tcp_port} {len(data)} {args.ftp}"
        print(f"FTP upload: {len(data)} bytes via {serial_port} -> "
              f"{args.switch_ip}:{tcp_port}  {args.ftp}")
    else:
        header = f"SEND {args.switch_ip} {tcp_port} {len(data)}"
        print(f"Sending {len(data)} bytes via {serial_port} -> "
              f"{args.switch_ip}:{tcp_port}")

    send_via_serial(serial_port, header, data)


if __name__ == "__main__":
    main()
