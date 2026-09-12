// PondMerge v1 - reference-model differential test (task-book v2 section 11.2).
//
// The model deliberately shares NO data structures with PondMerge and never
// predicts physical placement (that would mean reimplementing the allocator
// and would only duplicate its bugs). It is an independent oracle for every
// observable the task book lists:
//
//   * the number of live objects, globally and per pool;
//   * each object's payload, byte for byte, before every free;
//   * each object's pool membership -- established through the PUBLIC API
//     only: a local reference bound to pool P resolves if and only if the
//     object really lives in P (anything else reports PoolChanged);
//   * allocatability, predicted from get_stats().largest_free_block and
//     compared with the allocator's own verdict;
//   * the byte accounting used_bytes + free_bytes == capacity per pool.
//
// Physical layout invariants (no hole, no overlap, block headers agreeing with
// descriptors) are audited separately by pm::validate(), which recomputes the
// layout from descriptors plus block headers instead of trusting statistics.
//
// The PRNG is a fixed-seed xorshift; on failure the seed, the operation index
// and the recent operation trace are printed so the run is reproducible.
#include "pondmerge/pondmerge.hpp"

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t SEED_LIVE   = 0x51ED2701u; // fixed: failures must reproduce
constexpr uint32_t MAX_OBJS    = 64;
constexpr uint32_t MAX_POOLS   = 16; // == PM_MAX_POOLS
constexpr uint32_t TRACE_DEPTH = 16;
constexpr uint32_t SEGMENT     = 4096;

uint32_t g_checks = 0;
uint32_t g_fails = 0;
uint32_t g_seq = 0; // operation index, for diagnostics
const char* g_phase = "init";
uint32_t g_trace[TRACE_DEPTH] = {};

void trace_op(uint32_t op, uint32_t arg) {
    g_trace[g_seq % TRACE_DEPTH] = (op << 24) | (arg & 0x00FFFFFFu);
}

void model_check(bool cond, const char* what) {
    ++g_checks;
    if (cond) return;
    ++g_fails;
    printf("    MODEL CHECK failed [%s] op=%u: %s\n", g_phase, (unsigned)g_seq, what);
    printf("    seed=0x%08X trace(tail)= ", (unsigned)SEED_LIVE);
    for (uint32_t i = 0; i < TRACE_DEPTH; ++i)
        printf("%08X ", (unsigned)g_trace[(g_seq + i + 1) % TRACE_DEPTH]);
    printf("\n");
}

uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Block requirement for a payload, exactly as the header documents it.
uint32_t need_of(uint32_t size) {
    uint32_t payload = (size + (PM_ALIGNMENT - 1)) & ~(PM_ALIGNMENT - 1);
    uint32_t need = payload + 8u; // BLOCK_HEADER_SIZE
    if (need < PM_MIN_BLOCK) need = PM_MIN_BLOCK;
    return need;
}

uint8_t pat(uint32_t seed, uint32_t i) {
    uint32_t x = seed * 2654435761u + i * 40503u;
    x ^= x >> 13;
    x = x * 1274126177u;
    return (uint8_t)(x >> 24);
}

struct Obj {
    pm::RawRef ref;
    uint32_t size;
    uint32_t seed;
    uint32_t pool_mask; // bit p set: pool index p is an acceptable home
    bool live;
};

struct ModelPool {
    pm::PoolId id;
    bool live;
};

struct Model {
    Obj objs[MAX_OBJS];
    uint32_t nlive;
    ModelPool pools[MAX_POOLS];
    uint32_t npools;
    uint32_t zone_bytes;

    void reset(uint32_t zone) {
        memset(this, 0, sizeof(*this));
        zone_bytes = zone;
    }
    void add_pool(pm::PoolId id) {
        for (uint32_t i = 0; i < npools; ++i)
            if (pools[i].id == id) { pools[i].live = true; return; }
        pools[npools].id = id;
        pools[npools].live = true;
        ++npools;
    }
    void drop_pool(pm::PoolId id) {
        for (uint32_t i = 0; i < npools; ++i)
            if (pools[i].id == id) pools[i].live = false;
    }
    // Pool index in the mask space: the slot position of `id`.
    uint32_t slot_of(pm::PoolId id) const {
        for (uint32_t i = 0; i < npools; ++i)
            if (pools[i].id == id) return i;
        return MAX_POOLS;
    }
    uint32_t mask_of(pm::PoolId id) const {
        uint32_t s = slot_of(id);
        return s == MAX_POOLS ? 0u : (1u << s);
    }
    // After merge(src -> dst) every object that could have been in either pool
    // may now only be in dst.
    void fold_merge(pm::PoolId src, pm::PoolId dst) {
        uint32_t m = mask_of(src) | mask_of(dst);
        for (uint32_t i = 0; i < nlive; ++i)
            if (objs[i].pool_mask & m) objs[i].pool_mask = mask_of(dst);
        drop_pool(src);
    }
    // After split(s -> s + n) objects that could have been in s may now be in
    // either half.
    void fold_split(pm::PoolId s, pm::PoolId n) {
        add_pool(n);
        uint32_t m = mask_of(s) | mask_of(n);
        for (uint32_t i = 0; i < nlive; ++i)
            if (objs[i].pool_mask & mask_of(s)) objs[i].pool_mask = m;
    }
};

