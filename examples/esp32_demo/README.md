# PondMerge v1 ESP32-S3 demo

Runs the same demo scenario as the Host demo (`examples/host_demo.cpp`) on the
device and prints the same JSON Lines snapshot protocol (v1, see
`docs/DEMO_REQUIREMENTS.md`) over USB-Serial-JTAG. The scenario is scripted
with a fixed layout (repeatable by reset/power-cycle); interactive commands are
a Host-demo feature.

## Build and flash

```sh
source ~/esp/activate-idf.sh
cd examples/esp32_demo
idf.py set-target esp32s3          # first time only
idf.py -B build build
idf.py -B build -p /dev/ttyACM0 flash
```

## Watch

```sh
python3 -c 'import serial;s=serial.Serial("/dev/ttyACM0",115200,timeout=1)
while True:
    l=s.readline().decode("utf-8","replace").strip()
    print(l)'
```

or point the Host demo server at the device (display-only mode):

```sh
python3 examples/demo_server.py --serial /dev/ttyACM0 --port 8080
```

## Acceptance record

Record (per docs/DEMO_REQUIREMENTS.md §8.3): chip model/cores, ESP-IDF version,
commit/App version, ELF hash, Auto Zone config, snapshot count, and the
protocol version echoed in every record. Fill from the captured log.
