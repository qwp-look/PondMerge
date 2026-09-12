// PondMerge libFuzzer target -- structure-aware random operation sequences.
//
// WHAT IT IS FOR
// The acceptance suite is a curated set of scenarios with hand-written
// expectations. This target is the complement: it drives the PUBLIC API with
// arbitrary byte-derived sequences and looks for the three things a curated
// suite cannot reasonably cover -- crashes, hangs, and undefined behaviour
// (the last one via ASan + UBSan, which is why this is built with sanitizers
// rather than with the project's usual flags).
//
// WHAT IT DELIBERATELY DOES NOT DO
//   * It does not assert on returned statuses. The fuzzer's contract is "the
//     library must survive this input", not "this input should return OK".
//     Asserting expectations here would just duplicate the suite and would make
//     a legitimate refusal look like a bug.
//   * It does not run with PM_DEBUG=1. In Debug, a caller bug such as a
//     duplicate borrow_end aborts by design, and libFuzzer would report that
//     abort as a crash. Debug-mode aborts are the suite's business (R25 covers
//     the token discipline on a Release build); here we want the Release
//     semantics, where such a call is ignored instead of fatal. Build this
//     target with -DPM_DEBUG=0.
//
// HOW IT WORKS
// Each input byte stream is replayed as a sequence of operations against a
// freshly initialised instance, so every input is independent and a failure
// reproduces from that input alone. Two pools are created so that merge and
// split are reachable. A structure-aware encoding is used -- operation, then
// its arguments -- rather than consuming the bytes as an opaque blob, because
// the interesting states (a pinned fence, a borrowed object, a pool at the
// edge of the segment count) are only reachable through well-formed calls.

#include "pondmerge/pondmerge.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t ZONE_BYTES = 64u * 1024u;
constexpr uint32_t SEGMENT    = 4096u; // 16 segments per pool
constexpr uint32_t SEGS_PER_POOL = 8;
constexpr uint32_t MAX_TRACK  = 48;
constexpr uint32_t MAX_STEPS  = 400;
// See the comment in LLVMFuzzerTestOneInput: the opening steps are forced to be
// allocations so that every input starts from a pool with something in it.
constexpr uint32_t kOpeningAllocs = 8;

// Static storage: the fuzzer's own frames are tiny but the zone is not.
alignas(16) uint8_t g_zone[ZONE_BYTES];

pm::RawRef g_ref[MAX_TRACK];
uint32_t   g_track = 0;

// Bounds a length that came from a single byte.
inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

class Reader {
public:
    Reader(uint8_t const* d, size_t n) : d_(d), n_(n) {}
    bool done() const { return i_ >= n_; }
    uint8_t u8() { return i_ < n_ ? d_[i_++] : 0; }
    uint32_t u32() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v = (v << 8) | u8();
        return v;
    }
    // A byte already consumed, reused as a value.
    static uint32_t from_byte(uint8_t b, uint32_t lo, uint32_t hi) {
        return lo + (uint32_t)b % (hi - lo + 1);
    }

private:
    uint8_t const* d_;
    size_t n_;
    size_t i_ = 0;
};

