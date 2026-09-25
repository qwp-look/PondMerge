// PondMerge v1 -- real-scenario example: a sensor node's managed memory.
//
// This file is an INTEGRATION REFERENCE, not a benchmark and not a protocol
// demo: it is one product-shaped workload, written the way a consumer would
// write it, exercising the four things a real product actually needs from
// PondMerge at once:
//
//   1. typed, movable message objects whose references stay valid across a
//      compaction (the core promise -- verified here by payload checksums),
//   2. pinned DMA buffers that must never move, rotating over time (real
//      relocation barriers, drifting through the pool -- the thing that
//      fragments real pools),
//   3. the standard compaction flow of docs/COMPACTION_POLICY.md section 6:
//      a large contiguous demand fails -> read-only advice -> quiet window ->
//      compact() -> retry, with every outcome counted,
//   4. a low-frequency monitor loop on poll_compaction_advice() that never
//      executes anything by itself.
//
// THE SCENARIO (simulated, deterministic; no real peripherals):
// A battery-powered sensor node keeps a rolling history of its readings and
// periodically batch-uploads it in one large contiguous block.
//
//   every tick      an IMU sample arrives, is processed (RAII borrow), and
//                   is freed again -- the transient churn of a real node
//   every 8th tick  a temperature sample, same treatment
//   every 6th       a GPS fix, usually RETAINED in the history
//   every 9th       a waveform capture, usually retained as well; between
//                   them the history settles at ~512 B per entry
//   every 50th      the DMA ring (two 1 KiB pinned buffers) rotates: a NEW
//                   pinned buffer is allocated BEFORE the oldest is freed --
//                   double-buffering through a switchover, which is why the
//                   barriers drift instead of reusing their own holes
//   every 300th     a random history entry EXPIRES (server-side invalidation)
//   every 1200th    the server ACKs and the node flushes ~40% of its history
//                   at random positions -- one event opens a large DISPERSED
//                   free set between the pinned barriers, and the next
//                   exports run while the pool refills
//   every 100th     the node demands one 8 KiB contiguous batch buffer for
//                   upload; on failure it follows the advice/compact flow.
//                   At ~87% occupancy the four possible outcomes all occur
//                   on the host run: direct, rescued by compaction, failed
//                   after compaction (the pinned barriers keep the
//                   consolidated space in pockets -- see bench/RESULTS.md
//                   5.8), and skipped on advice (total free insufficient)
//
// WHY THESE MECHANICS AND NOT SIMPLER ONES: two earlier, simpler histories
// were considered and rejected for the same reasons bench/fragmentation.cpp
// documents. A pure same-size churn never fragments (free() coalesces), and
// drop-oldest-only eviction frees the lowest addresses, which first-fit
// refills -- the pool heals itself and the compaction path would be dead code
// in its own demo. Random expiry between drifting pinned barriers is what
// keeps holes stranded, which is also what real pools look like.
//
// WHAT IS ASSERTED (the example audits itself and its exit code says whether
// every audit passed):
//   payload integrity  every live reference -- history, DMA ring, export --
//                      is borrowed and its checksum re-derived after EVERY
//                      compaction and at the end. A mismatch means a
//                      reference did not survive relocation;
//   accounting         used + free == capacity and live objects == tracked
//                      objects, from get_stats();
//   structure          validate() == Ok at the end.
//
// BUILD (host):  g++ -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -Isrc
//                  examples/sensor_pipeline.cpp src/core.cpp -o build/sensor_pipeline
//                ./build/sensor_pipeline      (exit 0 = all audits passed)
// BUILD (device): examples/sensor_pipeline_esp32/ is an IDF project over the
// same source (PM_SENSOR_NO_HOST_MAIN); sizing is per-target in its CMake.
//
// No exceptions, no RTTI, no dynamic allocation; single owner, maintenance
// runs in a quiescent window -- the same contract the library documents.

#include "pondmerge/pondmerge.hpp"

#include <cstdint>
#include <cstdio>

#ifndef PM_SENSOR_NO_HOST_MAIN
int pm_sensor_pipeline_run(void);
int main() { return pm_sensor_pipeline_run(); }
#endif

