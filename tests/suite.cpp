// PondMerge v1 - host acceptance tests (doc section 18).
// Group numbers in the [n] tags map to the numbered requirements in the task
// book. Build/run: tests/run_host.sh
#include "pondmerge/pondmerge.hpp"
#include "../src/internal.h" // white-box: stats flags + corruption injection

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Audited byte-relocatable types used by the tests. Under the opt-in rule
// (task-book 3.5) every relocatable type must be explicitly registered; there
// is no trivially-copyable fallback.
template <> struct pm::pm_is_relocatable<uint8_t> : std::true_type {};
template <> struct pm::pm_is_relocatable<uint32_t> : std::true_type {};
template <> struct pm::pm_is_relocatable<uint64_t> : std::true_type {};

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

uint8_t g_zone[256 * 1024] __attribute__((aligned(16))); // extern: the device concurrency test borrows the low segments

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
    uint8_t* bytes = static_cast<uint8_t*>(p);
    for (uint32_t i = 0; i < size; ++i) bytes[i] = pat(seed, i);
    pm::borrow_end(ref);
}
static void verify(pm::RawRef ref, uint32_t size, uint32_t seed) {
    void* p = nullptr;
    pm::Status s = pm::borrow_begin(ref, size, 1, p);
    if (s != pm::Status::Ok) {
        CHECK_ST(s, pm::Status::Ok);
        return;
    }
    uint8_t const* bytes = static_cast<uint8_t const*>(p);
    for (uint32_t i = 0; i < size; ++i) {
        ++g_checks;
        if (bytes[i] != pat(seed, i)) {
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

// Typed resolve helpers. They keep the tests free of C-style casts and give
// one place to assert that resolution succeeded before a pointer is used.
template <class T>
static T* resolve_as(pm::RawRef ref) {
    void* raw = nullptr;
    CHECK_ST(pm::resolve(ref, 0, 1, raw), pm::Status::Ok);
    return static_cast<T*>(raw);
}
static void* resolve_void(pm::RawRef ref) {
    void* raw = nullptr;
    CHECK_ST(pm::resolve(ref, 0, 1, raw), pm::Status::Ok);
    return raw;
}
// Bounds of a block in the zone, derived from its descriptor.
static uint64_t desc_block_start_off(pm::RawRef r) {
    using namespace pm::internal;
    return (uint64_t)(uintptr_t)g().objects[r.index].address -
           (uint64_t)(uintptr_t)g().zone - BLOCK_HEADER_SIZE;
}

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

    void const* before = resolve_void(pin);

    CHECK_ST(pm::compact(pool), pm::Status::Ok);

    void const* after = resolve_void(pin);
    CHECK(before == after); // pinned object never moves
    verify(pin, 256, 999);
    verify(m[0], 128, 500);
    verify(m[2], 128, 502);
    verify(m[3], 128, 503);
    verify(m[5], 128, 505);

    // Packing respects the barrier: m2 stays below the pin, m3/m5 above it.
    uint8_t const* pin_addr = static_cast<uint8_t const*>(after);
    uint8_t const* m2_addr = resolve_as<uint8_t>(m[2]);
    CHECK(m2_addr < pin_addr);
    uint8_t const* m3_addr = resolve_as<uint8_t>(m[3]);
    CHECK(m3_addr > pin_addr);
    uint8_t const* m5_addr = resolve_as<uint8_t>(m[5]);
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

    void const* a0 = resolve_void(dma);
    void const* e0 = resolve_void(ext);
    void const* p0 = resolve_void(pinned);

    // Fragment then compact; movables shift, pinned classes stay.
    pm::RawRef pad{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 9, pad), pm::Status::Ok);
    CHECK_ST(pm::free(mov), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 96, 8, 0, 10, mov), pm::Status::Ok);
    CHECK_ST(pm::free(pad), pm::Status::Ok);
    CHECK_ST(pm::compact(pool), pm::Status::Ok);

    void const* a1 = resolve_void(dma);
    void const* e1 = resolve_void(ext);
    void const* p1 = resolve_void(pinned);
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
    pm::RawRef probe{};
    // The NoSpace probes get their own output slot: since the round-5 fix a
    // failed alloc clears its out reference (R30), so probing into `r2`
    // would wipe the successful allocation it still has to release below.
    CHECK_ST(pm::alloc(pools[0], 1024, 8, 0, 99, probe), pm::Status::NoSpace);
    CHECK(probe.generation == 0);
    CHECK_ST(pm::free(refs[3]), pm::Status::Ok);
    CHECK_ST(pm::alloc(pools[0], 1024, 8, 0, 98, r2), pm::Status::Ok);
    CHECK_ST(pm::alloc(pools[0], 4096, 8, 0, 97, probe), pm::Status::NoSpace);
    CHECK(probe.generation == 0);
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
template <> struct pm::pm_is_relocatable<PodThing> : std::true_type {};

// A type that is trivially copyable but holds a raw pointer: it must NOT be
// relocatable until explicitly registered (task-book 3.5).
struct RawHolder {
    uint8_t* raw;
    uint32_t v;
};
static_assert(std::is_trivially_copyable<RawHolder>::value,
              "RawHolder must be trivially copyable for this test to mean anything");
static_assert(!pm::pm_is_relocatable<RawHolder>::value,
              "opt-in default: raw-pointer-holding types are not relocatable");

// Explicitly registered relocatable type for the runtime part of the test.
struct RelocPod {
    uint32_t a;
    uint32_t b;
};
template <> struct pm::pm_is_relocatable<RelocPod> : std::true_type {};

static int g_dtors = 0;
struct PinnedThing {
    int v;
    uint8_t pad[8];
    explicit PinnedThing(int x) : v(x) { memset(pad, 0, sizeof(pad)); }
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
// (R1) task-book 3.1/8.1: failed allocations must not leak descriptor slots
// ---------------------------------------------------------------------------
static void test_slot_rollback_after_failed_allocs() {
    printf("  [R1] failed allocs do not leak descriptor slots\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
    // Valid requests the pool cannot serve: each one used to consume a slot.
    for (uint32_t i = 0; i < 1500; ++i) {
        pm::RawRef r{};
        pm::Status s = pm::alloc(pool, 200 * 1024, 8, 0, i, r);
        if (s != pm::Status::NoSpace) CHECK_ST(s, pm::Status::NoSpace);
    }
    {
        using namespace pm::internal;
        CHECK(g().free_slot_head != NO_SLOT);
        // White-box: walk the free-slot chain — with the leak a portion of
        // the slots was stranded, so the chain must be complete.
        uint32_t nfree = 0;
        for (uint16_t s = g().free_slot_head;
             s != NO_SLOT && nfree <= PM_MAX_OBJECTS;
             s = g().objects[s].next_free_slot)
            ++nfree;
        CHECK(nfree == PM_MAX_OBJECTS);
    }
    // Every one of the PM_MAX_OBJECTS slots must still be usable: allocate in
    // batches (no large static arrays — device BSS is tight).
    uint32_t total_ok = 0;
    pm::RawRef refs[128];
    for (uint32_t batch = 0; batch < PM_MAX_OBJECTS / 128; ++batch) {
        uint32_t ok_n = 0;
        for (uint32_t i = 0; i < 128; ++i)
            if (pm::alloc(pool, 8, 8, 0, i, refs[i]) == pm::Status::Ok) ++ok_n;
        CHECK(ok_n == 128);
        total_ok += ok_n;
        for (uint32_t i = 0; i < ok_n; ++i) CHECK_ST(pm::free(refs[i]), pm::Status::Ok);
    }
    CHECK(total_ok == PM_MAX_OBJECTS);
    VALIDATE(pool);
    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.used_bytes == 0 && st.object_count == 0);
    done();
}

// ---------------------------------------------------------------------------
// (R2) task-book 3.2/8.2: first-fit inside a shared SL bin
// ---------------------------------------------------------------------------
static void test_tlsf_same_bin_first_fit() {
    printf("  [R2] TLSF first-fit within a shared SL bin\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{}, x{}, b{}, y{};
    CHECK_ST(pm::alloc(pool, 252, 8, 0, 1, a), pm::Status::Ok);  // block 260
    CHECK_ST(pm::alloc(pool, 24, 8, 0, 2, x), pm::Status::Ok);   // block 32
    CHECK_ST(pm::alloc(pool, 282, 8, 0, 3, b), pm::Status::Ok);  // block 290
    CHECK_ST(pm::alloc(pool, 6000, 8, 0, 4, y), pm::Status::Ok); // block 6008
    // Free b first, then a: both blocks land in bin (fl=8, sl=0), with the
    // SMALLER block at the list head.
    CHECK_ST(pm::free(b), pm::Status::Ok);
    CHECK_ST(pm::free(a), pm::Status::Ok);
    VALIDATE(pool);
    // need = 288: the head block (260) is too small; the allocator must walk
    // the bin and take the 290-byte block instead of failing.
    pm::RawRef r{};
    CHECK_ST(pm::alloc(pool, 280, 8, 0, 9, r), pm::Status::Ok);
    // Coalescing still works afterwards.
    CHECK_ST(pm::free(r), pm::Status::Ok);
    CHECK_ST(pm::free(x), pm::Status::Ok);
    CHECK_ST(pm::free(y), pm::Status::Ok);
    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.largest_free_block == 2 * 4096);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R3) task-book 3.3/8.3: pm_destroy keeps the pointer on failure
// ---------------------------------------------------------------------------
static void test_destroy_pointer_kept_on_error() {
    printf("  [R3] pm_destroy keeps the pointer on failure\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);

    // Busy: an active borrow blocks the destroy; the pointer stays usable.
    auto made = pm::pm_make<uint32_t>(pool);
    CHECK(made.ok());
    auto pod = made.value;
    {
        auto acc = pod.try_borrow();
        CHECK(acc.ok());
        *acc.value = 7;
        CHECK_ST(pm::pm_destroy(pod), pm::Status::Busy);
        CHECK(pod.valid());
        CHECK(*acc.value == 7); // object untouched
    }
    CHECK_ST(pm::pm_destroy(pod), pm::Status::Ok);
    CHECK(!pod.valid()); // cleared only on success

    // PoolChanged: a local pointer whose object moved pools is NOT destroyed;
    // the object still exists behind a cross reference.
    pm::PoolId p1{}, p2{};
    CHECK_ST(pm::create_pool(p1, 2), pm::Status::Ok);
    CHECK_ST(pm::create_pool(p2, 2), pm::Status::Ok);
    auto m2 = pm::pm_make<uint64_t>(p2);
    CHECK(m2.ok());
    auto obj = m2.value;
    {
        auto acc = obj.try_borrow();
        *acc.value = 0xDEADBEEFCAFEBABFull;
    }
    CHECK_ST(pm::merge(p2, p1), pm::Status::Ok); // object now belongs to p1
    CHECK_ST(pm::pm_destroy(obj), pm::Status::PoolChanged);
    CHECK(obj.valid());
    auto cross = pm::pm_as_cross(obj);
    {
        auto acc = cross.try_borrow();
        CHECK(acc.ok());
        CHECK(*acc.value == 0xDEADBEEFCAFEBABFull); // same logical object
    }
    CHECK_ST(pm::pm_destroy(cross), pm::Status::Ok);
    CHECK(!cross.valid());

    // Stale generation: refuse and keep.
    auto m3 = pm::pm_make<uint32_t>(p1);
    CHECK(m3.ok());
    pm::RawRef stale = m3.value.raw();
    CHECK_ST(pm::pm_destroy(m3.value), pm::Status::Ok);
    pm::pm_local_ptr<uint32_t> stale_ptr{stale};
    CHECK(stale_ptr.valid()); // non-zero generation, but dead
    CHECK_ST(pm::pm_destroy(stale_ptr), pm::Status::InvalidRef);
    CHECK(stale_ptr.valid()); // kept on failure
    CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(p1), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R4) task-book 3.4/8.4: a local pointer cannot be bootstrapped from
//      a CROSS_HINT raw ref, and raw() copies cannot corrupt the pointer
// ---------------------------------------------------------------------------
static void test_local_ref_rejects_cross_hint() {
    printf("  [R4] local refs reject CROSS_HINT construction\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    auto made = pm::pm_make<uint32_t>(pool);
    CHECK(made.ok());

    pm::RawRef crossified = made.value.raw();
    crossified.pool_hint = pm::CROSS_HINT;
    pm::pm_local_ptr<uint32_t> evil{crossified};
    CHECK(!evil.valid()); // constructor invalidates the smuggling attempt
    CHECK_ST(evil.try_borrow().status, pm::Status::InvalidRef);
    CHECK_ST(evil.peek().status, pm::Status::InvalidRef);

    // raw() returns a copy: mutating it cannot bend the original pointer.
    pm::RawRef mutated = made.value.raw();
    mutated.generation = 9999;
    mutated.pool_hint = pm::CROSS_HINT;
    CHECK(mutated.generation == 9999);          // the copy carries the mutations...
    CHECK(mutated.pool_hint == pm::CROSS_HINT); // ...including the pool hint
    CHECK(made.value.valid());                  // ...but the pointer is unaffected
    CHECK(made.value.pool_hint() == pool);
    CHECK_ST(made.value.try_borrow().status, pm::Status::Ok);

    // The explicit cross path still works and produces a cross pointer.
    auto cross = pm::pm_as_cross(made.value);
    CHECK(cross.pool_hint() == pm::CROSS_HINT);
    CHECK(cross.valid());
    CHECK_ST(pm::pm_destroy(made.value), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R5) task-book 3.5/8.5: relocation is opt-in (runtime half; the compile-time
//      half lives in the static_asserts next to RawHolder)
// ---------------------------------------------------------------------------
static void test_relocatable_opt_in() {
    printf("  [R5] registered types relocate, unregistered do not compile in\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef pad{};
    CHECK_ST(pm::alloc(pool, 512, 8, 0, 1, pad), pm::Status::Ok); // pad ahead of the pod
    auto made = pm::pm_make<RelocPod>(pool);
    CHECK(made.ok());
    made.value->a = 11;
    made.value->b = 22;
    CHECK_ST(pm::free(pad), pm::Status::Ok); // gap before the pod -> it must move
    uint32_t epoch0 = 0, epoch1 = 0;
    {
        using namespace pm::internal;
        epoch0 = g().objects[made.value.raw().index].address_epoch;
    }
    CHECK_ST(pm::compact(pool), pm::Status::Ok);
    {
        using namespace pm::internal;
        epoch1 = g().objects[made.value.raw().index].address_epoch;
    }
    CHECK(epoch1 == epoch0 + 1); // moved: address_epoch bumped
    {
        auto acc = made.value.try_borrow();
        CHECK(acc.ok());
        CHECK(acc.value->a == 11 && acc.value->b == 22);
    }
    VALIDATE(pool);
    CHECK_ST(pm::pm_destroy(made.value), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R6) task-book 3.7/8.6: sub-object references cannot free or destroy
// ---------------------------------------------------------------------------
static void test_subobject_ref_cannot_free() {
    printf("  [R6] sub-object refs cannot free/destroy the parent\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);

    auto made = pm::pm_make<RelocPod>(pool);
    CHECK(made.ok());
    auto pod = made.value;
    auto sub = pod.at<uint32_t>(4);
    CHECK(sub.valid());
    CHECK_ST(pm::free(sub.raw()), pm::Status::InvalidRef);
    CHECK_ST(pm::pm_destroy(sub), pm::Status::InvalidRef);
    // Movable objects must refuse destroy callbacks entirely (task-book v2 10).
    CHECK_ST(pm::set_destroy_fn(pod.raw(), nullptr), pm::Status::NotRelocatable);
    { // parent untouched and writable
        auto acc = pod.try_borrow();
        CHECK(acc.ok());
        acc.value->a = 5;
        CHECK(acc.value->a == 5);
    }
    CHECK_ST(pm::pm_destroy(pod), pm::Status::Ok);

    // Same rule for a pinned object with a destructor: the destroy callback
    // must not run through a sub-object view.
    int dtors0 = g_dtors;
    auto pin = pm::pm_make_pinned<PinnedThing>(pool, 3);
    CHECK(pin.ok());
    auto psub = pin.value.at<uint32_t>(4); // genuine sub-object view
    CHECK_ST(pm::pm_destroy(psub), pm::Status::InvalidRef);
    CHECK(g_dtors == dtors0); // destructor did not run
    CHECK_ST(pm::pm_destroy(pin.value), pm::Status::Ok);
    CHECK(g_dtors == dtors0 + 1);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R7) task-book 3.6/8.7: every maintenance failure path leaves the pool
//      complete, running, and with all object data intact
// ---------------------------------------------------------------------------
static void test_maintenance_error_paths_are_clean() {
    printf("  [R7] maintenance failures are clean and pre-move\n");
    fresh();
    pm::PoolId s{};
    CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok);
    pm::RawRef pin{}, m0{}, m1{}, m2{};
    CHECK_ST(pm::alloc(s, 128, 8, pm::PM_PINNED, 1, pin), pm::Status::Ok);
    CHECK_ST(pm::alloc(s, 900, 8, 0, 2, m0), pm::Status::Ok);
    CHECK_ST(pm::alloc(s, 900, 8, 0, 3, m1), pm::Status::Ok);
    CHECK_ST(pm::alloc(s, 900, 8, 0, 4, m2), pm::Status::Ok);
    fill(pin, 128, 1);
    fill(m0, 900, 2);
    fill(m1, 900, 3);
    fill(m2, 900, 4);

    // Inject a planning-stage inconsistency (pinned block out of range):
    // compact must refuse BEFORE moving anything and restore Running.
    uint8_t* saved_addr = nullptr;
    {
        using namespace pm::internal;
        saved_addr = g().objects[pin.index].address;
        g().objects[pin.index].address = g().zone - 128;
    }
    CHECK_ST(pm::compact(s), pm::Status::CorruptMetadata);
    CHECK(pm::get_stats(s).state == 1); // Running again, nothing moved
    {
        using namespace pm::internal;
        g().objects[pin.index].address = saved_addr;
    }
    VALIDATE(s);
    verify(pin, 128, 1);
    verify(m0, 900, 2);
    verify(m1, 900, 3);
    verify(m2, 900, 4);

    // Split refusal (pinned crossing the boundary) is exercised with layout
    // assertions in test_pool_split and test_split_layout_details.
    CHECK_ST(pm::free(m0), pm::Status::Ok);
    CHECK_ST(pm::free(m1), pm::Status::Ok);
    CHECK_ST(pm::free(m2), pm::Status::Ok);
    CHECK_ST(pm::free(pin), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R8) task-book 8.8: validate() returns CorruptMetadata in bounded time on
//      cyclic lists and corrupted headers
// ---------------------------------------------------------------------------
static void test_validate_bounded_on_corruption() {
    printf("  [R8] bounded-time validate on corruption\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    auto made = pm::pm_make<uint32_t>(pool);
    CHECK(made.ok());
    uint32_t idx = made.value.raw().index;

    auto rd32 = [](void const* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    auto wr32 = [](void* p, uint32_t v) { memcpy(p, &v, 4); };

    // 1) address-order self-cycle
    {
        using namespace pm::internal;
        uint32_t saved = g().objects[idx].addr_next;
        g().objects[idx].addr_next = idx; // cycle
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        g().objects[idx].addr_next = saved;
    }
    VALIDATE(pool);

    // 2) free-list self-cycle
    {
        using namespace pm::internal;
        Pool const& P = g().pools[pool];
        uint32_t head_off = NULL_OFF;
        for (uint32_t f = 0; f < FL_COUNT && head_off == NULL_OFF; ++f)
            for (uint32_t sl2 = 0; sl2 < SL_COUNT && head_off == NULL_OFF; ++sl2)
                if (P.bins.head[f][sl2] != NULL_OFF) head_off = P.bins.head[f][sl2];
        CHECK(head_off != NULL_OFF);
        uint32_t saved_next = ptr_of(head_off)->next;
        ptr_of(head_off)->next = head_off; // cycle
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        ptr_of(head_off)->next = saved_next;
    }
    VALIDATE(pool);

    // 3) corrupted used-block header (size / free bit)
    {
        using namespace pm::internal;
        uint8_t* blk = g().objects[idx].address - BLOCK_HEADER_SIZE;
        uint32_t saved = rd32(blk);
        wr32(blk, 1); // size 0 + free bit on a live block
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        wr32(blk, saved);
    }
    VALIDATE(pool);

    // 4) corrupted prev_size chain
    {
        using namespace pm::internal;
        uint8_t* blk = g().objects[idx].address - BLOCK_HEADER_SIZE;
        uint32_t saved = rd32(blk + 4);
        wr32(blk + 4, 999999);
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        wr32(blk + 4, saved);
    }
    VALIDATE(pool);
    CHECK_ST(pm::pm_destroy(made.value), pm::Status::Ok);
    done();

    // 5) allocator on a cyclic free bin: bounded time, then full reset.
    //    The cycle head block itself may still be allocatable; what must hold
    //    is that neither validate() nor alloc() can hang.
    fresh();
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    {
        using namespace pm::internal;
        Pool const& P = g().pools[pool];
        uint32_t head_off = NULL_OFF;
        for (uint32_t f = 0; f < FL_COUNT && head_off == NULL_OFF; ++f)
            for (uint32_t sl2 = 0; sl2 < SL_COUNT && head_off == NULL_OFF; ++sl2)
                if (P.bins.head[f][sl2] != NULL_OFF) head_off = P.bins.head[f][sl2];
        CHECK(head_off != NULL_OFF);
        ptr_of(head_off)->next = head_off; // cycle, left in place
        pm::RawRef r2{};
        pm::Status as = pm::alloc(pool, 64, 8, 0, 1, r2);
        CHECK(as == pm::Status::Ok || as == pm::Status::NoSpace ||
              as == pm::Status::CorruptMetadata);
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    }
    // The system is left with deliberately corrupted metadata; a real product
    // would treat this as a fatal state. Simulate a power cycle (white-box)
    // instead of asking init() to wipe live state — re-init over a live
    // system must and does refuse with Busy (see R14).
    {
        using namespace pm::internal;
        GlobalState& G = g();
        memset(&G, 0, sizeof(G));
    }
    // g() is zeroed (uninitialized); nothing to deinit — return directly.
}

// ---------------------------------------------------------------------------
// (R9) task-book 8.9: sizes at the arithmetic limits are refused, not wrapped
// ---------------------------------------------------------------------------
static void test_huge_alloc_rejected() {
    printf("  [R9] huge sizes refused without overflow\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef r{};
    CHECK_ST(pm::alloc(pool, 0xFFFFFFF0u, 8, 0, 1, r), pm::Status::NoSpace);
    CHECK_ST(pm::alloc(pool, 0xFFFFFFFFu, 8, 0, 1, r), pm::Status::NoSpace);
    CHECK_ST(pm::alloc(pool, 0x80000000u, 8, 0, 1, r), pm::Status::NoSpace);
    CHECK_ST(pm::alloc(pool, 0x01000000u, 8, 0, 1, r), pm::Status::NoSpace); // beyond FL range
    CHECK_ST(pm::alloc(pool, 0x00F00000u, 8, 0, 1, r), pm::Status::NoSpace); // beyond zone
    CHECK_ST(pm::alloc(pool, 32, 8, 0, 2, r), pm::Status::Ok); // still healthy
    CHECK_ST(pm::free(r), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R10) task-book 8.10: pause/borrow/resume quiescent-window contract
// ---------------------------------------------------------------------------
static void test_quiescent_window_contract() {
    printf("  [R10] pause/borrow/resume contract\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    auto made = pm::pm_make<uint64_t>(pool);
    CHECK(made.ok());
    { auto acc = made.value.try_borrow(); *acc.value = 42; }

    void* p = nullptr;
    CHECK_ST(pm::borrow_begin(made.value.raw(), 8, 8, p), pm::Status::Ok);

    // pause is allowed while borrows are active; it only blocks NEW borrows.
    CHECK_ST(pm::pause(pool), pm::Status::Ok);
    CHECK_ST(pm::borrow_begin(made.value.raw(), 8, 8, p), pm::Status::Busy);
    CHECK_ST(pm::compact(pool), pm::Status::Busy);      // quiescence not reached
    CHECK(pm::get_stats(pool).state == 2);              // Paused (doc section 3)
    // borrow_end is safe while paused
    pm::borrow_end(made.value.raw());
    CHECK_ST(pm::resume(pool), pm::Status::Ok);
    {
        auto acc = made.value.try_borrow();
        CHECK(acc.ok());
        CHECK(*acc.value == 42);
    }
    CHECK_ST(pm::compact(pool), pm::Status::Ok);        // quiescent now
    VALIDATE(pool);
    CHECK_ST(pm::pm_destroy(made.value), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R11) task-book 8.11: split layout details — pinned immobility, side counts
// ---------------------------------------------------------------------------
static void test_split_layout_details() {
    printf("  [R11] split layout: pinned stay, sides repartition\n");
    fresh();
    pm::PoolId s{};
    CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok);
    pm::RawRef pin_low{}, m[8]{}, pin_high{};
    CHECK_ST(pm::alloc(s, 64, 8, pm::PM_PINNED, 1, pin_low), pm::Status::Ok);
    for (uint32_t i = 0; i < 4; ++i) { // lower group
        CHECK_ST(pm::alloc(s, 3584, 8, 0, 10 + i, m[i]), pm::Status::Ok);
        fill(m[i], 3584, 60 + i);
    }
    CHECK_ST(pm::alloc(s, 128, 8, pm::PM_PINNED, 2, pin_high), pm::Status::Ok);
    for (uint32_t i = 4; i < 8; ++i) { // upper group
        CHECK_ST(pm::alloc(s, 3584, 8, 0, 10 + i, m[i]), pm::Status::Ok);
        fill(m[i], 3584, 60 + i);
    }
    fill(pin_low, 64, 91);
    fill(pin_high, 128, 92);

    void const* low_before = resolve_void(pin_low);
    void const* high_before = resolve_void(pin_high);

    pm::PoolId n{};
    CHECK_ST(pm::split(s, 4, n), pm::Status::Ok);

    void const* low_after = resolve_void(pin_low);
    void const* high_after = resolve_void(pin_high);
    CHECK(low_before == low_after);    // pinned never moved
    CHECK(high_before == high_after);  // pinned never moved

    VALIDATE(s);
    VALIDATE(n);
    pm::PoolStats ss = pm::get_stats(s), sn = pm::get_stats(n);
    CHECK(ss.object_count + sn.object_count == 10);
    CHECK(ss.object_count >= 1 && sn.object_count >= 1);
    for (uint32_t i = 0; i < 8; ++i) {
        pm::RawRef cross = m[i];
        cross.pool_hint = pm::CROSS_HINT;
        void* p = nullptr;
        CHECK_ST(pm::borrow_begin(cross, 3584, 1, p), pm::Status::Ok);
        pm::borrow_end(cross);
        verify(cross, 3584, 60 + i);
        bool in_new = desc_pool(m[i]) == n;
        if (in_new) {
            pm::RawRef c2 = m[i]; c2.pool_hint = pm::CROSS_HINT;
            uint8_t const* a = resolve_as<uint8_t>(c2);
            CHECK(a >= static_cast<uint8_t const*>(low_after)); // above the pin
        }
    }
    verify(pin_low, 64, 91);
    verify(pin_high, 128, 92);
    for (uint32_t i = 0; i < 8; ++i) {
        pm::RawRef cross = m[i];
        cross.pool_hint = pm::CROSS_HINT;
        CHECK_ST(pm::free(cross), pm::Status::Ok);
    }
    CHECK_ST(pm::free(pin_low), pm::Status::Ok);
    CHECK_ST(pm::free(pin_high), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(n), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R12) task-book 8.12: generation / address_epoch / pool-hint combinations
// ---------------------------------------------------------------------------
static void test_generation_epoch_combinations() {
    printf("  [R12] generation/epoch/hint interaction matrix\n");
    fresh();
    pm::PoolId p1{}, p2{};
    CHECK_ST(pm::create_pool(p1, 2), pm::Status::Ok);
    CHECK_ST(pm::create_pool(p2, 2), pm::Status::Ok);
    pm::RawRef pad{};
    CHECK_ST(pm::alloc(p2, 512, 8, 0, 1, pad), pm::Status::Ok);
    auto made = pm::pm_make<uint32_t>(p2); // object lives in the SOURCE pool
    CHECK(made.ok());
    CHECK_ST(pm::free(pad), pm::Status::Ok); // force a real move on compact
    pm::RawRef ref = made.value.raw();
    uint32_t idx = ref.index;

    // compact bumps address_epoch; local ref keeps resolving (same pool)
    uint32_t e0, e1;
    {
        using namespace pm::internal;
        e0 = g().objects[idx].address_epoch;
    }
    CHECK_ST(pm::compact(p2), pm::Status::Ok);
    {
        using namespace pm::internal;
        e1 = g().objects[idx].address_epoch;
    }
    CHECK(e1 == e0 + 1);
    {
        auto acc = made.value.try_borrow();
        CHECK(acc.ok()); // epoch change is absorbed transparently
    }

    // merge bumps address_epoch even when the address is unchanged, and
    // flips the local binding (source p2's objects adopt target p1)
    CHECK_ST(pm::merge(p2, p1), pm::Status::Ok);
    {
        using namespace pm::internal;
        CHECK(g().objects[idx].address_epoch >= e1 + 1); // merge bump (+ repack)
        CHECK(g().objects[idx].pool_id == p1);
    }
    void* p = nullptr;
    CHECK_ST(pm::borrow_begin(ref, 4, 4, p), pm::Status::PoolChanged); // stale hint
    pm::RawRef cross = ref;
    cross.pool_hint = pm::CROSS_HINT;
    CHECK_ST(pm::borrow_begin(cross, 4, 4, p), pm::Status::Ok);
    pm::borrow_end(cross);

    // free bumps generation; the old ref is dead in every combination
    uint16_t g0;
    {
        using namespace pm::internal;
        g0 = g().objects[idx].generation;
    }
    CHECK_ST(pm::free(cross), pm::Status::Ok);
    {
        using namespace pm::internal;
        CHECK(g().objects[idx].generation == (uint16_t)(g0 + 1));
    }
    pm::RawRef stale = ref;
    CHECK_ST(pm::borrow_begin(stale, 4, 4, p), pm::Status::InvalidRef);
    stale.pool_hint = pm::CROSS_HINT;
    CHECK_ST(pm::borrow_begin(stale, 4, 4, p), pm::Status::InvalidRef); // gen beats hint
    CHECK_ST(pm::destroy_pool(p1), pm::Status::Ok); // p2 vanished in the merge
    done();
}


// ---------------------------------------------------------------------------
// (R13) task-book v2 4.4: split with a boundary-crossing movable, a hole in
// the upper region and two upper movables. With the old descending execution
// the second upper move overwrote the not-yet-moved source tail of the first
// upper object (16 bytes of payload corruption); ascending execution must
// keep every payload intact.
// ---------------------------------------------------------------------------
static void test_split_crossing_order() {
    printf("  [R13] split crossing + upper holes: no source clobbering\n");
    fresh();
    pm::PoolId s{};
    CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok);

    // Layout (pool = 32 KiB, boundary = 16 KiB):
    //   L [0,16376) fills the lower region
    //   C [16376,16484) crosses the boundary (x=8 below, y=92 above)
    //   h [16484,16508) small block, freed to make a 24B upper hole
    //   u1 [16508,20092)  u2 [20092,23676)
    pm::RawRef L{}, C{}, h{}, u1{}, u2{};
    CHECK_ST(pm::alloc(s, 16368, 8, 0, 1, L), pm::Status::Ok);
    fill(L, 16368, 1);
    CHECK_ST(pm::alloc(s, 100, 8, 0, 2, C), pm::Status::Ok);
    fill(C, 100, 2);
    CHECK_ST(pm::alloc(s, 16, 8, 0, 3, h), pm::Status::Ok);
    CHECK_ST(pm::alloc(s, 3584, 8, 0, 4, u1), pm::Status::Ok);
    fill(u1, 3584, 4);
    CHECK_ST(pm::alloc(s, 3584, 8, 0, 5, u2), pm::Status::Ok);
    fill(u2, 3584, 5);
    CHECK_ST(pm::free(h), pm::Status::Ok); // 24B hole between C and u1
    VALIDATE(s);

    pm::PoolId n{};
    CHECK_ST(pm::split(s, 4, n), pm::Status::Ok);
    VALIDATE(s);
    VALIDATE(n);

    // Every object keeps its payload, pool identity follows the side it
    // landed on, and generation/epoch stay sane.
    auto vfy = [&](pm::RawRef r, uint32_t sz, uint32_t seed) {
        pm::RawRef cross = r;
        cross.pool_hint = pm::CROSS_HINT;
        verify(cross, sz, seed);
    };
    vfy(L, 16368, 1);
    vfy(C, 100, 2);
    vfy(u1, 3584, 4);
    vfy(u2, 3584, 5);
    CHECK(desc_pool(L) == s);
    CHECK(desc_pool(C) == n); // crossing object adopted the new pool
    CHECK(desc_pool(u1) == n);
    CHECK(desc_pool(u2) == n);
    {
        using namespace pm::internal;
        CHECK(g().objects[C.index].generation != 0);
        CHECK(g().objects[C.index].address_epoch >= 2); // moved at least once
    }
    // Upper objects must now live at their packed addresses (>= boundary).
    {
        using namespace pm::internal;
        uint8_t const* boundary = seg_base(g().pools[n].segment_first);
        CHECK(g().objects[u1.index].address - BLOCK_HEADER_SIZE >= boundary);
        CHECK(g().objects[u2.index].address - BLOCK_HEADER_SIZE >= boundary);
        CHECK(g().objects[C.index].address - BLOCK_HEADER_SIZE == boundary);
    }
    pm::RawRef cL = L; cL.pool_hint = pm::CROSS_HINT;
    pm::RawRef cC = C; cC.pool_hint = pm::CROSS_HINT;
    pm::RawRef cu1 = u1; cu1.pool_hint = pm::CROSS_HINT;
    pm::RawRef cu2 = u2; cu2.pool_hint = pm::CROSS_HINT;
    CHECK_ST(pm::free(cL), pm::Status::Ok);
    CHECK_ST(pm::free(cC), pm::Status::Ok);
    CHECK_ST(pm::free(cu1), pm::Status::Ok);
    CHECK_ST(pm::free(cu2), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(n), pm::Status::Ok);
    done();
}


// ---------------------------------------------------------------------------
// (R14) task-book v2 6.2: init() validates before touching global state
// ---------------------------------------------------------------------------
static void test_init_lifecycle() {
    printf("  [R14] init validates first, never wipes live state\n");

    // Uninitialized: every invalid config must be refused and must leave no
    // half-initialized state behind (the following valid init succeeds).
    pm::Config bad1{g_zone, sizeof(g_zone), 1024}; // 256 segments > PM_MAX_SEGMENTS
    CHECK_ST(pm::init(bad1), pm::Status::NoSpace);
    CHECK_ST(pm::init(pm::Config{nullptr, 4096, 4096}), pm::Status::InvalidAlignment);
    CHECK_ST(pm::init(pm::Config{g_zone, 512, 4096}), pm::Status::InvalidAlignment);
    CHECK_ST(pm::init(pm::Config{g_zone, sizeof(g_zone), 6144}), pm::Status::InvalidAlignment); // not a power of two
    CHECK_ST(pm::init(pm::Config{g_zone, 2048, 4096}), pm::Status::InvalidAlignment);
    pm::Config cfg{g_zone, sizeof(g_zone), 4096};
    CHECK_ST(pm::init(cfg), pm::Status::Ok); // state was never touched above

    // Initialized with a live object: any re-init is refused, state intact.
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    auto made = pm::pm_make<uint32_t>(pool);
    CHECK(made.ok());
    { auto acc = made.value.try_borrow(); *acc.value = 0x1234; }
    CHECK_ST(pm::init(cfg), pm::Status::Busy);
    {
        auto acc = made.value.try_borrow();
        CHECK(acc.ok());
        CHECK(*acc.value == 0x1234); // object untouched
    }
    VALIDATE(pool);
    CHECK_ST(pm::init(pm::Config{g_zone, sizeof(g_zone), 1024}), pm::Status::Busy);

    // Clean shutdown + fresh start: brand-new generations, zeroed stats.
    CHECK_ST(pm::pm_destroy(made.value), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
    CHECK_ST(pm::deinit(), pm::Status::Ok);
    CHECK_ST(pm::init(cfg), pm::Status::Ok);
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    auto again = pm::pm_make<uint32_t>(pool);
    CHECK(again.ok());
    CHECK(again.value.raw().generation == 1); // lifecycle restarted
    CHECK(pm::global_stats().max_live_objects == 0 + 1);
    CHECK_ST(pm::pm_destroy(again.value), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R15) task-book v2 11.1 item 2: split boundary geometries.
//   (a) a crossing object 16 B past the boundary shifts EVERY upper object
//       right by 8, so the whole upper plan is a rightward prefix -- the case
//       the v2 pseudo-code's "ascending upper" rule does not cover. A wrong
//       order clobbers u1's source here.
//   (b) perfect tiling: the lower region ends exactly at the boundary, the
//       first upper object starts exactly on it, the last object ends exactly
//       at the pool end. Nothing moves and no free block exists.
//   (c) one huge crossing object ending 8 B before the pool end: it moves
//       right by 8 and leaves 8 B of tail slack in BOTH pools.
// ---------------------------------------------------------------------------
static void test_split_boundary_geometries() {
    printf("  [R15] split boundary geometries\n");

    // (a) crossing by 16 B: entire upper plan moves right
    {
        fresh();
        pm::PoolId s{};
        CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok); // 32 KiB, boundary 16 KiB
        pm::RawRef L{}, X{}, u1{}, u2{};
        CHECK_ST(pm::alloc(s, 16368, 8, 0, 1, L), pm::Status::Ok);  // [0,16376)
        CHECK_ST(pm::alloc(s, 16, 8, 0, 2, X), pm::Status::Ok);     // [16376,16400)
        CHECK_ST(pm::alloc(s, 3584, 8, 0, 3, u1), pm::Status::Ok);  // [16400,19992)
        CHECK_ST(pm::alloc(s, 3584, 8, 0, 4, u2), pm::Status::Ok);  // [19992,23584)
        fill(L, 16368, 11);
        fill(X, 16, 22);
        fill(u1, 3584, 33);
        fill(u2, 3584, 44);
        VALIDATE(s);

        pm::PoolId n{};
        CHECK_ST(pm::split(s, 4, n), pm::Status::Ok);
        VALIDATE(s);
        VALIDATE(n);

        pm::RawRef cL = L; cL.pool_hint = pm::CROSS_HINT;
        pm::RawRef cX = X; cX.pool_hint = pm::CROSS_HINT;
        pm::RawRef c1 = u1; c1.pool_hint = pm::CROSS_HINT;
        pm::RawRef c2 = u2; c2.pool_hint = pm::CROSS_HINT;
        verify(cL, 16368, 11);
        verify(cX, 16, 22);
        verify(c1, 3584, 33);
        verify(c2, 3584, 44);
        CHECK(desc_pool(L) == s);
        CHECK(desc_pool(X) == n && desc_pool(u1) == n && desc_pool(u2) == n);
        // Packed from the boundary in address order, with no holes.
        CHECK(desc_block_start_off(X) == 16384);
        CHECK(desc_block_start_off(u1) == 16384 + 24);
        CHECK(desc_block_start_off(u2) == 16384 + 24 + 3592);
        CHECK_ST(pm::free(cL), pm::Status::Ok);
        CHECK_ST(pm::free(cX), pm::Status::Ok);
        CHECK_ST(pm::free(c1), pm::Status::Ok);
        CHECK_ST(pm::free(c2), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(n), pm::Status::Ok);
        done();
    }

    // (b) perfect tiling: nothing moves, nothing is free
    {
        fresh();
        pm::PoolId s{};
        CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok);
        pm::RawRef o[4];
        uint64_t off0[4];
        for (uint32_t i = 0; i < 4; ++i) {
            CHECK_ST(pm::alloc(s, 8184, 8, 0, i, o[i]), pm::Status::Ok); // block 8192
            fill(o[i], 8184, 100 + i);
            off0[i] = desc_block_start_off(o[i]);
        }
        CHECK(off0[0] == 0 && off0[1] == 8192 && off0[2] == 16384 && off0[3] == 24576);
        VALIDATE(s);

        pm::PoolId n{};
        CHECK_ST(pm::split(s, 4, n), pm::Status::Ok);
        VALIDATE(s);
        VALIDATE(n);
        for (uint32_t i = 0; i < 4; ++i) {
            pm::RawRef c = o[i]; c.pool_hint = pm::CROSS_HINT;
            verify(c, 8184, 100 + i);
            CHECK(desc_block_start_off(o[i]) == off0[i]); // already packed
        }
        CHECK(desc_pool(o[0]) == s && desc_pool(o[1]) == s);
        CHECK(desc_pool(o[2]) == n && desc_pool(o[3]) == n);
        pm::PoolStats ss = pm::get_stats(s), sn = pm::get_stats(n);
        CHECK(ss.free_bytes == 0 && sn.free_bytes == 0);
        CHECK(ss.largest_free_block == 0 && sn.largest_free_block == 0);
        for (uint32_t i = 0; i < 4; ++i) {
            pm::RawRef c = o[i]; c.pool_hint = pm::CROSS_HINT;
            CHECK_ST(pm::free(c), pm::Status::Ok);
        }
        CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(n), pm::Status::Ok);
        done();
    }

    // (c) a single huge crossing object ending 8 B before the pool end
    {
        fresh();
        pm::PoolId s{};
        CHECK_ST(pm::create_pool(s, 8), pm::Status::Ok);
        pm::RawRef L{}, X{};
        CHECK_ST(pm::alloc(s, 16368, 8, 0, 1, L), pm::Status::Ok); // [0,16376)
        CHECK_ST(pm::alloc(s, 16368, 8, 0, 2, X), pm::Status::Ok); // [16376,32752)
        fill(L, 16368, 55);
        fill(X, 16368, 66);
        VALIDATE(s);

        pm::PoolId n{};
        CHECK_ST(pm::split(s, 4, n), pm::Status::Ok);
        VALIDATE(s);
        VALIDATE(n);
        pm::RawRef cL = L; cL.pool_hint = pm::CROSS_HINT;
        pm::RawRef cX = X; cX.pool_hint = pm::CROSS_HINT;
        verify(cL, 16368, 55);
        verify(cX, 16368, 66);
        CHECK(desc_pool(X) == n);
        CHECK(desc_block_start_off(X) == 16384); // moved right by one header
        // 8 B of tail slack in each pool; validate() must accept both.
        pm::PoolStats ss = pm::get_stats(s), sn = pm::get_stats(n);
        CHECK(ss.fragment_bytes == 8 && ss.free_bytes == 8);
        CHECK(sn.fragment_bytes == 8 && sn.free_bytes == 8);
        // Freeing the objects leaves a free block separated from the tail
        // slack: the second half of the R13 lesson -- validate() must still
        // accept it, which it does via the "gaps are sub-minimal" invariant.
        CHECK_ST(pm::free(cL), pm::Status::Ok);
        VALIDATE(s);
        CHECK_ST(pm::free(cX), pm::Status::Ok);
        VALIDATE(n);
        CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(n), pm::Status::Ok);
        done();
    }
}

// ---------------------------------------------------------------------------
// (R16) task-book v2 11.1 items 3-4: the state/borrow decision matrix for
//       compact, merge and split, plus the borrow counters underneath it.
// ---------------------------------------------------------------------------
static void test_maintenance_state_matrix() {
    printf("  [R16] maintenance state/borrow matrix\n");

    // ---- (A) compact: Running and Paused accepted, borrows refused ----
    {
        fresh();
        pm::PoolId c{};
        CHECK_ST(pm::create_pool(c, 2), pm::Status::Ok);
        pm::RawRef o{}, o2{};
        CHECK_ST(pm::alloc(c, 256, 8, 0, 1, o), pm::Status::Ok);
        CHECK_ST(pm::alloc(c, 256, 8, 0, 2, o2), pm::Status::Ok);
        fill(o, 256, 1);
        fill(o2, 256, 2);

        CHECK_ST(pm::compact(c), pm::Status::Ok); // entry Running
        CHECK(get_stats_state(c) == 1);           // success returns to service
        CHECK_ST(pm::pause(c), pm::Status::Ok);
        CHECK_ST(pm::compact(c), pm::Status::Ok); // entry Paused
        CHECK(get_stats_state(c) == 1);

        // Running + active borrow: Busy. Doc section 8's flow leaves it
        // Paused (compact enters Paused before checking borrows).
        void* p = nullptr;
        CHECK_ST(pm::borrow_begin(o, 256, 1, p), pm::Status::Ok);
        {
            using namespace pm::internal;
            CHECK(g().pools[c].borrow_count == 1);
            CHECK(g().objects[o.index].active_borrows == 1);
        }
        CHECK_ST(pm::compact(c), pm::Status::Busy);
        CHECK(get_stats_state(c) == 2);
        // Paused + active borrow: still Busy, still Paused.
        CHECK_ST(pm::compact(c), pm::Status::Busy);
        CHECK(get_stats_state(c) == 2);
        pm::borrow_end(o);
        {
            using namespace pm::internal;
            CHECK(g().pools[c].borrow_count == 0);
            CHECK(g().objects[o.index].active_borrows == 0);
        }
        CHECK_ST(pm::resume(c), pm::Status::Ok);
        CHECK_ST(pm::compact(c), pm::Status::Ok);
        verify(o, 256, 1);
        verify(o2, 256, 2);
        CHECK_ST(pm::free(o), pm::Status::Ok);
        CHECK_ST(pm::free(o2), pm::Status::Ok);
        VALIDATE(c);
        CHECK_ST(pm::destroy_pool(c), pm::Status::Ok);
        done();
    }

    // ---- (B) merge: Running and Paused accepted, borrows refused ----
    {
        fresh();
        pm::PoolId c{}, t{};
        CHECK_ST(pm::create_pool(c, 2), pm::Status::Ok); // segs 0-1
        CHECK_ST(pm::create_pool(t, 2), pm::Status::Ok); // segs 2-3
        pm::RawRef oc{}, ot{};
        CHECK_ST(pm::alloc(c, 256, 8, 0, 1, oc), pm::Status::Ok);
        CHECK_ST(pm::alloc(t, 256, 8, 0, 2, ot), pm::Status::Ok);
        fill(oc, 256, 1);
        fill(ot, 256, 2);

        // Active borrow: Busy, and NEITHER pool is flipped (merge returns in
        // the arming block, before any state write).
        void* p = nullptr;
        CHECK_ST(pm::borrow_begin(ot, 256, 1, p), pm::Status::Ok);
        CHECK_ST(pm::merge(t, c), pm::Status::Busy);
        CHECK(get_stats_state(t) == 1 && get_stats_state(c) == 1);
        pm::borrow_end(ot);

        // Both pools Paused: accepted (v2 section 5.1 uniformity).
        CHECK_ST(pm::pause(t), pm::Status::Ok);
        CHECK_ST(pm::pause(c), pm::Status::Ok);
        CHECK_ST(pm::merge(t, c), pm::Status::Ok);
        CHECK(get_stats_state(c) == 1); // result returns to service
        CHECK(get_stats_state(t) == 0); // source became Empty
        VALIDATE(c);
        pm::RawRef cross_oc = oc; cross_oc.pool_hint = pm::CROSS_HINT;
        pm::RawRef cross_ot = ot; cross_ot.pool_hint = pm::CROSS_HINT;
        verify(cross_oc, 256, 1);
        verify(cross_ot, 256, 2);
        CHECK_ST(pm::free(cross_oc), pm::Status::Ok);
        CHECK_ST(pm::free(cross_ot), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(c), pm::Status::Ok);
        done();
    }

    // ---- (C) split: Running and Paused accepted, borrows refused ----
    {
        fresh();
        pm::PoolId s{};
        CHECK_ST(pm::create_pool(s, 4), pm::Status::Ok); // segs 0-3
        pm::RawRef o{};
        CHECK_ST(pm::alloc(s, 256, 8, 0, 1, o), pm::Status::Ok);
        fill(o, 256, 3);

        void* p = nullptr;
        CHECK_ST(pm::borrow_begin(o, 256, 1, p), pm::Status::Ok);
        pm::PoolId n{};
        CHECK_ST(pm::split(s, 2, n), pm::Status::Busy);
        CHECK(get_stats_state(s) == 1); // source untouched, still Running
        pm::borrow_end(o);

        CHECK_ST(pm::pause(s), pm::Status::Ok);
        CHECK_ST(pm::split(s, 2, n), pm::Status::Ok); // entry Paused
        CHECK(get_stats_state(s) == 1 && get_stats_state(n) == 1);
        VALIDATE(s);
        VALIDATE(n);
        pm::RawRef cross_o = o; cross_o.pool_hint = pm::CROSS_HINT;
        verify(cross_o, 256, 3);
        CHECK(desc_block_start_off(o) < 2u * 4096u); // stayed below the boundary
        CHECK_ST(pm::destroy_pool(n), pm::Status::Ok); // new pool is empty
        CHECK_ST(pm::free(cross_o), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(s), pm::Status::Ok);
        done();
    }
}

// ---------------------------------------------------------------------------
// (R17) task-book v2 11.1 item 8: a descriptor that cannot describe a real
//       block is refused by validate, resolve, borrow_begin AND free -- the
//       address derived from it must never be handed out.
// ---------------------------------------------------------------------------
static void test_desc_block_consistency_rejected() {
    printf("  [R17] descriptor/block inconsistency refused everywhere\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{};
    CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, a), pm::Status::Ok);
    fill(a, 128, 7);

    auto expect_rejected = [&]() {
        void* p = nullptr;
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        CHECK_ST(pm::resolve(a, 128, 1, p), pm::Status::CorruptMetadata);
        CHECK_ST(pm::borrow_begin(a, 128, 1, p), pm::Status::CorruptMetadata);
        CHECK_ST(pm::free(a), pm::Status::CorruptMetadata); // no side effects
    };
    auto expect_accepted = [&]() {
        void* p = nullptr;
        CHECK_ST(pm::validate(pool), pm::Status::Ok);
        CHECK_ST(pm::resolve(a, 128, 1, p), pm::Status::Ok);
        CHECK_ST(pm::borrow_begin(a, 128, 1, p), pm::Status::Ok);
        pm::borrow_end(a);
    };

    using namespace pm::internal;
    ObjectDesc& d = g().objects[a.index];
    uint32_t const saved_bs = d.block_size;
    uint8_t* const saved_addr = d.address;
    CHECK(saved_bs == 136); // 128 payload + 8 header

    d.size = saved_bs; // payload claims one header too many
    expect_rejected();
    d.size = 128;

    d.block_size = PM_MIN_BLOCK - 8; // below the minimum block
    expect_rejected();
    d.block_size = saved_bs;

    d.block_size = saved_bs + 4; // not PM_ALIGNMENT aligned
    expect_rejected();
    d.block_size = saved_bs;

    d.address = saved_addr + 4; // unaligned payload
    expect_rejected();
    d.address = saved_addr;

    d.size = 0; // a live object always has a payload
    expect_rejected();
    d.size = 128;

    expect_accepted();
    verify(a, 128, 7);
    CHECK_ST(pm::free(a), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R18) task-book v2 11.1 item 9: get_stats() must return in bounded time on
//       a corrupted free list and must not dereference an out-of-range cursor.
// ---------------------------------------------------------------------------
static void test_get_stats_bounded_on_corruption() {
    printf("  [R18] get_stats bounded and range-checked on corruption\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{};
    CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, a), pm::Status::Ok);
    fill(a, 128, 7);
    CHECK(pm::get_stats(pool).largest_free_block > 0);

    using namespace pm::internal;
    CHECK(pm::get_stats(pool).valid == 1); // healthy walk is reported as such
    Pool& P = g().pools[pool];
    uint32_t hf = FL_COUNT, hs = SL_COUNT;
    for (uint32_t f = 0; f < FL_COUNT && hf == FL_COUNT; ++f)
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
            if (P.bins.head[f][sl] != NULL_OFF) { hf = f; hs = sl; break; }
    CHECK(hf < FL_COUNT);
    uint32_t const real_head = P.bins.head[hf][hs];
    uint32_t const real_next = ptr_of(real_head)->next;

    // (a) self-cycle: bounded, refuses to report, and SAYS so (valid == 0):
    // the "zero" of a refused walk is distinguishable from a pool without
    // free blocks (round-4 task book section 11).
    ptr_of(real_head)->next = real_head;
    {
        pm::PoolStats st = pm::get_stats(pool);
        CHECK(st.largest_free_block == 0 && st.valid == 0);
    }
    ptr_of(real_head)->next = real_next;
    {
        pm::PoolStats st = pm::get_stats(pool);
        CHECK(st.largest_free_block > 0 && st.valid == 1);
    }

    // (b) cursor outside the zone: refused BEFORE dereferencing -- a wild read
    //     here would be an out-of-bounds access that ASan would trap.
    P.bins.head[hf][hs] = 0xFFFFFFF0u;
    CHECK(pm::get_stats(pool).largest_free_block == 0);
    P.bins.head[hf][hs] = 64u * 1024u * 1024u;
    CHECK(pm::get_stats(pool).largest_free_block == 0);
    P.bins.head[hf][hs] = real_head;
    CHECK(pm::get_stats(pool).largest_free_block > 0);

    // (c) the allocator refuses the same corruption instead of following it,
    //     and since the round-4 task book (section 11) it reports it
    //     PRECISELY as CorruptMetadata -- the old assertion pinned only the
    //     refusal (NoSpace), which silently masked corruption as exhaustion;
    //     the new one is strictly stronger and keeps genuine exhaustion at
    //     NoSpace (see R29's healthy-allocation checks and the model's
    //     allocatability oracle, both unchanged).
    P.bins.head[hf][hs] = 0xFFFFFFF0u;
    pm::RawRef r{};
    pm::Status as = pm::alloc(pool, 64, 8, 0, 2, r);
    CHECK(as == pm::Status::CorruptMetadata);
    P.bins.head[hf][hs] = real_head;
    VALIDATE(pool);

    verify(a, 128, 7);
    CHECK_ST(pm::free(a), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R19) task-book v2 11.1 item 10: destroy callbacks exist for pinned objects
//       only, and the callback receives the object's payload address.
// ---------------------------------------------------------------------------
static int g_destroy_calls = 0;
static uint8_t const* g_destroy_last = nullptr;
// The signature must stay void(*)(void*): it is stored in pm::ObjectDesc and
// called by the library with a non-const payload pointer, so the callback
// cannot take a const pointer here (cppcheck's suggestion would need a cast
// at the assignment site and would weaken the contract).
// cppcheck-suppress constParameterCallback
static void counting_destroy(void* p) {
    ++g_destroy_calls;
    g_destroy_last = static_cast<uint8_t const*>(p);
}

static void test_destroy_fn_semantics() {
    printf("  [R19] destroy callback: pinned only, runs exactly once\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);

    // Movable: refused, and the refusal must not store the callback.
    pm::RawRef mov{};
    CHECK_ST(pm::alloc(pool, 64, 8, pm::PM_MOVABLE, 1, mov), pm::Status::Ok);
    CHECK_ST(pm::set_destroy_fn(mov, &counting_destroy), pm::Status::NotRelocatable);
    {
        using namespace pm::internal;
        CHECK(g().objects[mov.index].destroy_fn == nullptr);
    }
    // DMA/external imply pinned, so they may carry a callback.
    pm::RawRef ext{};
    CHECK_ST(pm::alloc(pool, 64, 8, pm::PM_EXTERNAL, 2, ext), pm::Status::Ok);
    CHECK_ST(pm::set_destroy_fn(ext, &counting_destroy), pm::Status::Ok);

    pm::RawRef pin{};
    CHECK_ST(pm::alloc(pool, 64, 8, pm::PM_PINNED, 3, pin), pm::Status::Ok);
    CHECK_ST(pm::set_destroy_fn(pin, &counting_destroy), pm::Status::Ok);

    void const* addr_pin = resolve_void(pin);
    g_destroy_calls = 0;
    g_destroy_last = nullptr;
    CHECK_ST(pm::free(pin), pm::Status::Ok);
    CHECK(g_destroy_calls == 1);
    CHECK(g_destroy_last == static_cast<uint8_t const*>(addr_pin));
    CHECK_ST(pm::free(ext), pm::Status::Ok);
    CHECK(g_destroy_calls == 2);
    CHECK_ST(pm::free(mov), pm::Status::Ok);
    CHECK(g_destroy_calls == 2); // movable object never had a callback

    // A dead reference can no longer install one.
    CHECK_ST(pm::set_destroy_fn(mov, &counting_destroy), pm::Status::InvalidRef);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R20) task-book v2 11.1 item 11: compact fault injection -- a damaged
//       descriptor must abort the maintenance BEFORE any memmove and leave
//       the pool Running with every payload intact.
// ---------------------------------------------------------------------------
static void test_compact_fault_injection() {
    printf("  [R20] compact refuses damaged descriptors pre-move\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
    pm::RawRef m[3];
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK_ST(pm::alloc(pool, 512, 8, 0, i, m[i]), pm::Status::Ok);
        fill(m[i], 512, 900 + i);
    }
    CHECK_ST(pm::free(m[1]), pm::Status::Ok); // a hole, so compact would move

    using namespace pm::internal;
    ObjectDesc& d = g().objects[m[2].index];
    uint32_t const sb = d.block_size;
    uint8_t* const sa = d.address;
    uint32_t const ss = d.size;

    auto refuse_and_stay_put = [&]() {
        CHECK_ST(pm::compact(pool), pm::Status::CorruptMetadata);
        CHECK(get_stats_state(pool) == 1); // Running again, nothing moved
    };
    auto surviving_payloads = [&]() {
        verify(m[0], 512, 900);
        verify(m[2], 512, 902);
    };

    d.address = sa + 4; // unaligned block start
    refuse_and_stay_put();
    d.address = sa;
    surviving_payloads();

    d.block_size = 4096; // overlaps the neighbouring block
    refuse_and_stay_put();
    d.block_size = sb;
    surviving_payloads();

    d.size = sb; // payload no longer fits its block
    refuse_and_stay_put();
    d.size = ss;
    surviving_payloads();

    d.block_size = PM_MIN_BLOCK - 8; // below the minimum block
    refuse_and_stay_put();
    d.block_size = sb;
    surviving_payloads();

    CHECK_ST(pm::validate(pool), pm::Status::Ok);
    CHECK_ST(pm::compact(pool), pm::Status::Ok); // healthy again
    surviving_payloads();
    CHECK_ST(pm::free(m[0]), pm::Status::Ok);
    CHECK_ST(pm::free(m[2]), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R21) task-book v2 11.1 item 12: every metadata domain is independently
//       audited -- statistics, order links, bitmaps, physical headers and the
//       prev_size chain each have to be caught on their own.
// ---------------------------------------------------------------------------
static void test_metadata_domain_corruption() {
    printf("  [R21] per-domain metadata corruption detection\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{}, b{};
    CHECK_ST(pm::alloc(pool, 256, 8, 0, 1, a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 256, 8, 0, 2, b), pm::Status::Ok);
    fill(a, 256, 3);
    fill(b, 256, 4);

    using namespace pm::internal;
    Pool& P = g().pools[pool];
    uint8_t* const blk_a = g().objects[a.index].address - BLOCK_HEADER_SIZE;
    auto rd32 = [](void const* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    auto wr32 = [](void* p, uint32_t v) { memcpy(p, &v, 4); };

    // (1) used_bytes
    uint32_t const saved_used = P.used_bytes;
    P.used_bytes += 8;
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.used_bytes = saved_used;
    VALIDATE(pool);

    // (2) free_bytes
    uint32_t const saved_free = P.free_bytes;
    P.free_bytes += 8;
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.free_bytes = saved_free;
    VALIDATE(pool);

    // (3) fragment_bytes
    uint32_t const saved_frag = P.fragment_bytes;
    P.fragment_bytes += 8;
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.fragment_bytes = saved_frag;
    VALIDATE(pool);

    // (4) live object count
    uint32_t const saved_live = P.live_objects;
    P.live_objects += 1;
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.live_objects = saved_live;
    VALIDATE(pool);

    // (5) address-order link hiding the second object
    uint32_t const saved_next = g().objects[a.index].addr_next;
    g().objects[a.index].addr_next = NO_ORDER;
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    g().objects[a.index].addr_next = saved_next;
    VALIDATE(pool);

    // (6) sl_bitmap bit with no list behind it
    uint32_t ef = FL_COUNT, es = SL_COUNT;
    for (uint32_t f = 0; f < FL_COUNT && ef == FL_COUNT; ++f)
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
            if (P.bins.head[f][sl] == NULL_OFF) { ef = f; es = sl; break; }
    CHECK(ef < FL_COUNT);
    uint16_t const saved_sl = P.bins.sl_bitmap[ef];
    P.bins.sl_bitmap[ef] = (uint16_t)(saved_sl | (1u << es));
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.bins.sl_bitmap[ef] = saved_sl;
    VALIDATE(pool);

    // (7) fl_bitmap bit with no level behind it
    uint32_t const saved_fl = P.bins.fl_bitmap;
    P.bins.fl_bitmap |= 1u << (FL_COUNT - 1);
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.bins.fl_bitmap = saved_fl;
    VALIDATE(pool);

    // (8) physical header of a live block disagreeing with its descriptor
    uint32_t const saved_hdr = rd32(blk_a);
    wr32(blk_a, saved_hdr + 8);
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    wr32(blk_a, saved_hdr);
    VALIDATE(pool);

    // (9) free-list head pushed outside the pool
    uint32_t hf = FL_COUNT, hs = SL_COUNT;
    for (uint32_t f = 0; f < FL_COUNT && hf == FL_COUNT; ++f)
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
            if (P.bins.head[f][sl] != NULL_OFF) { hf = f; hs = sl; break; }
    CHECK(hf < FL_COUNT);
    uint32_t const real_head = P.bins.head[hf][hs];
    P.bins.head[hf][hs] = 0xFFFFFFF0u;
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    P.bins.head[hf][hs] = real_head;
    VALIDATE(pool);

    // (10) prev_size of the first block (must be 0 at the pool start)
    uint32_t const saved_prev = rd32(blk_a + 4);
    wr32(blk_a + 4, 24);
    CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
    wr32(blk_a + 4, saved_prev);
    VALIDATE(pool);

    verify(a, 256, 3);
    verify(b, 256, 4);
    CHECK_ST(pm::free(a), pm::Status::Ok);
    CHECK_ST(pm::free(b), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// Round-3 fault-injection scaffolding: byte-exact snapshots of every metadata
// domain a maintenance operation must not touch on a planning failure.
// ---------------------------------------------------------------------------
namespace {
using pm::internal::ObjectDesc;
using pm::internal::Pool;

// Fault-injection snapshot storage. The device's static DRAM is nearly
// exhausted by the Auto Zone plus the fixed metadata, so the TEST
// SCAFFOLDING (not the library -- the core stays allocation-free) takes one
// ~9.5 KiB block from the free heap at first use and keeps it for the run.
struct SnapBufs {
    uint8_t zone[8 * 1024]; // Auto Zone byte-range snapshot (2 segments max)
    uint8_t pool_a[sizeof(Pool)];
    uint8_t pool_b[sizeof(Pool)];
    uint8_t desc[3][sizeof(ObjectDesc)];
};
SnapBufs* g_snap = nullptr;

void snap_all(pm::PoolId a, pm::PoolId b, pm::RawRef const* refs, uint32_t nrefs,
              void const* zone_from, uint32_t zone_len) {
    using namespace pm::internal;
    if (!g_snap) g_snap = static_cast<SnapBufs*>(malloc(sizeof(SnapBufs)));
    CHECK(g_snap != nullptr);
    if (!g_snap) return;
    memcpy(g_snap->pool_a, &g().pools[a], sizeof(Pool));
    memcpy(g_snap->pool_b, &g().pools[b], sizeof(Pool));
    for (uint32_t i = 0; i < nrefs && i < 3; ++i)
        memcpy(g_snap->desc[i], &g().objects[refs[i].index], sizeof(ObjectDesc));
    if (zone_len > sizeof(g_snap->zone)) zone_len = sizeof(g_snap->zone);
    memcpy(g_snap->zone, zone_from, zone_len);
}

void check_all_unchanged(pm::PoolId a, pm::PoolId b, pm::RawRef const* refs, uint32_t nrefs,
                         void const* zone_from, uint32_t zone_len) {
    using namespace pm::internal;
    auto first_diff = [](uint8_t const* x, uint8_t const* y, size_t n) -> long {
        for (size_t k = 0; k < n; ++k) if (x[k] != y[k]) return (long)k;
        return -1;
    };
    if (!g_snap) { ++g_fails; printf("    snap buffers missing\n"); return; }
    if (memcmp(&g().pools[a], g_snap->pool_a, sizeof(Pool)) != 0) {
        long k = first_diff(reinterpret_cast<uint8_t const*>(&g().pools[a]), g_snap->pool_a, sizeof(Pool));
        printf("    DIFF pool a byte %ld: now %02x snap %02x\n", k,
               reinterpret_cast<uint8_t const*>(&g().pools[a])[k], g_snap->pool_a[k]);
    }
    CHECK(memcmp(&g().pools[a], g_snap->pool_a, sizeof(Pool)) == 0);
    if (memcmp(&g().pools[b], g_snap->pool_b, sizeof(Pool)) != 0) {
        long k = first_diff(reinterpret_cast<uint8_t const*>(&g().pools[b]), g_snap->pool_b, sizeof(Pool));
        printf("    DIFF pool b byte %ld: now %02x snap %02x\n", k,
               reinterpret_cast<uint8_t const*>(&g().pools[b])[k], g_snap->pool_b[k]);
    }
    CHECK(memcmp(&g().pools[b], g_snap->pool_b, sizeof(Pool)) == 0);
    for (uint32_t i = 0; i < nrefs && i < 3; ++i) {
        if (memcmp(&g().objects[refs[i].index], g_snap->desc[i], sizeof(ObjectDesc)) != 0) {
            long k = first_diff(reinterpret_cast<uint8_t const*>(&g().objects[refs[i].index]),
                                g_snap->desc[i], sizeof(ObjectDesc));
            printf("    DIFF desc[%u] (slot %u) byte %ld: now %02x snap %02x\n",
                   (unsigned)i, (unsigned)refs[i].index, k,
                   reinterpret_cast<uint8_t const*>(&g().objects[refs[i].index])[k],
                   g_snap->desc[i][k]);
        }
        CHECK(memcmp(&g().objects[refs[i].index], g_snap->desc[i], sizeof(ObjectDesc)) == 0);
    }
    if (zone_len > sizeof(g_snap->zone)) zone_len = sizeof(g_snap->zone);
    if (memcmp(zone_from, g_snap->zone, zone_len) != 0) {
        long k = first_diff(reinterpret_cast<uint8_t const*>(zone_from), g_snap->zone, zone_len);
        printf("    DIFF zone byte %ld (abs %ld): now %02x snap %02x\n", k,
               (long)(reinterpret_cast<uint8_t const*>(zone_from) - g().zone) + k,
               reinterpret_cast<uint8_t const*>(zone_from)[k], g_snap->zone[k]);
    }
    CHECK(memcmp(zone_from, g_snap->zone, zone_len) == 0);
}
} // namespace

// ---------------------------------------------------------------------------
// (R22) round-3 guide 3.2: merge must be a read-only-planned transaction.
// Every planning failure returns CorruptMetadata with ZERO observable change:
// pool structs (state, segments, statistics, bins), descriptors (pool_id,
// address_epoch, order links) and the Auto Zone ranges stay byte-identical.
// ---------------------------------------------------------------------------
static void test_merge_transaction_faults() {
    printf("  [R22] merge transaction faults are side-effect free\n");
    pm::PoolId tgt = 0, src = 0;
    pm::RawRef ot{}, os{}, pin{};
    uint8_t* os_blk = nullptr;

    // Each injection gets a freshly built fixture: a merge that (wrongly)
    // half-commits consumes the source pool, and every case must stay
    // isolated -- on the fixed code each case is byte-exact anyway.
    auto rebuild = [&]() {
        using namespace pm::internal;
        memset(&g(), 0, sizeof(GlobalState)); // white-box power cycle
        pm::Config cfg{g_zone, sizeof(g_zone), 4096};
        CHECK_ST(pm::init(cfg), pm::Status::Ok);
        CHECK_ST(pm::create_pool(tgt, 1), pm::Status::Ok); // seg 0 (below)
        CHECK_ST(pm::create_pool(src, 1), pm::Status::Ok); // seg 1 (above)
        pm::RawRef tpad{}, spad{};
        CHECK_ST(pm::alloc(tgt, 256, 8, 0, 1, ot), pm::Status::Ok);
        CHECK_ST(pm::alloc(tgt, 512, 8, 0, 2, tpad), pm::Status::Ok);
        CHECK_ST(pm::alloc(src, 300, 8, 0, 3, os), pm::Status::Ok);
        CHECK_ST(pm::alloc(src, 512, 8, 0, 4, spad), pm::Status::Ok);
        CHECK_ST(pm::alloc(src, 128, 8, pm::PM_PINNED, 5, pin), pm::Status::Ok);
        fill(ot, 256, 11); fill(tpad, 512, 12); fill(os, 300, 13);
        fill(spad, 512, 14); fill(pin, 128, 15);
        // Holes on both sides, so the merge has real moving to do on success.
        CHECK_ST(pm::free(tpad), pm::Status::Ok);
        CHECK_ST(pm::free(spad), pm::Status::Ok);
        os_blk = g().objects[os.index].address - BLOCK_HEADER_SIZE;
        VALIDATE(tgt); VALIDATE(src);
    };

    // (1) source descriptor claims the wrong pool
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        uint16_t const saved = g().objects[os.index].pool_id;
        g().objects[os.index].pool_id = tgt;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK(get_stats_state(src) == 1 && get_stats_state(tgt) == 1);
        g().objects[os.index].pool_id = saved;
        VALIDATE(src); VALIDATE(tgt);
    }
    // (2) source descriptor address
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        uint8_t* const saved = g().objects[os.index].address;
        g().objects[os.index].address = g().zone + 5 * 4096 + 64;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        g().objects[os.index].address = saved;
        VALIDATE(src); VALIDATE(tgt);
    }
    // (3) source block header
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        uint32_t hdr;
        memcpy(&hdr, os_blk, 4);
        uint32_t const bad = hdr + 8;
        memcpy(os_blk, &bad, 4);
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        memcpy(os_blk, &hdr, 4);
        VALIDATE(src); VALIDATE(tgt);
    }
    // (4) target bins
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        Pool& T = g().pools[tgt];
        uint32_t hf = FL_COUNT, hs = SL_COUNT;
        for (uint32_t f = 0; f < FL_COUNT && hf == FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                if (T.bins.head[f][sl] != NULL_OFF) { hf = f; hs = sl; break; }
        CHECK(hf < FL_COUNT);
        uint32_t const saved_head = T.bins.head[hf][hs];
        T.bins.head[hf][hs] = 0xFFFFFFF0u;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        T.bins.head[hf][hs] = saved_head;
        VALIDATE(tgt); VALIDATE(src);
    }
    // (5) target statistics corrupted: used_bytes no longer matches the
    //     descriptors (merge must audit the target before touching anything)
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        uint32_t const saved_used = g().pools[tgt].used_bytes;
        g().pools[tgt].used_bytes = saved_used + 8;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        g().pools[tgt].used_bytes = saved_used;
        VALIDATE(tgt); VALIDATE(src);
    }
    // (6) Paused entry states
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        CHECK_ST(pm::pause(src), pm::Status::Ok);
        CHECK_ST(pm::pause(tgt), pm::Status::Ok);
        uint16_t const saved = g().objects[os.index].pool_id;
        g().objects[os.index].pool_id = tgt;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK(get_stats_state(src) == 2 && get_stats_state(tgt) == 2);
        g().objects[os.index].pool_id = saved;
        CHECK_ST(pm::resume(src), pm::Status::Ok);
        CHECK_ST(pm::resume(tgt), pm::Status::Ok);
        VALIDATE(src); VALIDATE(tgt);
    }
    // (7) addr_next cycle
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        ObjectDesc& d_os = g().objects[os.index];
        uint32_t const saved_next = d_os.addr_next;
        d_os.addr_next = pin.index;                    // os -> pin -> os -> ...
        g().objects[pin.index].addr_next = os.index;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        d_os.addr_next = saved_next;
        g().objects[pin.index].addr_next = NO_ORDER;
        VALIDATE(src); VALIDATE(tgt);
    }
    // (8) order_head
    {
        rebuild();
        pm::RawRef const refs[3] = {ot, os, pin};
        using namespace pm::internal;
        uint32_t const saved_head = g().pools[src].order_head;
        g().pools[src].order_head = PM_MAX_OBJECTS;
        snap_all(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        CHECK_ST(pm::merge(src, tgt), pm::Status::CorruptMetadata);
        check_all_unchanged(tgt, src, refs, 3, seg_base(g().pools[tgt].segment_first), 2 * 4096);
        g().pools[src].order_head = saved_head;
        VALIDATE(src); VALIDATE(tgt);
    }

    // Healthy merge still succeeds after all repairs.
    rebuild();
    CHECK_ST(pm::merge(src, tgt), pm::Status::Ok);
    CHECK(get_stats_state(src) == 0 && get_stats_state(tgt) == 1);
    VALIDATE(tgt);
    pm::RawRef c_ot = ot; c_ot.pool_hint = pm::CROSS_HINT;
    pm::RawRef c_os = os; c_os.pool_hint = pm::CROSS_HINT;
    pm::RawRef c_pin = pin; c_pin.pool_hint = pm::CROSS_HINT;
    verify(c_ot, 256, 11);
    verify(c_os, 300, 13);
    verify(c_pin, 128, 15);
    CHECK(desc_pool(ot) == tgt && desc_pool(os) == tgt && desc_pool(pin) == tgt);
    CHECK_ST(pm::free(c_ot), pm::Status::Ok);
    CHECK_ST(pm::free(c_os), pm::Status::Ok);
    CHECK_ST(pm::free(c_pin), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(tgt), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R23) round-3 guide 5.2: a descriptor whose block lies outside its pool is
// refused by validate, resolve, borrow_begin AND free -- the address derived
// from it must never be handed out, and free must not run the destroy
// callback or touch any state before the physical proof succeeds.
// ---------------------------------------------------------------------------
static void test_descriptor_outside_pool() {
    printf("  [R23] out-of-pool descriptors refused at every entry\n");
    fresh();
    pm::PoolId a{}, b{};
    CHECK_ST(pm::create_pool(a, 1), pm::Status::Ok);
    CHECK_ST(pm::create_pool(b, 1), pm::Status::Ok);
    pm::RawRef y{}, a1{};
    CHECK_ST(pm::alloc(b, 128, 8, 0, 1, y), pm::Status::Ok);
    CHECK_ST(pm::alloc(a, 128, 8, pm::PM_PINNED, 2, a1), pm::Status::Ok);
    CHECK_ST(pm::set_destroy_fn(a1, &counting_destroy), pm::Status::Ok);
    fill(a1, 128, 21);
    fill(y, 128, 22);

    using namespace pm::internal;
    ObjectDesc& d = g().objects[a1.index];
    Pool& P = g().pools[a];
    uint8_t* const saved_addr = d.address;
    uint32_t const saved_bs = d.block_size;
    pm::RawRef const refs[2] = {a1, y};

    auto expect_all_refuse = [&]() {
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(1)); // failure paths must clear it (R26)
        CHECK_ST(pm::validate(a), pm::Status::CorruptMetadata);
        CHECK_ST(pm::resolve(a1, 128, 1, p), pm::Status::CorruptMetadata);
        CHECK(p == nullptr);
        p = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        CHECK_ST(pm::borrow_begin(a1, 128, 1, p), pm::Status::CorruptMetadata);
        CHECK(p == nullptr);
        int const dc = g_destroy_calls;
        snap_all(a, b, refs, 2, seg_base(P.segment_first), 1 * 4096);
        CHECK_ST(pm::free(a1), pm::Status::CorruptMetadata);
        CHECK(g_destroy_calls == dc);  // no callback before the physical proof
        check_all_unchanged(a, b, refs, 2, seg_base(P.segment_first), 1 * 4096);
    };
    auto repair = [&]() {
        d.address = saved_addr;
        d.block_size = saved_bs;
        VALIDATE(a);
    };

    // (a) aligned address in the zone but beyond pool a's end
    d.address = g().zone + 5 * 4096 + 64;
    expect_all_refuse();
    repair();
    // (b) aligned address inside ANOTHER live pool
    d.address = g().objects[y.index].address;
    expect_all_refuse();
    repair();
    // (c) address before the zone base
    d.address = g().zone - 128;
    expect_all_refuse();
    repair();
    // (d) forged near-UINTPTR_MAX field, 8-aligned, never dereferenced
    d.address = reinterpret_cast<uint8_t*>(~(uintptr_t)0x7);
    expect_all_refuse();
    repair();
    // (e) block_size passes every descriptor-local check but crosses the
    //     pool tail (a pool-filling block would be geometrically legal)
    d.block_size = 2 * 4096 + 8;
    expect_all_refuse();
    repair();

    // Healthy again: the callback runs exactly once on the real free.
    verify(a1, 128, 21);
    int const dc = g_destroy_calls;
    CHECK_ST(pm::free(a1), pm::Status::Ok);
    CHECK(g_destroy_calls == dc + 1);
    CHECK_ST(pm::free(y), pm::Status::Ok);
    VALIDATE(a);
    VALIDATE(b);
    done();
}

// ---------------------------------------------------------------------------
// (R24) round-3 guide 6.1: free() verifies the physical neighbours (own
// header, prev_size chain, successor sanity, free-list membership with
// reciprocal links) BEFORE calling the destroy callback or touching any
// state. A corrupt header must cost nothing: no callback, no statistics, no
// bins write. A destroy callback re-entering the allocator for other objects
// must leave the surrounding free() consistent.
// ---------------------------------------------------------------------------
static pm::PoolId g_reenter_pool = 0xFFFF;
static int g_reenter_calls = 0;
// cppcheck-suppress constParameterCallback
static void reenter_destroy(void* p) {
    // Legal re-entrancy: the callback allocates and frees OTHER objects in
    // the SAME pool the enclosing free() is merging in.
    ++g_reenter_calls;
    pm::RawRef r{};
    if (pm::alloc(g_reenter_pool, 64, 8, 0, 77, r) == pm::Status::Ok)
        CHECK_ST(pm::free(r), pm::Status::Ok);
    (void)p;
}

static void test_free_physical_header_faults() {
    printf("  [R24] free verifies physical headers before any side effect\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 1), pm::Status::Ok);
    // Layout: [pad0 128][pin 128][pad2 512]; pad0 freed -> free predecessor.
    pm::RawRef pad0{}, pin{}, pad2{};
    CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, pad0), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_PINNED, 2, pin), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 512, 8, 0, 3, pad2), pm::Status::Ok);
    CHECK_ST(pm::set_destroy_fn(pin, &counting_destroy), pm::Status::Ok);
    fill(pad0, 128, 31); fill(pin, 128, 32); fill(pad2, 512, 33);
    CHECK_ST(pm::free(pad0), pm::Status::Ok);
    VALIDATE(pool);

    using namespace pm::internal;
    ObjectDesc const& d = g().objects[pin.index];
    Pool const& P = g().pools[pool];
    uint8_t* blk = d.address - BLOCK_HEADER_SIZE;
    uint8_t* pblk = seg_base(P.segment_first);          // pad0's free block
    uint8_t* nblk = blk + d.block_size;     // pad2's live block
    pm::RawRef const refs[3] = {pad0, pin, pad2};
    auto rd32 = [](void const* q) { uint32_t v; memcpy(&v, q, 4); return v; };
    auto wr32 = [](void* q, uint32_t v) { memcpy(q, &v, 4); };

    uint32_t const own0 = rd32(blk);        // 136, used
    uint32_t const pv0 = rd32(blk + 4);     // 136, pad0's block size
    uint32_t const pfree0 = rd32(pblk);     // 136 | FREE
    uint32_t const nown0 = rd32(nblk);      // 520, used

    auto refuses = [&]() {
        snap_all(pool, pool, refs, 3, seg_base(P.segment_first), 1 * 4096);
        int const dc = g_destroy_calls;
        CHECK_ST(pm::free(pin), pm::Status::CorruptMetadata);
        CHECK(g_destroy_calls == dc);  // callback waits for the physical proof
        check_all_unchanged(pool, pool, refs, 3, seg_base(P.segment_first), 1 * 4096);
    };

    // (1) own header size disagrees with the descriptor
    wr32(blk, own0 + 8);
    refuses();
    wr32(blk, own0);
    // (2) own header claims FREE
    wr32(blk, own0 | 1u);
    refuses();
    wr32(blk, own0);
    // (3) prev_size runs past the pool start
    wr32(blk + 4, 2 * 4096 + 16);
    refuses();
    wr32(blk + 4, pv0);
    // (4) prev_size points at a word that is not a matching block header
    wr32(blk + 4, 8); // lands inside pad0's free-list link area
    refuses();
    wr32(blk + 4, pv0);
    // (5) predecessor header bent to another size: the prev_size chain no
    //     longer closes, and its (wrong) bin holds no such block
    wr32(pblk, (own0 + 8) | BLOCK_FREE_BIT);
    wr32(blk + 4, own0 + 8);
    refuses();
    wr32(pblk, pfree0);
    wr32(blk + 4, pv0);
    // (6) live successor faking the free bit: not present in any bin
    wr32(nblk, nown0 | BLOCK_FREE_BIT);
    refuses();
    wr32(nblk, nown0);
    // (7) successor free with an insane size (past the pool end)
    wr32(nblk, 0x7FF8u | BLOCK_FREE_BIT);
    refuses();
    wr32(nblk, nown0);
    // (8) successor free with a bent size class: no such block in that bin
    wr32(nblk, (nown0 + 8) | BLOCK_FREE_BIT);
    refuses();
    wr32(nblk, nown0);

    VALIDATE(pool);

    // (9) reentrant callback allocating in the SAME pool while free() is
    //     merging: the enclosing free must re-verify and stay consistent.
    {
        pm::RawRef pin2{};
        CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_PINNED, 5, pin2), pm::Status::Ok);
        CHECK_ST(pm::set_destroy_fn(pin2, &reenter_destroy), pm::Status::Ok);
        fill(pin2, 128, 35);
        g_reenter_pool = pool;
        int const dc = g_destroy_calls;
        int const rc0 = g_reenter_calls;
        CHECK_ST(pm::free(pin2), pm::Status::Ok);
        CHECK(g_reenter_calls == rc0 + 1); // callback ran exactly once
        CHECK(g_destroy_calls == dc);      // the re-entrancy freed no pinned obj
        g_reenter_pool = 0xFFFF;
        VALIDATE(pool);
    }

    // Healthy free: callback runs exactly once, neighbours merge cleanly.
    verify(pin, 128, 32);
    verify(pad2, 512, 33);
    int const dc = g_destroy_calls;
    CHECK_ST(pm::free(pin), pm::Status::Ok);
    CHECK(g_destroy_calls == dc + 1);
    CHECK_ST(pm::free(pad2), pm::Status::Ok);
    pm::PoolStats st = pm::get_stats(pool);
    CHECK(st.largest_free_block == 1 * 4096); // everything coalesced back
    VALIDATE(pool);
    CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R25) round-3 guide 7.1: borrow_end validates the token and decrements
// inside ONE critical section. A duplicate, stale or wrong-pool end must
// never move any counter (Release; Debug asserts these as caller bugs).
// The concurrent half of this requirement runs on the dual-core device
// (tests/concurrency_esp32.cpp) where PM_LOCK is a real spinlock.
// ---------------------------------------------------------------------------
static void test_borrow_end_token_discipline() {
    printf("  [R25] borrow_end: single locked check-and-decrement\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{}, b{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 2, b), pm::Status::Ok);

    using namespace pm::internal;
    void* p = nullptr;
    CHECK_ST(pm::borrow_begin(a, 64, 1, p), pm::Status::Ok);
    CHECK(g().pools[pool].borrow_count == 1);
    CHECK(g().objects[a.index].active_borrows == 1);
    pm::borrow_end(a);
    CHECK(g().pools[pool].borrow_count == 0);
    CHECK(g().objects[a.index].active_borrows == 0);

#if !PM_DEBUG
    // Refusal paths: Debug asserts them as caller bugs (and would abort), so
    // the no-side-effect guarantee is exercised on Release builds.
    auto counters = [&]() {
        return g().pools[pool].borrow_count * 16u + g().objects[a.index].active_borrows;
    };
    CHECK_ST(pm::borrow_begin(a, 64, 1, p), pm::Status::Ok);
    pm::borrow_end(a);
    uint32_t const c0 = counters();
    CHECK(c0 == 0);

    pm::borrow_end(a); // duplicate end: no underflow
    CHECK(counters() == c0);

    pm::RawRef stale = a;
    stale.generation = (uint16_t)(a.generation + 1);
    pm::borrow_end(stale); // stale token: refused
    CHECK(counters() == c0);

    pm::RawRef wrong = a;
    wrong.pool_hint = (uint16_t)(pool + 1); // concrete but wrong pool
    pm::borrow_end(wrong);                  // refused
    CHECK(counters() == c0);

    pm::RawRef bogus{};
    bogus.index = PM_MAX_OBJECTS + 3;
    bogus.generation = 1;
    pm::borrow_end(bogus); // out of range: ignored
    CHECK(counters() == c0);
    CHECK(g().pools[pool].borrow_count == 0);
#endif

    CHECK_ST(pm::free(a), pm::Status::Ok);
    CHECK_ST(pm::free(b), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R26) round-3 guide P2: a failed resolve/borrow_begin writes nullptr, so a
// caller that reuses an old pointer variable can never keep a stale address.
// ---------------------------------------------------------------------------
static void test_resolve_clears_output() {
    printf("  [R26] failed resolve/borrow_begin clear the output\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef a{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, a), pm::Status::Ok);
    fill(a, 64, 5);

    auto expect_cleared = [&](pm::RawRef ref, uint32_t sz, uint32_t al, pm::Status want) {
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        CHECK_ST(pm::resolve(ref, sz, al, p), want);
        if (want == pm::Status::Ok) CHECK(p != nullptr); // success writes through
        else CHECK(p == nullptr);                        // failure clears
        p = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        CHECK_ST(pm::borrow_begin(ref, sz, al, p), want);
        if (want == pm::Status::Ok) CHECK(p != nullptr);
        else CHECK(p == nullptr);
        if (want == pm::Status::Ok) pm::borrow_end(ref);
    };

    expect_cleared(a, 64, 1, pm::Status::Ok); // sanity: success writes through

    pm::RawRef bad_index = a;
    bad_index.index = PM_MAX_OBJECTS + 1;
    expect_cleared(bad_index, 64, 1, pm::Status::InvalidRef);

    pm::RawRef zero_gen = a;
    zero_gen.generation = 0;
    expect_cleared(zero_gen, 64, 1, pm::Status::InvalidRef);

    pm::RawRef oob = a;
    oob.offset = 64; // at the very end: nothing left to access
    expect_cleared(oob, 1, 1, pm::Status::InvalidRef);

    pm::RawRef oob2 = a;
    oob2.offset = 60;
    expect_cleared(oob2, 8, 1, pm::Status::InvalidRef); // 60+8 > 64

    pm::RawRef misaligned = a;
    misaligned.offset = 1;
    expect_cleared(misaligned, 4, 4, pm::Status::InvalidAlignment);

    pm::RawRef wrong_pool = a;
    wrong_pool.pool_hint = (uint16_t)(pool + 1);
    expect_cleared(wrong_pool, 64, 1, pm::Status::PoolChanged);

    // Paused pool: Busy, and the output is still cleared.
    CHECK_ST(pm::pause(pool), pm::Status::Ok);
    expect_cleared(a, 64, 1, pm::Status::Busy);
    CHECK_ST(pm::resume(pool), pm::Status::Ok);

    // Corrupt descriptor: refused everywhere, output cleared.
    {
        using namespace pm::internal;
        ObjectDesc& d = g().objects[a.index];
        uint32_t const saved = d.size;
        d.size = 0; // a live object always has a payload: desc-local refusal
        expect_cleared(a, 64, 1, pm::Status::CorruptMetadata);
        d.size = saved;
    }

    // Stale generation after a free/realloc cycle.
    pm::RawRef stale = a;
    CHECK_ST(pm::free(a), pm::Status::Ok);
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 2, a), pm::Status::Ok);
    CHECK(stale.index == a.index && stale.generation != a.generation);
    expect_cleared(stale, 64, 1, pm::Status::InvalidRef);

    CHECK_ST(pm::free(a), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R27) round-3 guide 7.2: the local-binding boundary is the documented one.
// Constructing a pm_local_ptr from a caller-supplied RawRef is a deliberate
// low-level capability; the binding is enforced at RESOLUTION time (a forged
// concrete pool hint gets PoolChanged, never access), and CROSS_HINT still
// cannot bootstrap a local pointer. This test pins that exact semantics so
// the implementation and the README cannot drift apart.
// ---------------------------------------------------------------------------
static void test_local_binding_semantics() {
    printf("  [R27] local binding enforced at resolution, not construction\n");
    fresh();
    pm::PoolId pool0{}, pool1{};
    CHECK_ST(pm::create_pool(pool0, 2), pm::Status::Ok);
    CHECK_ST(pm::create_pool(pool1, 2), pm::Status::Ok);
    auto made = pm::pm_make<uint32_t>(pool0);
    CHECK(made.ok());
    auto pod = made.value;

    // (1) forged local binding to another concrete pool: construction is
    // possible, but every resolution refuses with PoolChanged -- the object
    // is unreachable through a pool it does not live in.
    pm::RawRef forged = pod.raw();
    forged.pool_hint = pool1;
    pm::pm_local_ptr<uint32_t> evil{forged};
    CHECK(evil.valid()); // not invalidated at construction (documented)
    CHECK_ST(evil.try_borrow().status, pm::Status::PoolChanged);
    CHECK_ST(evil.peek().status, pm::Status::PoolChanged);

    // (2) CROSS_HINT still cannot bootstrap a local pointer (R4's rule).
    pm::RawRef crossified = pod.raw();
    crossified.pool_hint = pm::CROSS_HINT;
    pm::pm_local_ptr<uint32_t> smuggled{crossified};
    CHECK(!smuggled.valid());
    CHECK_ST(smuggled.try_borrow().status, pm::Status::InvalidRef);

    // (3) a forged hint equal to the true pool is indistinguishable from the
    // genuine binding: same object, same access rights.
    pm::RawRef twin = pod.raw();
    twin.pool_hint = pool0;
    pm::pm_local_ptr<uint32_t> twin_ptr{twin};
    {
        auto acc = twin_ptr.try_borrow();
        CHECK(acc.ok());
        *acc.value = 7;
    }
    {
        auto acc = pod.try_borrow();
        CHECK(acc.ok());
        CHECK(*acc.value == 7);
    }

    // (4) the explicit cross path forces CROSS_HINT and works.
    auto cross = pm::pm_cross_ref<uint32_t>(pod.raw());
    CHECK(cross.pool_hint() == pm::CROSS_HINT);
    CHECK_ST(cross.try_borrow().status, pm::Status::Ok);

    CHECK_ST(pm::pm_destroy(pod), pm::Status::Ok);
    CHECK_ST(pm::destroy_pool(pool1), pm::Status::Ok);
    done();
}

// ---------------------------------------------------------------------------
// (R28) round-3 guide 8: maintenance publishes Running only in the final
// commit. A refused operation leaves no maintenance state behind, bumps no
// epoch, creates no pool, and writes no out parameter.
// ---------------------------------------------------------------------------
static void test_state_publication() {
    printf("  [R28] state published only at the final commit\n");

    // (a) failed compact: no epoch bump, Running restored, payload intact.
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef a{};
        CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, a), pm::Status::Ok);
        fill(a, 128, 1);
        CHECK_ST(pm::compact(pool), pm::Status::Ok); // epoch 1 -> 2
        CHECK(pm::get_stats(pool).structure_epoch == 2);
        {
            using namespace pm::internal;
            uint32_t const saved = g().objects[a.index].block_size;
            g().objects[a.index].block_size = saved + 8;
            CHECK_ST(pm::compact(pool), pm::Status::CorruptMetadata);
            g().objects[a.index].block_size = saved;
        }
        CHECK(get_stats_state(pool) == 1); // no maintenance state left
        CHECK(pm::get_stats(pool).structure_epoch == 2); // failure published nothing
        verify(a, 128, 1);
        VALIDATE(pool);
        CHECK_ST(pm::free(a), pm::Status::Ok);
        done();
    }

    // (b) failed merge: both pools restored, target epoch untouched, the
    // object still belongs to the source pool.
    {
        fresh();
        pm::PoolId s{}, t{};
        CHECK_ST(pm::create_pool(t, 2), pm::Status::Ok);
        CHECK_ST(pm::create_pool(s, 2), pm::Status::Ok);
        pm::RawRef os{};
        CHECK_ST(pm::alloc(s, 128, 8, 0, 1, os), pm::Status::Ok);
        fill(os, 128, 2);
        uint32_t const t_epoch = pm::get_stats(t).structure_epoch;
        uint32_t const s_epoch = pm::get_stats(s).structure_epoch;
        {
            using namespace pm::internal;
            uint32_t const saved = g().objects[os.index].block_size;
            g().objects[os.index].block_size = saved + 8;
            CHECK_ST(pm::merge(s, t), pm::Status::CorruptMetadata);
            g().objects[os.index].block_size = saved;
        }
        CHECK(get_stats_state(s) == 1 && get_stats_state(t) == 1);
        CHECK(pm::get_stats(t).structure_epoch == t_epoch);
        CHECK(pm::get_stats(s).structure_epoch == s_epoch);
        CHECK(desc_pool(os) == s); // ownership unchanged
        verify(os, 128, 2);
        VALIDATE(s); VALIDATE(t);
        CHECK_ST(pm::free(os), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(t), pm::Status::Ok);
        done();
    }

    // (c) failed split: no pool appears in the table, out_new untouched.
    {
        fresh();
        pm::PoolId s{};
        CHECK_ST(pm::create_pool(s, 4), pm::Status::Ok);
        pm::RawRef big{}, pin{};
        CHECK_ST(pm::alloc(s, 8000, 8, 0, 1, big), pm::Status::Ok);
        CHECK_ST(pm::alloc(s, 1024, 8, pm::PM_PINNED, 2, pin), pm::Status::Ok);
        uint32_t live_pools = 0;
        for (uint32_t i = 0; i < 16; ++i)
            if (get_stats_state((pm::PoolId)i) != 0) ++live_pools;
        CHECK(live_pools == 1);
        uint32_t const s_epoch = pm::get_stats(s).structure_epoch;
        pm::PoolId nid = (pm::PoolId)0xBEEF;
        CHECK_ST(pm::split(s, 2, nid), pm::Status::PinnedConflict);
        CHECK(nid == (pm::PoolId)0xBEEF); // out parameter untouched
        live_pools = 0;
        for (uint32_t i = 0; i < 16; ++i)
            if (get_stats_state((pm::PoolId)i) != 0) ++live_pools;
        CHECK(live_pools == 1); // no new pool was published
        CHECK(get_stats_state(s) == 1);
        CHECK(pm::get_stats(s).structure_epoch == s_epoch);
        VALIDATE(s);
        CHECK_ST(pm::free(big), pm::Status::Ok);
        CHECK_ST(pm::free(pin), pm::Status::Ok);
        done();
    }

    // (d) successful merge publishes exactly once: source Empty, target
    // Running, target epoch bumped by one.
    {
        fresh();
        pm::PoolId s{}, t{};
        CHECK_ST(pm::create_pool(t, 2), pm::Status::Ok);
        CHECK_ST(pm::create_pool(s, 2), pm::Status::Ok);
        pm::RawRef os{};
        CHECK_ST(pm::alloc(s, 128, 8, 0, 1, os), pm::Status::Ok);
        fill(os, 128, 3);
        uint32_t const t_epoch = pm::get_stats(t).structure_epoch;
        CHECK_ST(pm::merge(s, t), pm::Status::Ok);
        CHECK(get_stats_state(s) == 0);
        CHECK(get_stats_state(t) == 1);
        CHECK(pm::get_stats(t).structure_epoch == t_epoch + 1);
        pm::RawRef c = os; c.pool_hint = pm::CROSS_HINT;
        verify(c, 128, 3);
        CHECK_ST(pm::free(c), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(t), pm::Status::Ok);
        done();
    }
}

// ---------------------------------------------------------------------------
// (R29) round-4 task book sections 7/8/11: the extended fault matrix --
// reciprocal free-list links, duplicate bin membership, segment fields,
// runtime-state fields, and alloc's own bounded order-list walk. Each case
// names the mechanism it pins; all of them must be side-effect free.
// ---------------------------------------------------------------------------
static void test_round4_fault_matrix() {
    printf("  [R29] extended fault matrix (links, duplicates, segments, state)\n");

    // (1) free refuses a binned free block whose reciprocal links are bent
    //     (free_block_binned verifies the found node's own links).
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef pad0{}, pin{}, pad2{};
        CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, pad0), pm::Status::Ok);
        CHECK_ST(pm::alloc(pool, 128, 8, pm::PM_PINNED, 2, pin), pm::Status::Ok);
        CHECK_ST(pm::alloc(pool, 512, 8, 0, 3, pad2), pm::Status::Ok);
        CHECK_ST(pm::set_destroy_fn(pin, &counting_destroy), pm::Status::Ok);
        fill(pad0, 128, 61); fill(pin, 128, 62); fill(pad2, 512, 63);
        CHECK_ST(pm::free(pad0), pm::Status::Ok);
        VALIDATE(pool);

        using namespace pm::internal;
        Pool const& P = g().pools[pool];
        auto* fb = reinterpret_cast<FreeBlock*>(seg_base(P.segment_first));
        pm::RawRef const refs[3] = {pad0, pin, pad2};
        auto refuses_free_pin = [&]() {
            snap_all(pool, pool, refs, 3, seg_base(P.segment_first), 1 * 4096);
            int const dc = g_destroy_calls;
            CHECK_ST(pm::free(pin), pm::Status::CorruptMetadata);
            CHECK(g_destroy_calls == dc); // callback waits for the proof
            check_all_unchanged(pool, pool, refs, 3, seg_base(P.segment_first), 1 * 4096);
        };
        uint32_t const saved_next = fb->next;
        fb->next = 64; // readable in-pool offset, but not a list neighbour
        refuses_free_pin();
        fb->next = saved_next;
        uint32_t const saved_prev = fb->prev;
        fb->prev = 64;
        refuses_free_pin();
        fb->prev = saved_prev;
        VALIDATE(pool);
        int const dc = g_destroy_calls;
        CHECK_ST(pm::free(pin), pm::Status::Ok); // healthy again
        CHECK(g_destroy_calls == dc + 1);
        CHECK_ST(pm::free(pad2), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
        done();
    }

    // (2) validate detects a free block that also appears as another bin's
    //     head (duplicate membership): the size-class check refuses it.
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef a{};
        CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, a), pm::Status::Ok);
        CHECK_ST(pm::free(a), pm::Status::Ok);
        VALIDATE(pool);

        using namespace pm::internal;
        Pool& P = g().pools[pool];
        uint32_t hf = FL_COUNT, hs = SL_COUNT;
        for (uint32_t f = 0; f < FL_COUNT && hf == FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                if (P.bins.head[f][sl] != NULL_OFF) { hf = f; hs = sl; break; }
        CHECK(hf < FL_COUNT);
        uint32_t const dup = P.bins.head[hf][hs];
        uint32_t ef = FL_COUNT, es = SL_COUNT;
        for (uint32_t f = 0; f < FL_COUNT && ef == FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                if (P.bins.head[f][sl] == NULL_OFF) { ef = f; es = sl; break; }
        CHECK(ef < FL_COUNT);
        // A consistent-looking second head: bitmaps agree, so the walk truly
        // reaches the block under the WRONG size class.
        P.bins.head[ef][es] = dup;
        P.bins.sl_bitmap[ef] = (uint16_t)(P.bins.sl_bitmap[ef] | (1u << es));
        P.bins.fl_bitmap |= 1u << ef;
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        P.bins.head[ef][es] = NULL_OFF;
        P.bins.sl_bitmap[ef] = (uint16_t)(P.bins.sl_bitmap[ef] & ~(1u << es));
        P.bins.fl_bitmap &= ~(1u << ef);
        VALIDATE(pool);
        done();
    }

    // (3) segment fields: a shifted window or a wrong capacity breaks the
    //     byte accounting and must be refused by validate AND by maintenance.
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef a{};
        CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, a), pm::Status::Ok);
        fill(a, 128, 71);
        using namespace pm::internal;
        Pool& P = g().pools[pool];
        uint16_t const saved_first = P.segment_first;
        uint16_t const saved_count = P.segment_count;

        P.segment_first = (uint16_t)(saved_first + 1); // window moved away
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        P.segment_first = saved_first;
        VALIDATE(pool);

        P.segment_count = (uint16_t)(saved_count + 1); // capacity inflated
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata);
        CHECK_ST(pm::compact(pool), pm::Status::CorruptMetadata); // pre-move
        CHECK(get_stats_state(pool) == 1);                        // restored
        P.segment_count = saved_count;
        VALIDATE(pool);
        verify(a, 128, 71);
        CHECK_ST(pm::free(a), pm::Status::Ok);
        done();
    }

    // (4) runtime-state fields: corruption cannot be *audited* (they are
    //     control fields, not invariants), but every affected entry must
    //     refuse safely and recover once repaired.
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef a{};
        CHECK_ST(pm::alloc(pool, 128, 8, 0, 1, a), pm::Status::Ok);
        using namespace pm::internal;
        Pool& P = g().pools[pool];

        P.state = PoolState::Compacting; // phantom maintenance state
        void* p = nullptr;
        pm::RawRef b{};
        CHECK_ST(pm::alloc(pool, 8, 8, 0, 2, b), pm::Status::Busy);
        CHECK_ST(pm::borrow_begin(a, 128, 1, p), pm::Status::Busy);
        CHECK_ST(pm::pause(pool), pm::Status::Busy);
        CHECK_ST(pm::resume(pool), pm::Status::Busy);
        CHECK_ST(pm::compact(pool), pm::Status::Busy);
        pm::PoolId nid{};
        CHECK_ST(pm::split(pool, 1, nid), pm::Status::Busy);
        P.state = PoolState::Running; // repair
        CHECK_ST(pm::alloc(pool, 8, 8, 0, 3, b), pm::Status::Ok);

        P.borrow_count = 1; // phantom borrow: maintenance must refuse
        CHECK_ST(pm::compact(pool), pm::Status::Busy);
        CHECK_ST(pm::split(pool, 1, nid), pm::Status::Busy);
        P.borrow_count = 0; // repair
        CHECK_ST(pm::compact(pool), pm::Status::Ok);

        g().objects[a.index].active_borrows = 2; // phantom object borrow
        CHECK_ST(pm::free(a), pm::Status::Busy);
        g().objects[a.index].active_borrows = 0; // repair
        CHECK_ST(pm::free(a), pm::Status::Ok);
        CHECK_ST(pm::free(b), pm::Status::Ok);
        VALIDATE(pool);
        CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
        done();
    }

    // (5) alloc's own cycle guard: a cyclic order list is refused with
    //     CorruptMetadata in bounded time (previously alloc could hang in
    //     the unbounded insertion walk), the slot is conserved, and the
    //     pool recovers after repair.
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
        pm::RawRef lo{}, hi{};
        CHECK_ST(pm::alloc(pool, 3584, 8, 0, 1, lo), pm::Status::Ok);
        CHECK_ST(pm::alloc(pool, 3584, 8, 0, 2, hi), pm::Status::Ok);
        using namespace pm::internal;
        ObjectDesc& dlo = g().objects[lo.index];
        uint32_t const saved_next = dlo.addr_next;
        dlo.addr_next = lo.index; // cycle at the list head
        // A fresh allocation lands in the tail block (above both objects),
        // so the insertion walk runs into the cycle and must trip its cap.
        pm::RawRef r{};
        CHECK_ST(pm::alloc(pool, 64, 8, 0, 3, r), pm::Status::CorruptMetadata);
        {
            // the refused alloc rolled its slot back: the free chain holds
            // every slot except the two live objects lo and hi
            uint32_t nfree = 0;
            for (uint16_t s = g().free_slot_head;
                 s != NO_SLOT && nfree <= PM_MAX_OBJECTS;
                 s = g().objects[s].next_free_slot)
                ++nfree;
            CHECK(nfree == PM_MAX_OBJECTS - 2);
        }
        CHECK_ST(pm::validate(pool), pm::Status::CorruptMetadata); // still sick
        dlo.addr_next = saved_next;                                // repair
        CHECK_ST(pm::alloc(pool, 64, 8, 0, 3, r), pm::Status::Ok);
        VALIDATE(pool);
        CHECK_ST(pm::free(r), pm::Status::Ok);
        CHECK_ST(pm::free(lo), pm::Status::Ok);
        CHECK_ST(pm::free(hi), pm::Status::Ok);
        CHECK_ST(pm::destroy_pool(pool), pm::Status::Ok);
        done();
    }
}

