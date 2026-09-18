// Measurement instrument for the PondMerge benchmarks.
//
// =============================================================================
// THE FAILURE THIS FILE EXISTS TO PREVENT
// =============================================================================
// The benchmarks were originally timed per operation with
// std::chrono::steady_clock: t0 = now(); op(); t1 = now(). That is only valid
// while the clock call is cheap compared to the operation. On the measurement
// host it is not:
//
//     empty loop body                0.01 ns/iteration
//     sink += 1                      0.44 ns/iteration
//     sink += fn()                   0.28 ns/iteration
//     64-bit LCG (dependent chain)   1.21 ns/iteration
//     ___rdtsc()                10,244.37 ns/iteration   <-- the clock
//     clock_gettime(MONOTONIC)   9,341.18 ns/call
//     (raw syscall variant)     19,529.96 ns/call   so the vDSO IS in use;
//                                                   the vDSO itself is slow
//
// The host is a VMware guest whose RDTSC is trapped: the timestamp VALUE is
// correct (a 200 ms wait measures 200.024 ms), but reading it costs a VM exit.
// At ~9-10 us per read against operations of 40-90 ns, the instrument was
// 100-250x the cost of the thing being measured, and the resulting numbers were
// dominated by the timer while still looking plausible.
//
// Note what the 200 ms check does and does not prove: it proves the clock's
// VALUE is right, which is exactly why the defect was invisible. Accuracy of the
// value says nothing about the cost of reading it.
//
// =============================================================================
// THE RULE THAT FOLLOWS
// =============================================================================
// Time a GROUP of operations between two clock reads, never one operation
// between two. With N operations in the interval the instrument contributes
// 2 * call_cost / N per operation instead of 2 * call_cost. At N = 4096 that is
// ~4.5 ns per operation on this host instead of 18,000 ns, and repeating the
// trial and taking the median rejects the preemption stalls (the delta
// distribution on this host has a worst case of 2.7 ms, so a mean is not
// enough).
//
// Per-operation timings are therefore only produced where the operation is far
// longer than the clock: compaction at ~25 us is a borderline case and is
// reported as an aggregate; percentiles of it belong on the device, where the
// cycle counter is free. report() states which regime a run is in, so a result
// declares its own validity instead of relying on the reader to remember.

#ifndef PM_BENCH_TIMER_H
#define PM_BENCH_TIMER_H

#include <chrono>
#include <cstdint>
#include <cstdio>

#if defined(PM_ESP32)
#include "esp_cpu.h"
#include "sdkconfig.h"
#endif