// Public-API pool interrogation: a local reference bound to P resolves only
// while the object actually lives in P. Returns the PoolId, NO_POOL on failure
// to find one, or AMBIGUOUS when two pools both claim the object.
constexpr pm::PoolId NO_POOL = 0xFFFF;
constexpr pm::PoolId AMBIGUOUS = 0xFFFE;

pm::PoolId probe_pool(pm::RawRef r, uint32_t size, const Model& M, pm::Status& bad) {
    pm::PoolId found = NO_POOL;
    bad = pm::Status::Ok;
    for (uint32_t i = 0; i < M.npools; ++i) {
        if (!M.pools[i].live) continue;
        pm::RawRef q = r;
        q.pool_hint = M.pools[i].id;
        void* p = nullptr;
        pm::Status st = pm::borrow_begin(q, size, 1, p);
        if (st == pm::Status::Ok) {
            pm::borrow_end(q);
            if (found != NO_POOL) return AMBIGUOUS;
            found = M.pools[i].id;
        } else if (st != pm::Status::PoolChanged) {
            bad = st; // CorruptMetadata / InvalidRef: not a mere pool change
            return NO_POOL;
        }
    }
    return found;
}

// Payload access must go through a CROSS ref: after a merge or a split the
// owning pool is not the one the object was allocated in, and a local ref
// would (correctly) report PoolChanged instead of reading the bytes.
bool verify_payload(pm::RawRef r, uint32_t size, uint32_t seed) {
    pm::RawRef q = r;
    q.pool_hint = pm::CROSS_HINT;
    void* p = nullptr;
    pm::Status st = pm::borrow_begin(q, size, 1, p);
    if (st != pm::Status::Ok) return false;
    uint8_t const* bytes = static_cast<uint8_t const*>(p);
    bool ok = true;
    for (uint32_t i = 0; i < size; ++i)
        if (bytes[i] != pat(seed, i)) { ok = false; break; }
    pm::borrow_end(q);
    return ok;
}

void write_payload(pm::RawRef r, uint32_t size, uint32_t seed) {
    pm::RawRef q = r;
    q.pool_hint = pm::CROSS_HINT;
    void* p = nullptr;
    if (pm::borrow_begin(q, size, 1, p) != pm::Status::Ok) return;
    uint8_t* bytes = static_cast<uint8_t*>(p);
    for (uint32_t i = 0; i < size; ++i) bytes[i] = pat(seed, i);
    pm::borrow_end(q);
}

// Global invariants, checked after every operation.
void check_globals(const Model& M, const pm::PoolId* ids, uint32_t n) {
    g_phase = "globals";
    uint32_t per_pool_objects = 0;
    for (uint32_t i = 0; i < n; ++i) {
        pm::PoolStats s = pm::get_stats(ids[i]);
        model_check(s.state == 1 || s.state == 2, "live pool must be Running or Paused");
        model_check((uint64_t)s.used_bytes + s.free_bytes ==
                        (uint64_t)s.segment_count * SEGMENT,
                    "used + free must equal the pool capacity");
        per_pool_objects += s.object_count;
    }
    model_check(per_pool_objects == M.nlive, "per-pool object counts must sum to the model");

    for (uint32_t i = 0; i < M.nlive; ++i) {
        const Obj& o = M.objs[i];
        pm::Status bad = pm::Status::Ok;
        pm::PoolId actual = probe_pool(o.ref, o.size, M, bad);
        model_check(bad == pm::Status::Ok, "object probe must not report corrupt metadata");
        model_check(actual != AMBIGUOUS, "object must live in exactly one pool");
        model_check(actual != NO_POOL, "live object must be reachable in some pool");
        if (actual != NO_POOL && actual != AMBIGUOUS) {
            uint32_t slot = M.slot_of(actual);
            model_check(slot < MAX_POOLS && (o.pool_mask & (1u << slot)) != 0,
                        "object landed in a pool the operation could not have chosen");
        }
    }
}