// ---------------------------------------------------------------------------
// (R30) round-5 guide section 3: alloc() clears the output reference on EVERY
// failure path, so a caller that reuses an old RawRef cannot mistake a failed
// allocation for a fresh one. The old reference itself must stay usable.
// ---------------------------------------------------------------------------
static void test_alloc_clears_output_on_failure() {
    printf("  [R30] alloc clears the output reference on failure\n");
    fresh();
    pm::PoolId pool{};
    CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
    pm::RawRef old_ref{};
    CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, old_ref), pm::Status::Ok);
    fill(old_ref, 64, 81);

    auto expect_cleared = [&](auto&& call, const char* what) {
        pm::RawRef out = old_ref; // caller reuses a still-valid reference
        pm::Status st = call(out);
        ++g_checks;
        if (st == pm::Status::Ok) {
            printf("    CHECK failed %s:%d: %s unexpectedly succeeded\n", __FILE__,
                   __LINE__, what);
            ++g_fails;
            pm::free(out);
            return;
        }
        CHECK(out.generation == 0);   // invalid, never the old value
        CHECK(out.index == 0 && out.pool_hint == 0 && out.offset == 0);
    };
    expect_cleared([&](pm::RawRef& out) { return pm::alloc((pm::PoolId)99, 64, 8, 0, 2, out); },
                   "alloc(InvalidPool)");
    expect_cleared([&](pm::RawRef& out) { return pm::alloc(pool, 0, 8, 0, 2, out); },
                   "alloc(size=0)");
    expect_cleared([&](pm::RawRef& out) { return pm::alloc(pool, 64, 3, 0, 2, out); },
                   "alloc(alignment)");
    expect_cleared([&](pm::RawRef& out) { return pm::alloc(pool, 64, 8, 0xFF00, 2, out); },
                   "alloc(flags)");
    expect_cleared([&](pm::RawRef& out) { return pm::alloc(pool, 200 * 1024, 8, 0, 2, out); },
                   "alloc(NoSpace)");
    expect_cleared([&](pm::RawRef& out) { return pm::alloc(pool, 0x01000000u, 8, 0, 2, out); },
                   "alloc(beyond FL range)");

    // The corrupt-metadata path clears the output too (R18's injection).
    {
        using namespace pm::internal;
        Pool& P = g().pools[pool];
        uint32_t hf = FL_COUNT, hs = SL_COUNT;
        for (uint32_t f = 0; f < FL_COUNT && hf == FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                if (P.bins.head[f][sl] != NULL_OFF) { hf = f; hs = sl; break; }
        CHECK(hf < FL_COUNT);
        uint32_t const saved_head = P.bins.head[hf][hs];
        P.bins.head[hf][hs] = 0xFFFFFFF0u;
        pm::RawRef out = old_ref;
        CHECK_ST(pm::alloc(pool, 64, 8, 0, 2, out), pm::Status::CorruptMetadata);
        CHECK(out.generation == 0);
        P.bins.head[hf][hs] = saved_head;
    }

    // The old reference is untouched by every failed call above and can
    // still be used and released independently.
    verify(old_ref, 64, 81);
    CHECK_ST(pm::free(old_ref), pm::Status::Ok);
    VALIDATE(pool);
    done();
}

