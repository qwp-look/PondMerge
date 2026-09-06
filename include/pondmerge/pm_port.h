// PondMerge v1 - platform port layer.
// Single-core critical sections + a microsecond tick source for maintenance
// window statistics. Define PM_ESP32 when building inside ESP-IDF.
#pragma once

#include <stdint.h>

#if defined(PM_ESP32)
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
extern portMUX_TYPE pm_spinlock;
#define PM_LOCK()   portENTER_CRITICAL(&pm_spinlock)
#define PM_UNLOCK() portEXIT_CRITICAL(&pm_spinlock)
static inline uint64_t pm_port_ticks_us() { return (uint64_t)esp_timer_get_time(); }
#else
#include <ctime>
#define PM_LOCK()   ((void)0)
#define PM_UNLOCK() ((void)0)
static inline uint64_t pm_port_ticks_us() {
    return (uint64_t)((double)clock() * 1000000.0 / CLOCKS_PER_SEC);
}
#endif
