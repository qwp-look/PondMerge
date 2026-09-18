// On-target driver for the sensor-pipeline example. The example itself is
// examples/sensor_pipeline.cpp, compiled UNCHANGED (PM_SENSOR_NO_HOST_MAIN
// compiles out its host main()); this file only reports the target it runs
// on and hands over.
//
// Watch it with a reader that never stops draining the port
// (tests/serial_cap.py): the USB-Serial-JTAG console blocks printf once its
// transmit queue fills, which looks like "the test hung".
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include <cstdint>
#include <cstdio>

int pm_sensor_pipeline_run(void);

extern "C" void app_main(void) {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    std::printf("=== PondMerge sensor-pipeline example on-target ===\n");
    std::printf("chip: model=%d rev=%d.%d cores=%d, cpu %d MHz\n",
                (int)info.model, info.revision >> 4, info.revision & 0xf,
                (int)info.cores, (int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    std::printf("free internal heap: %u bytes (8-bit capable)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                  MALLOC_CAP_8BIT));
    std::fflush(stdout);

    int const rc = pm_sensor_pipeline_run();
    std::printf("[driver] sensor pipeline returned %d\n", rc);
    std::fflush(stdout);
}