// ---------------------------------------------------------------------------
// (R31) round-6 requirements doc section 6/11: compaction advice. The advice
// must be strictly read-only (every Pool, ObjectDesc and Auto Zone byte
// unchanged), distinguish advice/Busy-class/invalid-metadata, honour
// queryable thresholds, suppress repeat prompts, and never fake estimates.
// ---------------------------------------------------------------------------
static void test_compaction_advice() {
    printf("  [R31] compaction advice: read-only, verdicts, thresholds\n");

    using Verdict = pm::CompactionVerdict;
    pm::CompactionThresholds const def = pm::get_compaction_thresholds();
    CHECK(def.fragment_ratio_permille == 100 && def.fragment_min_bytes == 512);

    // ---- (1) zero side effects on a fragmented pool ----
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
        pm::RawRef o[5];
        for (uint32_t i = 0; i < 5; ++i) {
            CHECK_ST(pm::alloc(pool, 1000, 8, 0, i, o[i]), pm::Status::Ok);
            fill(o[i], 1000, 500 + i);
        }
        CHECK_ST(pm::free(o[1]), pm::Status::Ok);
        CHECK_ST(pm::free(o[3]), pm::Status::Ok);
        VALIDATE(pool);

        pm::RawRef const refs[5] = {o[0], o[2], o[4], o[1], o[3]};
        using namespace pm::internal;
        snap_all(pool, pool, refs, 5, seg_base(g().pools[pool].segment_first), 4 * 4096);
        pm::CompactionRequest req{600, 8, 0, 42};
        pm::CompactionAdvice a1 = pm::analyze_compaction(pool);
        pm::CompactionAdvice a2 = pm::analyze_compaction(pool, &req);
        bool changed = true;
        pm::CompactionAdvice a3 = pm::poll_compaction_advice(pool, &req, &changed);
        (void)a1; (void)a2; (void)a3;
        check_all_unchanged(pool, pool, refs, 5, seg_base(g().pools[pool].segment_first), 4 * 4096);
        CHECK_ST(pm::validate(pool), pm::Status::Ok);
        CHECK_ST(pm::free(o[0]), pm::Status::Ok);
        CHECK_ST(pm::free(o[2]), pm::Status::Ok);
        CHECK_ST(pm::free(o[4]), pm::Status::Ok);
        done();
    }

    // ---- (2) verdict matrix ----
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok); // 16 KiB
        // Four 3000 B objects + a filler that consumes the tail, so the only
        // free space after the frees below is the two 3008 B holes.
        pm::RawRef o[4], filler{};
        for (uint32_t i = 0; i < 4; ++i) {
            CHECK_ST(pm::alloc(pool, 3000, 8, 0, i, o[i]), pm::Status::Ok);
            fill(o[i], 3000, 500 + i);
        }
        uint32_t const used = 4 * 3008;
        CHECK_ST(pm::alloc(pool, 4 * 4096 - used - 16, 8, 0, 9, filler),
                 pm::Status::Ok);
        // (a) packed pool: no free space at all -> NO_ACTION, and the
        //     trivially exact move estimate is 0.
        pm::CompactionAdvice a = pm::analyze_compaction(pool);
        CHECK(a.verdict == Verdict::NO_ACTION);
        CHECK(a.estimated_moved_objects == 0 && a.estimated_moved_bytes == 0);
        CHECK(a.external_quiescence_required == 0);
        CHECK(a.capacity == 4 * 4096 && a.live_objects == 5);
        CHECK(a.stats_valid == 1 && a.has_pinned_objects == 0);

        // (b) two 3008 B holes: stranded 3008 B = 183 permille >= defaults
        //     -> RECOMMENDED.
        CHECK_ST(pm::free(o[1]), pm::Status::Ok);
        CHECK_ST(pm::free(o[3]), pm::Status::Ok);
        a = pm::analyze_compaction(pool);
        CHECK(a.verdict == Verdict::COMPACT_RECOMMENDED);
        CHECK(a.external_quiescence_required == 1);
        CHECK(a.estimated_moved_objects == pm::COMPACTION_ESTIMATE_UNKNOWN);
        CHECK(a.estimated_moved_bytes == pm::COMPACTION_ESTIMATE_UNKNOWN);
        CHECK(a.free_bytes == 2 * 3008 && a.largest_free_block == 3008);

        // (c) active borrow -> BLOCKED (advice never hides the blocker).
        void* p = nullptr;
        CHECK_ST(pm::borrow_begin(o[0], 3000, 1, p), pm::Status::Ok);
        a = pm::analyze_compaction(pool);
        CHECK(a.verdict == Verdict::COMPACT_BLOCKED);
        CHECK(a.borrow_count == 1);
        pm::borrow_end(o[0]);

        // (d) maintenance state -> BLOCKED (white-box state injection).
        {
            using namespace pm::internal;
            pm::internal::PoolState saved = g().pools[pool].state;
            g().pools[pool].state = pm::internal::PoolState::Compacting;
            a = pm::analyze_compaction(pool);
            CHECK(a.verdict == Verdict::COMPACT_BLOCKED);
            g().pools[pool].state = saved;
        }

        // (e) request that fits a 3008 B hole right now -> NO_ACTION.
        pm::CompactionRequest req{600, 8, 0, 7};
        a = pm::analyze_compaction(pool, &req);
        CHECK(a.verdict == Verdict::NO_ACTION);
        CHECK(a.request_can_fit_now == 1);
        CHECK(a.expected_request_size == 600 && a.expected_request_alignment == 8);

        CHECK_ST(pm::free(o[0]), pm::Status::Ok);
        CHECK_ST(pm::free(o[2]), pm::Status::Ok);
        CHECK_ST(pm::free(filler), pm::Status::Ok);
        done();
    }

    // ---- (3) request that needs compaction / can never fit ----
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
        pm::RawRef o[4], filler{};
        for (uint32_t i = 0; i < 4; ++i) {
            CHECK_ST(pm::alloc(pool, 3000, 8, 0, i, o[i]), pm::Status::Ok);
        }
        CHECK_ST(pm::alloc(pool, 4 * 4096 - 4 * 3008 - 16, 8, 0, 9, filler),
                 pm::Status::Ok);
        CHECK_ST(pm::free(o[1]), pm::Status::Ok);
        CHECK_ST(pm::free(o[3]), pm::Status::Ok);
        // 4000 B request: does not fit a 3008 B hole, but the stranded
        // 6016 B would cover it after compaction -> RECOMMENDED.
        pm::CompactionRequest req{4000, 8, 0, 8};
        pm::CompactionAdvice a = pm::analyze_compaction(pool, &req);
        CHECK(a.verdict == Verdict::COMPACT_RECOMMENDED);
        CHECK(a.request_can_fit_now == 0);
        CHECK(a.request_can_fit_after_compaction_estimate == 1);
        CHECK(a.external_quiescence_required == 1);
        // 15000 B request: free_bytes ~ 6016 -- can never fit ->
        // UNLIKELY_TO_HELP (no fake optimism).
        pm::CompactionRequest big{15000, 8, 0, 9};
        a = pm::analyze_compaction(pool, &big);
        CHECK(a.verdict == Verdict::COMPACT_UNLIKELY_TO_HELP);
        CHECK(a.request_can_fit_after_compaction_estimate == 0);
        CHECK_ST(pm::free(o[0]), pm::Status::Ok);
        CHECK_ST(pm::free(o[2]), pm::Status::Ok);
        CHECK_ST(pm::free(filler), pm::Status::Ok);
        done();
    }

    // ---- (4) pinned objects are reported ----
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef pin{};
        CHECK_ST(pm::alloc(pool, 256, 8, pm::PM_PINNED, 1, pin), pm::Status::Ok);
        pm::CompactionAdvice a = pm::analyze_compaction(pool);
        CHECK(a.has_pinned_objects == 1);
        CHECK_ST(pm::free(pin), pm::Status::Ok);
        done();
    }

    // ---- (5) INVALID_METADATA: bad pool, malformed request, damaged bins ----
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 2), pm::Status::Ok);
        pm::RawRef a_ref{};
        CHECK_ST(pm::alloc(pool, 64, 8, 0, 1, a_ref), pm::Status::Ok);
        CHECK_ST(pm::free(a_ref), pm::Status::Ok);
        CHECK(pm::analyze_compaction((pm::PoolId)77).verdict ==
              Verdict::INVALID_METADATA);
        pm::CompactionRequest bad{64, 3, 0, 1}; // alignment not a power of two
        CHECK(pm::analyze_compaction(pool, &bad).verdict == Verdict::INVALID_METADATA);
        using namespace pm::internal;
        Pool& P = g().pools[pool];
        uint32_t hf = FL_COUNT, hs = SL_COUNT;
        for (uint32_t f = 0; f < FL_COUNT && hf == FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                if (P.bins.head[f][sl] != NULL_OFF) { hf = f; hs = sl; break; }
        if (hf < FL_COUNT) { // guard: a healthy pool always has one, but do
                             // not index out of bounds even on a failed CHECK
            uint32_t const saved = P.bins.head[hf][hs];
            P.bins.head[hf][hs] = 0xFFFFFFF0u;
            CHECK(pm::analyze_compaction(pool).verdict == Verdict::INVALID_METADATA);
            P.bins.head[hf][hs] = saved;
        }
        VALIDATE(pool);
        done();
    }

    // ---- (6) thresholds are queryable/configurable + poll suppression ----
    {
        fresh();
        pm::PoolId pool{};
        CHECK_ST(pm::create_pool(pool, 4), pm::Status::Ok);
        pm::RawRef o[5];
        for (uint32_t i = 0; i < 5; ++i) {
            CHECK_ST(pm::alloc(pool, 1000, 8, 0, i, o[i]), pm::Status::Ok);
        }
        CHECK_ST(pm::free(o[1]), pm::Status::Ok);
        CHECK_ST(pm::free(o[3]), pm::Status::Ok);
        // State: two 1008 B holes + the 11344 B tail; stranded 2016 B
        // (123 permille) -> above the default thresholds.

        // (a) suppression: the first poll after init always reports (the
        //     advice cache starts empty); an unchanged state is suppressed.
        bool changed = false;
        pm::poll_compaction_advice(pool, nullptr, &changed);
        CHECK(changed == true);
        pm::poll_compaction_advice(pool, nullptr, &changed);
        CHECK(changed == false);
        // (b) merging a hole with the next one does NOT move the largest
        //     block (the tail dominates), nor the epoch -> no repeat prompt.
        CHECK_ST(pm::free(o[0]), pm::Status::Ok);
        pm::poll_compaction_advice(pool, nullptr, &changed);
        CHECK(changed == false);
        // (c) a compaction bumps structure_epoch and (here) clears the
        //     stranded bytes -> the verdict changes -> reports.
        CHECK_ST(pm::compact(pool), pm::Status::Ok);
        pm::poll_compaction_advice(pool, nullptr, &changed);
        CHECK(changed == true);
        pm::poll_compaction_advice(pool, nullptr, &changed);
        CHECK(changed == false);

        // (d) thresholds: defaults make the (re-fragmented) pool RECOMMENDED;
        //     a stricter ratio silences it; a looser one keeps it.
        CHECK_ST(pm::free(o[2]), pm::Status::Ok);
        CHECK_ST(pm::free(o[4]), pm::Status::Ok);
        // Re-fragment: refill the packed area and free alternating objects.
        pm::RawRef n[5];
        for (uint32_t i = 0; i < 5; ++i) {
            CHECK_ST(pm::alloc(pool, 1000, 8, 0, 20 + i, n[i]), pm::Status::Ok);
        }
        CHECK_ST(pm::free(n[1]), pm::Status::Ok);
        CHECK_ST(pm::free(n[3]), pm::Status::Ok);
        pm::CompactionThresholds t = pm::get_compaction_thresholds();
        CHECK(t.fragment_ratio_permille == 100 && t.fragment_min_bytes == 512);
        pm::set_compaction_thresholds({5000, 512}); // 500%: never reached
        CHECK(pm::analyze_compaction(pool).verdict == Verdict::NO_ACTION);
        pm::set_compaction_thresholds({10, 100}); // 1% / 100 B
        CHECK(pm::analyze_compaction(pool).verdict == Verdict::COMPACT_RECOMMENDED);
        pm::set_compaction_thresholds(t);
        CHECK(pm::get_compaction_thresholds().fragment_ratio_permille == 100);
        CHECK(pm::analyze_compaction(pool).verdict == Verdict::COMPACT_RECOMMENDED);

        CHECK_ST(pm::free(n[0]), pm::Status::Ok);
        CHECK_ST(pm::free(n[2]), pm::Status::Ok);
        CHECK_ST(pm::free(n[4]), pm::Status::Ok);
        VALIDATE(pool);
        done();
    }
}