bool adjacent(pm::PoolStats const& a, pm::PoolStats const& b) {
    return (uint32_t)a.segment_first + a.segment_count == b.segment_first ||
           (uint32_t)b.segment_first + b.segment_count == a.segment_first;
}

} // namespace

// Runs the differential test. Returns the number of failed checks.
uint32_t pondmerge_run_model(uint32_t zone_bytes, uint32_t ops) {
#ifdef PM_DEVICE_BUILD
    // Device build (round-5 guide section 7): static DRAM is too tight for a
    // second 64 KiB zone, so the model reuses the suite's zone buffer -- it
    // runs strictly AFTER pondmerge_run_tests(), which has deinited.
    extern uint8_t g_zone[]; // tests/suite.cpp, 256 KiB, 16-byte aligned
    uint8_t* zone = g_zone;
    if (zone_bytes > 256u * 1024u) zone_bytes = 256u * 1024u;
#else
    static uint8_t zone[64 * 1024] __attribute__((aligned(16)));
    if (zone_bytes > sizeof(zone)) zone_bytes = sizeof(zone);
#endif

    pm::Config cfg{zone, zone_bytes, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) {
        printf("  [model] init failed\n");
        return 1;
    }
    model_check(pm::get_stats(0).state == 0, "pool 0 must start Empty");

    Model M;
    M.reset(zone_bytes);
    pm::PoolId base{};
    if (pm::create_pool(base, 2) != pm::Status::Ok) {
        printf("  [model] create_pool failed\n");
        return 1;
    }
    M.add_pool(base);

    uint32_t rng = SEED_LIVE;

    for (g_seq = 0; g_seq < ops; ++g_seq) {
        uint32_t r = xorshift(rng);
        uint32_t choice = (r >> 8) % 10u;

        // Snapshot of the live pools, used by every operation and by the
        // global check at the end.
        pm::PoolId ids[MAX_POOLS];
        pm::PoolStats stats[MAX_POOLS];
        uint32_t n = 0;
        for (uint32_t i = 0; i < M.npools && n < MAX_POOLS; ++i) {
            if (!M.pools[i].live) continue;
            ids[n] = M.pools[i].id;
            stats[n] = pm::get_stats(ids[n]);
            ++n;
        }
        if (n == 0) break;

        if (choice < 4 && M.nlive < MAX_OBJS) { // ---- alloc
            g_phase = "alloc";
            trace_op(1, M.nlive);
            uint32_t pi = xorshift(rng) % n;
            uint32_t size = 1 + (xorshift(rng) % 700);
            uint32_t need = need_of(size);
            pm::PoolId pid = ids[pi];
            // Allocatability oracle: a free block at least `need` bytes large
            // exists <=> the allocation must succeed.
            bool predicted =
                stats[pi].largest_free_block >= need && need < (1u << PM_FL_MAX);
            pm::RawRef ref{};
            pm::Status st = pm::alloc(pid, size, 8, 0, M.nlive, ref);
            model_check(predicted == (st == pm::Status::Ok),
                        "alloc verdict must follow largest_free_block");
            if (st == pm::Status::Ok) {
                uint32_t seed = (ref.index << 8) | (M.nlive & 0xFF);
                seed = seed * 2654435761u;
                write_payload(ref, size, seed);
                M.objs[M.nlive].ref = ref;
                M.objs[M.nlive].size = size;
                M.objs[M.nlive].seed = seed;
                M.objs[M.nlive].pool_mask = M.mask_of(pid);
                M.objs[M.nlive].live = true;
                ++M.nlive;
            }
        } else if (choice < 6 && M.nlive > 0) { // ---- free
            g_phase = "free";
            uint32_t oi = xorshift(rng) % M.nlive;
            trace_op(2, oi);
            Obj& o = M.objs[oi];
            model_check(verify_payload(o.ref, o.size, o.seed),
                        "payload must be intact before free");
            pm::RawRef cross = o.ref;
            cross.pool_hint = pm::CROSS_HINT;
            model_check(pm::free(cross) == pm::Status::Ok, "free must succeed");
            o = M.objs[--M.nlive];
        } else if (choice < 7) { // ---- compact
            g_phase = "compact";
            uint32_t pi = xorshift(rng) % n;
            trace_op(3, pi);
            pm::Status st = pm::compact(ids[pi]);
            model_check(st == pm::Status::Ok, "compact must succeed when quiescent");
        } else if (choice < 8) { // ---- merge two adjacent pools
            g_phase = "merge";
            uint32_t a = 0, b = 0;
            bool found = false;
            for (uint32_t i = 0; i < n && !found; ++i)
                for (uint32_t j = 0; j < n && !found; ++j)
                    if (i != j && adjacent(stats[i], stats[j])) { a = i; b = j; found = true; }
            if (found) {
                trace_op(4, (ids[a] << 8) | ids[b]);
                pm::Status st = pm::merge(ids[a], ids[b]);
                model_check(st == pm::Status::Ok, "merge of adjacent pools must succeed");
                if (st == pm::Status::Ok) {
                    M.fold_merge(ids[a], ids[b]);
                    model_check(pm::get_stats(ids[a]).state == 0,
                                "merge source must become Empty");
                    model_check(pm::get_stats(ids[b]).object_count ==
                                    stats[a].object_count + stats[b].object_count,
                                "merge target must own both object sets");
                }
            }
        } else if (choice < 9) { // ---- split
            g_phase = "split";
            uint32_t pi = xorshift(rng) % n;
            if (stats[pi].segment_count >= 2) {
                uint32_t keep_new = 1 + (xorshift(rng) % (stats[pi].segment_count - 1));
                trace_op(5, (ids[pi] << 8) | keep_new);
                pm::PoolId new_id = NO_POOL;
                pm::Status st = pm::split(ids[pi], keep_new, new_id);
                // NoSpace is a legitimate verdict: a side may be unable to hold
                // its objects (the model deliberately does not predict layout).
                // What must hold is that a refusal changes NOTHING.
                model_check(st == pm::Status::Ok || st == pm::Status::NoSpace,
                            "split must report Ok or NoSpace");
                if (st == pm::Status::NoSpace) {
                    pm::PoolStats s2 = pm::get_stats(ids[pi]);
                    model_check(s2.state == 1, "refused split must restore Running");
                    model_check(s2.object_count == stats[pi].object_count,
                                "refused split must not move objects");
                    model_check(s2.segment_count == stats[pi].segment_count,
                                "refused split must not resize the source");
                }
                if (st == pm::Status::Ok) {
                    M.fold_split(ids[pi], new_id);
                    pm::PoolStats sn = pm::get_stats(new_id);
                    model_check(sn.state == 1, "new pool must be Running");
                    model_check(pm::get_stats(ids[pi]).object_count + sn.object_count ==
                                    stats[pi].object_count,
                                "split must partition the object set");
                    model_check(pm::get_stats(ids[pi]).segment_count + sn.segment_count ==
                                    stats[pi].segment_count,
                                "split must partition the segment range");
                    model_check(new_id < PM_MAX_POOLS && new_id != ids[pi],
                                "split must hand back a fresh pool id");
                }
            }
        }

        // ---- global invariants + structural audit
        {
            pm::PoolId cur_ids[MAX_POOLS] = {};
            uint32_t cn = 0;
            for (uint32_t i = 0; i < M.npools && cn < MAX_POOLS; ++i) {
                if (!M.pools[i].live) continue;
                cur_ids[cn] = M.pools[i].id;
                ++cn;
            }
            check_globals(M, cur_ids, cn);
            if ((g_seq % 16u) == 15u) {
                g_phase = "validate";
                for (uint32_t i = 0; i < cn; ++i)
                    model_check(pm::validate(cur_ids[i]) == pm::Status::Ok,
                                "validate must accept the pool after every operation");
            }
        }
    }

    // ---- teardown: every payload must still be readable, then drain ----
    g_phase = "teardown";
    for (uint32_t i = 0; i < M.nlive; ++i) {
        Obj const& o = M.objs[i];
        model_check(verify_payload(o.ref, o.size, o.seed), "payload must survive the run");
        pm::RawRef cross = o.ref;
        cross.pool_hint = pm::CROSS_HINT;
        model_check(pm::free(cross) == pm::Status::Ok, "teardown free must succeed");
    }
    for (uint32_t i = 0; i < M.npools; ++i) {
        if (!M.pools[i].live) continue;
        model_check(pm::validate(M.pools[i].id) == pm::Status::Ok,
                    "drained pool must validate");
        model_check(pm::destroy_pool(M.pools[i].id) == pm::Status::Ok,
                    "drained pool must be destroyable");
    }
    if (pm::deinit() != pm::Status::Ok) model_check(false, "deinit must succeed");

    printf("  [model] %u checks, %u failures\n", (unsigned)g_checks, (unsigned)g_fails);
    return g_fails;
}