void drop_all() {
    for (uint32_t i = 0; i < g_track; ++i) (void)pm::free(g_ref[i]);
    g_track = 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Every input starts from a clean, freshly initialised instance.
    drop_all();
    (void)pm::deinit();

    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) return 0;

    pm::PoolId p[2]{};
    if (pm::create_pool(p[0], SEGS_PER_POOL) != pm::Status::Ok) return 0;
    if (pm::create_pool(p[1], SEGS_PER_POOL) != pm::Status::Ok) return 0;

    Reader r(data, size);
    for (uint32_t step = 0; step < MAX_STEPS && !r.done(); ++step) {
        uint8_t const raw = r.u8();
        // Bias the opening steps towards allocation. Edge coverage alone does
        // not reward reaching the interesting states, so an unguided run spends
        // most of its budget on pools holding one object -- where compaction,
        // merge and split have nothing to do. Forcing a small population first
        // makes every input start from a state worth exercising.
        uint32_t op = raw % 14;
        if (g_track < kOpeningAllocs && op != 0 && op != 1) op = raw & 1u;
        switch (op) {
        case 0: // alloc
        case 1: {
            uint8_t const b = r.u8();
            uint32_t const size = Reader::from_byte(b, 1, 4096);
            uint32_t const align = 1u << (raw % 4); // 1, 2, 4, 8 -- and 8 invalid? all <= 8
            uint16_t flags = 0;
            uint8_t const f = r.u8();
            if (f & 1) flags |= pm::PM_MOVABLE;
            if (f & 2) flags |= pm::PM_PINNED;
            if (f & 4) flags |= pm::PM_DMA;
            if (f & 8) flags |= pm::PM_EXTERNAL;
            if (f & 16) flags |= pm::PM_ZERO_INIT;
            pm::RawRef ref{};
            pm::Status const st =
                pm::alloc(p[raw % 2], size, align, flags, step, ref);
            if (st == pm::Status::Ok && g_track < MAX_TRACK)
                g_ref[g_track++] = ref;
            break;
        }
        case 2: // free a tracked object
            if (g_track) {
                uint32_t const i = Reader::from_byte(r.u8(), 0, g_track - 1);
                (void)pm::free(g_ref[i]);
                g_ref[i] = g_ref[--g_track]; // swap-remove (order is irrelevant)
            }
            break;
        case 3: // borrow / resolve / peek round trip
            if (g_track) {
                uint32_t const i = Reader::from_byte(r.u8(), 0, g_track - 1);
                void* addr = nullptr;
                if (pm::borrow_begin(g_ref[i], 8, 8, addr) == pm::Status::Ok)
                    pm::borrow_end(g_ref[i]);
                (void)pm::resolve(g_ref[i], 8, 8, addr);
                (void)pm::get_stats(p[r.u8() % 2]);
            }
            break;
        case 4: // compact
            (void)pm::compact(p[r.u8() % 2]);
            break;
        case 5: // merge, both directions
            (void)pm::merge(p[r.u8() % 2], p[(r.u8() + 1) % 2]);
            break;
        case 6: // split
        {
            uint32_t const segs = Reader::from_byte(r.u8(), 1, SEGS_PER_POOL);
            pm::PoolId out{};
            (void)pm::split(p[r.u8() % 2], segs, out);
            break;
        }
        case 7: // pause / resume
            (void)pm::pause(p[r.u8() % 2]);
            (void)pm::resume(p[r.u8() % 2]);
            break;
        case 8: // structure audit
            (void)pm::validate(p[r.u8() % 2]);
            break;
        case 9: // advice
        {
            pm::CompactionRequest req{Reader::from_byte(r.u8(), 0, 8192),
                                      (r.u8() % 2) ? 8u : 0u, 0, 0};
            bool changed = false;
            (void)pm::poll_compaction_advice(p[r.u8() % 2], &req, &changed);
            break;
        }
        case 10: // destroy a pool
            (void)pm::destroy_pool(p[r.u8() % 2]);
            break;
        case 11: // statistics / globals
            (void)pm::global_stats();
            (void)pm::get_stats(p[r.u8() % 2]);
            (void)pm::get_compaction_thresholds();
            break;
        case 12: // sub-object view via at(): must never be freeable
            if (g_track) {
                uint32_t const i = Reader::from_byte(r.u8(), 0, g_track - 1);
                pm::RawRef sub = g_ref[i];
                sub.offset = r.u32();
                (void)pm::free(sub); // must be refused (offset != 0)
                void* addr = nullptr;
                (void)pm::borrow_begin(sub, 4, 4, addr);
                if (addr) pm::borrow_end(sub);
            }
            break;
        default: // cross-pool reference handling
            if (g_track) {
                uint32_t const i = Reader::from_byte(r.u8(), 0, g_track - 1);
                pm::RawRef x = g_ref[i];
                x.pool_hint = pm::CROSS_HINT;
                void* addr = nullptr;
                if (pm::borrow_begin(x, 4, 4, addr) == pm::Status::Ok)
                    pm::borrow_end(x);
            }
            break;
        }
    }

    // Final audits: they must terminate and must not be confused by whatever
    // the sequence produced.
    (void)pm::validate(p[0]);
    (void)pm::validate(p[1]);
    (void)pm::global_stats();
    drop_all();
    return 0;
}
