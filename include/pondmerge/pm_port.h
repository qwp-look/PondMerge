// PondMerge v1 - platform port layer.
// Task-context critical section (on PM_ESP32: an SMP spinlock plus local
// interrupt disable via portENTER_CRITICAL; on host: compiled to nothing) +
// a microsecond tick source for maintenance window statistics. The locked
// section must stay SHORT: it is a spinlock that other cores spin on, and
// FreeRTOS forbids most API calls inside it. Define PM_ESP32 when building
// inside ESP-IDF.
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
#include <pthread.h>
#include <time.h>
#define PM_LOCK()   ((void)0)
#define PM_UNLOCK() ((void)0)
// Host tick source: WALL time in microseconds.
//
// This used to be clock(), i.e. CLOCK_PROCESS_CPUTIME_ID. Two problems, both
// measured rather than assumed:
//
//  1. Cost. On glibc clock() is a real syscall: 20.9 us per call on the x86-64
//     reference host. compact() calls this twice, so the library's own
//     maintenance-window statistic was adding ~42 us to every compaction --
//     about 15x the cost of the memory it actually moved (the 184 KB workload
//     behind it copies in 2.8 us; see bench/RESULTS.md). The instrumentation
//     dominated the thing it was instrumenting.
//  2. Semantics. clock() reports CPU time consumed by the process, not elapsed
//     time, so compact_time_us under-reported any maintenance window that was
//     preempted and could not be compared against a wall-clock budget. A
//     "maintenance window statistic" is a wall-clock quantity.
//
// CLOCK_MONOTONIC is the right quantity and an order of magnitude cheaper. The
// ESP32 branch is untouched: esp_timer_get_time() is already the correct
// wall-clock source there, and is a register read rather than a syscall.
static inline uint64_t pm_port_ticks_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
// Host: the POSIX thread id (glibc >= 2.34 keeps pthread_self in libc, so no
// extra link flags are needed for the tests/demo).
static inline uintptr_t pm_port_context_id(void) {
    return (uintptr_t)pthread_self();
}
#endif
