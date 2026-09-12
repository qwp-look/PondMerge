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
// Stable per-context id for the Debug-only advice owner gate (round-8):
// the current FreeRTOS task handle. Lives until the task is deleted; the
// advice owner binding itself is reset by init()/deinit().
static inline uintptr_t pm_port_context_id(void) {
    return (uintptr_t)xTaskGetCurrentTaskHandle();
}
#else
#include <ctime>
#include <pthread.h>
#define PM_LOCK()   ((void)0)
#define PM_UNLOCK() ((void)0)
static inline uint64_t pm_port_ticks_us() {
    return (uint64_t)((double)clock() * 1000000.0 / CLOCKS_PER_SEC);
}
// Host: the POSIX thread id (glibc >= 2.34 keeps pthread_self in libc, so no
// extra link flags are needed for the tests/demo).
static inline uintptr_t pm_port_context_id(void) {
    return (uintptr_t)pthread_self();
}
#endif
