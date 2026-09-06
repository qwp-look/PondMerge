// PondMerge v1 - core implementation.
// All state lives in pm::internal::g() plus fixed scratch arrays: the
// Metadata region of the doc's layout. The Auto Zone itself only ever holds
// block data.
#include "pondmerge/pondmerge.hpp"
#include "internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace pm {

#if defined(PM_ESP32)
// Spinlock guarding borrow counters on SMP-capable targets (port layer).
portMUX_TYPE pm_spinlock = portMUX_INITIALIZER_UNLOCKED;
#endif

using namespace internal;

namespace {

uint32_t load32(void const* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
void store32(void* p, uint32_t v) { memcpy(p, &v, 4); }

inline uint32_t blk_size_of(void const* hdr) { return load32(hdr) & ~BLOCK_FREE_BIT; }
inline bool blk_is_free(void const* hdr) { return (load32(hdr) & BLOCK_FREE_BIT) != 0; }

inline uint32_t align_up_u(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t log2_floor_u(uint32_t v) {
    return v <= 1 ? 0 : (uint32_t)(31 - __builtin_clz(v));
}

} // unnamed namespace

namespace internal {

GlobalState& g() {
    static GlobalState G;
    return G;
}

// --- TLSF bins (doc section 7.2) --------------------------------------------
void TlsfBins::reset() {
    fl_bitmap = 0;
    memset(sl_bitmap, 0, sizeof(sl_bitmap));
    // 0 is a valid zone offset: empty heads must be NULL_OFF, not zero.
    for (uint32_t f = 0; f < FL_COUNT; ++f)
        for (uint32_t s = 0; s < SL_COUNT; ++s)
            head[f][s] = NULL_OFF;
}

uint32_t fl_index(uint32_t size) {
    if (size < (1u << MIN_FL)) return MIN_FL;
    uint32_t fl = log2_floor_u(size);
    return fl < PM_FL_MAX ? fl : PM_FL_MAX - 1;
}

uint32_t sl_index(uint32_t size, uint32_t fl) {
    uint32_t base = 1u << fl;
    uint32_t sl = ((size - base) * SL_COUNT) >> fl;
    return sl < SL_COUNT ? sl : SL_COUNT - 1;
}

void bins_insert(TlsfBins& b, FreeBlock* blk) {
    uint32_t sz = blk_size_of(blk);
    uint32_t fl = fl_index(sz);
    uint32_t sl = sl_index(sz, fl);
    uint32_t off = off_of(blk);
    blk->prev = NULL_OFF;
    blk->next = b.head[fl - MIN_FL][sl];
    if (blk->next != NULL_OFF) ptr_of(blk->next)->prev = off;
    b.head[fl - MIN_FL][sl] = off;
    b.sl_bitmap[fl - MIN_FL] |= (uint16_t)(1u << sl);
    b.fl_bitmap |= 1u << (fl - MIN_FL);
}

void bins_remove(TlsfBins& b, FreeBlock* blk) {
    uint32_t sz = blk_size_of(blk);
    uint32_t fl = fl_index(sz);
    uint32_t sl = sl_index(sz, fl);
    if (blk->prev != NULL_OFF) ptr_of(blk->prev)->next = blk->next;
    else b.head[fl - MIN_FL][sl] = blk->next;
    if (blk->next != NULL_OFF) ptr_of(blk->next)->prev = blk->prev;
    if (b.head[fl - MIN_FL][sl] == NULL_OFF) {
        b.sl_bitmap[fl - MIN_FL] &= (uint16_t)~(1u << sl);
        if (b.sl_bitmap[fl - MIN_FL] == 0) b.fl_bitmap &= ~(1u << (fl - MIN_FL));
    }
    blk->prev = blk->next = NULL_OFF;
}

FreeBlock* bins_find(TlsfBins& b, uint32_t need) {
    // Sizes beyond the representable FL range are refused by the caller; a
    // defensive check here keeps the search from wrapping into low bins.
    if (need >= (1u << PM_FL_MAX)) return nullptr;

    // Cap the number of list hops so a corrupted (cyclic) free list can never
    // hang the allocator: no bin can hold more blocks than the zone has room
    // for at minimum block size.
    const uint32_t max_hops = g().zone_size / PM_MIN_BLOCK + 1;
    uint32_t hops = 0;

    uint32_t fl = fl_index(need);
    uint32_t sl = sl_index(need, fl);
    uint32_t fli = fl - MIN_FL;

    while (true) {
        uint32_t sl_map = b.sl_bitmap[fli] & (uint16_t)(0xFFFFu << sl);
        while (sl_map != 0) {
            uint32_t s = (uint32_t)__builtin_ctz(sl_map);
            // First-fit inside the bin: several block sizes share one SL bin,
            // so the head block can be smaller than the request. Walk the bin
            // (never wider than the bin itself) for a real fit.
            for (uint32_t off = b.head[fli][s]; off != NULL_OFF; off = ptr_of(off)->next) {
                if (++hops > max_hops) return nullptr; // corrupt list guard
                FreeBlock* cand = ptr_of(off);
                if (blk_size_of(cand) >= need) return cand;
            }
            sl_map &= ~(1u << s); // this bin has no fit; try the next SL bin
        }
        // No suitable bin in this first level: lowest non-empty higher level.
        // Every block in a higher level is >= 2^(MIN_FL+fli+1) > need, so its
        // head block is a fit; still verified below via the same path.
        uint32_t fl_map = b.fl_bitmap & ~((2u << fli) - 1u);
        if (fl_map == 0) return nullptr;
        fli = (uint32_t)__builtin_ctz(fl_map);
        sl = 0; // scan all SL bins of the higher level
        // Higher-level head blocks always satisfy the size check, so the
        // inner loop returns on its first candidate; the walk stays O(bins).
    }
}

} // namespace internal

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------
namespace {

inline uint8_t* pool_start(Pool const& P) { return seg_base(P.segment_first); }
inline uint8_t* pool_end(Pool const& P) { return seg_base((uint32_t)P.segment_first + P.segment_count); }
inline uint32_t pool_capacity(Pool const& P) { return (uint32_t)P.segment_count * g().segment_size; }

Pool* pool_at(PoolId id) {
    if (id >= PM_MAX_POOLS) return nullptr;
    Pool& P = g().pools[id];
    return P.state == PoolState::Empty ? nullptr : &P;
}

inline void bump_epoch(uint32_t& epoch) {
    // Wrap-around guard: 0 is reserved (doc section 4). For uint32 epochs
    // this only fires at 0xFFFFFFFF; cppcheck-style "always false" notes are
    // wrong for the uint16 generation counterpart below.
    if (++epoch == 0) epoch = 1;
}

// --- address-order list (sorted by block address) ---------------------------
void order_unlink(Pool& P, uint32_t idx) {
    ObjectDesc& D = g().objects[idx];
    if (D.addr_prev != NO_ORDER) g().objects[D.addr_prev].addr_next = D.addr_next;
    else P.order_head = D.addr_next;
    if (D.addr_next != NO_ORDER) g().objects[D.addr_next].addr_prev = D.addr_prev;
    D.addr_prev = D.addr_next = NO_ORDER;
}

void order_insert_sorted(Pool& P, uint32_t idx) {
    ObjectDesc* D = g().objects;
    uint32_t cur = P.order_head;
    uint32_t prev = NO_ORDER;
    while (cur != NO_ORDER && D[cur].address < D[idx].address) {
        prev = cur;
        cur = D[cur].addr_next;
    }
    D[idx].addr_prev = prev;
    D[idx].addr_next = cur;
    if (prev != NO_ORDER) D[prev].addr_next = idx;
    else P.order_head = idx;
    if (cur != NO_ORDER) D[cur].addr_prev = idx;
}

// --- descriptor slots --------------------------------------------------------
uint16_t slot_alloc() {
    GlobalState& G = g();
    uint16_t s = G.free_slot_head;
    if (s == NO_SLOT) return NO_SLOT;
    G.free_slot_head = G.objects[s].next_free_slot;
    return s;
}

void slot_release(uint16_t s) {
    GlobalState& G = g();
    G.objects[s].next_free_slot = G.free_slot_head;
    G.free_slot_head = s;
}

uint16_t next_generation(uint16_t gen) {
    uint16_t n = (uint16_t)(gen + 1); // wraps at 0xFFFF -> 0
    return n == 0 ? 1 : n;            // 0 reserved as invalid (doc section 4)
}

// --- reference validation (doc section 5 resolution rules 1-4) --------------
struct RefCheck {
    Pool* pool;
    ObjectDesc* desc;
};

Status check_ref(RawRef const& ref, uint32_t access_size, uint32_t access_align,
                 RefCheck& rc, bool require_running, void** out_addr) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;
    if (ref.index >= PM_MAX_OBJECTS || ref.generation == 0) return Status::InvalidRef;
    ObjectDesc& d = G.objects[ref.index];
    if (d.state != ObjState::Live) return Status::InvalidRef;
    if (d.generation != ref.generation) return Status::InvalidRef;
    Pool* P = pool_at((PoolId)d.pool_id);
    if (!P) return Status::CorruptMetadata;
    if (ref.pool_hint != CROSS_HINT && ref.pool_hint != d.pool_id) return Status::PoolChanged;
    if (require_running && P->state != PoolState::Running) return Status::Busy;
    if (access_align == 0 || (access_align & (access_align - 1)) != 0 ||
        access_align > PM_MAX_ALIGNMENT)
        return Status::InvalidAlignment;
    if (ref.offset > d.size || access_size > d.size - ref.offset) return Status::InvalidRef;
    uint8_t* addr = d.address + ref.offset;
    if (((uintptr_t)addr & (access_align - 1)) != 0) return Status::InvalidAlignment;
    rc.pool = P;
    rc.desc = &d;
    if (out_addr) *out_addr = addr;
    return Status::Ok;
}

// --- maintenance pre-check (task-book v2 section 8) --------------------------
// Full descriptor audit before ANY maintenance planning. Plan A's "execution
// cannot fail" rests on this function: order list, descriptor fields and
// physical block relationships are all verified here, with bounded walks so
// corrupted (cyclic) metadata is refused instead of hanging. Runs in Release
// too -- its verdict gates every memmove. All range math uses zone offsets,
// never raw pointer differences on possibly-corrupted values. The prev_size
// chain is deliberately NOT checked: after a pool merge the physical
// predecessor of a block may come from the other pool until the post-merge
// compaction rewrites all headers (validate() enforces the chain on settled
// layouts).
Status precheck_pool(Pool const& P, PoolId pid, uint8_t const* start, uint8_t const* end) {
    GlobalState const& G = g();
    const uint64_t zone_base = (uint64_t)(uintptr_t)G.zone;
    const uint64_t start_off = (uint64_t)(uintptr_t)start - zone_base;
    const uint64_t end_off = (uint64_t)(uintptr_t)end - zone_base;
    const uint32_t max_steps = PM_MAX_OBJECTS + 1;

    uint32_t count = 0, used = 0;
    uint32_t prev_idx = NO_ORDER;
    uint64_t prev_end = start_off;

    for (uint32_t idx = P.order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
        if (++count > max_steps) return Status::CorruptMetadata; // cycle/overrun
        if (idx >= PM_MAX_OBJECTS) return Status::CorruptMetadata;
        ObjectDesc const& d = G.objects[idx];
        if (d.state != ObjState::Live || d.pool_id != pid || d.generation == 0)
            return Status::CorruptMetadata;
        if (d.addr_prev != prev_idx) return Status::CorruptMetadata;
        if (d.block_size < PM_MIN_BLOCK || (d.block_size & (PM_ALIGNMENT - 1)) != 0)
            return Status::CorruptMetadata;
        if (d.size > d.block_size - BLOCK_HEADER_SIZE) return Status::CorruptMetadata;
        uint64_t aabs = (uint64_t)(uintptr_t)d.address;
        if (aabs < zone_base + BLOCK_HEADER_SIZE) return Status::CorruptMetadata;
        uint64_t aoff = aabs - zone_base;         // payload offset in zone
        uint64_t boff = aoff - BLOCK_HEADER_SIZE; // block start, zone offset
        if (boff < start_off || aoff + d.size > end_off) return Status::CorruptMetadata;
        if (boff < prev_end) return Status::CorruptMetadata; // order + overlap
        // Physical header must already agree with the descriptor.
        uint32_t own = load32(G.zone + boff);
        if ((own & BLOCK_FREE_BIT) != 0 || (own & ~BLOCK_FREE_BIT) != d.block_size)
            return Status::CorruptMetadata;
        prev_end = boff + d.block_size;
        used += d.block_size;
        prev_idx = idx;
    }
    if (prev_idx != NO_ORDER && G.objects[prev_idx].addr_next != NO_ORDER)
        return Status::CorruptMetadata;
    if (count != P.live_objects || used != P.used_bytes) return Status::CorruptMetadata;
    if (P.free_bytes != (uint32_t)(end_off - start_off) - used) return Status::CorruptMetadata;
    return Status::Ok;
}

// --- layout finalization -----------------------------------------------------
// Rebuild block headers, free blocks and TLSF bins for the physical range
// [start, end) from the pool's address-ordered live blocks. Shared by
// compact, merge and split. Sub-minimal gaps become poisoned slack
// (prev_size 0) so free()'s physical walk can never run into them.
//
// Transactional contract (task-book 3.6, plan A): this function runs in the
// EXECUTION phase, after the callers' read-only planning has verified object
// placement, pinned barriers, alignment and range bounds. It only writes
// already-validated metadata, so it cannot fail; the internal checks are
// debug assertions. No maintenance op may move data and then report an error.
void finalize_layout(Pool& P, uint8_t* start, uint8_t* end) {
    P.bins.reset();
    uint32_t capacity = (uint32_t)(end - start);
    uint8_t* prev_end = start;
    uint32_t prev_own = 0; // predecessor's own size, 0 = none/slack
    uint32_t used = 0, fragment = 0, count = 0;

    for (uint32_t idx = P.order_head; idx != NO_ORDER; idx = g().objects[idx].addr_next) {
        ObjectDesc& d = g().objects[idx];
        uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
        uint32_t bsize = d.block_size;
        // Invariants guaranteed by planning; a violation here is a bug, not
        // a runtime condition.
        PM_ASSERT(bstart >= prev_end);
        PM_ASSERT((uint64_t)(bstart - start) + bsize <= capacity);

        uint32_t gap = (uint32_t)(bstart - prev_end);
        if (gap >= PM_MIN_BLOCK) {
            auto* fb = reinterpret_cast<FreeBlock*>(prev_end); // cppcheck-suppress dangerousTypeCast; 8B-aligned block start
            fb->header = gap | BLOCK_FREE_BIT;
            fb->prev_size = prev_own;
            fb->prev = fb->next = NULL_OFF;
            bins_insert(P.bins, fb);
            prev_own = gap;
        } else if (gap > 0) {
            fragment += gap;
            prev_own = 0; // slack: poison the physical walk here
            store32(prev_end, 0); // slack head reads as a 0-size used block,
                                  // so free()'s forward-merge bit check and
                                  // backward prev_size walk both stop here
        }
        store32(bstart, bsize);         // own size, bit clear = in use
        store32(bstart + 4, prev_own);  // prev_size
        prev_end = bstart + bsize;
        prev_own = bsize;
        used += bsize;
        count++;
    }

    uint32_t tail = (uint32_t)(end - prev_end);
    if (tail >= PM_MIN_BLOCK) {
        auto* fb = reinterpret_cast<FreeBlock*>(prev_end); // cppcheck-suppress dangerousTypeCast; 8B-aligned block start
        fb->header = tail | BLOCK_FREE_BIT;
        fb->prev_size = prev_own;
        fb->prev = fb->next = NULL_OFF;
        bins_insert(P.bins, fb);
    } else if (tail > 0) {
        fragment += tail;
    }

    P.used_bytes = used;
    P.free_bytes = capacity - used;
    P.fragment_bytes = fragment;
    PM_ASSERT(P.live_objects == count);
}

// --- compaction scratch (fixed Metadata, outside the Auto Zone) --------------
struct MovePlanEntry {
    uint32_t slot;
    uint32_t dst_off; // zone offset of the new block start
    uint32_t size;
};
MovePlanEntry s_plan[PM_MAX_OBJECTS];    // lower-side packing (ascending moves)
MovePlanEntry s_upper[PM_MAX_OBJECTS];   // upper-side packing (descending moves)
uint8_t* s_barriers[PM_MAX_OBJECTS]; // pinned block starts (address order)

// --- compaction core ---------------------------------------------------------
// Requires: state == Compacting, borrow_count == 0 (caller validated).
// Address-order stable packing with pinned barriers; plan fully before any
// move (doc sections 8.1-8.3).
Status compact_impl(Pool& P) {
    GlobalState& G = g();
    if (P.borrow_count != 0) return Status::Busy;
    uint64_t t0 = pm_port_ticks_us();
    uint8_t* start = pool_start(P);
    uint8_t* end = pool_end(P);
    Status st = Status::Ok;
    uint32_t nbar = 0, nplan = 0;

    // Gate every memmove on a full descriptor audit (bounded walks).
    st = precheck_pool(P, (PoolId)(&P - G.pools), start, end);
    if (st != Status::Ok) goto fail;

    for (uint32_t idx = P.order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
        ObjectDesc& d = G.objects[idx];
        if (!(d.flags & PM_PINNED)) continue;
        if (d.address - BLOCK_HEADER_SIZE < start || d.address + d.size > end) {
            st = Status::CorruptMetadata;
            goto fail;
        }
        s_barriers[nbar++] = d.address - BLOCK_HEADER_SIZE;
    }

    {
        uint8_t* cursor = start;
        uint32_t bar = 0;
        for (uint32_t idx = P.order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
            ObjectDesc& d = G.objects[idx];
            uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
            uint32_t bsize = d.block_size;
            if (d.flags & PM_PINNED) {
                if (cursor > bstart) { st = Status::PinnedConflict; goto fail; }
                cursor = bstart + bsize;
                bar++;
                continue;
            }
            uint8_t* barrier = (bar < nbar) ? s_barriers[bar] : end;
            if (cursor + bsize > barrier) {
                st = (bar < nbar) ? Status::PinnedConflict : Status::NoSpace;
                goto fail;
            }
            if (bstart != cursor) {
                s_plan[nplan].slot = idx;
                s_plan[nplan].dst_off = off_of(cursor);
                s_plan[nplan].size = bsize;
                nplan++;
            }
            cursor += bsize;
        }
    }

    {
        uint32_t moved_objs = 0, moved_bytes = 0;
        for (uint32_t i = 0; i < nplan; ++i) {
            ObjectDesc& d = G.objects[s_plan[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_plan[i].dst_off;
            if (dst != src) memmove(dst, src, s_plan[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
            moved_objs++;
            moved_bytes += s_plan[i].size;
        }
        // Execution phase: cannot fail (plan A). Planning verified every
        // placement; finalize only writes already-validated metadata.
        finalize_layout(P, start, end);

        P.objects_moved = moved_objs;
        P.bytes_moved = moved_bytes;
        P.compact_time_us = pm_port_ticks_us() - t0;
        if (moved_bytes > G.max_bytes_moved) G.max_bytes_moved = moved_bytes;
        if (P.compact_time_us > G.max_compact_time_us) G.max_compact_time_us = P.compact_time_us;
        if (P.fragment_bytes > G.max_fragment_bytes) G.max_fragment_bytes = P.fragment_bytes;

        P.state = PoolState::Running;
        P.structure_epoch++;
        return Status::Ok;
    }

fail:
    // Planning failed BEFORE any data was moved: the pool is semantically
    // untouched, so restore Running instead of leaving it Paused. (The
    // borrow-Busy refusal lives in compact(), which keeps the pool Paused
    // per doc section 8; only plan failures land here.)
    P.state = PoolState::Running;
    return st;
}

} // unnamed namespace

namespace internal {

uint32_t metadata_scratch_bytes() {
    return (uint32_t)(sizeof(s_plan) + sizeof(s_upper) + sizeof(s_barriers));
}

} // namespace internal

// ---------------------------------------------------------------------------
// Debug abort
// ---------------------------------------------------------------------------
void pm_debug_abort(const char* file, int line) {
#if defined(PM_ESP32)
    printf("PondMerge ASSERT %s:%d\n", file, line);
#else
    fprintf(stderr, "PondMerge ASSERT %s:%d\n", file, line);
#endif
    abort();
}

// ---------------------------------------------------------------------------
// init / deinit (doc section 2)
// ---------------------------------------------------------------------------
Status init(Config const& cfg) {
    GlobalState& G = g();
    // Lifecycle (task-book v2 section 6): validate EVERYTHING before touching
    // global state. A repeated or failed init must never wipe a live system.
    if (G.initialized != 0) return Status::Busy;

    if (!cfg.zone) return Status::InvalidAlignment;
    if (cfg.segment_size < 1024) return Status::InvalidAlignment;
    if (cfg.segment_size & (cfg.segment_size - 1)) return Status::InvalidAlignment;
    if (cfg.zone_size < cfg.segment_size) return Status::InvalidAlignment;
    if (((uintptr_t)cfg.zone & (PM_ALIGNMENT - 1)) != 0) return Status::InvalidAlignment;
    if (cfg.zone_size / cfg.segment_size > PM_MAX_SEGMENTS) return Status::NoSpace;

    uint32_t seg_count = cfg.zone_size / cfg.segment_size;
    if (seg_count == 0) return Status::NoSpace;
    // Overflow check on the rounded zone size (task-book v2 section 6.1).
    if (seg_count > UINT32_MAX / cfg.segment_size) return Status::NoSpace;

    // All checks passed: only now may global state be (re)initialized.
    memset(&G, 0, sizeof(G));
    G.zone = cfg.zone;
    G.segment_size = cfg.segment_size;
    G.segment_count = seg_count;
    G.zone_size = seg_count * cfg.segment_size;
    for (uint32_t i = 0; i < PM_MAX_OBJECTS; ++i) {
        G.objects[i].addr_prev = G.objects[i].addr_next = NO_ORDER;
        G.objects[i].next_free_slot = (uint16_t)((i + 1 < PM_MAX_OBJECTS) ? i + 1 : NO_SLOT);
    }
    G.free_slot_head = 0;
    G.initialized = 1;
    return Status::Ok;
}

Status deinit() {
    GlobalState& G = g();
    if (!G.initialized) return Status::InvalidPool;
    // Object-less pools hold no resources beyond segment bookkeeping, which
    // dies with the zone; only live objects block a shutdown.
    if (G.live_object_count != 0) return Status::Busy;
    G.initialized = 0;
    memset(&G, 0, sizeof(G));
    return Status::Ok;
}

GlobalStats global_stats() {
    GlobalState& G = g();
    GlobalStats s{};
    s.max_live_objects = G.max_live_objects;
    s.max_borrow_count = G.max_borrow_count;
    s.max_fragment_bytes = G.max_fragment_bytes;
    s.max_bytes_moved = G.max_bytes_moved;
    s.max_compact_time_us = G.max_compact_time_us;
    s.metadata_bytes = (uint32_t)sizeof(GlobalState) + metadata_scratch_bytes();
    return s;
}

// ---------------------------------------------------------------------------
// Pools
// ---------------------------------------------------------------------------
Status create_pool(PoolId& out, uint32_t segment_count) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;
    if (segment_count == 0) return Status::NoSpace;

    bool used[PM_MAX_SEGMENTS] = {};
    for (uint32_t i = 0; i < PM_MAX_POOLS; ++i) {
        Pool const& P = G.pools[i];
        if (P.state == PoolState::Empty) continue;
        for (uint32_t s = 0; s < P.segment_count; ++s)
            used[P.segment_first + s] = true;
    }
    int32_t base = -1, run = 0;
    for (uint32_t s = 0; s < G.segment_count && base < 0; ++s) {
        if (!used[s]) {
            if (++run >= (int32_t)segment_count) base = (int32_t)(s + 1 - segment_count);
        } else {
            run = 0;
        }
    }
    if (base < 0) return Status::NoSpace;

    for (uint32_t i = 0; i < PM_MAX_POOLS; ++i) {
        Pool& P = G.pools[i];
        if (P.state != PoolState::Empty) continue;
        memset(&P, 0, sizeof(P));
        P.state = PoolState::Running;
        P.segment_first = (uint16_t)base;
        P.segment_count = (uint16_t)segment_count;
        P.structure_epoch = 1;
        P.order_head = NO_ORDER;
        P.free_bytes = pool_capacity(P);
        P.bins.reset();
        auto* blk = reinterpret_cast<FreeBlock*>(pool_start(P)); // cppcheck-suppress dangerousTypeCast; segment-aligned
        blk->header = pool_capacity(P) | BLOCK_FREE_BIT;
        blk->prev_size = 0;
        blk->prev = blk->next = NULL_OFF;
        bins_insert(P.bins, blk);
        out = (PoolId)i;
        return Status::Ok;
    }
    return Status::NoSpace;
}

Status destroy_pool(PoolId id) {
    GlobalState& G = g();
    Pool* P = pool_at(id);
    if (!G.initialized || !P) return Status::InvalidPool;
    if (P->live_objects != 0 || P->borrow_count != 0) return Status::Busy;
    if (P->state == PoolState::Compacting || P->state == PoolState::Merging ||
        P->state == PoolState::Splitting)
        return Status::Busy;
    PM_ASSERT(P->order_head == NO_ORDER);
    memset(&G.pools[id], 0, sizeof(Pool));
    return Status::Ok;
}

Status pause(PoolId id) {
    Pool* P = pool_at(id);
    if (!P) return Status::InvalidPool;
    // The state flip must be mutually exclusive with borrow_begin's
    // Running-state check so a borrow can never start "between" the check
    // and the flip (task-book section 4).
    PM_LOCK();
    Status st;
    if (P->state == PoolState::Paused) st = Status::AlreadyPaused;
    else if (P->state != PoolState::Running) st = Status::Busy;
    else { P->state = PoolState::Paused; st = Status::Ok; }
    PM_UNLOCK();
    return st;
}

Status resume(PoolId id) {
    Pool* P = pool_at(id);
    if (!P) return Status::InvalidPool;
    PM_LOCK();
    Status st;
    if (P->state == PoolState::Running) st = Status::Ok;
    else if (P->state != PoolState::Paused) st = Status::Busy;
    else { P->state = PoolState::Running; st = Status::Ok; }
    PM_UNLOCK();
    return st;
}

PoolStats get_stats(PoolId id) {
    PoolStats s{};
    Pool* P = pool_at(id);
    if (!P) return s;
    s.state = (uint8_t)P->state;
    s.segment_first = P->segment_first;
    s.segment_count = P->segment_count;
    s.used_bytes = P->used_bytes;
    s.free_bytes = P->free_bytes;
    s.fragment_bytes = P->fragment_bytes;
    s.object_count = P->live_objects;
    s.borrow_count = P->borrow_count;
    s.structure_epoch = P->structure_epoch;
    s.objects_moved = P->objects_moved;
    s.bytes_moved = P->bytes_moved;
    s.compact_time_us = P->compact_time_us;
    uint32_t largest = 0;
    // Bounded walk: get_stats must terminate on a corrupted free list
    // (task-book v2 section 9.2) instead of looping forever.
    const uint32_t max_steps =
        (g().segment_size ? g().segment_size : 1) * PM_MAX_SEGMENTS / PM_MIN_BLOCK + 1;
    for (uint32_t f = 0; f < FL_COUNT; ++f)
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
            for (uint32_t off = P->bins.head[f][sl], steps = 0; off != NULL_OFF;) {
                if (++steps > max_steps) {
                    s.largest_free_block = 0;
                    return s; // corrupted; refuse to report
                }
                FreeBlock* b = ptr_of(off);
                if (blk_size_of(b) > largest) largest = blk_size_of(b);
                off = b->next;
            }
    s.largest_free_block = largest;
    return s;
}

// ---------------------------------------------------------------------------
// Allocation (doc section 7.3)
// ---------------------------------------------------------------------------
Status alloc(PoolId pool_id, uint32_t size, uint32_t alignment, uint16_t flags,
             uint32_t user_tag, RawRef& out) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;
    Pool* P = pool_at(pool_id);
    if (!P) return Status::InvalidPool;
    if (P->state != PoolState::Running) return Status::Busy;
    if (size == 0) return Status::InvalidRef;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > PM_MAX_ALIGNMENT)
        return Status::InvalidAlignment;
    if (flags & ~uint16_t(PM_MOVABLE | PM_PINNED | PM_DMA | PM_EXTERNAL | PM_ZERO_INIT))
        return Status::InvalidRef;
    if (flags & (PM_DMA | PM_EXTERNAL)) flags |= PM_PINNED; // doc section 13

    // Overflow-safe size math (task-book section 7): reject instead of wrap.
    // FL ceiling: bins only represent block sizes below 2^PM_FL_MAX; larger
    // requests are refused outright instead of silently clamped into the top
    // bin.
    if (size > UINT32_MAX - (PM_ALIGNMENT - 1)) return Status::NoSpace;
    uint32_t payload = align_up_u(size, PM_ALIGNMENT);
    if (payload > UINT32_MAX - BLOCK_HEADER_SIZE) return Status::NoSpace;
    uint32_t need = payload + BLOCK_HEADER_SIZE;
    if (need < PM_MIN_BLOCK) need = PM_MIN_BLOCK;
    if (need >= (1u << PM_FL_MAX)) return Status::NoSpace;

    // The descriptor slot is a transactional resource: it is only committed
    // once the block header, descriptor, address-order link and statistics
    // are all written. Every failure path below must give it back WITHOUT
    // touching its generation (ABA guard stays intact).
    uint16_t slot = slot_alloc();
    if (slot == NO_SLOT) return Status::NoSpace;

    FreeBlock* blk = bins_find(P->bins, need);
    if (!blk || blk_size_of(blk) < need) {
        slot_release(slot);
        return Status::NoSpace; // bitmap is a hint only
    }
    bins_remove(P->bins, blk);

    uint8_t* blkaddr = reinterpret_cast<uint8_t*>(blk);
    uint32_t blksize = blk_size_of(blk);
    uint32_t old_prev = blk->prev_size;
    uint8_t* poolEnd = pool_end(*P);

    // Split when the remainder can hold a minimal block (doc section 7.1).
    if (blksize - need >= PM_MIN_BLOCK) {
        FreeBlock* rem = reinterpret_cast<FreeBlock*>(blkaddr + need);
        rem->header = (blksize - need) | BLOCK_FREE_BIT; // free block: own bit set
        rem->prev_size = need;                           // prev = the allocation
        rem->prev = rem->next = NULL_OFF;
        bins_insert(P->bins, rem);
        uint8_t* after = blkaddr + blksize;
        if (after < poolEnd) store32(after + 4, blksize - need); // its prev = rem
        blksize = need;
    } else {
        // Remainder smaller than a minimal block is absorbed into the used
        // block (doc 7.1); it stays inside used_bytes and is NOT counted as
        // fragment_bytes — that stat means unreachable slack (doc 8.2).
        uint8_t* after = blkaddr + blksize;
        if (after < poolEnd) store32(after + 4, blksize); // its prev = allocation
    }
    // The allocation itself: own size, bit clear (in use), keeps its
    // predecessor. The header carries the FULL block size — when a tiny
    // remainder was absorbed (no split), that is blksize, not need.
    store32(blkaddr, blksize);
    store32(blkaddr + 4, old_prev);

    // Preserve the slot's lifecycle generation across reuse (ABA guard);
    // free() already bumped it.
    uint16_t gen = next_generation(G.objects[slot].generation);

    ObjectDesc& d = G.objects[slot];
    memset(&d, 0, sizeof(d));
    d.address = blkaddr + BLOCK_HEADER_SIZE;
    d.size = size;
    d.block_size = blksize;
    d.user_tag = user_tag;
    d.pool_id = pool_id;
    d.flags = flags;
    d.generation = gen;
    d.address_epoch = 1;
    d.addr_prev = d.addr_next = NO_ORDER;
    d.state = ObjState::Live;
    if (flags & PM_ZERO_INIT) memset(d.address, 0, size);

    order_insert_sorted(*P, slot);
    P->used_bytes += blksize;
    P->free_bytes = pool_capacity(*P) - P->used_bytes;
    P->live_objects++;
    G.live_object_count++;
    if (G.live_object_count > G.max_live_objects) G.max_live_objects = G.live_object_count;
    if (P->fragment_bytes > G.max_fragment_bytes) G.max_fragment_bytes = P->fragment_bytes;

    out.index = slot;
    out.generation = d.generation;
    out.pool_hint = pool_id;
    out.offset = 0;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Free (doc section 7.4)
// ---------------------------------------------------------------------------
Status free(RawRef const& ref) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;
    // Only a root reference (offset 0) may release an object; sub-object
    // views from pm_ptr::at() must never destroy the parent (task-book 3.7).
    if (ref.offset != 0) return Status::InvalidRef;
    RefCheck rc{};
    Status st = check_ref(ref, 0, 1, rc, /*require_running=*/true, nullptr);
    if (st != Status::Ok) return st;

    Pool& P = *rc.pool;
    ObjectDesc& d = *rc.desc;
    if (d.active_borrows != 0) return Status::Busy;

    d.state = ObjState::Destroying;
    if (d.destroy_fn) d.destroy_fn(d.address);
    if (d.state != ObjState::Destroying) return Status::CorruptMetadata;

    uint8_t* poolEnd = pool_end(P);
    uint8_t* block = d.address - BLOCK_HEADER_SIZE;
    uint32_t bsize = d.block_size;

    // Backward merge: the predecessor is found via this block's prev_size.
    // prev_size == 0 (pool start / slack) never merges.
    uint32_t psize = load32(block + 4);
    if (psize != 0) {
        uint8_t* prev = block - psize;
        if (blk_is_free(prev)) {
            bins_remove(P.bins, reinterpret_cast<FreeBlock*>(prev));
            bsize += blk_size_of(prev);
            block = prev;
        }
    }
    // Forward merge: the successor's own header carries its free bit.
    // Sub-minimal slack is poisoned with prev_size 0 (see finalize_layout);
    // slack is never a free block, so the bit check cannot walk into it.
    uint8_t* next = block + bsize;
    if (next < poolEnd && blk_is_free(next)) {
        FreeBlock* nb = reinterpret_cast<FreeBlock*>(next);
        bins_remove(P.bins, nb);
        bsize += blk_size_of(nb);
    }

    FreeBlock* fb = reinterpret_cast<FreeBlock*>(block);
    fb->header = bsize | BLOCK_FREE_BIT; // prev_size field is already correct
    fb->prev = fb->next = NULL_OFF;
    bins_insert(P.bins, fb);
    next = block + bsize;
    if (next < poolEnd) store32(next + 4, bsize); // successor's prev = merged block

    order_unlink(P, ref.index);
    P.used_bytes -= d.block_size;
    P.free_bytes = pool_capacity(P) - P.used_bytes;
    P.live_objects--;
    G.live_object_count--;

    d.state = ObjState::Free;
    d.generation = next_generation(d.generation);
    d.address = nullptr;
    d.active_borrows = 0;
    d.destroy_fn = nullptr;
    slot_release(ref.index);
    return Status::Ok;
}

Status set_destroy_fn(RawRef const& ref, void (*fn)(void*)) {
    RefCheck rc{};
    Status st = check_ref(ref, 0, 1, rc, /*require_running=*/false, nullptr);
    if (st != Status::Ok) return st;
    // Destroy callbacks exist for pinned non-trivial objects only; a movable
    // object would have its destructor run after an unchecked memmove
    // (task-book v2 section 10). Completed as part of the wrap-up.
    if (!(rc.desc->flags & PM_PINNED)) return Status::NotRelocatable;
    rc.desc->destroy_fn = fn;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Borrow accounting (doc section 6)
// ---------------------------------------------------------------------------
Status borrow_begin(RawRef const& ref, uint32_t access_size, uint32_t access_align,
                    void*& out_addr) {
    PM_LOCK();
    RefCheck rc{};
    void* addr = nullptr;
    Status st = check_ref(ref, access_size, access_align, rc, /*require_running=*/true, &addr);
    if (st == Status::Ok) {
        rc.pool->borrow_count++;
        rc.desc->active_borrows++;
        GlobalState& G = g();
        if (rc.pool->borrow_count > G.max_borrow_count)
            G.max_borrow_count = rc.pool->borrow_count;
        out_addr = addr;
    } else {
        out_addr = nullptr; // no half-initialized output on failure
    }
    PM_UNLOCK();
    return st;
}

void borrow_end(RawRef const& ref) {
    GlobalState& G = g();
    if (ref.index >= PM_MAX_OBJECTS) return;
    ObjectDesc& d = G.objects[ref.index];
    // The end token must match what begin handed out: same live object, same
    // generation, same pool binding. A mismatch means a caller held the
    // borrow across a free (impossible while the borrow counter blocks free)
    // or fabricated a reference; refuse to touch the counters.
    if (d.state != ObjState::Live || d.generation != ref.generation) {
        PM_ASSERT(0 && "borrow_end token does not match its borrow_begin");
        return;
    }
    if (ref.pool_hint != CROSS_HINT && ref.pool_hint != d.pool_id) {
        PM_ASSERT(0 && "borrow_end pool hint mismatch");
        return;
    }
    PM_ASSERT(d.active_borrows > 0);
    Pool* P = pool_at((PoolId)d.pool_id);
    PM_ASSERT(P);
    PM_LOCK();
    d.active_borrows--;
    P->borrow_count--;
    PM_UNLOCK();
}

Status resolve(RawRef const& ref, uint32_t access_size, uint32_t access_align, void*& out_addr) {
    RefCheck rc{};
    void* addr = nullptr;
    Status st = check_ref(ref, access_size, access_align, rc, /*require_running=*/true, &addr);
    if (st == Status::Ok) out_addr = addr;
    return st;
}

// ---------------------------------------------------------------------------
// Compaction (doc section 8)
// ---------------------------------------------------------------------------
Status compact(PoolId id) {
    GlobalState& G = g();
    Pool* P = pool_at(id);
    if (!G.initialized || !P) return Status::InvalidPool;
    // Decide the whole state transition under the same lock borrow_begin
    // uses (task-book section 4), then run the maintenance body outside the
    // lock during the quiescent window.
    PM_LOCK();
    Status st;
    if (P->state == PoolState::Compacting || P->state == PoolState::Merging ||
        P->state == PoolState::Splitting) {
        st = Status::Busy;
    } else {
        // Doc section 8: pause first, then check borrows. A refused compact
        // leaves the pool Paused; the caller resumes explicitly.
        if (P->state == PoolState::Running) P->state = PoolState::Paused;
        if (P->state != PoolState::Paused) st = Status::Busy;
        else if (P->borrow_count != 0) st = Status::Busy;
        else { P->state = PoolState::Compacting; st = Status::Ok; }
    }
    PM_UNLOCK();
    if (st != Status::Ok) return st;
    return compact_impl(*P);
}

// ---------------------------------------------------------------------------
// Pool merge (doc section 10): physically adjacent only; source segments
// transfer to target, object pool_ids are rewritten, then the combined range
// is compacted (rebuilding headers, order list and bins).
// ---------------------------------------------------------------------------
Status merge(PoolId source_id, PoolId target_id) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;

    // Arming (validity, adjacency, state, borrows) happens under the same
    // lock borrow_begin uses, so no new borrow can slip in between the check
    // and the Merging state (task-book v2 section 5). Planning here is the
    // adjacency check itself; the heavy work runs unlocked inside the
    // quiescent window, and the final commit locks again.
    Pool* S = pool_at(source_id);
    Pool* T = pool_at(target_id);
    bool source_above, source_below;
    {
        PM_LOCK();
        if (source_id == target_id) { PM_UNLOCK(); return Status::InvalidPool; }
        S = pool_at(source_id);
        T = pool_at(target_id);
        if (!S || !T) { PM_UNLOCK(); return Status::InvalidPool; }
        if (S->state != PoolState::Running || T->state != PoolState::Running) {
            PM_UNLOCK();
            return Status::Busy;
        }
        if (S->borrow_count != 0 || T->borrow_count != 0) {
            PM_UNLOCK();
            return Status::Busy;
        }
        source_above = (S->segment_first == (uint32_t)T->segment_first + T->segment_count);
        source_below = (T->segment_first == (uint32_t)S->segment_first + S->segment_count);
        if (!source_above && !source_below) { PM_UNLOCK(); return Status::NoSpace; }
        S->state = PoolState::Merging;
        T->state = PoolState::Merging;
        PM_UNLOCK();
    }

    uint16_t first = source_below ? S->segment_first : T->segment_first;
    uint16_t count = (uint16_t)(S->segment_count + T->segment_count);

    for (uint32_t idx = S->order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
        ObjectDesc& d = G.objects[idx];
        d.pool_id = target_id;
        bump_epoch(d.address_epoch); // even if the address is unchanged
    }
    for (uint32_t idx = S->order_head; idx != NO_ORDER;) {
        uint32_t nxt = G.objects[idx].addr_next;
        order_unlink(*S, idx);
        order_insert_sorted(*T, idx);
        idx = nxt;
    }
    S->order_head = NO_ORDER;
    T->segment_first = first;
    T->segment_count = count;
    T->live_objects += S->live_objects;
    T->used_bytes += S->used_bytes; // precheck (inside compact) audits these
    T->free_bytes += S->free_bytes; // the combined range holds both free sets
    S->segment_first = 0;
    S->segment_count = 0;
    S->live_objects = 0;
    S->used_bytes = S->free_bytes = S->fragment_bytes = 0;
    S->bins.reset();

    T->state = PoolState::Compacting;
    // compact_impl's planning cannot fail here: both pools were individually
    // valid (compactable) and the combined range only adds free room below
    // every barrier, so execution-phase failures do not exist (plan A). The
    // defensive branch below stays for Debug assertion coverage.
    Status st = compact_impl(*T);
    if (st != Status::Ok) {
        PM_LOCK();
        S->state = PoolState::Paused;
        T->state = PoolState::Paused;
        PM_UNLOCK();
        return st;
    }
    T->structure_epoch++;
    PM_LOCK();
    memset(&G.pools[source_id], 0, sizeof(Pool)); // source becomes Empty
    PM_UNLOCK();
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Pool split (doc section 11): transactional, two-sided packing.
//
// The segment-aligned boundary acts as a virtual pinned barrier. Objects
// below it (plus movable objects crossing it) pack toward the pool start;
// objects above it (plus the crossing movables) pack from the boundary
// upward. Pinned objects may not cross the boundary (PinnedConflict); if a
// side cannot hold its objects (NoSpace) nothing is moved. Upper-side moves
// run in descending address order, lower-side moves ascending, so no
// not-yet-moved block is ever clobbered.
// ---------------------------------------------------------------------------
Status split(PoolId source_id, uint32_t new_pool_segments, PoolId& out_new) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;

    // Arming under the borrow lock (task-book v2 section 5): state, borrow
    // count, segment math and pool-table capacity are all checked before the
    // pool leaves Running.
    {
        PM_LOCK();
        Pool* Sc = pool_at(source_id);
        if (!Sc) { PM_UNLOCK(); return Status::InvalidPool; }
        if (Sc->state != PoolState::Running) { PM_UNLOCK(); return Status::Busy; }
        if (Sc->borrow_count != 0) { PM_UNLOCK(); return Status::Busy; }
        if (new_pool_segments == 0 || new_pool_segments >= Sc->segment_count) {
            PM_UNLOCK();
            return Status::NoSpace;
        }
        Sc->state = PoolState::Splitting;
        PM_UNLOCK();
    }
    Pool* S = pool_at(source_id);
    PoolId nid = PM_MAX_POOLS;
    for (uint32_t i = 0; i < PM_MAX_POOLS; ++i)
        if (G.pools[i].state == PoolState::Empty) { nid = (PoolId)i; break; }
    if (nid == PM_MAX_POOLS) {
        PM_LOCK();
        S->state = PoolState::Running;
        PM_UNLOCK();
        return Status::NoSpace;
    }

    uint8_t* start = pool_start(*S);
    uint8_t* end = pool_end(*S);
    uint32_t keep = S->segment_count - new_pool_segments;
    uint8_t* boundary = seg_base(S->segment_first + keep);
    Status st = Status::Ok;
    uint32_t nbar = 0, nlow = 0, nup = 0, n_right = 0;

    // Gate every memmove on a full descriptor audit (bounded walks); a
    // pre-check failure restores Running with zero side effects.
    st = precheck_pool(*S, source_id, start, end);
    if (st != Status::Ok) goto fail_restore;

    // Collect pinned barriers (address order).
    for (uint32_t idx = S->order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
        ObjectDesc& d = G.objects[idx];
        if (!(d.flags & PM_PINNED)) continue;
        s_barriers[nbar++] = d.address - BLOCK_HEADER_SIZE;
    }

    // Plan phase (read-only).
    {
        uint8_t* low_cursor = start;
        uint8_t* up_cursor = boundary;
        uint32_t bar = 0;
        for (uint32_t idx = S->order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
            ObjectDesc& d = G.objects[idx];
            uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
            uint32_t bsize = d.block_size;
            bool upper = (bstart >= boundary) || (bstart + bsize > boundary); // crossing or above
            if (d.flags & PM_PINNED) {
                if (bstart < boundary && bstart + bsize > boundary) {
                    st = Status::PinnedConflict;
                    goto fail_restore;
                }
                uint8_t* cur = upper ? up_cursor : low_cursor;
                if (cur > bstart) { st = Status::PinnedConflict; goto fail_restore; }
                if (upper) up_cursor = bstart + bsize;
                else low_cursor = bstart + bsize;
                bar++;
                continue;
            }
            if (!upper) {
                // Pack toward the pool start; never cross the boundary or a
                // pinned barrier below it.
                uint8_t* barrier = boundary;
                if (bar < nbar && s_barriers[bar] < boundary) barrier = s_barriers[bar];
                if (low_cursor + bsize > barrier) {
                    st = (bar < nbar && s_barriers[bar] < boundary) ? Status::PinnedConflict
                                                                    : Status::NoSpace;
                    goto fail_restore;
                }
                s_plan[nlow].slot = idx;
                s_plan[nlow].dst_off = off_of(low_cursor);
                s_plan[nlow].size = bsize;
                nlow++;
                low_cursor += bsize;
            } else {
                // Pack from the boundary upward; barriers are pinned objects
                // above the boundary and the pool end.
                uint8_t* barrier = (bar < nbar) ? s_barriers[bar] : end;
                if (up_cursor + bsize > barrier) {
                    st = (bar < nbar) ? Status::PinnedConflict : Status::NoSpace;
                    goto fail_restore;
                }
                s_upper[nup].slot = idx;
                s_upper[nup].dst_off = off_of(up_cursor);
                s_upper[nup].size = bsize;
                // Rightward entries (source below packed destination) form a
                // prefix of the address-ordered plan: up_cursor - bstart only
                // ever grows by accumulated holes, so once an entry packs
                // left of its source every later entry does too. Counting
                // them here avoids storing per-entry source offsets.
                if (bstart < up_cursor) n_right++;
                nup++;
                up_cursor += bsize;
            }
        }
    }

    // Execution (task-book v2 section 4.2, corrected order). With packing
    // from the boundary, entry i (address order) satisfies
    //   dst_i - src_i = x - sum(holes before i),  x = crossing bytes below
    // the boundary. Rightward entries (dst > src) are therefore a PREFIX of
    // the plan; leftward entries (dst <= src) are the suffix.
    //
    // Safe order, proved against not-yet-moved sources:
    //   1. rightward prefix in DESCENDING source order: dst_i > src_i >=
    //      src_j + size_j for every lower-index j still unmoved, so the write
    //      range never reaches a live source below it;
    //   2. leftward suffix in ASCENDING source order: writes span
    //      [dst_i, dst_{i+1}) and dst_{i+1} <= src_{i+1}, so later sources
    //      are never touched;
    //   3. the rightward group's topmost write end equals the leftward
    //      group's first destination, which is <= its source (packing is
    //      contiguous), so the groups cannot clobber each other.
    // The crossing object (exactly one, by address order) is the first plan
    // entry and always rightward; it runs last in pass 1.
    {
        for (uint32_t i = n_right; i-- > 0;) { // pass 1: rightward, descending
            ObjectDesc& d = G.objects[s_upper[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_upper[i].dst_off;
            PM_ASSERT(i < n_right ? dst >= src : dst <= src);
            if (dst != src) memmove(dst, src, s_upper[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
        }
        for (uint32_t i = n_right; i < nup; ++i) { // pass 2: leftward, ascending
            ObjectDesc& d = G.objects[s_upper[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_upper[i].dst_off;
            if (dst != src) memmove(dst, src, s_upper[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
        }
        // Lower side: ascending, dst <= src, region below the boundary.
        for (uint32_t i = 0; i < nlow; ++i) {
            ObjectDesc& d = G.objects[s_plan[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_plan[i].dst_off;
            if (dst != src) memmove(dst, src, s_plan[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
        }
    }

    // Commit: transfer the upper segments and re-partition the order list.
    {
        Pool& N = G.pools[nid];
        memset(&N, 0, sizeof(N));
        N.state = PoolState::Running;
        N.segment_first = (uint16_t)(S->segment_first + keep);
        N.segment_count = (uint16_t)new_pool_segments;
        N.structure_epoch = 1;
        N.order_head = NO_ORDER;

        uint32_t moved_count = 0;
        for (uint32_t idx = S->order_head; idx != NO_ORDER;) {
            uint32_t nxt = G.objects[idx].addr_next;
            order_unlink(*S, idx);
            ObjectDesc& d = G.objects[idx];
            if (d.address >= boundary) {
                d.pool_id = nid;
                order_insert_sorted(N, idx);
                ++moved_count;
            } else {
                order_insert_sorted(*S, idx);
            }
            idx = nxt;
        }
        S->live_objects -= moved_count;
        N.live_objects = moved_count;

        S->segment_count = (uint16_t)keep;
        // Execution phase: cannot fail (plan A, task-book 3.6).
        finalize_layout(*S, start, boundary);
        finalize_layout(N, boundary, end);

        S->structure_epoch++;
        PM_LOCK();
        S->state = PoolState::Running;
        N.state = PoolState::Running;
        PM_UNLOCK();
        out_new = nid;
        return Status::Ok;
    }

fail_restore:
    // Plan failure: nothing was moved; restore the original state.
    PM_LOCK();
    S->state = PoolState::Running;
    PM_UNLOCK();
    return st;
}

// ---------------------------------------------------------------------------
// Validation (doc section 15, task-book section 6).
// Every linked walk is step-limited so corrupted (cyclic) metadata returns
// CorruptMetadata in bounded time instead of hanging. The checker never
// assumes the lists are well-formed.
// ---------------------------------------------------------------------------
Status validate(PoolId id) {
    GlobalState& G = g();
    if (!G.initialized) return Status::CorruptMetadata;
    Pool* P = pool_at(id);
    if (!P) return Status::InvalidPool;
    uint8_t* start = pool_start(*P);
    uint8_t* end = pool_end(*P);
    uint32_t capacity = (uint32_t)(end - start);
    const uint32_t max_order_steps = PM_MAX_OBJECTS + 1;
    const uint32_t max_free_steps = capacity / PM_MIN_BLOCK + 1;

    // prev_size chain: a block's prev_size must match the physical
    // predecessor's own size, or be 0 (pool start / poisoned slack).
    auto check_prev = [&](uint8_t* bstart) -> Status {
        uint32_t psize = load32(bstart + 4);
        if (psize == 0) return Status::Ok;
        if (psize > (uint32_t)(bstart - start) || psize < PM_MIN_BLOCK)
            return Status::CorruptMetadata;
        uint8_t* prevb = bstart - psize;
        if (blk_size_of(prevb) != psize) return Status::CorruptMetadata;
        return Status::Ok;
    };

    // 1) address_order list: bounded walk, descriptor consistency, strictly
    //    increasing non-overlapping blocks, header and prev_size agreement.
    uint32_t count = 0, used = 0;
    uint32_t prev_idx = NO_ORDER;
    uint8_t* prev_end = nullptr;
    for (uint32_t idx = P->order_head; idx != NO_ORDER; idx = G.objects[idx].addr_next) {
        if (++count > max_order_steps) return Status::CorruptMetadata;
        if (idx >= PM_MAX_OBJECTS) return Status::CorruptMetadata;
        ObjectDesc& d = G.objects[idx];
        if (d.state != ObjState::Live || d.pool_id != id || d.generation == 0)
            return Status::CorruptMetadata;
        if (d.addr_prev != prev_idx) return Status::CorruptMetadata;
        uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
        if (bstart < start || d.address + d.size > end) return Status::CorruptMetadata;
        if (((uintptr_t)d.address & (PM_ALIGNMENT - 1)) != 0) return Status::CorruptMetadata;
        if (prev_end && bstart < prev_end) return Status::CorruptMetadata; // order/overlap
        if (blk_is_free(bstart) || blk_size_of(bstart) != d.block_size)
            return Status::CorruptMetadata; // used block header must agree
        if (check_prev(bstart) != Status::Ok) return Status::CorruptMetadata;
        prev_end = bstart + d.block_size;
        used += d.block_size;
        prev_idx = idx;
    }
    if (prev_idx != NO_ORDER && G.objects[prev_idx].addr_next != NO_ORDER)
        return Status::CorruptMetadata;
    if (count != P->live_objects || used != P->used_bytes) return Status::CorruptMetadata;
    if (P->free_bytes != capacity - used) return Status::CorruptMetadata;

    // 2) bins: non-empty list <=> bitmap bit; bounded walks; every free block
    //    sane, linked consistently, chained to its physical predecessor, and
    //    not overlapping any live block. The size total must reconcile with
    //    the pool's byte accounting: a duplicated or missing free block (e.g.
    //    the same block inserted into two bins) breaks the total.
    uint64_t free_total = 0;
    for (uint32_t f = 0; f < FL_COUNT; ++f) {
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl) {
            uint32_t off = P->bins.head[f][sl];
            bool any = off != NULL_OFF;
            bool bit = (P->bins.sl_bitmap[f] >> sl) & 1u;
            if (any != bit) return Status::CorruptMetadata;
            uint32_t steps = 0;
            for (; off != NULL_OFF; off = ptr_of(off)->next) {
                if (++steps > max_free_steps) return Status::CorruptMetadata;
                FreeBlock* b = ptr_of(off);
                uint8_t* baddr = reinterpret_cast<uint8_t*>(b);
                if (baddr < start || baddr + blk_size_of(b) > end) return Status::CorruptMetadata;
                if (blk_size_of(b) < PM_MIN_BLOCK) return Status::CorruptMetadata;
                if (!blk_is_free(baddr)) return Status::CorruptMetadata;
                if (fl_index(blk_size_of(b)) - MIN_FL != f ||
                    sl_index(blk_size_of(b), fl_index(blk_size_of(b))) != sl)
                    return Status::CorruptMetadata;
                if (b->next != NULL_OFF && ptr_of(b->next)->prev != off)
                    return Status::CorruptMetadata;
                if (check_prev(baddr) != Status::Ok) return Status::CorruptMetadata;
                free_total += blk_size_of(b);
                uint8_t* fend = baddr + blk_size_of(b);
                for (uint32_t idx = P->order_head; idx != NO_ORDER;
                     idx = G.objects[idx].addr_next) {
                    ObjectDesc& d = G.objects[idx];
                    uint8_t* ls = d.address - BLOCK_HEADER_SIZE;
                    if (ls < fend && baddr < ls + d.block_size)
                        return Status::CorruptMetadata;
                }
            }
        }
    }
    if (free_total != (uint64_t)P->free_bytes - (uint64_t)P->fragment_bytes)
        return Status::CorruptMetadata;

    uint32_t flcheck = 0;
    for (uint32_t f = 0; f < FL_COUNT; ++f)
        if (P->bins.sl_bitmap[f]) flcheck |= 1u << f;
    if (flcheck != P->bins.fl_bitmap) return Status::CorruptMetadata;
    return Status::Ok;
}

} // namespace pm