namespace {

// ---- sizing (host defaults; the device CMake overrides per target) --------
#ifndef PM_SP_ZONE_BYTES
#define PM_SP_ZONE_BYTES (256u * 1024u)
#endif
#ifndef PM_SP_KEEP_MAX
#define PM_SP_KEEP_MAX 480u   // history entries (an object-count AND a story cap)
#endif
#ifndef PM_SP_TICKS
#define PM_SP_TICKS 20000u
#endif
#ifndef PM_SP_DMA_SLOTS
#define PM_SP_DMA_SLOTS 2u    // a ping-pong DMA ring: two pinned barriers
#endif
#ifndef PM_SP_ROTATE_EVERY
#define PM_SP_ROTATE_EVERY 50u
#endif
#ifndef PM_SP_EXPIRY_EVERY
#define PM_SP_EXPIRY_EVERY 300u
#endif
#ifndef PM_SP_EXPORT_BYTES
#define PM_SP_EXPORT_BYTES (8u * 1024u)
#endif
#ifndef PM_SP_FLUSH_EVERY
#define PM_SP_FLUSH_EVERY 1200u
#endif

constexpr uint32_t ZONE_BYTES   = PM_SP_ZONE_BYTES;
constexpr uint32_t SEGMENT      = 4096u;
constexpr uint32_t KEEP_MAX     = PM_SP_KEEP_MAX;
constexpr uint32_t TICKS        = PM_SP_TICKS;
constexpr uint32_t DMA_SLOTS    = PM_SP_DMA_SLOTS;
constexpr uint32_t ROTATE_EVERY = PM_SP_ROTATE_EVERY;
constexpr uint32_t TEMP_EVERY   = 8u;
constexpr uint32_t GPS_EVERY    = 6u;
constexpr uint32_t WAVE_EVERY   = 9u;
constexpr uint32_t EXPIRY_EVERY = PM_SP_EXPIRY_EVERY;
constexpr uint32_t EXPORT_EVERY = 100u;
constexpr uint32_t EXPORT_BYTES = PM_SP_EXPORT_BYTES;
constexpr uint32_t FLUSH_EVERY  = PM_SP_FLUSH_EVERY;
constexpr uint32_t MONITOR_EVERY = 64u;

static_assert(KEEP_MAX + DMA_SLOTS + 8u < PM_MAX_OBJECTS,
              "live objects must fit the descriptor table with headroom");

// ---- message types ---------------------------------------------------------
// Plain data only. The relocatability trait is OPT-IN and PROJECT-audited
// (task-book 3.5): these four contain no addresses, no handles, no sync
// primitives, so this project specializes the trait -- exactly the audit a
// real consumer performs for its own types.
struct Header {         // every payload carries one: seq, type and checksum
    uint32_t seq;
    uint32_t tag;       // 'I' 'T' 'G' 'W' 'D' -- a cheap type mark
    uint32_t sum;       // sum of the payload words below
};

struct ImuSample   { Header h; uint32_t w[12];  };  // 64 B
struct TempSample  { Header h; uint32_t w[4];   };  // 32 B
struct GpsFix      { Header h; uint32_t w[60];  };  // 256 B
struct Waveform    { Header h; uint32_t w[188]; };  // 768 B
struct DmaBuffer   { Header h; uint32_t w[253]; };  // ~1 KiB, pinned: never moves

} // namespace

// The trait specializations MUST sit in pm's namespace, outside the anonymous
// one -- this is the pattern a consumer's own header would carry.
namespace pm {
template <> struct pm_is_relocatable<::ImuSample>  : std::true_type {};
template <> struct pm_is_relocatable<::TempSample> : std::true_type {};
template <> struct pm_is_relocatable<::GpsFix>     : std::true_type {};
template <> struct pm_is_relocatable<::Waveform>   : std::true_type {};
} // namespace pm

