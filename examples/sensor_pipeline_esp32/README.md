# sensor_pipeline_esp32 — the real-scenario example on a target

Runs [../../sensor_pipeline.cpp](../../sensor_pipeline.cpp) — the
product-shaped integration reference — on an ESP32 target. The example source
is compiled UNCHANGED (`PM_SENSOR_NO_HOST_MAIN` compiles out its host
`main()`); the only per-target difference is the zone size and history cap
(see `main/CMakeLists.txt` for the sizing rationale). The pondmerge component
is the SAME one the acceptance firmware uses (`esp32/components/pondmerge`).

## Build, flash, capture

```sh
source ~/esp/activate-idf.sh
cd examples/sensor_pipeline_esp32

# ESP32-S3 (default target of this project)
rm -f sdkconfig && idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/ttyACM0 flash

# classic ESP32 (explicit set-target, as everywhere in this repo)
rm -f sdkconfig && idf.py set-target esp32 && idf.py build
idf.py -p /dev/ttyUSB0 flash

# capture (the reader must never stop draining the port)
python3 tests/serial_cap.py /dev/ttyACM0 120 115200 "=== sensor pipeline done"
```

The run is deterministic (fixed seed): the summary counts reproduce
byte-for-byte on the same build. The exit line is
`=== sensor pipeline done: ALL AUDITS PASSED ===` — anything else means one of
the self-audits (payload integrity, byte accounting, `validate()`) failed.

What the run shows, and why it matters, is documented in the example's header
comment and in README.md's integration section.
