#!/usr/bin/env python3
"""One-shot NS2 *pairing* capture from the joy_bridge serial console.

Reads the DIAG-instrumented log (build with CONFIG_JOY_BRIDGE_DIAG_HEXDUMP=y)
and auto-stops a few seconds after a real pairing is seen: a cmd-0x15 (key
agreement) AND security level 4. A mere auto-reconnect (no 0x15) does NOT count
as complete -- it runs to the timeout so a miss is obvious. DTR/RTS are left
deasserted on open to avoid bumping the board into download mode.

Usage:
    python3 scripts/capture_pair.py [out.log] [serial-port]

    Defaults: out=ns2_pair_capture.log
              port=$JB_SERIAL or /dev/cu.usbserial-3120

To guarantee a *fresh* pairing (a real cmd-0x15, not a silent reconnect) the
controller bond must be cleared AND capture must be live from the blank boot.
Chain it tight after a flash so there is no window for an unobserved pair:

    PORT=/dev/cu.usbserial-3120
    esptool --port $PORT --chip esp32s3 --baud 460800 erase-flash \\
      && west flash -d build --esp-device $PORT --esp-baud-rate 460800 \\
      && python3 scripts/capture_pair.py

Recover the link LTK offline from the captured cleartext: it is the XOR of the
A1 value (from the NS2, cmd 0x15/0x04) and the controller's B1 reply --
LTK = A1 ^ B1 (the codec stores its byte-reverse as g_controller_firmware.ltk).
"""
import os
import sys
import time
import datetime
import serial

OUT = sys.argv[1] if len(sys.argv) > 1 else "ns2_pair_capture.log"
PORT = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("JB_SERIAL", "/dev/cu.usbserial-3120")
BAUD = 115200
MAX_SECONDS = 180      # safety timeout (time for the NS2 to connect + pair)
DONE_GRACE = 3.0       # keep capturing this long after pairing completes

try:
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
except Exception as e:
    print(f"ERROR: could not open {PORT}: {e}")
    sys.exit(1)

start = time.time()
done_at = None
saw_0x15 = False
saw_level4 = False
n = 0
with open(OUT, "w", buffering=1) as f:
    f.write(f"# NS2 pair capture  port={PORT} baud={BAUD}  "
            f"started={datetime.datetime.now().isoformat(timespec='seconds')}\n")
    while True:
        try:
            raw = ser.readline()
        except Exception as e:
            f.write(f"# read error: {e}\n")
            break
        if raw:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            f.write(f"[{time.time() - start:8.3f}] {line}\n")
            n += 1
            if "cmd 0x15/" in line:
                saw_0x15 = True
            if "security changed: level=4" in line:
                saw_level4 = True
            if saw_0x15 and saw_level4 and done_at is None:
                done_at = time.time()
                f.write(f"# >>> PAIRING complete (0x15 + level 4) at "
                        f"{done_at - start:.3f}s; capturing {DONE_GRACE}s more\n")
        now = time.time()
        if done_at and now - done_at >= DONE_GRACE:
            f.write("# done: pairing captured\n")
            break
        if now - start >= MAX_SECONDS:
            f.write(f"# stop: {MAX_SECONDS}s timeout (0x15={saw_0x15} level4={saw_level4})\n")
            break
ser.close()
print(f"RESULT lines={n} cmd0x15={saw_0x15} level4={saw_level4} out={OUT}")