namespace pm_bench {

// ---- backend ---------------------------------------------------------------

inline const char* g_name = "std::chrono::steady_clock";
inline uint64_t g_hz = 0;

#if defined(PM_ESP32)

// The CPU cycle counter (mcycle) is a register read: free, and monotonic.
// It is 32-bit and wraps every ~17.9 s at 240 MHz, so it is accumulated into a
// 64-bit cycle total; the unsigned delta makes the accumulation wrap-safe.
//
// The conversion deliberately avoids a 128-bit multiply. The ESP32-S3 is Xtensa,
// and the Xtensa toolchain has no TImode, so __uint128_t does not exist there --
// a fact this file learned from a failed build, not from documentation. Instead
// the cycles are scaled directly: cycles * 1000 / MHz is exact integer ns, and
// the intermediate fits in 64 bits until ~1.8e16 cycles, i.e. about two years of
// continuous counting. The product is formed before the division so there is no
// per-sample truncation bias.
inline uint64_t raw_now() { return (uint64_t)esp_cpu_get_cycle_count(); }
inline const char* backend_name() { return "esp_cpu_get_cycle_count (mcycle)"; }

inline uint64_t now_ns() {
    static uint32_t last = 0;
    static uint64_t cycles = 0;
    static bool started = false;
    uint32_t const c = (uint32_t)raw_now();
    if (!started) {
        started = true;
        last = c;
        g_hz = (uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ull;
        g_name = backend_name();
    }
    cycles += (uint32_t)(c - last); // unsigned delta: wrap-safe
    last = c;
    return cycles * 1000ull / (uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
}

#else

// Portable path. Slow on the measurement host, which is precisely why the
// batch discipline above is mandatory rather than advisory.
inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#endif

// ---- instrument self-report -------------------------------------------------

// Cost of one clock call, measured by calling it many times inside ONE long
// interval. The interval's two reads are negligible in a total of ~200 ms.
// Cached in g_call_ns because benchmarks use it to decide whether a measurement
// is above the instrument's noise floor.
inline uint64_t g_call_ns = 0;

inline uint64_t call_cost_ns() {
    if (g_call_ns) return g_call_ns;
    uint32_t const N = 20000;
    uint64_t const t0 = now_ns();
    for (uint32_t i = 0; i < N; ++i) (void)now_ns();
    uint64_t const t1 = now_ns();
    g_call_ns = (t1 - t0) / N;
    return g_call_ns;
}

// Cost added to EVERY measured interval, i.e. to each `t0 = now(); ...;
// now() - t0`. This is not simply twice the call cost: the guest->hypervisor
// transition that the second read triggers happens after the guest has resumed,
// so the interval carries one round trip rather than two. It is measured
// directly rather than inferred, and the MINIMUM is taken because the bias is
// purely additive and has a heavy tail (a single read was observed to take
// 2.7 ms). Anything that reports a per-event time on a host like this must
// subtract this figure and say so.
inline uint64_t g_interval_ns = 0;

inline uint64_t interval_cost_ns() {
    if (g_interval_ns) return g_interval_ns;
    uint64_t best = 0;
    for (uint32_t i = 0; i < 20000; ++i) {
        uint64_t const a = now_ns();
        uint64_t const b = now_ns();
        if (best == 0 || b - a < best) best = b - a;
    }
    g_interval_ns = best;
    return g_interval_ns;
}

inline void report(const char* who) {
    // Prime the backend before measuring it.
    (void)now_ns();
    uint64_t const cc = call_cost_ns();
    uint64_t const ic = interval_cost_ns();
    std::printf("--- timer -------------------------------------------------------\n");
    std::printf("  %s\n", who);
    std::printf("  backend            : %s\n",
                g_hz ? g_name : "std::chrono::steady_clock");
    if (g_hz) {
        std::printf("  frequency          : %llu Hz (%.3f ns/tick)\n",
                    (unsigned long long)g_hz, 1e9 / (double)g_hz);
    }
    std::printf("  clock call cost    : %llu ns  (measured, not assumed)\n",
                (unsigned long long)cc);
    std::printf("  interval cost      : %llu ns  (ANNOTATE per-event timings"
                " with this, or subtract it)\n", (unsigned long long)ic);
    if (cc > 1000) {
        std::printf("  verdict            : DO NOT QUOTE PER-OPERATION TIMINGS.\n"
                    "                       The clock costs %.1f us per read; the\n"
                    "                       numbers below are batched only.\n",
                    (double)cc / 1000.0);
    } else if (cc > 100) {
        std::printf("  verdict            : per-operation timings are marginal;\n"
                    "                       prefer the batched figures.\n");
    } else {
        std::printf("  verdict            : per-operation timings are usable.\n");
    }
    std::printf("-----------------------------------------------------------------\n");
}

// ---- trial harness ----------------------------------------------------------

// Run one measurement `trials` times and return the median. The median rather
// than the mean because the failure mode on a shared host is a long stall
// (measured here: a single clock read once took 2.7 ms) which would drag a mean
// arbitrarily far while leaving the median untouched.
//
// `Trial` must be callable as `uint64_t trial()` and return nanoseconds for one
// unit of work; the caller divides by its own batch size.
template <class Trial>
inline uint64_t median_ns(Trial&& trial, uint32_t trials) {
    constexpr uint32_t kMax = 31;
    if (trials > kMax) trials = kMax;
    if (trials == 0) return 0;
    uint64_t v[kMax];
    for (uint32_t i = 0; i < trials; ++i) v[i] = trial();
    // insertion sort, no recursion, no allocation
    for (uint32_t i = 1; i < trials; ++i) {
        uint64_t const key = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > key) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = key;
    }
    return v[trials / 2];
}

} // namespace pm_bench

#endif // PM_BENCH_TIMER_H
