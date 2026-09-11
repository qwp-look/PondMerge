// PondMerge v1 - ESP32-S3 dual-core lock-boundary test (HANDOVER_v4 T1 /
// round-3 guide R25 device part). Host builds NEVER compile this file: the
// host PM_LOCK is a no-op, so only the real FreeRTOS spinlock on two cores
// can exercise the borrow/pause/compact boundary.
//
// What is tested (and what is deliberately NOT tested):
//   * borrow_begin/borrow_end on DIFFERENT objects from two tasks running on
//     different cores, while a third task flips pause -> compact -> resume in
//     a loop. This is the contract PondMerge DOES promise: the borrow
//     accounting and the state flips are internally synchronized, so no
//     borrow can start once pause completed, and no compact can run while a
//     borrow is live.
//   * Not tested here: concurrent alloc/free (design says single owner, not
//     promised) and duplicate borrow_end of one token (a caller bug; Debug
//     asserts it, and this build has PM_DEBUG=1 -- the counter logic for the
//     refusal paths is covered by host R25 in a Release build).
//
// Detection strategy is consequence-based, so the test cannot false-fail on
// benign interleavings: every observed status must belong to the set the
// contract allows, every borrow begin is paired with exactly one end, the
// payload must survive, and at the end the counters must balance and
// validate() must pass. A lost race between pause and borrow_begin would
// surface as a payload/validate failure (data moved under a live borrow) or
// as an impossible status -- not as a timing guess.
#include "pondmerge/pondmerge.hpp"
#include "../src/internal.h" // white-box: borrow-count conservation reads

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdint>

extern uint8_t g_zone[]; // 256 KiB, 16-byte aligned (tests/suite.cpp)