namespace {

alignas(16) uint8_t g_zone[ZONE_BYTES];

const char* verdict_name(pm::CompactionVerdict v) {
    switch (v) {
    case pm::CompactionVerdict::NO_ACTION:               return "NO_ACTION";
    case pm::CompactionVerdict::COMPACT_RECOMMENDED:     return "RECOMMENDED";
    case pm::CompactionVerdict::COMPACT_BLOCKED:         return "BLOCKED";
    case pm::CompactionVerdict::COMPACT_UNLIKELY_TO_HELP:return "UNLIKELY_TO_HELP";
    case pm::CompactionVerdict::INVALID_METADATA:        return "INVALID_METADATA";
    default:                                             return "INVALID_REQUEST";
    }
}

// ---- deterministic pattern source ------------------------------------------
uint32_t g_rng;
inline uint32_t rng() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

uint32_t g_seq = 0;

// Fill a payload deterministically and write the header (checksum included).
template <class T>
void stamp(T& t, char tag) {
    t.h.seq = ++g_seq;
    t.h.tag = (uint32_t)tag;
    uint32_t s = 0;
    for (uint32_t& x : t.w) { x = rng(); s += x; }
    t.h.sum = s;
}

// Re-derive and compare -- the reference-stability check. Borrowed access
// only; nothing here holds a raw pointer across a maintenance call.
template <class T>
bool intact(pm::pm_local_ptr<T> const& p) {
    auto a = p.try_borrow();
    if (!a.ok()) return false;
    uint32_t s = 0;
    for (uint32_t const x : a.value->w) s += x;
    return s == a.value->h.sum;
}

// ---- bookkeeping -----------------------------------------------------------
struct HistoryEntry {
    pm::pm_local_ptr<Waveform> as_wave;
    pm::pm_local_ptr<GpsFix>   as_gps;
    uint8_t  kind;      // 'G' or 'W'
    uint32_t seq;
};
HistoryEntry g_hist[KEEP_MAX];
uint32_t g_hist_n = 0;

pm::pm_local_ptr<DmaBuffer> g_dma[DMA_SLOTS];
uint32_t g_dma_tail = 0;    // oldest slot

struct Counts {
    uint32_t imu, temp, gps, wave, retained, expired, flushed;
    uint32_t exports_ok, exports_rescued, exports_failed, exports_skipped;
    uint32_t compactions, compact_refused;
    uint32_t advice_counts[6];
    uint32_t monitor_changes, monitor_polls;
    uint32_t integrity_checks, integrity_failures;
} g_c;

pm::CompactionVerdict g_last_verdict = pm::CompactionVerdict::INVALID_REQUEST;

bool audit_all(const char* when) {
    bool ok = true;
    for (uint32_t i = 0; i < g_hist_n; ++i) {
        g_c.integrity_checks++;
        bool e_ok = g_hist[i].kind == 'G' ? intact(g_hist[i].as_gps)
                                          : intact(g_hist[i].as_wave);
        if (!e_ok) { ok = false; g_c.integrity_failures++; }
    }
    for (uint32_t i = 0; i < DMA_SLOTS; ++i) {
        g_c.integrity_checks++;
        if (!intact(g_dma[i])) { ok = false; g_c.integrity_failures++; }
    }
    if (!ok) printf("  [AUDIT] payload integrity FAILED after %s\n", when);
    return ok;
}

void drop_history(uint32_t idx) {
    if (g_hist[idx].kind == 'G') (void)pm::pm_destroy(g_hist[idx].as_gps);
    else                         (void)pm::pm_destroy(g_hist[idx].as_wave);
    g_hist[idx] = g_hist[g_hist_n - 1u];   // swap-remove: no shifting
    g_hist_n--;
}

// "Evict oldest" by arrival sequence: the keep list uses swap-remove, so
// index 0 is NOT the oldest once a random expiry has shuffled it.
void evict_oldest() {
    uint32_t oldest = 0;
    for (uint32_t i = 1; i < g_hist_n; ++i)
        if (g_hist[i].seq < g_hist[oldest].seq) oldest = i;
    drop_history(oldest);
}

// The one place the standard compaction flow lives (COMPACTION_POLICY.md 6).
// Returns: 1 = demand satisfied (maybe after compaction), 0 = not satisfied.
int export_batch(pm::PoolId pool) {
    pm::CompactionRequest req{EXPORT_BYTES, 1, 0, 0, 0, 0}; // 0 limits = full pass
    pm::pm_local_ptr<uint8_t> batch = pm::pm_alloc_buffer(pool, EXPORT_BYTES,
                                                          pm::PM_MOVABLE).value;
    if (batch.valid()) { g_c.exports_ok++; (void)pm::pm_destroy(batch); return 1; }

    // Read-only advice first, as a product would do. The request is passed so
    // the advice can say whether the demand WOULD fit after a compaction.
    pm::CompactionAdvice a = pm::analyze_compaction(pool, &req);
    g_c.advice_counts[a.verdict <= pm::CompactionVerdict::INVALID_METADATA
                          ? (uint32_t)a.verdict : 5u]++;
    printf("  [export] demand %u B failed; advice=%s largest=%u free=%u\n",
           (unsigned)EXPORT_BYTES, verdict_name(a.verdict),
           (unsigned)a.largest_free_block, (unsigned)a.free_bytes);

    if (a.verdict == pm::CompactionVerdict::COMPACT_BLOCKED ||
        a.verdict == pm::CompactionVerdict::COMPACT_UNLIKELY_TO_HELP) {
        g_c.exports_skipped++;
        return 0;   // product decision: drop this cycle, stream instead
    }

    // The quiet window: a real product stops DMA/tasks here. In this
    // single-owner simulation the window is trivially quiet -- stated, not
    // hidden: nothing here proves anything about concurrency.
    pm::Status cs = pm::compact(pool);
    if (cs != pm::Status::Ok) {
        g_c.compact_refused++;
        (void)pm::resume(pool);   // a refused compact stays Paused
        g_c.exports_failed++;
        return 0;
    }
    g_c.compactions++;

    // References were stable across the moves -- checked, not assumed.
    audit_all("compaction");

    batch = pm::pm_alloc_buffer(pool, EXPORT_BYTES, pm::PM_MOVABLE).value;
    if (batch.valid()) { g_c.exports_rescued++; (void)pm::pm_destroy(batch); return 1; }
    g_c.exports_failed++;
    return 0;
}

} // namespace