// ---------------------------------------------------------------------------
static void run(const char* name, void (*fn)()) {
    printf("[TEST] %s\n", name);
    uint32_t const f0 = g_fails;
    fn();
    // cppcheck-suppress knownConditionTrueFalse ; fn() bumps g_fails through
    // the CHECK macro, which cppcheck cannot follow across the call.
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
    run("R1_slot_rollback", test_slot_rollback_after_failed_allocs);
    run("R2_tlsf_same_bin_first_fit", test_tlsf_same_bin_first_fit);
    run("R3_destroy_pointer_kept_on_error", test_destroy_pointer_kept_on_error);
    run("R4_local_rejects_cross_hint", test_local_ref_rejects_cross_hint);
    run("R5_relocatable_opt_in", test_relocatable_opt_in);
    run("R6_subobject_cannot_free", test_subobject_ref_cannot_free);
    run("R7_maintenance_errors_clean", test_maintenance_error_paths_are_clean);
    run("R8_validate_bounded_on_corruption", test_validate_bounded_on_corruption);
    run("R9_huge_alloc_rejected", test_huge_alloc_rejected);
    run("R10_quiescent_window_contract", test_quiescent_window_contract);
    run("R11_split_layout_details", test_split_layout_details);
    run("R12_generation_epoch_combinations", test_generation_epoch_combinations);
    run("R13_split_crossing_order", test_split_crossing_order);
    run("R14_init_lifecycle", test_init_lifecycle);
    run("R15_split_boundary_geometries", test_split_boundary_geometries);
    run("R16_maintenance_state_matrix", test_maintenance_state_matrix);
    run("R17_desc_block_consistency_rejected", test_desc_block_consistency_rejected);
    run("R18_get_stats_bounded_on_corruption", test_get_stats_bounded_on_corruption);
    run("R19_destroy_fn_semantics", test_destroy_fn_semantics);
    run("R20_compact_fault_injection", test_compact_fault_injection);
    run("R21_metadata_domain_corruption", test_metadata_domain_corruption);
    run("R22_merge_transaction_faults", test_merge_transaction_faults);
    run("R23_descriptor_outside_pool", test_descriptor_outside_pool);
    run("R24_free_physical_header_faults", test_free_physical_header_faults);
    run("R25_borrow_end_token_discipline", test_borrow_end_token_discipline);
    run("R26_resolve_clears_output", test_resolve_clears_output);
    run("R27_local_binding_semantics", test_local_binding_semantics);
    run("R28_state_publication", test_state_publication);
    run("R29_round4_fault_matrix", test_round4_fault_matrix);
    run("R30_alloc_clears_output_on_failure", test_alloc_clears_output_on_failure);
    run("R31_compaction_advice", test_compaction_advice);

    printf("\n%u checks, %u failures\n", (unsigned)g_checks, (unsigned)g_fails);
    return g_fails == 0 ? 0 : 1;
}
