#!/bin/bash
# PondMerge device verification runner: build -> flash -> capture -> verdict.
#
# Consolidates the serial/jtag workflow that v18-v20 hardened the hard way:
#
#   * The S3's USB-Serial-JTAG CDC control lines (DTR/RTS ioctls) fail after a
#     flash ("OSError: [Errno 84]"), so the capture reader starts BEFORE the
#     reset and a mid-run reboot is told from the normal end-of-run reboot by
#     counting ROM banners AFTER the app banner (a plain grep for "ESP-ROM"
#     false-positives on every boot).
#   * esptool flashes fail ~50% of the time with "Packet content transfer
#     stopped" -- a retry loop is the fix, not a diagnosis.
#   * A competing reader steals esptool's sync bytes: never start the capture
#     before the flash on the same port... on the S3 the flash itself resets
#     the chip (early output is lost), so after flashing we reset again via
#     openocd's JTAG path (USJ), which needs no CDC control lines at all.
#   * The classic ESP32 (CH340) has none of these problems: tests/serial_cap.py
#     works as documented, including its reset.
#
# Usage:
#   scripts/device_run.sh s3|esp32 [ops]     # build+flash+capture+verdict
#   scripts/device_run.sh s3|esp32 capture   # re-run on the flashed firmware
#
# The target selects the sdkconfig flow (both fixtures run 64 segments x 1 KiB
# since v20, so BOTH targets build the full suite):
#   s3    -> set-target esp32s3, single defaults file, /dev/ttyACM0
#   esp32 -> SDKCONFIG_DEFAULTS with both files, set-target esp32, /dev/ttyUSB0
set -u

TARGET="${1:-}"
OPS="${2:-2000}"
MODE="${3:-full}"
if [ "$TARGET" != "s3" ] && [ "$TARGET" != "esp32" ]; then
    echo "usage: $0 s3|esp32 [ops] [full|capture]" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT=/dev/ttyACM0
[ "$TARGET" = "esp32" ] && PORT=/dev/ttyUSB0
CAP="/tmp/device_run_${TARGET}.log"
OCD=/home/qwp/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32/bin/openocd
OCDScripts=/home/qwp/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32/share/openocd/scripts

log() { printf '[%s] %s\n' "$(date +%T)" "$*"; }

# ---- reader: raw fd read, never touches DTR/RTS (see header) ----
make_reader() {
    cat > /tmp/pm_raw_reader.py <<'EOF'
import os, sys, time
port, out_path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
f = open(out_path, 'wb')
deadline = time.time() + seconds
total = 0
while time.time() < deadline:
    try:
        chunk = os.read(fd, 8192)
        if chunk:
            f.write(chunk); f.flush(); total += len(chunk)
        else:
            time.sleep(0.02)
    except BlockingIOError:
        time.sleep(0.02)
    except OSError as e:
        f.write(f"\n[read error: {e}]\n".encode()); f.flush()
        time.sleep(0.05)
f.close(); os.close(fd)
EOF
}

start_reader() {
    pkill -f pm_raw_reader.py 2>/dev/null; sleep 1
    rm -f "$CAP"
    python3 /tmp/pm_raw_reader.py "$PORT" "$CAP" 600 > /dev/null 2>&1 &
    sleep 1
    kill -0 %1 2>/dev/null
}