int pm_sensor_pipeline_run() {
    g_rng = 0x6C078965u;
    g_seq = 0;

    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) { printf("init failed\n"); return 1; }
    pm::PoolId pool{};
    if (pm::create_pool(pool, ZONE_BYTES / SEGMENT) != pm::Status::Ok) {
        printf("create_pool failed\n"); return 1;
    }

    printf("PondMerge sensor-pipeline example (integration reference)\n");
    printf("zone=%u B segment=%u history<=%u ticks=%u export=%u B/%u ticks\n",
           (unsigned)ZONE_BYTES, (unsigned)SEGMENT, (unsigned)KEEP_MAX,
           (unsigned)TICKS, (unsigned)EXPORT_BYTES, (unsigned)EXPORT_EVERY);
    printf("metadata budget (runtime value, not a formula): %u B\n\n",
           (unsigned)pm::global_stats().metadata_bytes);

    // The DMA ring starts fully resident; its buffers are pinned for their
    // whole life. Rotation below is what moves the barriers around.
    for (uint32_t i = 0; i < DMA_SLOTS; ++i) {
        auto b = pm::pm_make_pinned<DmaBuffer>(pool);
        if (!b.ok()) { printf("dma alloc failed\n"); return 1; }
        g_dma[i] = b.value;
        if (auto acc = g_dma[i].try_borrow(); acc.ok()) stamp(*acc.value, 'D');
    }

    bool audits_ok = true;
    for (uint32_t tick = 1; tick <= TICKS && audits_ok; ++tick) {

        // ---- transient IMU sample: process under RAII borrow, then free ----
        {
            auto s = pm::pm_make<ImuSample>(pool);
            if (s.ok()) {
                if (auto acc = s->try_borrow(); acc.ok()) stamp(*acc.value, 'I');
                g_c.imu++;
                (void)pm::pm_destroy(s.value);
            }
        }
        if (tick % TEMP_EVERY == 0) {
            auto s = pm::pm_make<TempSample>(pool);
            if (s.ok()) {
                if (auto acc = s->try_borrow(); acc.ok()) stamp(*acc.value, 'T');
                g_c.temp++;
                (void)pm::pm_destroy(s.value);
            }
        }

        // ---- retained readings ---------------------------------------------
        if (tick % GPS_EVERY == 0) {
            auto s = pm::pm_make<GpsFix>(pool);
            if (s.ok()) {
                if (auto acc = s->try_borrow(); acc.ok()) stamp(*acc.value, 'G');
                if ((rng() % 100u) < 60u) {         // usually retained
                    if (g_hist_n == KEEP_MAX) evict_oldest();       // cap: evict oldest
                    g_hist[g_hist_n].as_gps = s.value;
                    g_hist[g_hist_n].kind = 'G';
                    g_hist[g_hist_n].seq = g_seq;
                    g_hist_n++;
                    g_c.retained++;
                } else {
                    (void)pm::pm_destroy(s.value);
                }
                g_c.gps++;
            }
        }
        if (tick % WAVE_EVERY == 0) {
            auto s = pm::pm_make<Waveform>(pool);
            if (s.ok()) {
                if (auto acc = s->try_borrow(); acc.ok()) stamp(*acc.value, 'W');
                if ((rng() % 100u) < 85u) {
                    if (g_hist_n == KEEP_MAX) drop_history(0);
                    g_hist[g_hist_n].as_wave = s.value;
                    g_hist[g_hist_n].kind = 'W';
                    g_hist[g_hist_n].seq = g_seq;
                    g_hist_n++;
                    g_c.retained++;
                } else {
                    (void)pm::pm_destroy(s.value);
                }
                g_c.wave++;
            }
        }

        // ---- random mid-history expiry: holes in the MIDDLE of the pool ----
        if (tick % EXPIRY_EVERY == 0 && g_hist_n > 0u) {
            drop_history(rng() % g_hist_n);
            g_c.expired++;
        }

        // ---- server ack: a random ~40%% of the history is flushed at once --
        // The batched-upload contract: the server acks what it received, the
        // node drops those entries wherever they sit. One event frees a large
        // DISPERSED set -- holes at random positions between pinned barriers,
        // the shape that first-fit refills only slowly and that makes the
        // next 12 KiB export fail until the pool is consolidated.
        if (tick % FLUSH_EVERY == 0 && g_hist_n > 0u) {
            uint32_t const n = g_hist_n * 2u / 5u;
            for (uint32_t k = 0; k < n && g_hist_n > 0u; ++k)
                drop_history(rng() % g_hist_n);
            g_c.flushed += n;
        }

        // ---- DMA ring rotation: allocate the NEW buffer BEFORE freeing the
        // old one (double-buffering through the switchover). The new pinned
        // block cannot land in the old buffer's hole (it is still busy), so
        // the barriers drift through the pool over the node's life.
        if (tick % ROTATE_EVERY == 0) {
            auto nb = pm::pm_make_pinned<DmaBuffer>(pool);
            if (nb.ok()) {
                if (auto acc = nb->try_borrow(); acc.ok()) stamp(*acc.value, 'D');
                (void)pm::pm_destroy(g_dma[g_dma_tail]);
                g_dma[g_dma_tail] = nb.value;
                g_dma_tail = (g_dma_tail + 1u) % DMA_SLOTS;
            }
        }

        // ---- the batch upload: the demand that drives compaction ----------
        if (tick % EXPORT_EVERY == 0) (void)export_batch(pool);

        // ---- monitor loop: report-only, never executes anything ------------
        if (tick % MONITOR_EVERY == 0) {
            pm::CompactionRequest req{EXPORT_BYTES, 1, 0, 0, 0, 0}; // 0 limits = full pass
            bool changed = false;
            pm::CompactionAdvice a = pm::poll_compaction_advice(pool, &req, &changed);
            g_c.monitor_polls++;
            // The poll's own suppression keys on every diagnostic, so it
            // re-prompts on largest-block jitter; a product logs the VERDICT
            // transition, which is the part a maintainer acts on.
            if (a.verdict != g_last_verdict) {
                printf("  [monitor] tick %u: %s (largest %u B, live %u)\n",
                       (unsigned)tick, verdict_name(a.verdict),
                       (unsigned)a.largest_free_block, (unsigned)a.live_objects);
                g_last_verdict = a.verdict;
                g_c.monitor_changes++;
            }
        }
    }

    // ---- final audits -------------------------------------------------------
    audits_ok = audit_all("end of run") && audits_ok;
    pm::PoolStats s = pm::get_stats(pool);
    bool const structure_ok = (pm::validate(pool) == pm::Status::Ok);

    printf("\n--- summary ---\n");
    printf("  messages: imu %u, temp %u, gps %u, waveform %u; retained %u,"
         " expired %u, ack-flushed %u\n", (unsigned)g_c.imu,
         (unsigned)g_c.temp, (unsigned)g_c.gps, (unsigned)g_c.wave,
         (unsigned)g_c.retained, (unsigned)g_c.expired, (unsigned)g_c.flushed);
    printf("  history live at end: %u of <= %u\n", (unsigned)g_hist_n,
           (unsigned)KEEP_MAX);
    printf("  exports (every %u ticks): direct %u, rescued by compaction %u,"
           " failed after compaction %u, skipped by advice %u\n",
           (unsigned)EXPORT_EVERY, (unsigned)g_c.exports_ok,
           (unsigned)g_c.exports_rescued, (unsigned)g_c.exports_failed,
           (unsigned)g_c.exports_skipped);
    printf("  compactions: %u ok, %u refused; advice verdicts: NO_ACTION %u,"
           " RECOMMENDED %u, BLOCKED %u, UNLIKELY %u, INVALID %u\n",
           (unsigned)g_c.compactions, (unsigned)g_c.compact_refused,
           (unsigned)g_c.advice_counts[0], (unsigned)g_c.advice_counts[1],
           (unsigned)g_c.advice_counts[2], (unsigned)g_c.advice_counts[3],
           (unsigned)g_c.advice_counts[4] + (unsigned)g_c.advice_counts[5]);
    printf("  monitor: %u verdict transitions over %u polls\n",
           (unsigned)g_c.monitor_changes, (unsigned)g_c.monitor_polls);
    printf("  payload integrity: %u checks, %u failures\n",
           (unsigned)g_c.integrity_checks, (unsigned)g_c.integrity_failures);
    uint32_t const capacity = (uint32_t)s.segment_count * SEGMENT;
    printf("  accounting: used %u + free %u = %u (capacity %u) %s\n",
           (unsigned)s.used_bytes, (unsigned)s.free_bytes,
           (unsigned)(s.used_bytes + s.free_bytes), (unsigned)capacity,
           (s.used_bytes + s.free_bytes == capacity) ? "OK" : "MISMATCH");
    printf("  structure audit : %s\n", structure_ok ? "OK" : "FAILED");
    printf("  live objects %u, borrow count %u, epoch %u\n",
           (unsigned)s.object_count, (unsigned)s.borrow_count,
           (unsigned)s.structure_epoch);

    // ---- teardown: history, DMA ring, then the instance ---------------------
    while (g_hist_n > 0u) drop_history(g_hist_n - 1u);
    for (uint32_t i = 0; i < DMA_SLOTS; ++i) (void)pm::pm_destroy(g_dma[i]);
    pm::Status const dst = pm::deinit();
    printf("  deinit: %s\n", pm::status_name(dst));

    bool const ok = audits_ok && structure_ok &&
                    g_c.integrity_failures == 0u &&
                    (s.used_bytes + s.free_bytes == capacity) &&
                    (dst == pm::Status::Ok);
    printf("=== sensor pipeline done: %s ===\n", ok ? "ALL AUDITS PASSED"
                                                             : "AUDITS FAILED");
    return ok ? 0 : 1;
}