namespace {

uint32_t c_checks = 0;
uint32_t c_fails = 0;

#define CCHECK(cond)                                                                  \
    do {                                                                              \
        ++c_checks;                                                                   \
        if (!(cond)) {                                                                \
            printf("    CONC CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++c_fails;                                                                \
        }                                                                             \
    } while (0)

#define CCHECK_ST(expr, want)                                                         \
    do {                                                                              \
        pm::Status _s = (expr);                                                       \
        ++c_checks;                                                                   \
        if (_s != (want)) {                                                           \
            printf("    CONC CHECK failed %s:%d: %s -> %s (want %s)\n", __FILE__,     \
                   __LINE__, #expr, pm::status_name(_s), pm::status_name(want));      \
            ++c_fails;                                                                \
        }                                                                             \
    } while (0)

// The device Auto Zone for this test lives in the low segments of the
// suite's zone buffer (shared with tests/suite.cpp): static DRAM is too
// tight for another zone-sized buffer. g_zone has external linkage in
// tests/suite.cpp; the extern declaration must stay OUTSIDE the anonymous
// namespace below, or it would declare a NEW internal-linkage object.
uint8_t* const c_zone = ::g_zone;

// Task-safe accounting: every mutation happens inside this test's own
// spinlock, so the counters are exact even across cores.
portMUX_TYPE c_mux = portMUX_INITIALIZER_UNLOCKED;
#define C_LOCK()   portENTER_CRITICAL(&c_mux)
#define C_UNLOCK() portEXIT_CRITICAL(&c_mux)

struct Counters {
    pm::PoolId pool;
    pm::RawRef ref_a;       // object borrowed by task A (core 0)
    pm::RawRef ref_b;       // object borrowed by task B (core 1)
    volatile bool stop;
    volatile bool a_done;
    volatile bool b_done;

    uint32_t a_ok, a_end, a_busy;
    uint32_t b_ok, b_end, b_busy;
    uint32_t pause_ok, pause_already, compact_ok, compact_busy, resume_ok;
    uint32_t bad_status;
    pm::Status first_bad;
};

Counters c{};

void note_status_bad(pm::Status s, const char* who) {
    C_LOCK();
    ++c.bad_status;
    if (c.bad_status == 1) {
        c.first_bad = s;
        printf("    CONC unexpected status from %s: %s\n", who, pm::status_name(s));
    }
    C_UNLOCK();
}

void borrower_task(void* arg) {
    bool const is_a = (arg != nullptr);
    pm::RawRef const ref = is_a ? c.ref_a : c.ref_b;
    uint32_t iters = 0;
    while (!c.stop) {
        void* p = nullptr;
        pm::Status st = pm::borrow_begin(ref, 64, 1, p);
        if (st == pm::Status::Ok) {
            static_cast<uint8_t*>(p)[0] = 0xA5; // touch the payload while borrowed
            pm::borrow_end(ref);
            C_LOCK();
            if (is_a) { ++c.a_ok; ++c.a_end; } else { ++c.b_ok; ++c.b_end; }
            C_UNLOCK();
        } else if (st == pm::Status::Busy) {
            C_LOCK();
            if (is_a) ++c.a_busy; else ++c.b_busy;
            C_UNLOCK();
        } else {
            note_status_bad(st, is_a ? "borrower A" : "borrower B");
        }
        // Yield to the co-resident maintainer, and once in a while block a
        // tick so the IDLE task (task watchdog) gets CPU as well.
        if ((++iters & 63) == 0) taskYIELD();
        if ((iters & 511) == 0) vTaskDelay(1);
    }
    if (is_a) c.a_done = true;
    else c.b_done = true;
    vTaskDelete(nullptr);
}

void maintainer_task(void*) {
    uint32_t rounds = 0;
    while (!c.stop && rounds < 200) {
        pm::Status ps = pm::pause(c.pool);
        if (ps == pm::Status::Ok) { C_LOCK(); ++c.pause_ok; C_UNLOCK(); }
        else if (ps == pm::Status::AlreadyPaused) { C_LOCK(); ++c.pause_already; C_UNLOCK(); }
        else note_status_bad(ps, "pause");

        pm::Status cs = pm::compact(c.pool); // internally requires quiescence
        if (cs == pm::Status::Ok) { C_LOCK(); ++c.compact_ok; C_UNLOCK(); }
        else if (cs == pm::Status::Busy) { C_LOCK(); ++c.compact_busy; C_UNLOCK(); }
        else note_status_bad(cs, "compact");

        pm::Status rs = pm::resume(c.pool);
        if (rs == pm::Status::Ok) { C_LOCK(); ++c.resume_ok; C_UNLOCK(); }
        else note_status_bad(rs, "resume");

        ++rounds;
        taskYIELD();
    }
    c.stop = true;
    vTaskDelete(nullptr);
}

} // namespace

int pondmerge_run_concurrency_tests(void) {
    printf("[CONC] dual-core borrow/pause/compact lock boundary\n");
    uint32_t const f0 = c_fails;

    pm::Config cfg{c_zone, 2 * 4096, 4096}; // first two segments of the shared zone
    CCHECK_ST(pm::init(cfg), pm::Status::Ok);
    CCHECK_ST(pm::create_pool(c.pool, 2), pm::Status::Ok);
    CCHECK_ST(pm::alloc(c.pool, 64, 8, 0, 1, c.ref_a), pm::Status::Ok);
    CCHECK_ST(pm::alloc(c.pool, 64, 8, 0, 2, c.ref_b), pm::Status::Ok);
    {
        // Known payload except byte 0, which the borrowers keep flipping:
        // after the run byte 0 must be 0xA5 (last write) and the rest intact.
        void* p = nullptr;
        CCHECK_ST(pm::borrow_begin(c.ref_a, 64, 1, p), pm::Status::Ok);
        uint8_t* bytes = static_cast<uint8_t*>(p);
        for (uint32_t i = 0; i < 64; ++i) bytes[i] = (uint8_t)(0x10 + i);
        pm::borrow_end(c.ref_a);
        CCHECK_ST(pm::borrow_begin(c.ref_b, 64, 1, p), pm::Status::Ok);
        pm::borrow_end(c.ref_b);
    }

    TaskHandle_t ha = nullptr, hb = nullptr, hm = nullptr;
    // A and the maintainer share core 0 (they must timeshare), B pins core 1.
    // All three are created at app_main's OWN priority first: the first task
    // created at a higher priority would preempt app_main immediately and the
    // remaining xTaskCreate calls would never run (observed on device as an
    // eternal pm_borrow_a with IDLE1 on core 1 and no verdict). Only after
    // all three exist are they raised to their stress priority.
    const UBaseType_t stress_prio = 5;
    CCHECK(xTaskCreatePinnedToCore(borrower_task, "pm_borrow_a", 3072,
                                   (void*)1, 1, &ha, 0) == pdPASS);
    CCHECK(xTaskCreatePinnedToCore(maintainer_task, "pm_maint", 3072,
                                   nullptr, 1, &hm, 0) == pdPASS);
    CCHECK(xTaskCreatePinnedToCore(borrower_task, "pm_borrow_b", 3072,
                                   nullptr, 1, &hb, 1) == pdPASS);
    vTaskPrioritySet(ha, stress_prio);
    vTaskPrioritySet(hm, stress_prio);
    vTaskPrioritySet(hb, stress_prio);

    // The maintainer stops after 200 rounds; wait (bounded) for both
    // borrowers to observe the stop flag and exit.
    for (int i = 0; i < 3000 && !(c.a_done && c.b_done); ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    CCHECK(c.a_done && c.b_done);

    Counters snap;
    C_LOCK();
    snap = c;
    C_UNLOCK();

    printf("    rounds: borrow_a ok=%u busy=%u | borrow_b ok=%u busy=%u\n",
           (unsigned)snap.a_ok, (unsigned)snap.a_busy,
           (unsigned)snap.b_ok, (unsigned)snap.b_busy);
    printf("    maintainer: pause=%u(already %u) compact ok=%u busy=%u resume=%u\n",
           (unsigned)snap.pause_ok, (unsigned)snap.pause_already,
           (unsigned)snap.compact_ok, (unsigned)snap.compact_busy,
           (unsigned)snap.resume_ok);

    // Both sides actually ran and overlapped.
    CCHECK(snap.a_ok > 0 && snap.b_ok > 0);
    CCHECK(snap.compact_ok > 0);
    CCHECK(snap.a_busy > 0); // the borrowers DID hit paused/compacting windows
    // Contract: the only allowed failure for a borrow against a valid object
    // in a paused/compacting pool is Busy.
    CCHECK(snap.bad_status == 0);
    // Every begin was paired with exactly one end.
    CCHECK(snap.a_ok == snap.a_end && snap.b_ok == snap.b_end);
    // Every pause cycle was completed.
    CCHECK(snap.resume_ok == snap.pause_ok + snap.pause_already);

    // Post-run integrity: counters balanced, payload intact, structure valid.
    {
        using namespace pm::internal;
        CCHECK(g().pools[c.pool].borrow_count == 0);
        CCHECK(g().objects[c.ref_a.index].active_borrows == 0);
        CCHECK(g().objects[c.ref_b.index].active_borrows == 0);
    }
    {
        void* p = nullptr;
        CCHECK_ST(pm::borrow_begin(c.ref_a, 64, 1, p), pm::Status::Ok);
        uint8_t const* bytes = static_cast<uint8_t const*>(p);
        CCHECK(bytes[0] == 0xA5);
        bool intact = true;
        for (uint32_t i = 1; i < 64; ++i)
            if (bytes[i] != (uint8_t)(0x10 + i)) intact = false;
        CCHECK(intact); // no compaction ever moved the data under a borrow
        pm::borrow_end(c.ref_a);
    }
    CCHECK_ST(pm::validate(c.pool), pm::Status::Ok);
    CCHECK_ST(pm::free(c.ref_a), pm::Status::Ok);
    CCHECK_ST(pm::free(c.ref_b), pm::Status::Ok);
    CCHECK_ST(pm::destroy_pool(c.pool), pm::Status::Ok);
    CCHECK_ST(pm::deinit(), pm::Status::Ok);

    printf("  [CONC] %u checks, %u failures\n", (unsigned)c_checks, (unsigned)c_fails);
    (void)f0;
    return c_fails == 0 ? 0 : 1;
}