# ---- verdict: anchored to the LAST ROM banner; a reboot AFTER the app
# banner is a crash, a reboot after "Returned from app_main" is the normal
# end of a run (IDF restarts when main returns). ----
verdict() {
    # Anchored verdict: only content AFTER the app banner counts, and a crash
    # is a ROM banner appearing AFTER that banner but BEFORE the normal
    # completion ("Returned from app_main" -> IDF restarts the chip, which
    # prints one more ROM banner -- that is the normal end of a run, not a
    # crash). The openocd reset itself also echoes TWO banners (reset + boot),
    # so position anchoring, not counting, is what works.
    python3 - "$CAP" 480 <<'EOF'
import sys, time
path, timeout = sys.argv[1], float(sys.argv[2])
t0, last_size, last_growth = time.time(), -1, time.time()
while time.time() - t0 < timeout:
    try:
        data = open(path, 'rb').read().decode('utf-8', 'replace')
    except FileNotFoundError:
        time.sleep(2); continue
    app = data.find('PondMerge v1 acceptance firmware')
    if app == -1:
        time.sleep(2); continue
    t2 = data[app:]
    if '=== model PASSED' in t2 or 'Returned from app_main' in t2:
        print('RESULT: ALL PASSED'); sys.exit(0)
    if 'ESP-ROM:esp32s3' in t2:
        print('RESULT: REBOOT MID-RUN (crash)'); sys.exit(1)
    if 'FAIL' in t2:
        print('RESULT: FAIL SEEN'); sys.exit(1)
    if len(data) != last_size:
        last_size, last_growth = len(data), time.time()
    elif time.time() - last_growth > 25:
        print('RESULT: OUTPUT STALLED (hang)'); sys.exit(1)
    time.sleep(2)
print('RESULT: TIMEOUT'); sys.exit(1)
EOF
}

# ---- build ----
if [ "$MODE" = "full" ]; then
    log "building for $TARGET"
    ( source "$HOME/esp/activate-idf.sh" >/dev/null 2>&1
      cd "$ROOT/esp32" || exit 1
      if [ ! -f sdkconfig ] || ! grep -q "CONFIG_IDF_TARGET=\"$TARGET\"" sdkconfig; then
          rm -f sdkconfig
          if [ "$TARGET" = "esp32" ]; then
              SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" \
                  idf.py set-target esp32 >> /tmp/device_build.log 2>&1
          else
              idf.py set-target esp32s3 >> /tmp/device_build.log 2>&1
          fi
      fi
      idf.py -B build build >> /tmp/device_build.log 2>&1
    ) || { log "BUILD FAILED (see /tmp/device_build.log)"; exit 1; }
    log "build OK"

    # ---- flash with retries (see header) ----
    local_ok=0
    for attempt in 1 2 3 4; do
        ( source "$HOME/esp/activate-idf.sh" >/dev/null 2>&1
          cd "$ROOT/esp32" || exit 1
          idf.py -B build -p "$PORT" flash >> /tmp/device_flash.log 2>&1
        ) && local_ok=1 && break
        log "flash attempt $attempt failed, retrying"; sleep 3
    done
    [ "$local_ok" = 1 ] || { log "FLASH FAILED (see /tmp/device_flash.log)"; exit 1; }
    log "flash OK"
fi

make_reader

# ---- capture: S3 needs a JTAG reset (CDC lines are unreliable post-flash);
#      the classic ESP32 reboots itself out of the flash's hard reset, so a
#      plain reader catches the run from the start. ----
if [ "$TARGET" = "s3" ]; then
    start_reader || { log "READER FAILED"; exit 1; }
    log "reader up, resetting via openocd USJ"
    "$OCD" -f "$OCDScripts/interface/esp_usb_jtag.cfg" \
           -f "$OCDScripts/target/esp32s3.cfg" \
           -c 'init; reset run; shutdown' >/dev/null 2>&1
    log "reset issued"
else
    start_reader || { log "READER FAILED"; exit 1; }
    log "reader up; power-cycle the board (or pulse EN) to start a run"
    # The classic ESP32 was flashed with --after hard-reset a moment ago when
    # MODE=full, so the run below is already in flight; for capture-only,
    # pulse EN via esptool's reset (CH340 control lines work fine here).
    if [ "$MODE" = "capture" ]; then
        ( source "$HOME/esp/activate-idf.sh" >/dev/null 2>&1
          python -m esptool --chip esp32 -p "$PORT" --before default-reset \
              --after hard-reset chip-id >/dev/null 2>&1 )
    fi
fi

verdict "$CAP" 480
rc=$?
log "verdict rc=$rc, capture $(wc -c < "$CAP") bytes"
grep -aE 'App version|checks, 0 failures|PASSED|FAILED' "$CAP" | tail -6
exit $rc
