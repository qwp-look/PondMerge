// PondMerge v1 - host acceptance tests (doc section 18).
// Group numbers in the [n] tags map to the numbered requirements in the task
// book. Build/run: tests/run_host.sh
#include "pondmerge/pondmerge.hpp"
#include "../src/internal.h" // white-box: stats flags + corruption injection

#include <cstdio>
#include <cstring>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Framework
// ---------------------------------------------------------------------------
static uint32_t g_checks = 0;
static uint32_t g_fails = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            printf("    CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_fails;                                                                 \
        }                                                                              \
    } while (0)

#define CHECK_ST(expr, want)                                                           \
    do {                                                                               \
        pm::Status _s = (expr);                                                        \
        ++g_checks;                                                                    \
        if (_s != (want)) {                                                            \
            printf("    CHECK failed %s:%d: %s -> %s (want %s)\n", __FILE__,           \
                   __LINE__, #expr, pm::status_name(_s), pm::status_name(want));       \
            ++g_fails;                                                                 \
        }                                                                              \
    } while (0)

static uint8_t g_zone[256 * 1024] __attribute__((aligned(16)));

static void fresh() {
    pm::Config cfg{g_zone, sizeof(g_zone), 4096};
    pm::Status s = pm::init(cfg);
    if (s != pm::Status::Ok) {
        printf("    init failed: %s\n", pm::status_name(s));
        exit(2);
    }
}

static void done() { CHECK_ST(pm::deinit(), pm::Status::Ok); }
static void VALIDATE(pm::PoolId id) { CHECK_ST(pm::validate(id), pm::Status::Ok); }

static uint8_t pat(uint32_t seed, uint32_t i) {
    uint32_t x = seed * 2654435761u + i * 40503u;
    x ^= x >> 13;
    x = x * 1274126177u;
    return (uint8_t)(x >> 24);
}
static void fill(pm::RawRef ref, uint32_t size, uint32_t seed) {
    void* p = nullptr;
    pm::Status s = pm::borrow_begin(ref, size, 1, p);
    if (s != pm::Status::Ok) {
        CHECK_ST(s, pm::Status::Ok);
        return;
    }
    for (uint32_t i = 0; i < size; ++i) ((uint8_t*)p)[i] = pat(seed, i);
    pm::borrow_end(ref);
}
static void verify(pm::RawRef ref, uint32_t size, uint32_t seed) {
    void* p = nullptr;
    pm::Status s = pm::borrow_begin(ref, size, 1, p);
    if (s != pm::Status::Ok) {
        CHECK_ST(s, pm::Status::Ok);
        return;
    }
    for (uint32_t i = 0; i < size; ++i) {
        ++g_checks;
        if (((uint8_t*)p)[i] != pat(seed, i)) {
            printf("    payload mismatch %s:%d (ref %u byte %u)\n", __FILE__, __LINE__,
                   (unsigned)ref.index, (unsigned)i);
            ++g_fails;
            break;
        }
    }
    pm::borrow_end(ref);
}

static uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static uint8_t get_stats_state(pm::PoolId id) { return pm::get_stats(id).state; }
static uint16_t desc_pool(pm::RawRef r) { return pm::internal::g().objects[r.index].pool_id; }

// ---------------------------------------------------------------------------
// 1. 连续分配、释放、重复利用
// ---------------------------------------------------------------------------
static void test_basic_alloc_free() {
    printf("  [1] basic alloc/free/reuse\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
    VALIDATE(pool);

    pm::RawRef a{}, b{}, c{};
    CHECK_ST(pm::alloc(pool, 100, 8, 0, 1, a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 200, 8, 0, 2, b), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 50, 8, 0, 3, c), pm::Status::Ok);
    CHECK(a.generation != 0 && b.generation != 0 && c.generation != 0);
    VALIDATE(pool);

    void* pa = nullptr;
    CHECK_ST(pm::borrow_begin(a, 100, 1, pa), pm::Status::Ok);
    memset(pa, 0xAB, 100);
    pm::borrow_end(a);

    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.object_count == 3);
    CHECK(st.used_bytes == 112 + 208 + 64); // block sizes incl. 8B headers
    CHECK(st.free_bytes == 4 * 4096 - (112 + 208 + 64));

    // Same-size alloc right after a free reuses the slot (LIFO free list),
    // with a bumped generation.
    pm::RawRef old_c = c;
    CHECK_ST(pm::free(c), pm::Status::Ok);
    pm::RawRef d{};
    CHECK_ST(pm::alloc(pool, 50, 8, 0, 4, d), pm::Status::Ok);
    CHECK(d.index == old_c.index);
    CHECK(d.generation != old_c.generation);
    VALIDATE(pool);

    // Freeing everything coalesces back into one pool-spanning block.
    CHECK_ST(pm::free(a), pm::Status::Ok);
    CHECK_ST(pm::free(b), pm::Status::Ok);
    CHECK_ST(pm::free(d), pm::Status::Ok);
    st = pm::get_stats(pool);
    CHECK(st.object_count == 0);
    CHECK(st.used_bytes == 0);
    CHECK(st.largest_free_block == 4 * 4096);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// 2. 随机分配/释放 10000 次后执行整理
// ---------------------------------------------------------------------------
static void test_stress_random(uint32_t ops) {
    printf("  [2] random stress (%u ops)\n", (unsigned)ops);
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 32), pm::Status::Ok);

    struct Live {
        pm::RawRef ref;
        uint32_t size;
        uint32_t seed;
    };
    static Live live[160];
    uint32_t nlive = 0;
    uint32_t seed = 12345;

    for (uint32_t op = 0; op < ops; ++op) {
        uint32_t r = xorshift(seed);
        if ((r & 1) == 0 || nlive == 0) {
            if (nlive >= 160) continue;
            uint32_t size = 1 + (xorshift(seed) % 1500);
            pm::RawRef ref{};
            if (pm::alloc(pool, size, 8, 0, op, ref) == pm::Status::Ok) {
                uint32_t s = ref.index * 7 + op;
                fill(ref, size, s);
                live[nlive].ref = ref;
                live[nlive].size = size;
                live[nlive].seed = s;
                ++nlive;
            }
        } else {
            uint32_t i = xorshift(seed) % nlive;
            verify(live[i].ref, live[i].size, live[i].seed);
            CHECK_ST(pm::free(live[i].ref), pm::Status::Ok);
            live[i] = live[nlive - 1];
            --nlive;
        }
        if (op % 400 == 399) {
            CHECK_ST(pm::compact(pool), pm::Status::Ok);
            for (uint32_t i = 0; i < nlive; ++i)
                verify(live[i].ref, live[i].size, live[i].seed);
        }
        if (op % 50 == 49) VALIDATE(pool);
    }
    for (uint32_t i = 0; i < nlive; ++i) {
        verify(live[i].ref, live[i].size, live[i].seed);
        CHECK_ST(pm::free(live[i].ref), pm::Status::Ok);
    }
    VALIDATE(pool);
    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.used_bytes == 0 && st.object_count == 0);
    pm::GlobalStats gs = pm::global_stats();
    printf("    max_live=%u max_borrow=%u max_moved=%u max_compact_us=%llu meta=%u\n",
           (unsigned)gs.max_live_objects, (unsigned)gs.max_borrow_count,
           (unsigned)gs.max_bytes_moved, (unsigned long long)gs.max_compact_time_us,
           (unsigned)gs.metadata_bytes);
    done();
}

