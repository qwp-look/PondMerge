#!/usr/bin/env python3
"""Capture an ESP32-S3 acceptance run over the on-chip USB-Serial-JTAG.

Why this exists (task-book v2 section 12.3 wants a reproducible device record):
  * The console is CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG, so if the HOST does not
    keep reading, the transmit queue fills up and printf BLOCKS -- the suite
    then looks like it hangs after a couple of test groups. This script always
    drains the port.
  * It resets the chip itself, so the captured log starts at the ROM banner and
    includes the app_init lines carrying `App version` (ESP-IDF derives it from
    git, which is how the commit cross-check of task-book section 3.1 works)
    and the ELF SHA256.

Usage:
    python3 serial_cap.py [port] [seconds] [baud]
    python3 serial_cap.py /dev/ttyACM0 900 115200

Reset polarity gotcha: a NORMAL boot keeps IO0 HIGH (DTR=False) and only pulses
EN (RTS). Pulling IO0 low enters download mode instead (the log then shows
`rst:0x15 (USB_UART_CHIP_RESET),boot:0x23 (DOWNLOAD(USB/UART0))` and
`waiting for download`), which is easy to misread as "the board is dead".
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 900.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

s = serial.Serial(port, baud, timeout=0.2)


def dtr(v):
    s.dtr = v


def rts(v):
    s.rts = v


def drain(secs):
    """Read and discard, so a previous in-flight run cannot block the chip."""
    t0 = time.time()
    done = 0
    while time.time() - t0 < secs:
        chunk = s.read(8192)
        done += len(chunk)
    return done


def reset():
    """Normal-boot reset: IO0 HIGH (DTR=False), pulse EN (RTS) low -> high."""
    dtr(False)
    rts(False)
    time.sleep(0.2)
    rts(True)
    time.sleep(0.2)
    rts(False)
    time.sleep(0.05)
    dtr(False)


# 1) opening the port can itself toggle DTR/RTS and start a spurious boot, and
#    a previous run may still be blocked on a full transmit queue: reset once
#    and throw the result away, so the boot recorded below is complete.
reset()
discard = drain(1.5)

# 2) the boot we record
reset()

# 3) capture until the app reports its FINAL verdict. The firmware runs the
#    main suite, then the dual-core concurrency test, then the reference-model
#    differential -- the LAST verdict line is the model's; earlier verdicts
#    must not stop the capture.
buf = b""
t0 = time.time()
while time.time() - t0 < seconds:
    chunk = s.read(8192)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
        if b"=== model PASSED" in buf or b"=== model FAILED" in buf:
            break
time.sleep(0.3)
s.close()

sys.stdout.write(
    "\n[serial_cap] %u bytes captured in %.1fs (%u bytes discarded before reset)\n"
    % (len(buf), time.time() - t0, discard)
)
if b"=== model PASSED" not in buf and b"=== model FAILED" not in buf:
    sys.stdout.write(
        "[serial_cap] WARNING: no model verdict found in the capture\n")
    sys.exit(1)