// ---------------------------------------------------------------------------
// 3. 整理前后所有 pm_ptr 仍指向同一个逻辑对象
// ---------------------------------------------------------------------------
static void test_refs_stable_across_compact() {
    printf("  [3] refs stable across compaction\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 8), pm::Status::Ok);

    pm::RawRef refs[10];
    uint32_t sizes[10];
    for (uint32_t i = 0; i < 10; ++i) {
        sizes[i] = 100 + i * 64;
        CHECK_ST(pm::alloc(pool, sizes[i], 8, 0, i, refs[i]), pm::Status::Ok);
        fill(refs[i], sizes[i], i * 31 + 7);
    }
    // Fragment: free every other object, realloc each gap half-size.
    for (uint32_t i = 1; i < 10; i += 2) CHECK_ST(pm::free(refs[i]), pm::Status::Ok);
    for (uint32_t i = 1; i < 10; i += 2) {
        CHECK_ST(pm::alloc(pool, sizes[i] / 2, 8, 0, 100 + i, refs[i]), pm::Status::Ok);
        sizes[i] /= 2;
        fill(refs[i], sizes[i], i * 31 + 7);
    }
    VALIDATE(pool);

    CHECK_ST(pm::compact(pool), pm::Status::Ok);
    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.objects_moved > 0); // something actually relocated

    for (uint32_t i = 0; i < 10; ++i) {
        void* p = nullptr;
        CHECK_ST(pm::resolve(refs[i], 0, 1, p), pm::Status::Ok);
        verify(refs[i], sizes[i], i * 31 + 7); // same logical object, same data
    }
    for (uint32_t i = 0; i < 10; ++i) CHECK_ST(pm::free(refs[i]), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// 4. 整理后活跃的 pm_access 使整理返回 BUSY
// ---------------------------------------------------------------------------
static void test_compact_busy_and_pause() {
    printf("  [4] active borrow blocks compaction\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{}, b{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 2, b), pm::Status::Ok);

    void* pa = nullptr;
    CHECK_ST(pm::borrow_begin(a, 64, 1, pa), pm::Status::Ok);

    CHECK_ST(pm::compact(pool), pm::Status::Busy); // active borrow
    // The failed compact left the pool Paused; the paused pool refuses new
    // borrows and frees (doc section 8).
    void* pc = nullptr;
    CHECK_ST(pm::borrow_begin(b, 64, 1, pc), pm::Status::Busy);
    CHECK_ST(pm::free(b), pm::Status::Busy);
    CHECK_ST(pm::resume(pool), pm::Status::Ok);

    CHECK_ST(pm::free(a), pm::Status::Busy); // borrowed object cannot die
    pm::borrow_end(a);
    CHECK_ST(pm::free(a), pm::Status::Ok);
    VALIDATE(pool);

    // Explicit pause/resume round trip.
    pm::RawRef c{};
    CHECK_ST(pm::alloc(pool, 32, 8, 0, 3, c), pm::Status::Ok);
    CHECK_ST(pm::pause(pool), pm::Status::Ok);
    CHECK_ST(pm::pause(pool), pm::Status::AlreadyPaused);
    CHECK_ST(pm::borrow_begin(c, 32, 1, pc), pm::Status::Busy);
    CHECK_ST(pm::resume(pool), pm::Status::Ok);
    CHECK_ST(pm::borrow_begin(c, 32, 1, pc), pm::Status::Ok);
    pm::borrow_end(c);
    CHECK_ST(pm::free(c), pm::Status::Ok);
    CHECK_ST(pm::free(b), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// 5. generation/address_epoch 不匹配、double free、越界 offset 都能被捕获
// ---------------------------------------------------------------------------
static void test_invalid_refs() {
    printf("  [5] stale refs, double free, bounds\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);

    // Stale generation + double free.
    pm::RawRef a{}, b{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, a), pm::Status::Ok);
    pm::RawRef stale = a;
    CHECK_ST(pm::free(a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 2, b), pm::Status::Ok); // reuses the slot
    CHECK(b.index == stale.index && b.generation != stale.generation);
    void* p = nullptr;
    CHECK_ST(pm::borrow_begin(stale, 64, 1, p), pm::Status::InvalidRef);
    CHECK_ST(pm::free(stale), pm::Status::InvalidRef);
    CHECK_ST(pm::resolve(stale, 64, 1, p), pm::Status::InvalidRef);

    // Out-of-bounds offset/size, misaligned sub-object access.
    pm::RawRef o = b;
    o.offset = 64;
    CHECK_ST(pm::borrow_begin(o, 1, 1, p), pm::Status::InvalidRef);
    o.offset = 63;
    CHECK_ST(pm::borrow_begin(o, 2, 1, p), pm::Status::InvalidRef);
    o.offset = 60;
    CHECK_ST(pm::borrow_begin(o, 4, 1, p), pm::Status::Ok); // exact fit
    pm::borrow_end(o);
    o.offset = 61; // in bounds (61+3=64) but misaligned for a 4-byte access
    CHECK_ST(pm::borrow_begin(o, 3, 4, p), pm::Status::InvalidAlignment);

    // Pool-hint violations: a local ref refuses to follow a pool change.
    pm::PoolId other{};
    CHECK_ST(pm::create_pool(other, 2), pm::Status::Ok);
    pm::RawRef m{};
    CHECK_ST(pm::alloc(pool, 32, 8, 0, 5, m), pm::Status::Ok);
    pm::RawRef fake = m;
    fake.pool_hint = other;
    CHECK_ST(pm::borrow_begin(fake, 32, 1, p), pm::Status::PoolChanged);
    CHECK_ST(pm::free(fake), pm::Status::PoolChanged);
    pm::RawRef cross = m;
    cross.pool_hint = pm::CROSS_HINT;
    CHECK_ST(pm::borrow_begin(cross, 32, 1, p), pm::Status::Ok);
    pm::borrow_end(cross);
    CHECK_ST(pm::free(cross), pm::Status::Ok);

    // Request-side validation.
    pm::RawRef r{};
    CHECK_ST(pm::alloc(pool, 0, 8, 0, 6, r), pm::Status::InvalidRef);
    CHECK_ST(pm::alloc(pool, 32, 16, 0, 6, r), pm::Status::InvalidAlignment);
    CHECK_ST(pm::alloc(pool, 32, 3, 0, 6, r), pm::Status::InvalidAlignment);
    CHECK_ST(pm::alloc(pool, 32, 8, 0xFF00, 6, r), pm::Status::InvalidRef);
    pm::RawRef bad{};
    CHECK_ST(pm::borrow_begin(bad, 8, 1, p), pm::Status::InvalidRef);
    CHECK_ST(pm::free(b), pm::Status::Ok);
    VALIDATE(pool);
    VALIDATE(other);
    done();
}

// ---------------------------------------------------------------------------
// 6. pinned 对象前后都有碎片时，压缩不会越过 pinned 地址
// ---------------------------------------------------------------------------
static void test_pinned_barriers() {
    printf("  [6] pinned barriers\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 8), pm::Status::Ok);

    pm::RawRef m[6], pin{};
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK_ST(pm::alloc(pool, 128, 8, 0, i, m[i]), pm::Status::Ok);
        fill(m[i], 128, 500 + i);
    }
    CHECK_ST(pm::alloc(pool, 256, 8, pm::PM_PINNED, 99, pin), pm::Status::Ok);
    fill(pin, 256, 999);
    for (uint32_t i = 3; i < 6; ++i) {
        CHECK_ST(pm::alloc(pool, 128, 8, 0, i, m[i]), pm::Status::Ok);
        fill(m[i], 128, 500 + i);
    }
    // Fragments on both sides of the pinned object.
    CHECK_ST(pm::free(m[1]), pm::Status::Ok);
    CHECK_ST(pm::free(m[4]), pm::Status::Ok);

    void* before = nullptr;
    CHECK_ST(pm::resolve(pin, 0, 1, before), pm::Status::Ok);

    CHECK_ST(pm::compact(pool), pm::Status::Ok);

    void* after = nullptr;
    CHECK_ST(pm::resolve(pin, 0, 1, (void*&)after), pm::Status::Ok);
    CHECK(before == after); // pinned object never moves
    verify(pin, 256, 999);
    verify(m[0], 128, 500);
    verify(m[2], 128, 502);
    verify(m[3], 128, 503);
    verify(m[5], 128, 505);

    // Packing respects the barrier: m2 stays below the pin, m3/m5 above it.
    uint8_t* pin_addr = (uint8_t*)after;
    uint8_t* m2_addr = nullptr;
    CHECK_ST(pm::resolve(m[2], 0, 1, (void*&)m2_addr), pm::Status::Ok);
    CHECK(m2_addr < pin_addr);
    uint8_t* m3_addr = nullptr;
    CHECK_ST(pm::resolve(m[3], 0, 1, (void*&)m3_addr), pm::Status::Ok);
    CHECK(m3_addr > pin_addr);
    uint8_t* m5_addr = nullptr;
    CHECK_ST(pm::resolve(m[5], 0, 1, (void*&)m5_addr), pm::Status::Ok);
    CHECK(m5_addr > pin_addr);
    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.fragment_bytes < 4096);
    VALIDATE(pool);
    CHECK_ST(pm::free(m[0]), pm::Status::Ok);
    CHECK_ST(pm::free(m[2]), pm::Status::Ok);
    CHECK_ST(pm::free(m[3]), pm::Status::Ok);
    CHECK_ST(pm::free(m[5]), pm::Status::Ok);
    CHECK_ST(pm::free(pin), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// 7. DMA、外部对象和固定地址对象不会被搬迁
// ---------------------------------------------------------------------------
static void test_non_relocatable_flags() {
    printf("  [7] DMA/external/pinned objects never move\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);

    pm::RawRef dma{}, ext{}, pinned{}, mov{};
    CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_DMA, 1, dma), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_EXTERNAL, 2, ext), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_PINNED, 3, pinned), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_MOVABLE, 4, mov), pm::Status::Ok);
    {
        using namespace pm::internal;
        CHECK((g().objects[dma.index].flags & pm::PM_PINNED) != 0); // implied
        CHECK((g().objects[ext.index].flags & pm::PM_PINNED) != 0);
    }

    void* a0 = nullptr; CHECK_ST(pm::resolve(dma, 0, 1, a0), pm::Status::Ok);
    void* e0 = nullptr; CHECK_ST(pm::resolve(ext, 0, 1, e0), pm::Status::Ok);
    void* p0 = nullptr; CHECK_ST(pm::resolve(pinned, 0, 1, p0), pm::Status::Ok);

    // Fragment then compact; movables shift, pinned classes stay.
    pm::RawRef pad{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 9, pad), pm::Status::Ok);
    CHECK_ST(pm::free(mov), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 96, 8, 0, 10, mov), pm::Status::Ok);
    CHECK_ST(pm::free(pad), pm::Status::Ok);
    CHECK_ST(pm::compact(pool), pm::Status::Ok);

    void* a1 = nullptr; CHECK_ST(pm::resolve(dma, 0, 1, (void*&)a1), pm::Status::Ok);
    void* e1 = nullptr; CHECK_ST(pm::resolve(ext, 0, 1, (void*&)e1), pm::Status::Ok);
    void* p1 = nullptr; CHECK_ST(pm::resolve(pinned, 0, 1, (void*&)p1), pm::Status::Ok);
    CHECK(a0 == a1);
    CHECK(e0 == e1);
    CHECK(p0 == p1);
    VALIDATE(pool);
    CHECK_ST(pm::free(dma), pm::Status::Ok);
    CHECK_ST(pm::free(ext), pm::Status::Ok);
    CHECK_ST(pm::free(pinned), pm::Status::Ok);
    CHECK_ST(pm::free(mov), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// 8. 池合并后源对象引用仍可解析
// ---------------------------------------------------------------------------
static void test_pool_merge() {
    printf("  [8] pool merge\n");
    fresh();
    pm::PoolId a{}, mid{}, b{};
    CHECK_ST(pm::create_pool(a, 2), pm::Status::Ok);   // segs 0-1
    CHECK_ST(pm::create_pool(mid, 2), pm::Status::Ok); // segs 2-3
    CHECK_ST(pm::create_pool(b, 2), pm::Status::Ok);   // segs 4-5
    CHECK(a == 0 && mid == 1 && b == 2);

    // Non-adjacent merge is refused.
    CHECK_ST(pm::merge(a, b), pm::Status::NoSpace);
    CHECK_ST(pm::merge(b, a), pm::Status::NoSpace);

    pm::RawRef oa{}, ob{};
    CHECK_ST(pm::alloc(a, 300, 8, 0, 1, oa), pm::Status::Ok);
    CHECK_ST(pm::alloc(b, 500, 8, 0, 2, ob), pm::Status::Ok);
    fill(oa, 300, 11);
    fill(ob, 500, 22);
    pm::RawRef cross_a = oa; cross_a.pool_hint = pm::CROSS_HINT;
    pm::RawRef cross_b = ob; cross_b.pool_hint = pm::CROSS_HINT;

    // Make a physically adjacent source: replace mid with a used pool.
    pm::PoolId filler{};
    CHECK_ST(pm::destroy_pool(mid), pm::Status::Ok);
    CHECK_ST(pm::create_pool(filler, 2), pm::Status::Ok); // segs 2-3
    pm::RawRef of{};
    CHECK_ST(pm::alloc(filler, 200, 8, 0, 3, of), pm::Status::Ok);
    fill(of, 200, 33);
    pm::RawRef cross_f = of; cross_f.pool_hint = pm::CROSS_HINT;

    // merge(source=filler segs 2-3, target=a segs 0-1).
    CHECK_ST(pm::merge(filler, a), pm::Status::Ok);
    pm::PoolStats sa = pm::get_stats(a);
    CHECK(sa.segment_count == 4);
    CHECK(sa.object_count == 2);
    CHECK(get_stats_state(filler) == 0); // source became Empty
    VALIDATE(a);
    verify(cross_a, 300, 11);
    verify(cross_f, 200, 33);

    void* p = nullptr;
    // Local ref of the merged-away object now reports PoolChanged; the cross
    // ref follows the object into the target pool.
    CHECK_ST(pm::borrow_begin(of, 200, 1, p), pm::Status::PoolChanged);
    CHECK_ST(pm::borrow_begin(cross_f, 200, 1, p), pm::Status::Ok);
    pm::borrow_end(cross_f);

    // b (segs 4-5) is now adjacent to a (segs 0-3).
    CHECK_ST(pm::merge(b, a), pm::Status::Ok);
    sa = pm::get_stats(a);
    CHECK(sa.segment_count == 6);
    CHECK(sa.object_count == 3);
    verify(cross_b, 500, 22);
    CHECK_ST(pm::borrow_begin(ob, 500, 1, p), pm::Status::PoolChanged);
    VALIDATE(a);

    CHECK_ST(pm::free(cross_a), pm::Status::Ok);
    CHECK_ST(pm::free(cross_f), pm::Status::Ok);
    CHECK_ST(pm::free(cross_b), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// 9. 池拆分后跨边界对象引用仍可解析
// ---------------------------------------------------------------------------

static void test_pool_split() {
    printf("  [9] pool split\n");
    fresh();
    pm::PoolId s{};
    CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok);

    // 8 x 3584B movables pack 4 below / 4 above the 4-segment boundary.
    pm::RawRef objs[8];
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK_ST(pm::alloc(s, 3584, 8, 0, i, objs[i]), pm::Status::Ok);
        fill(objs[i], 3584, 700 + i);
    }
    pm::PoolId n{};
    CHECK_ST(pm::split(s, 4, n), pm::Status::Ok);
    pm::PoolStats ss = pm::get_stats(s);
    pm::PoolStats sn = pm::get_stats(n);
    CHECK(ss.segment_count == 4 && sn.segment_count == 4);
    CHECK(ss.object_count == 4 && sn.object_count == 4);
    VALIDATE(s);
    VALIDATE(n);

    uint32_t in_new = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        bool moved_pool = desc_pool(objs[i]) == n;
        if (moved_pool) ++in_new;
        pm::RawRef cross = objs[i];
        cross.pool_hint = pm::CROSS_HINT;
        void* p = nullptr;
        CHECK_ST(pm::borrow_begin(cross, 3584, 1, p), pm::Status::Ok);
        pm::borrow_end(cross);
        verify(cross, 3584, 700 + i);
        pm::Status ls = pm::borrow_begin(objs[i], 3584, 1, p);
        if (moved_pool) {
            CHECK(ls == pm::Status::PoolChanged); // local ref: object changed pool
        } else {
            CHECK_ST(ls, pm::Status::Ok);
            pm::borrow_end(objs[i]);
        }
    }
    CHECK(in_new == 4);

    for (uint32_t i = 0; i < 8; ++i) {
        pm::RawRef cross = objs[i];
        cross.pool_hint = pm::CROSS_HINT;
        CHECK_ST(pm::free(cross), pm::Status::Ok);
    }
    CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(n), pm::Status::Ok);

    // A pinned object straddling the segment boundary refuses the split; the
    // source pool stays intact and running.
    pm::PoolId t{};
    CHECK_ST(pm::create_pool(t, 4), pm::Status::Ok);
    pm::RawRef big{}, pin{};
    CHECK_ST(pm::alloc(t, 8000, 8, 0, 1, big), pm::Status::Ok);
    fill(big, 8000, 4242);
    CHECK_ST(pm::alloc(t, 1024, 8, pm::PM_PINNED, 2, pin), pm::Status::Ok); // spans 8 KiB
    pm::PoolId nx{};
    CHECK_ST(pm::split(t, 2, nx), pm::Status::PinnedConflict);
    CHECK_ST(pm::validate(t), pm::Status::Ok); // untouched and still Running
    CHECK_ST(pm::resume(t), pm::Status::Ok);   // no-op when Running
    verify(big, 8000, 4242);
    CHECK_ST(pm::free(big), pm::Status::Ok);
    CHECK_ST(pm::free(pin), pm::Status::Ok);
    VALIDATE(t);

    // A movable straddling the boundary with no room above is NoSpace
    // (crossing block 12008B > upper capacity 8192B).
    CHECK_ST(pm::alloc(t, 3000, 8, 0, 3, big), pm::Status::Ok);
    CHECK_ST(pm::alloc(t, 12000, 8, 0, 4, pin), pm::Status::Ok); // spans 8 KiB, movable
    CHECK_ST(pm::split(t, 2, nx), pm::Status::NoSpace);
    CHECK_ST(pm::validate(t), pm::Status::Ok);
    CHECK_ST(pm::free(big), pm::Status::Ok);
    CHECK_ST(pm::free(pin), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(t), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// 10. Auto Zone 满、临时池满、segment 不足时返回确定错误
// ---------------------------------------------------------------------------
static void test_exhaustion() {
    printf("  [10] exhaustion errors\n");
    fresh();
    // Pool-table exhaustion (PM_MAX_POOLS = 16) and segment exhaustion.
    pm::PoolId pools[16];
    uint32_t made = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (pm::create_pool(pools[made], 4) != pm::Status::Ok) break;
        ++made;
    }
    CHECK(made == 16);                 // 16 pools x 4 segs = whole 64-seg zone
    pm::PoolId extra{};
    CHECK_ST(pm::create_pool(extra, 1), pm::Status::NoSpace);

    // Pool capacity exhaustion + reuse after free (pool = 16 KiB).
    pm::RawRef refs[16];
    uint32_t kept = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (pm::alloc(pools[0], 1024, 8, 0, i, refs[kept]) == pm::Status::Ok) ++kept;
    }
    CHECK(kept == 15); // 16384 / 1032B blocks
    pm::RawRef r2{};
    CHECK_ST(pm::alloc(pools[0], 1024, 8, 0, 99, r2), pm::Status::NoSpace);
    CHECK_ST(pm::free(refs[3]), pm::Status::Ok);
    CHECK_ST(pm::alloc(pools[0], 1024, 8, 0, 98, r2), pm::Status::Ok);
    CHECK_ST(pm::alloc(pools[0], 4096, 8, 0, 97, r2), pm::Status::NoSpace);
    VALIDATE(pools[0]);

    // Destroy rules: objects present -> Busy; empty -> Ok, segments reusable.
    CHECK_ST(pm::destroy_pool(pools[0]), pm::Status::Busy);
    CHECK_ST(pm::destroy_pool(pools[15]), pm::Status::Ok);
    CHECK_ST(pm::create_pool(extra, 1), pm::Status::Ok);
    VALIDATE(extra);

    for (uint32_t i = 0; i < kept; ++i) {
        if (i == 3) continue; // already freed and replaced by r2
        CHECK_ST(pm::free(refs[i]), pm::Status::Ok);
    }
    CHECK_ST(pm::free(r2), pm::Status::Ok);
    for (uint32_t i = 0; i < made; ++i)
        if (pools[i] != extra)  // 'extra' may reuse a destroyed slot
            CHECK_ST(pm::destroy_pool(pools[i]), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(extra), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// 11. 所有对象大小、对齐、最大值边界测试
// ---------------------------------------------------------------------------
static void test_size_alignment_edges() {
    printf("  [11] size/alignment edges\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok); // 16 KiB

    // Sizes 1..40 live side by side without corrupting each other.
    pm::RawRef refs[40];
    for (uint32_t sz = 1; sz <= 40; ++sz) {
        CHECK_ST(pm::alloc(pool, sz, 8, 0, sz, refs[sz - 1]), pm::Status::Ok);
        fill(refs[sz - 1], sz, sz * 13);
    }
    for (uint32_t sz = 1; sz <= 40; ++sz) verify(refs[sz - 1], sz, sz * 13);
    for (uint32_t sz = 1; sz <= 40; ++sz) CHECK_ST(pm::free(refs[sz - 1]), pm::Status::Ok);
    VALIDATE(pool);

    // Alignment ladder (1..8 accepted, payload always 8-aligned).
    pm::RawRef r{};
    void* p = nullptr;
    CHECK_ST(pm::alloc(pool, 10, 1, 0, 1, r), pm::Status::Ok);
    CHECK_ST(pm::resolve(r, 0, 1, p), pm::Status::Ok);
    CHECK(((uintptr_t)p & 7) == 0);
    CHECK_ST(pm::free(r), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 10, 2, 0, 1, r), pm::Status::Ok);
    CHECK_ST(pm::free(r), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 10, 4, 0, 1, r), pm::Status::Ok);
    CHECK_ST(pm::free(r), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 10, 8, 0, 1, r), pm::Status::Ok);
    CHECK_ST(pm::free(r), pm::Status::Ok);

    // Max object in the pool; one block too much fails.
    CHECK_ST(pm::alloc(pool, 16384 - 256, 8, 0, 2, r), pm::Status::Ok);
    CHECK_ST(pm::free(r), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 16384, 8, 0, 2, r), pm::Status::NoSpace);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// 12. Debug 下每个公共操作后调用 pm_validate()（含注入损坏检测）
// ---------------------------------------------------------------------------
static void test_validate_detects_corruption() {
    printf("  [12] validate + corruption injection\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{}, b{};
    CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 128, 8, 0, 2, b), pm::Status::Ok);
    VALIDATE(pool);

    using namespace pm::internal;
    uint32_t saved = g().objects[a.index].block_size;
    g().objects[a.index].block_size = 4096; // now overlaps b
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    g().objects[a.index].block_size = saved;
    VALIDATE(pool);

    uint32_t saved_bm = g().pools[pool].bins.fl_bitmap;
    g().pools[pool].bins.fl_bitmap |= 0x80000000u; // bit with no list
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    g().pools[pool].bins.fl_bitmap = saved_bm;
    VALIDATE(pool);

    uint16_t saved_gen = g().objects[b.index].generation;
    g().objects[b.index].generation = 0; // reserved value
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    g().objects[b.index].generation = saved_gen;
    VALIDATE(pool);

    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.objects_moved == 0 && st.bytes_moved == 0 && st.structure_epoch == 1);
    CHECK_ST(pm::compact(pool), pm::Status::Ok);
    st = pm::get_stats(pool);
    CHECK(st.structure_epoch == 2); // maintenance bumps the epoch
    CHECK_ST(pm::free(a), pm::Status::Ok);
    CHECK_ST(pm::free(b), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// Typed C++ API: pm_make / pm_access / pm_as_cross / destructor hooks
// ---------------------------------------------------------------------------
struct PodThing {
    uint32_t a;
    uint32_t b;
    uint8_t buf[24];
};

static int g_dtors = 0;
struct PinnedThing {
    int v;
    explicit PinnedThing(int x) : v(x) {}
    ~PinnedThing() { ++g_dtors; }
};

static void test_typed_api() {
    printf("  [13] typed C++ API\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);

    auto made = pm::pm_make<PodThing>(pool);
    CHECK(made.ok());
    pm::pm_local_ptr<PodThing> pod = made.value;

    pod->a = 42;      // operator-> borrows for the full expression
    pod->b = 1000;
    {
        auto acc = pod.try_borrow();
        CHECK(acc.ok());
        CHECK(acc.value->a == 42);
        acc.value->buf[0] = 7;
    }
    // pm_ptr survives compaction transparently (lazy re-resolve).
    CHECK_ST(pm::compact(pool), pm::Status::Ok);
    {
        auto acc = pod.try_borrow();
        CHECK(acc.ok());
        CHECK(acc.value->a == 42 && acc.value->b == 1000 && acc.value->buf[0] == 7);
    }

    auto cross = pm::pm_as_cross(pod);
    CHECK(cross.pool_hint() == pm::CROSS_HINT);
    CHECK(pod.pool_hint() == pool);

    // Pinned non-trivial type: destructor runs exactly once at destroy.
    auto pinned = pm::pm_make_pinned<PinnedThing>(pool, 5);
    CHECK(pinned.ok());
    CHECK(pinned.value->v == 5);
    CHECK(g_dtors == 0);
    CHECK_ST(pm::pm_destroy(pinned.value), pm::Status::Ok);
    CHECK(g_dtors == 1);

    // Zero-initialized buffer.
    auto buf = pm::pm_alloc_buffer(pool, 128, pm::PM_ZERO_INIT);
    CHECK(buf.ok());
    {
        auto acc = buf.value.try_borrow();
        CHECK(acc.ok());
        bool all_zero = true;
        for (uint32_t i = 0; i < 128; ++i)
            if (acc->get()[i] != 0) all_zero = false;
        CHECK(all_zero);
    }

    // Typed sub-object views via offset.
    auto sub = pod.at<uint32_t>(4);
    {
        auto acc = sub.try_borrow();
        CHECK(acc.ok());
        CHECK(*acc.value == 1000);
    }
    auto oob = pod.at<uint32_t>(24); // inside buf
    CHECK(oob.try_borrow().ok());
    auto bad = pod.at<uint32_t>(sizeof(PodThing)); // past the end
    CHECK(!bad.try_borrow().ok());

    CHECK_ST(pm::pm_destroy(pod), pm::Status::Ok);
    CHECK_ST(pm::pm_destroy(buf.value), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
static void run(const char* name, void (*fn)()) {
    printf("[TEST] %s\n", name);
    uint32_t f0 = g_fails;
    fn();
    if (g_fails == f0) printf("  PASS\n");
    else printf("  FAIL (%u new failed checks)\n", (unsigned)(g_fails - f0));
}

static uint32_t g_stress_ops = 10000;

// Entry point shared by the host runner and the ESP32-S3 application.
int pondmerge_run_tests(uint32_t stress_ops) {
    g_stress_ops = stress_ops;

    run("basic_alloc_free", test_basic_alloc_free);
    run("stress_random", [] { test_stress_random(g_stress_ops); });
    run("refs_stable_across_compact", test_refs_stable_across_compact);
    run("compact_busy_and_pause", test_compact_busy_and_pause);
    run("invalid_refs", test_invalid_refs);
    run("pinned_barriers", test_pinned_barriers);
    run("non_relocatable_flags", test_non_relocatable_flags);
    run("pool_merge", test_pool_merge);
    run("pool_split", test_pool_split);
    run("exhaustion", test_exhaustion);
    run("size_alignment_edges", test_size_alignment_edges);
    run("validate_detects_corruption", test_validate_detects_corruption);
    run("typed_api", test_typed_api);

    printf("\n%u checks, %u failures\n", (unsigned)g_checks, (unsigned)g_fails);
    return g_fails == 0 ? 0 : 1;
}
