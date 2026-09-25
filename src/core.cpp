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

// ===========================================================================
// The relocation copy primitive.
//
// Byte-for-byte identical to memmove(), but the primitive is chosen from the
// two addresses instead of always taking the general one.
//
// Why this is even a question: the device reports the maintenance window as
// ~7.6 ms for ~139 KB moved, i.e. ~18 MB/s, on internal SRAM at 240 MHz. That
// is far below what a word-at-a-time loop should reach, so the copy itself is
// a suspect and has to be measured against a calibration loop rather than
// assumed (see probe.cpp E7).
//
// Why it is legal: every block start and every block size the planner produces
// is a multiple of PM_ALIGNMENT (the block format requires 8-byte alignment and
// all sizes are round-tripped through it), so the region can be copied in
// 4-byte words. dst < src copies forward, dst > src copies backward -- which is
// exactly memmove's contract, derived from the same two addresses.
//
// If the alignment precondition is ever violated the helper falls back to
// memmove: a wrong guess here would corrupt memory, so it is checked rather
// than asserted away.
static void move_block(uint8_t* dst, uint8_t* src, uint32_t size) {
    if (dst == src || size == 0) return;
    uintptr_t const d = (uintptr_t)dst;
    uintptr_t const s = (uintptr_t)src;
    uintptr_t const n = (uintptr_t)size;
    // Disjointness is provable in O(1) from the two addresses and the size, and
    // in compaction it is the common case: the planner packs objects into gaps,
    // so a move either ends before its source or starts after it. When it
    // holds, `memcpy` is the right primitive and on this target it is 19x the
    // throughput of `memmove` (measured, probe.cpp E7a: 378 MB/s vs 19.9 MB/s,
    // flat in size -- the platform's memmove is byte-wise, its memcpy is not).
    if (d + n <= s || s + n <= d) {
        memcpy(dst, src, size);
        return;
    }
    if ((d | s | n) & (uintptr_t)(PM_ALIGNMENT - 1)) {
        memmove(dst, src, size); // contract violated; stay correct
        return;
    }
    uint32_t const words = size / 4;
    uint32_t* dp = reinterpret_cast<uint32_t*>(dst);
    uint32_t const* sp = reinterpret_cast<uint32_t const*>(src);
    if (dst < src) {
        for (uint32_t i = 0; i < words; ++i) dp[i] = sp[i];
    } else {
        for (uint32_t i = words; i-- > 0;) dp[i] = sp[i];
    }
}

inline uint32_t blk_size_of(void const* hdr) { return load32(hdr) & ~BLOCK_FREE_BIT; }
inline bool blk_is_free(void const* hdr) { return (load32(hdr) & BLOCK_FREE_BIT) != 0; }

inline uint32_t align_up_u(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t log2_floor_u(uint32_t v) {
    return v <= 1 ? 0 : (uint32_t)(31 - __builtin_clz(v));
}

} // unnamed namespace

namespace internal {

GlobalState& g() {
    // Zero-initialised by its static storage duration, and init() memsets it
    // before any field is read, so it is deliberately never assigned here.
    // cppcheck-suppress unassignedVariable
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

FreeBlock* bins_find(TlsfBins const& b, uint32_t need) {
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
                // Screen the offset before dereferencing (task-book v2 section
                // 9.2): a damaged head/link must be refused, not followed.
                if (!zone_off_readable(off)) return nullptr;
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

// O(1) geometry proof for a pool's segment window (round-11 fix 4). Both the
// segment sum and the byte volume are computed in uint64 so a corrupted
// segment_count can neither wrap nor overflow its way into a valid-looking
// window; the byte bound also holds the window against G.zone_size
// independently of the segment count. Every entry that derives a range or a
// pointer from pool geometry calls this exactly once:
//   check_ref()        -- free/borrow_begin/resolve/set_destroy_fn
//   alloc()            -- before any mutation
//   precheck_pool()    -- compact/merge/split (and the advice audit via its
//                         own counter reconciliation)
//   create_pool()      -- for every EXISTING pool (round-11 fix 3: the
//                         used[] scan below must never trust an unproven
//                         window, or it writes out of bounds and hides live
//                         segments from the free-run search)
inline bool pool_geometry_ok(Pool const& P) {
    GlobalState const& G = g();
    uint64_t const seg_end = (uint64_t)P.segment_first + P.segment_count;
    if (seg_end > G.segment_count) return false;
    return seg_end * (uint64_t)G.segment_size <= (uint64_t)G.zone_size;
}

Pool* pool_at(PoolId id) {
    if (id >= PM_MAX_POOLS) return nullptr;
    Pool& P = g().pools[id];
    return P.state == PoolState::Empty ? nullptr : &P;
}

// Round-11 fix 7 (task-book section 4: "Debug should report an impending
// wrap"): the two counters below remap their wrap to 1 because 0 is reserved.
// The remap was silent; Debug now reports it. Compiled to nothing in Release,
// and the branch is reachable only after 2^32 epoch bumps or 65535 slot
// reuses, so the hot paths pay nothing.
#if PM_DEBUG
void pm_wrap_note(char const* what) {
#if defined(PM_ESP32)
    printf("PM_DEBUG: %s wrapped; counter remapped to 1 (0 is reserved)\n", what);
#else
    fprintf(stderr, "PM_DEBUG: %s wrapped; counter remapped to 1 (0 is reserved)\n", what);
#endif
}
#endif

inline void bump_epoch(uint32_t& epoch) {
    // Wrap-around guard: 0 is reserved (doc section 4). Only reachable after
    // 2^32 bumps; it is kept because the counter is a public diagnostic and
    // a zero epoch would read as "no address change" to a caller.
    // cppcheck-suppress knownConditionTrueFalse ; the wrap from 0xFFFFFFFF is
    // real, cppcheck's value-range analysis cannot see the counter's history.
    if (++epoch == 0) {
#if PM_DEBUG
        pm_wrap_note("address_epoch");
#endif
        epoch = 1;
    }
}

// --- address-order list (sorted by block address) ---------------------------
void order_unlink(Pool& P, uint32_t idx) {
    ObjectDesc& D = g().objects[idx];
    if (D.addr_prev != NO_ORDER) g().objects[D.addr_prev].addr_next = D.addr_next;
    else P.order_head = D.addr_next;
    if (D.addr_next != NO_ORDER) g().objects[D.addr_next].addr_prev = D.addr_prev;
    D.addr_prev = D.addr_next = NO_ORDER;
}

// O(1) append to the pool's live-slot list.
//
// The list is deliberately NOT kept in address order any more. Keeping it
// sorted inside alloc() was ~100% of alloc's total cost and grew linearly with
// the live-object count (measured: 232 ns at live=256, 799 ns at live=1024, of
// which the walk is essentially all -- see bench/RESULTS.md). Address order is
// needed only by the maintenance paths, which can afford one O(n log n) sort
// per invocation on the cold path while alloc sits on the hot path.
//
// Consequence, recorded in docs/AUDIT_LEDGER.md: alloc no longer detects a
// damaged list. Detection moved to collect_live_sorted(), which every
// maintenance entry and validate() go through. Appending cannot fail, so alloc
// now has NO failure path at all after its block/bin mutation begins -- which
// is strictly stronger than the previous "link first, then mutate" ordering
// that round-4 R29 was written to guarantee.
void order_append(Pool& P, uint32_t idx) {
    ObjectDesc* D = g().objects;
    // The head is screened by alloc() (round-11 fix 1) because the write
    // below dereferences it without a walk; the assert keeps a Debug tripwire
    // on the write itself.
    PM_ASSERT(P.order_head == NO_ORDER || P.order_head < PM_MAX_OBJECTS);
    D[idx].addr_prev = NO_ORDER;
    D[idx].addr_next = P.order_head;
    if (P.order_head != NO_ORDER) D[P.order_head].addr_prev = idx;
    P.order_head = idx;
}

// In-place heapsort of descriptor indices by block address, tie-broken by index
// so the result is deterministic.
//
// Heapsort is chosen deliberately over std::sort: the core has no <algorithm>
// dependency, and this needs no recursion, no allocation, an O(n log n) worst
// case, and about twenty lines that can be audited by eye.
void sort_slots_by_address(uint16_t* a, uint32_t n) {
    GlobalState const& G = g();
    auto key = [&](uint16_t i) { return (uintptr_t)G.objects[i].address; };
    auto less = [&](uint16_t x, uint16_t y) {
        return key(x) != key(y) ? key(x) < key(y) : x < y;
    };
    auto sift = [&](uint32_t root, uint32_t end) {
        for (;;) {
            uint32_t child = 2 * root + 1;
            if (child > end) return;
            if (child + 1 <= end && less(a[child], a[child + 1])) ++child;
            if (!less(a[root], a[child])) return;
            uint16_t const t = a[root];
            a[root] = a[child];
            a[child] = t;
            root = child;
        }
    };
    if (n < 2) return;
    for (uint32_t i = n / 2; i-- > 0;) sift(i, n - 1);
    for (uint32_t i = n - 1; i > 0; --i) {
        uint16_t const t = a[0];
        a[0] = a[i];
        a[i] = t;
        sift(0, i - 1);
    }
}

// In-place heapsort of a u32 array in ascending order. Same shape and the same rationale as sort_slots_by_address
// above -- no recursion, no allocation, auditable by eye, O(n log n) worst case.
void sort_u32_asc(uint32_t* a, uint32_t n) {
    auto sift = [&](uint32_t root, uint32_t end) {
        for (;;) {
            uint32_t child = 2 * root + 1;
            if (child > end) return;
            if (child + 1 <= end && a[child] < a[child + 1]) ++child;
            if (a[root] >= a[child]) return;
            uint32_t const t = a[root];
            a[root] = a[child];
            a[child] = t;
            root = child;
        }
    };
    if (n < 2) return;
    for (uint32_t i = n / 2; i-- > 0;) sift(i, n - 1);
    for (uint32_t i = n - 1; i > 0; --i) {
        uint32_t const t = a[0];
        a[0] = a[i];
        a[i] = t;
        sift(0, i - 1);
    }
}

// Packed-key sort scratch: key and slot in ONE u32, (zone_offset << 8) | slot.
//
// Why: measured on the device, the maintenance window costs ~3,100 ns per live
// object, of which the address-order sort is the single largest piece
// (callgrind: 213 instructions per object inside collect_live_sorted). The
// shipped comparator reads G.objects[i].address, so every comparison chases two
// random 52-byte descriptors; packing the key and the slot index into ONE u32,
// (zone_offset << 8) | slot, turns that into a sort of a compact u32 array with
// no indirection. The key is monotone in the address, so the resulting order --
// including the "tie-break by slot index" rule -- is unchanged.
//
// Requires zone_size <= 2^24 (24-bit offset) and PM_MAX_OBJECTS <= 256 (8-bit
// slot); collect_live_sorted checks both and falls back to the shipped sort.
//
// Declared here rather than beside the other scratch because collect_live_sorted,
// the only user, is defined above those.
uint32_t s_ord_key[PM_MAX_OBJECTS];

// Adaptive sorter for the packed keys. order_append() pushes to the head of the
// list, so the walk returns the objects in REVERSE creation order -- and because
// the allocator hands out ascending addresses, that is usually DESCENDING
// address order already. Both fast paths therefore matter: an already-ascending
// input returns untouched, an already-descending one is simply reversed, each in
// O(n) with zero comparisons.
//
// Getting only the ascending check right -- the first version of this function
// -- made the adaptive path UNREACHABLE for the layout the allocator actually
// produces, so the "optimised" sort quietly ran the full O(n log n) work on a
// fully reversed array, i.e. on runs of length one. The phase instrument in
// compact_impl is what exposed it: the sort showed up as 39% of the maintenance
// window on a pool that had been filled sequentially.
//
// Anything else falls through to the in-place heapsort, which needs no scratch
// at all. That is sound here because the keys are DISTINCT -- the slot index
// occupies the low 8 bits, and two objects can never share a slot -- so the
// sorted order is unique and therefore independent of the algorithm. No
// stability requirement, and no second buffer to size or to justify.
void sort_packed_keys(uint32_t* a, uint32_t n) {
    if (n < 2) return;
    bool ascending = true, descending = true;
    for (uint32_t k = 1; k < n; ++k) {
        if (a[k - 1] > a[k]) ascending = false;
        if (a[k - 1] < a[k]) descending = false;
        if (!ascending && !descending) break;
    }
    if (ascending) return;
    if (descending) { // the common case, and the one the first version missed
        for (uint32_t i = 0; i < n / 2; ++i) {
            uint32_t const t = a[i];
            a[i] = a[n - 1 - i];
            a[n - 1 - i] = t;
        }
        return;
    }
    sort_u32_asc(a, n);
}

// Collect a pool's live descriptors into `out` in address order.
//
// This is the ONLY sanctioned way to enumerate a pool: every maintenance entry,
// validate() and the advice analysis go through it. It re-establishes, on the
// cold path, the ordering that alloc() no longer maintains -- and it is
// therefore also where a damaged live-slot list is detected, with the same
// bounded walk and structural checks the traversal always had:
//   * a step cap (cycle / overrun guard),
//   * index range and descriptor state / pool / generation checks,
//   * the addr_prev link must agree with the walk,
//   * duplicate addresses are refused (they would be an overlap).
// Sorting reads metadata VALUES only; nothing here dereferences an address
// derived from untrusted metadata.
Status collect_live_sorted(Pool const& P, PoolId pid, uint16_t* out,
                           uint32_t out_cap, uint32_t& out_n) {
    GlobalState const& G = g();
    uint32_t prev = NO_ORDER;
    uint32_t steps = 0;
    uint32_t n = 0;
    for (uint32_t idx = P.order_head; idx != NO_ORDER;
         idx = G.objects[idx].addr_next) {
        if (++steps > PM_MAX_OBJECTS + 1) return Status::CorruptMetadata;
        if (idx >= PM_MAX_OBJECTS) return Status::CorruptMetadata;
        ObjectDesc const& d = G.objects[idx];
        if (d.state != ObjState::Live || d.pool_id != pid || d.generation == 0)
            return Status::CorruptMetadata;
        if (d.addr_prev != prev) return Status::CorruptMetadata;
        if (n >= out_cap) return Status::CorruptMetadata;
        out[n++] = (uint16_t)idx;
        prev = idx;
    }
    // Packed-key sort: (zone_offset << 8) | slot, one u32, no indirection.
    // The width assumption is checked, not assumed -- outside it the shipped
    // comparator sort runs instead, so the ordering is correct either way.
    if (G.zone_size <= (1u << 24) && PM_MAX_OBJECTS <= 256u) {
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t const off = (uint32_t)((uintptr_t)G.objects[out[i]].address -
                                            (uintptr_t)G.zone);
            s_ord_key[i] = (off << 8) | (uint32_t)out[i];
        }
        // s_plan cannot be borrowed here: it is declared further down the file
        // and, more importantly, borrowing it would couple this sort to
        // "collect always runs before planning" -- a property no signature
        // states. A dedicated PM_MAX_OBJECTS scratch with no second buffer is
        // cheaper than that coupling.
        sort_packed_keys(s_ord_key, n);
        for (uint32_t i = 0; i < n; ++i) out[i] = (uint16_t)(s_ord_key[i] & 0xFFu);
    } else
    {
        sort_slots_by_address(out, n);
    }
    for (uint32_t i = 1; i < n; ++i) {
        if (G.objects[out[i]].address == G.objects[out[i - 1]].address)
            return Status::CorruptMetadata;
    }
    out_n = n;
    return Status::Ok;
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

// uint16 generation bump. 0xFFFF -> 0 would collide with the "invalid"
// generation, so it is remapped to 1. This is a real wrap-around, not a
// dead branch: a slot reused 65535 times reaches it.
inline uint16_t next_generation(uint16_t gen) {
    uint16_t n = (uint16_t)(gen + 1); // wraps at 0xFFFF -> 0
    // cppcheck-suppress knownConditionTrueFalse ; the wrap is real, cppcheck
    // cannot prove the incoming value's range.
    if (n == 0) {                     // 0 reserved as invalid (doc section 4)
#if PM_DEBUG
        pm_wrap_note("generation");
#endif
        return 1;
    }
    return n;
}

// --- reference validation (doc section 5 resolution rules 1-4) --------------
struct RefCheck {
    Pool* pool;
    ObjectDesc* desc;
};

// Descriptor <-> block relationship, descriptor-only (no zone dereference so
// it stays O(1) on every borrow/resolve). A descriptor that cannot describe a
// real block means the metadata is corrupt: the address derived from it must
// never be handed out (task-book v2 section 9.1 / 11.1 item 8).
inline bool desc_block_consistent(ObjectDesc const& d) {
    if (d.block_size < PM_MIN_BLOCK) return false;
    if ((d.block_size & (PM_ALIGNMENT - 1)) != 0) return false;
    if (d.size > d.block_size - BLOCK_HEADER_SIZE) return false;
    if (d.size == 0) return false;
    if (((uintptr_t)d.address & (PM_ALIGNMENT - 1)) != 0) return false;
    return true;
}

Status check_ref(RawRef const& ref, uint32_t access_size, uint32_t access_align,
                 RefCheck& rc, bool require_running, void** out_addr) {
    GlobalState& G = g();
    if (!G.initialized) return Status::NotInitialized;
    if (ref.index >= PM_MAX_OBJECTS || ref.generation == 0) return Status::InvalidRef;
    ObjectDesc& d = G.objects[ref.index];
    if (d.state != ObjState::Live) return Status::InvalidRef;
    if (d.generation != ref.generation) return Status::InvalidRef;
    if (!desc_block_consistent(d)) return Status::CorruptMetadata;
    // Round-11 fix 2: order_unlink() dereferences these links directly during
    // free() (O(1) contract, no walk), so a Live descriptor whose links point
    // outside the descriptor table must be refused before any caller reaches
    // a mutation path. One O(1) check here covers free/borrow/resolve alike;
    // collect_live_sorted() keeps its own (stronger) chain checks on the
    // maintenance side and is not duplicated.
    if ((d.addr_prev != NO_ORDER && d.addr_prev >= PM_MAX_OBJECTS) ||
        (d.addr_next != NO_ORDER && d.addr_next >= PM_MAX_OBJECTS))
        return Status::CorruptMetadata;
    Pool* P = pool_at((PoolId)d.pool_id);
    if (!P) return Status::CorruptMetadata;
    // Pool-range proof (round-3 guide 5.1): the descriptor's block must sit
    // inside the descriptor's own pool BEFORE any pointer is derived from it.
    // All math on possibly-corrupt fields uses uint64 zone offsets -- pointer
    // arithmetic on a forged address would be undefined behaviour, and a
    // pool-outside-but-aligned address must never be handed out (R23).
    {
        // Round-11 fix 4: pstart/pend below are derived from the pool's own
        // segment fields; prove the geometry against the zone limit first
        // (O(1)) or an inflated window widens the proof past the zone end.
        if (!pool_geometry_ok(*P)) return Status::CorruptMetadata;
        uint64_t const zbase = (uint64_t)(uintptr_t)G.zone;
        uint64_t const aabs = (uint64_t)(uintptr_t)d.address;
        if (aabs < zbase + BLOCK_HEADER_SIZE) return Status::CorruptMetadata;
        uint64_t const aoff = aabs - zbase;
        uint64_t const boff = aoff - BLOCK_HEADER_SIZE;
        uint64_t const pstart = (uint64_t)(uintptr_t)pool_start(*P) - zbase;
        uint64_t const pend = pstart + (uint64_t)pool_capacity(*P);
        if (boff < pstart || boff >= pend) return Status::CorruptMetadata;
        if ((uint64_t)d.block_size > pend - boff) return Status::CorruptMetadata;
        if (aoff + (uint64_t)d.size > pend) return Status::CorruptMetadata;
    }
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

// Maintenance entry contract (task-book v2 section 5.1). All three maintenance
// operations accept a pool in Running OR Paused, so a caller may either pause
// explicitly (doc section 8's quiescent window) or let the operation do it.
// A pool already inside a maintenance state, or one with live borrows, is
// refused. On a planning failure the entry state is restored, so a caller that
// paused stays paused.
inline bool pool_maintainable(Pool const& P) {
    return P.state == PoolState::Running || P.state == PoolState::Paused;
}

// True when `fb` is a member of the bin its own size selects, with neighbour
// links that reciprocate the list position (round-3 guide 6.1).
//
// ===========================================================================
// The O(1) replacement for the reachability walk this function used to do.
// ===========================================================================
//
// The shipped version walks the bin from its head until it meets `fb`, which
// costs O(bin chain length) -- and free() calls it up to twice per call, so
// the documented bound for free() is "O(1 + neighbour bin chains)".
//
// This variant replaces the reachability walk with an O(1) *anchored link
// proof*. It establishes exactly the properties that the bins_remove() call
// which follows actually depends on:
//
//   * `fb` and both of its neighbours lie inside the pool window, and are
//     screened BEFORE they are dereferenced, exactly as every other traversal
//     in this file does;
//   * the links reciprocate: prev->next == fb and next->prev == fb;
//   * if prev == NULL_OFF then `fb` IS the head of the bin its own size
//     selects, so a removal taking the "replace the head" branch cannot orphan
//     a chain it is not at the root of.
//
// bins_remove() writes only to fb itself, to ptr_of(fb->prev), to
// ptr_of(fb->next), and (when prev == NULL_OFF) to the bin head. Every one of
// those targets is proven in-pool above, so no out-of-bounds write becomes
// reachable through this change.
//
// WHAT IS GIVEN UP. "The node is reachable from its bin head" is no longer
// proven here. Reachability is still enforced, unchanged, by
// audit_pool_bins() -- which runs at every maintenance entry and on alloc()'s
// failure path -- and by validate(). What is preserved: no out-of-bounds
// write, no unbounded walk, and a deterministic refusal of an out-of-pool
// cursor. Whether that trade is worth making is the empirical question that
// probe.cpp exists to answer.
bool free_block_binned(Pool const& P, FreeBlock const* fb) {
    uint32_t const sz = blk_size_of(fb);
    uint32_t const fl = fl_index(sz);
    uint32_t const sl = sl_index(sz, fl);
    uint32_t const off = off_of(fb);
    uint64_t const zbase = (uint64_t)(uintptr_t)g().zone;
    uint64_t const start_off = (uint64_t)(uintptr_t)pool_start(P) - zbase;
    uint64_t const end_off = start_off + (uint64_t)pool_capacity(P);
    // Screen every offset before forming a pointer from it (task-book v2
    // section 9.2): a damaged link must be refused, not followed.
    auto in_pool = [&](uint32_t o) -> bool {
        return (uint64_t)o >= start_off &&
               (uint64_t)o + BLOCK_HEADER_SIZE <= end_off;
    };
    if (!in_pool(off)) return false;
    uint32_t const head = P.bins.head[fl - MIN_FL][sl];
    // The head is read in either branch below, so screen it once here.
    if (head != NULL_OFF && !in_pool(head)) return false;
    if (fb->prev != NULL_OFF) {
        if (!in_pool(fb->prev)) return false;
        if (ptr_of(fb->prev)->next != off) return false;
    } else if (head != off) {
        return false; // claims to be its bin's root, but the head is another
    }
    if (fb->next != NULL_OFF) {
        if (!in_pool(fb->next)) return false;
        if (ptr_of(fb->next)->prev != off) return false;
    }
    return true;
}

// --- maintenance pre-check (task-book v2 section 8) --------------------------
// Full descriptor audit before ANY maintenance planning, expressed over the
// address-ordered slot array produced by collect_live_sorted(). Plan A's
// "execution cannot fail" rests on this function: descriptor fields, ordering,
// overlap, pool range and the byte statistics are all verified here with
// bounded work, so corrupted (cyclic) metadata is refused instead of hanging.
// Runs in Release too -- its verdict gates every memmove. All range math uses
// uint64 zone offsets, never raw pointer differences on possibly-corrupted
// values. The prev_size chain is deliberately NOT checked: after a pool merge
// the physical predecessor of a block may come from the other pool until the
// post-merge compaction rewrites all headers (validate() enforces the chain on
// settled layouts).
//
// `out` / `out_cap` / `out_n` receive the audited, address-sorted slot array.
// Every caller plans from THAT array and never re-traverses the (untrusted)
// list -- which is what makes collect_live_sorted the single detection point
// for a damaged live-slot list.
Status precheck_pool(Pool const& P, PoolId pid, uint8_t const* start,
                     uint8_t const* end, uint16_t* out, uint32_t out_cap,
                     uint32_t& out_n) {
    GlobalState const& G = g();
    // Round-11 fix 4: every range below is derived from the pool's segment
    // fields; prove the geometry against the zone limit once (O(1)) before
    // any of it is used, or compact/merge/split plan against a window that
    // crosses the zone end.
    if (!pool_geometry_ok(P)) return Status::CorruptMetadata;
    const uint64_t zone_base = (uint64_t)(uintptr_t)G.zone;
    const uint64_t start_off = (uint64_t)(uintptr_t)start - zone_base;
    const uint64_t end_off = (uint64_t)(uintptr_t)end - zone_base;

    Status st = collect_live_sorted(P, pid, out, out_cap, out_n);
    if (st != Status::Ok) return st;

    uint32_t used = 0;
    // The SAME predicates, evaluated in native pointer width instead of 64-bit
    // zone offsets.
    //
    // The shipped loop converts every descriptor address into a 64-bit zone
    // offset, which costs ~7 64-bit operations and 5 64-bit comparisons per
    // object -- 2-3 instructions each on a 32-bit target. Measured, this loop
    // is ~34% of the whole maintenance window (probe.cpp E7c).
    //
    // All of it is expressible directly in pointers, because the window bounds
    // are themselves valid in-zone addresses:
    //   * `lo` is the lowest legal payload address; `end` the exclusive limit;
    //   * once `d.address >= lo` holds, `d.address - BLOCK_HEADER_SIZE` is a
    //     pointer into the zone, so it cannot wrap, and every difference below
    //     is non-negative once `bstart >= end` and `d.address > end` have been
    //     rejected. Those two are implied by the shipped arithmetic (a block
    //     needs at least PM_MIN_BLOCK bytes and a payload at least one), so the
    //     reject set is unchanged -- check for check:
    //       shipped aabs < zone_base+8   is subsumed by d.address < lo, and the
    //                                    check it is followed by (boff >=
    //                                    start_off) is exactly d.address >= lo;
    //       shipped boff + bsize > end_off   <=> bsize > end - bstart
    //       shipped aoff + size  > end_off   <=> size  > end - d.address
    uint8_t const* const lo = start + BLOCK_HEADER_SIZE;
    uint8_t const* prev_end = start;
    for (uint32_t i = 0; i < out_n; ++i) {
        ObjectDesc const& d = G.objects[out[i]];
        if (d.block_size < PM_MIN_BLOCK || (d.block_size & (PM_ALIGNMENT - 1)) != 0)
            return Status::CorruptMetadata;
        if (d.size > d.block_size - BLOCK_HEADER_SIZE) return Status::CorruptMetadata;
        if (d.address < lo || ((uintptr_t)d.address & (PM_ALIGNMENT - 1)) != 0)
            return Status::CorruptMetadata;
        uint8_t const* const bstart = d.address - BLOCK_HEADER_SIZE;
        if (bstart >= end || d.address > end) return Status::CorruptMetadata;
        if (d.block_size > (uint32_t)(end - bstart)) return Status::CorruptMetadata;
        if (d.size > (uint32_t)(end - d.address)) return Status::CorruptMetadata;
        if (bstart < prev_end) return Status::CorruptMetadata; // ordering + overlap
        // Physical header must already agree with the descriptor.
        uint32_t const own = load32(bstart);
        if ((own & BLOCK_FREE_BIT) != 0 || (own & ~BLOCK_FREE_BIT) != d.block_size)
            return Status::CorruptMetadata;
        prev_end = bstart + d.block_size;
        used += d.block_size;
    }
    if (out_n != P.live_objects || used != P.used_bytes) return Status::CorruptMetadata;
    if (P.free_bytes != (uint32_t)(end_off - start_off) - used) return Status::CorruptMetadata;
    return Status::Ok;
}

// prev_size chain: a block's prev_size must match the physical predecessor's
// own size, or be 0 (pool start / poisoned slack). `boff` must already have
// passed the caller's bounds check.
Status prev_link_ok(uint64_t boff, uint64_t start_off) {
    GlobalState const& G = g();
    uint32_t psize = load32(G.zone + boff + 4);
    if (psize == 0) return Status::Ok;
    if (psize < PM_MIN_BLOCK || (uint64_t)psize > boff - start_off)
        return Status::CorruptMetadata;
    uint64_t poff = boff - psize;
    if (blk_size_of(G.zone + poff) != psize) return Status::CorruptMetadata;
    return Status::Ok;
}

// --- bounded bins audit -------------------------------------------------------
// Free-list structure of one pool: bitmap/list agreement, in-pool cursors,
// size-class agreement, reciprocal neighbour links and the prev_size chain.
// Shared by validate() and the maintenance pre-audits -- merge must reject a
// pool with damaged bins BEFORE planning, not paper over them in finalize
// (round-3 guide 3.1). O(free blocks). When `free_total_out` is non-null it
// receives the sum of all binned block sizes.
Status audit_pool_bins(Pool const& P, uint64_t* free_total_out) {
    // Pure-integer geometry: this audit runs on pools whose segment fields
    // are not proven yet, so no pointer may be formed from them (A4-10).
    uint64_t const start_off = pool_start_off(P);
    uint64_t const end_off = start_off + (uint64_t)pool_capacity(P);
    const uint32_t max_free_steps = (uint32_t)(pool_capacity(P) / PM_MIN_BLOCK) + 1;

    auto in_pool = [&](uint64_t off, uint64_t len) -> bool {
        return off >= start_off && off <= end_off && len <= end_off - off;
    };
    auto head_ok = [&](uint64_t off) -> bool { return in_pool(off, BLOCK_HEADER_SIZE); };

    uint64_t total = 0;
    for (uint32_t f = 0; f < FL_COUNT; ++f) {
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl) {
            uint32_t off = P.bins.head[f][sl];
            bool const any = off != NULL_OFF;
            bool const bit = (P.bins.sl_bitmap[f] >> sl) & 1u;
            if (any != bit) return Status::CorruptMetadata;
            uint32_t steps = 0;
            for (; off != NULL_OFF; off = ptr_of(off)->next) {
                if (++steps > max_free_steps) return Status::CorruptMetadata;
                // The cursor must be a readable block start before it is
                // dereferenced; `off` is the only untrusted value here.
                if (!head_ok(off)) return Status::CorruptMetadata;
                FreeBlock* b = ptr_of(off);
                uint32_t bsize = blk_size_of(b);
                if (bsize < PM_MIN_BLOCK || (bsize & (PM_ALIGNMENT - 1)) != 0)
                    return Status::CorruptMetadata;
                if (!in_pool(off, bsize)) return Status::CorruptMetadata;
                if (!blk_is_free(b)) return Status::CorruptMetadata;
                if (fl_index(bsize) - MIN_FL != f ||
                    sl_index(bsize, fl_index(bsize)) != sl)
                    return Status::CorruptMetadata;
                // Neighbour links must point at readable block starts too.
                if (b->next != NULL_OFF) {
                    if (!head_ok(b->next)) return Status::CorruptMetadata;
                    if (ptr_of(b->next)->prev != off) return Status::CorruptMetadata;
                }
                if (b->prev != NULL_OFF) {
                    if (!head_ok(b->prev)) return Status::CorruptMetadata;
                    if (ptr_of(b->prev)->next != off) return Status::CorruptMetadata;
                }
                if (prev_link_ok(off, start_off) != Status::Ok)
                    return Status::CorruptMetadata;
                total += bsize;
            }
        }
    }
    // The first-level bitmap must agree with the second-level map.
    uint32_t flcheck = 0;
    for (uint32_t f = 0; f < FL_COUNT; ++f)
        if (P.bins.sl_bitmap[f]) flcheck |= 1u << f;
    if (flcheck != P.bins.fl_bitmap) return Status::CorruptMetadata;
    if (free_total_out) *free_total_out = total;
    return Status::Ok;
}

// --- layout finalization -----------------------------------------------------
// Rebuild block headers, free blocks and TLSF bins for the physical range
// [start, end) from an address-ordered slot array. Shared by compact, merge and
// split. Sub-minimal gaps become poisoned slack (prev_size 0) so free()'s
// physical walk can never run into them.
//
// Transactional contract (task-book 3.6, plan A): this function runs in the
// EXECUTION phase, after the callers' read-only planning has verified object
// placement, pinned barriers, alignment and range bounds. It only writes
// already-validated metadata, so it cannot fail; the internal checks are
// debug assertions. No maintenance op may move data and then report an error.
//
// The array is address-ascending and covers every live descriptor of the
// pool(s) being finalized. Entries outside [start, end) belong to the other
// side of a split and are skipped -- so split can hand BOTH halves the same
// array instead of relying on "everything below the boundary is a prefix",
// which would be an unverified assumption inside an execution phase that is
// not allowed to fail.
void finalize_layout(Pool& P, uint8_t* start, uint8_t const* end,
                     uint16_t const* slots, uint32_t nslot) {
    P.bins.reset();
    uint32_t capacity = (uint32_t)(end - start);
    uint8_t* prev_end = start;
    uint32_t prev_own = 0; // predecessor's own size, 0 = none/slack
    uint32_t used = 0, fragment = 0, count = 0;

    for (uint32_t i = 0; i < nslot; ++i) {
        ObjectDesc const& d = g().objects[slots[i]];
        uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
        uint32_t bsize = d.block_size;
        if (bstart < start || bstart + bsize > end) continue; // other side
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
        // Poison the TAIL slack as well. free()'s forward merge tests the word
        // at `block + block_size` for a free bit; when the last block is
        // followed by tail slack that test would otherwise read stale bytes
        // and could mistake them for a free block, corrupting the bins. The
        // word was previously left untouched here, which made a freed
        // last block able to "merge" with garbage.
        store32(prev_end, 0);
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

// --- compaction advice state (round-6 doc section 6) --------------------------
// ADVICE state, separate fixed storage -- never allocator metadata. The
// zero-side-effect guarantee of analyze_compaction covers every Pool,
// ObjectDesc and Auto Zone byte (R31 snapshots prove it).
CompactionThresholds s_advice_thresholds = {100, 512}; // documented defaults
struct AdviceCache {
    uint8_t valid;
    uint8_t verdict;
    uint32_t structure_epoch;
    uint32_t borrow_count;
    uint32_t largest_free_block;
    uint32_t fragment_bytes;
    uint32_t request_size;
    uint32_t request_alignment;
    // round-7: the change key covers every input the advice depends on
    uint8_t pool_state;
    uint8_t has_pinned;
    uint8_t stats_valid;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t live_objects;
    uint32_t request_flags;
    uint32_t request_tag;
    uint32_t thr_ratio_permille;
    uint32_t thr_min_bytes;
};
AdviceCache s_advice_cache[PM_MAX_POOLS];

// Round-8 (guide section 3, plan A): Debug-only owner gate for the whole
// advice family (analyze/poll/threshold accessors). The first advice-family
// call after init() binds the calling context; a call from a different
// context is a single-owner contract violation and aborts in Debug. Release
// performs no runtime check and REQUIRES the caller to follow the contract
// (same discipline as alloc/free/resolve/get_stats/validate). Lifecycle: the
// binding is cleared by init()/deinit(); on ESP32 the id is the task handle,
// so a deleted owner task requires a fresh init() to re-bind.
#if PM_DEBUG
uintptr_t s_advice_owner = 0;
uint8_t s_advice_owner_bound = 0;
bool advice_owner_check() {
    uintptr_t const ctx = pm_port_context_id();
    if (!s_advice_owner_bound) {
        s_advice_owner = ctx;
        s_advice_owner_bound = 1;
        return true;
    }
    return s_advice_owner == ctx;
}
#endif
uint16_t s_slots[PM_MAX_OBJECTS];    // audited address-order slot list

// Address-order scratch for the binned free blocks, so validate()'s coverage
// sweep can be linearised. Cost: 4 B x (PM_MAX_OBJECTS + 1) -- 1028 B at the
// device's 256, 4100 B at 1024.
//
// Why PM_MAX_OBJECTS + 1 is enough: a free block is a maximal run of at least
// PM_MIN_BLOCK free bytes, and every such run is bounded on each side by
// either a live block or a pool end. With L live blocks there are at most L+1
// such runs, and L <= PM_MAX_OBJECTS because every live object occupies a
// descriptor slot. Hence
//     free_blocks <= live_objects + 1 <= PM_MAX_OBJECTS + 1.
// A binned count above the cap is therefore corruption, not a capacity
// problem: the earlier checks enforce bin structure, not maximality, so a
// tampered pool can exceed it (uniform 16 B blocks pass audit_pool_bins) and
// validate() refuses it in O(nfree) -- R57. The quadratic fallback that once
// handled the decline is gone (it turned hostile metadata into a
// multi-second walk; v17 registered it as a dead branch, v19 removes it).
constexpr uint32_t kFreeOffCap = PM_MAX_OBJECTS + 1;
uint32_t s_free_off[kFreeOffCap];

// --- compaction core ---------------------------------------------------------
// Requires: state == Compacting, borrow_count == 0 (caller validated).
// Address-order stable packing with pinned barriers; plan fully before any
// move (doc sections 8.1-8.3). This function NEVER publishes pool state: the
// caller commits Running/structure_epoch under the lock (round-3 guide 8).
Status compact_impl(Pool& P, CompactionRequest const* req) {
    GlobalState& G = g();
    if (P.borrow_count != 0) return Status::Busy;
    uint64_t t0 = pm_port_ticks_us();
    uint8_t* start = pool_start(P);
    uint8_t* end = pool_end(P);
    Status st = Status::Ok;
    PoolId const pid = (PoolId)(&P - G.pools);
    uint32_t nslot = 0, nbar = 0, nplan = 0;

    // Gate every memmove on a full descriptor audit; it also produces the
    // address-ordered slot array, and every later loop iterates THAT array
    // rather than re-traversing the (untrusted) live-slot list.
    st = precheck_pool(P, pid, start, end, s_slots, PM_MAX_OBJECTS, nslot);
    if (st != Status::Ok) return st;

    for (uint32_t i = 0; i < nslot; ++i) {
        ObjectDesc const& d = G.objects[s_slots[i]];
        if (!(d.flags & PM_PINNED)) continue;
        if (d.address - BLOCK_HEADER_SIZE < start || d.address + d.size > end)
            return Status::CorruptMetadata;
        s_barriers[nbar++] = d.address - BLOCK_HEADER_SIZE;
    }

    // Partial compaction (v1.2): a request bounds the plan two ways. The
    // TARGET (requested_size/alignment) ends the plan at the first prefix
    // after which an allocation of that size would succeed; the BUDGET
    // (max_move_bytes / max_move_objects) ends it before the move that would
    // exceed the cap. Whichever binds first wins; blocks after the cut keep
    // their addresses (holes included) and finalize_layout rebuilds the whole
    // stream from the final addresses exactly as it does for a full pass.
    //
    // The target check needs the largest free run reachable at each prefix:
    // after packing blocks 0..k-1 the free space is the gap [cursor, p_k)
    // (every hole the cursor walked over, vacated slots included) plus the
    // ORIGINAL holes above p_k. The latter is a suffix max over the gaps,
    // precomputed here in O(n) into s_ord_key (free since
    // collect_live_sorted is done with it).
    uint32_t need = 0;            // roundup8(target block bytes), 0 = no target
    uint32_t budget_b = 0;        // 0 = unlimited
    uint32_t budget_o = 0;        // 0 = unlimited
    if (req) {
        if (req->requested_size) {
            if (req->requested_alignment != 0 &&
                ((req->requested_alignment & (req->requested_alignment - 1)) != 0 ||
                 req->requested_alignment > PM_MAX_ALIGNMENT))
                return Status::InvalidRequest;
            need = (req->requested_size + BLOCK_HEADER_SIZE + 7u) & ~7u;
        }
        budget_b = req->max_move_bytes;
        budget_o = req->max_move_objects;
    }

    {
        uint8_t* cursor = start;
        uint32_t bar = 0;
        // suffix_max_hole[i] = largest original hole at or after slot i
        // (hole_j = the gap between slot j and slot j+1; the tail after the
        // last slot counts too). These are holes of the UNPROCESSED region,
        // whose layout is untouched by the moves planned below it.
        if (nslot != 0) { // an empty pool has nothing to move or reorder
        ObjectDesc const& dl = G.objects[s_slots[nslot - 1]];
            uint64_t run = (uint64_t)(uintptr_t)end -
                           ((uint64_t)(uintptr_t)dl.address - BLOCK_HEADER_SIZE +
                            dl.block_size);
            s_ord_key[nslot - 1] = (uint32_t)(run > 0xFFFFFFFFu ? 0xFFFFFFFFu : run);
            for (uint32_t i = nslot - 1; i-- > 0;) {
                ObjectDesc const& d = G.objects[s_slots[i]];
                ObjectDesc const& dn = G.objects[s_slots[i + 1]];
                uint64_t hole_after_i =
                    ((uint64_t)(uintptr_t)dn.address - BLOCK_HEADER_SIZE) -
                    ((uint64_t)(uintptr_t)d.address - BLOCK_HEADER_SIZE +
                     d.block_size);
                if (hole_after_i > run) run = hole_after_i;
                s_ord_key[i] = (uint32_t)(run > 0xFFFFFFFFu ? 0xFFFFFFFFu : run);
            }
        } // nslot != 0
        uint64_t moved_b = 0;
        uint32_t moved_o = 0;
        for (uint32_t i = 0; i < nslot; ++i) {
            ObjectDesc const& d = G.objects[s_slots[i]];
            uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
            uint32_t bsize = d.block_size;
            if (d.flags & PM_PINNED) {
                if (cursor > bstart) return Status::PinnedConflict;
                cursor = bstart + bsize;
                bar++;
                continue;
            }
            uint8_t const* barrier = (bar < nbar) ? s_barriers[bar] : end;
            if (cursor + bsize > barrier) {
                // Full compact refuses here. A partial plan simply ends:
                // the object cannot pack below the next pinned barrier, and
                // blocks after it belong to the region above.
                if (req) break;
                return (bar < nbar) ? Status::PinnedConflict : Status::NoSpace;
            }
            // Target check BEFORE spending the move: at this prefix the free
            // runs are the walked-over gap [cursor, bstart) and the original
            // gaps from bstart onward (suffix max, tail included).
            if (need != 0) {
                uint64_t walked_gap =
                    (uint64_t)(uintptr_t)bstart - (uint64_t)(uintptr_t)cursor;
                uint64_t best = walked_gap > (uint64_t)s_ord_key[i]
                                    ? walked_gap : (uint64_t)s_ord_key[i];
                if (best >= need) break; // target reached without this move
            }
            // Budget check BEFORE the move: block granularity, no overshoot.
            if ((budget_b != 0 && moved_b + bsize > budget_b) ||
                (budget_o != 0 && moved_o + 1 > budget_o))
                break; // budget spent; target (if any) stays unmet
            if (bstart != cursor) {
                s_plan[nplan].slot = s_slots[i];
                s_plan[nplan].dst_off = off_of(cursor);
                s_plan[nplan].size = bsize;
                nplan++;
                moved_b += bsize;
                moved_o++;
            }
            cursor += bsize;
        }
    }

    uint32_t moved_objs = 0, moved_bytes = 0;
    for (uint32_t i = 0; i < nplan; ++i) {
        ObjectDesc& d = G.objects[s_plan[i].slot];
        uint8_t* src = d.address - BLOCK_HEADER_SIZE;
        uint8_t* dst = G.zone + s_plan[i].dst_off;
        move_block(dst, src, s_plan[i].size);
        d.address = dst + BLOCK_HEADER_SIZE;
        bump_epoch(d.address_epoch);
        moved_objs++;
        moved_bytes += s_plan[i].size;
    }
    // Execution phase: cannot fail (plan A). Planning verified every
    // placement; finalize only writes already-validated metadata.
    finalize_layout(P, start, end, s_slots, nslot);

    P.objects_moved = moved_objs;
    P.bytes_moved = moved_bytes;
    P.compact_time_us = pm_port_ticks_us() - t0;
    if (moved_bytes > G.max_bytes_moved) G.max_bytes_moved = moved_bytes;
    if (P.compact_time_us > G.max_compact_time_us) G.max_compact_time_us = P.compact_time_us;
    if (P.fragment_bytes > G.max_fragment_bytes) G.max_fragment_bytes = P.fragment_bytes;
    return Status::Ok;
}

} // unnamed namespace

namespace internal {

uint32_t metadata_scratch_bytes() {
    return (uint32_t)(sizeof(s_plan) + sizeof(s_upper) + sizeof(s_barriers) +
                      sizeof(s_slots)
                      + sizeof(s_free_off)
                      + sizeof(s_ord_key)
    );
}

// The compaction-advice state is fixed static storage the library owns just
// like the plan scratch, but it scales with PM_MAX_POOLS rather than
// PM_MAX_OBJECTS, so it is reported separately. It used to be omitted from
// global_stats().metadata_bytes entirely, which made that field under-report a
// quantity its own documentation calls "size of all static metadata": the
// measured shortfall was 128 B at PM_MAX_POOLS=2 and 960 B at 16.
uint32_t metadata_advice_bytes() {
    uint32_t bytes =
        (uint32_t)(sizeof(s_advice_cache) + sizeof(s_advice_thresholds));
#if PM_DEBUG
    // Debug-only advice owner gate: the bound context id and its flag.
    bytes += (uint32_t)(sizeof(s_advice_owner) + sizeof(s_advice_owner_bound));
#endif
    return bytes;
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

void pm_debug_assert_fail(const char* file, int line, const char* msg) {
#if defined(PM_ESP32)
    printf("PondMerge ASSERT %s:%d: %s\n", file, line, msg);
#else
    fprintf(stderr, "PondMerge ASSERT %s:%d: %s\n", file, line, msg);
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
    // FL ceiling (task-book v2 section 7.2). A single pool may own the whole
    // zone, so the largest block the bins could ever see is the rounded zone
    // size. fl_index() saturates at PM_FL_MAX-1, so a zone at or above
    // 2^PM_FL_MAX would be clamped into the top bin and corrupt the bin
    // accounting. Refuse the configuration instead of degrading silently.
    if (seg_count * cfg.segment_size >= (1u << PM_FL_MAX)) return Status::NoSpace;

    // All checks passed: only now may global state be (re)initialized.
    memset(&G, 0, sizeof(G));
    memset(s_advice_cache, 0, sizeof(s_advice_cache)); // advice state reset
#if PM_DEBUG
    s_advice_owner_bound = 0; // the owner re-binds on the next advice call
#endif
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
    if (!G.initialized) return Status::NotInitialized;
    // Object-less pools hold no resources beyond segment bookkeeping, which
    // dies with the zone; only live objects block a shutdown.
    if (G.live_object_count != 0) return Status::Busy;
    G.initialized = 0;
    memset(&G, 0, sizeof(G));
#if PM_DEBUG
    s_advice_owner_bound = 0;
#endif
    return Status::Ok;
}

GlobalStats global_stats() {
    GlobalState const& G = g();
    GlobalStats s{};
    s.max_live_objects = G.max_live_objects;
    s.max_borrow_count = G.max_borrow_count;
    s.max_fragment_bytes = G.max_fragment_bytes;
    s.max_bytes_moved = G.max_bytes_moved;
    s.max_compact_time_us = G.max_compact_time_us;
    s.metadata_bytes = (uint32_t)sizeof(GlobalState) + metadata_scratch_bytes() +
                       metadata_advice_bytes();
    return s;
}

// ---------------------------------------------------------------------------
// Pools
// ---------------------------------------------------------------------------
Status create_pool(PoolId& out, uint32_t segment_count) {
    GlobalState& G = g();
    if (!G.initialized) return Status::NotInitialized;
    if (segment_count == 0) return Status::NoSpace;
    // A count above the zone's segment count can never name a window here, so
    // refuse it before any geometry math (R56). Without this bound the run
    // search's (int32_t) cast sign-wrapped huge counts into a "found" run and
    // the truncated uint16_t fields built a pool far outside the zone: tested
    // 0x8000000C wrote through a wild pointer, 0xFFFFFFFF returned Ok with
    // free_bytes ~1024x the zone. One zone-side bound suffices: the
    // PM_MAX_SEGMENTS <= 0xFFFF static_assert keeps it below the uint16_t
    // descriptor fields as well.
    if (segment_count > G.segment_count) return Status::NoSpace;

    bool used[PM_MAX_SEGMENTS] = {};
    for (uint32_t i = 0; i < PM_MAX_POOLS; ++i) {
        Pool const& P = G.pools[i];
        if (P.state == PoolState::Empty) continue;
        // Round-11 fix 3: the window is written into used[] below and hides
        // its segments from the free-run search, so an unproven window meant
        // an out-of-bounds used[] write (bool[PM_MAX_SEGMENTS]) and/or a new
        // pool created physically on top of a live pool's blocks. The proof
        // is the same O(1) geometry check every other entry uses (fix 4).
        if (!pool_geometry_ok(P)) return Status::CorruptMetadata;
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
    if (!G.initialized) return Status::NotInitialized;
    if (!P) return Status::InvalidPool;
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
    Pool const* P = pool_at(id);
    if (!P) return s;
    s.valid = 1; // unless a refusal path below says otherwise
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
    // (task-book v2 section 9.2) instead of looping forever, and it must
    // refuse out-of-range cursors instead of dereferencing them. No bin can
    // hold more blocks than the pool has room for at minimum block size.
    // Pure-integer geometry: get_stats runs before any proof of the pool's
    // segment fields (A4-10).
    const uint64_t start_off = pool_start_off(*P);
    const uint64_t end_off = start_off + pool_capacity(*P);
    const uint32_t max_steps = pool_capacity(*P) / PM_MIN_BLOCK + 1;
    // The SAME reject set, evaluated in 32-bit. Both bounds are in-zone offsets, so they fit in
    // 32 bits and the 64-bit form only pays for an always-zero high half.
    // Overflow-safe: BLOCK_HEADER_SIZE is subtracted instead of added.
    uint32_t const lo32 = (uint32_t)start_off;
    uint32_t const hi32 = (uint32_t)end_off - BLOCK_HEADER_SIZE;
    for (uint32_t f = 0; f < FL_COUNT; ++f)
        for (uint32_t sl = 0; sl < SL_COUNT; ++sl) {
            uint32_t off = P->bins.head[f][sl];
            for (uint32_t steps = 0; off != NULL_OFF; ++steps) {
                if (steps > max_steps) {
                    s.largest_free_block = 0;
                    s.valid = 0;
                    return s; // cyclic list; refuse to report
                }
                if (off < lo32 || off > hi32) {
                    s.largest_free_block = 0;
                    s.valid = 0;
                    return s; // cursor outside the pool
                }
                FreeBlock const* b = ptr_of(off);
                if (blk_size_of(b) > largest) largest = blk_size_of(b);
                off = b->next;
            }
        }
    s.largest_free_block = largest;
    return s;
}

// ---------------------------------------------------------------------------
// alloc()'s refusal diagnosis: an O(bins) bitmap/head consistency check.
// ---------------------------------------------------------------------------
// The shipped code answers "why did bins_find fail?" by auditing every free
// block in the pool -- O(free blocks) -- because the refusal has to be
// DISTINGUISHABLE: a damaged free list is CorruptMetadata, real exhaustion is
// NoSpace. On the target that costs ~160 us per refused alloc, about 20x a
// successful one, and it is paid on exactly the path the fragmentation
// scenario hammers.
//
// A byte-accounting fast path was tested and rejected (it gates on
// free_bytes < need) because it does not fire in that scenario: a
// fragmented pool still has plenty of free BYTES; it just has no free BLOCK.
//
// This variant instead replaces the full audit with a bitmap/head consistency
// check: O(FL_COUNT x SL_COUNT), independent of how many free blocks exist.
// It verifies that every bitmap bit agrees with its head pointer, that a
// non-empty head is a readable in-pool free block whose own size selects the
// bin it is the head of, and that the first-level bitmap agrees with the
// second-level one.
//
// WHAT IS GIVEN UP, explicitly: this no longer detects, on this one path,
// (a) a chain that breaks or cycles beyond its head, (b) a reciprocal-link
// violation deeper in a bin, (c) the prev_size chain, and (d) size-class
// agreement of non-head members. Note that bins_find() already screens every
// offset it walks and refuses a bad one itself, so the "follow a damaged link
// into the weeds" class is caught before this point. The full audit still runs
// at every maintenance entry and inside validate().
static bool bins_bitmap_consistent(Pool const& P) {
    // Pure-integer geometry: this screen runs before the geometry proof (A4-10).
    uint64_t const start_off = pool_start_off(P);
    uint64_t const end_off = start_off + (uint64_t)pool_capacity(P);
    for (uint32_t f = 0; f < FL_COUNT; ++f) {
        for (uint32_t s = 0; s < SL_COUNT; ++s) {
            uint32_t const off = P.bins.head[f][s];
            bool const bit = (P.bins.sl_bitmap[f] & (uint16_t)(1u << s)) != 0;
            if (off == NULL_OFF) {
                if (bit) return false;
                continue;
            }
            if (!bit) return false;
            if ((uint64_t)off < start_off ||
                (uint64_t)off + BLOCK_HEADER_SIZE > end_off)
                return false;
            FreeBlock const* b = ptr_of(off);
            if (!blk_is_free(b)) return false;
            uint32_t const sz = blk_size_of(b);
            uint32_t const bf = fl_index(sz);
            if (bf - MIN_FL != f || sl_index(sz, bf) != s) return false;
        }
    }
    for (uint32_t f = 0; f < FL_COUNT; ++f) {
        bool const any = P.bins.sl_bitmap[f] != 0;
        bool const bit = (P.bins.fl_bitmap & (1u << f)) != 0;
        if (any != bit) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Allocation (doc section 7.3)
// ---------------------------------------------------------------------------
Status alloc(PoolId pool_id, uint32_t size, uint32_t alignment, uint16_t flags,
             uint32_t user_tag, RawRef& out) {
    // Failure must never leave the caller's old reference standing (round-5
    // guide section 3, same rule as resolve/borrow_begin): a reused RawRef
    // would otherwise look like a fresh, valid allocation. generation == 0
    // reads as invalid everywhere.
    out = RawRef{};
    GlobalState& G = g();
    if (!G.initialized) return Status::NotInitialized;
    Pool* P = pool_at(pool_id);
    if (!P) return Status::InvalidPool;
    if (P->state != PoolState::Running) return Status::Busy;
    // O(1) corruption screens before any mutation (round-11 fixes 1 and 4):
    // order_append() dereferences the pool's order_head without a walk, so a
    // head outside [0, PM_MAX_OBJECTS) would write past the descriptor table;
    // and the pool's byte range is derived from corruptible segment fields.
    // Both refusals clear the output reference already (first statement) and
    // touch nothing else -- the rollback below stays exactly as it was.
    if (P->order_head != NO_ORDER && P->order_head >= PM_MAX_OBJECTS)
        return Status::CorruptMetadata;
    if (!pool_geometry_ok(*P)) return Status::CorruptMetadata;
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
    if (!blk) {
        // The bitmap is a hint only -- but a refusal must be DISTINGUISHABLE
        // (round-4 task book section 11): a damaged free list is
        // CorruptMetadata, genuine exhaustion is NoSpace. The audit runs
        // only on this failure path, O(free blocks).
        bool const consistent = bins_bitmap_consistent(*P);
        slot_release(slot);
        return consistent ? Status::NoSpace : Status::CorruptMetadata;
    }
    if (blk_size_of(blk) < need) {
        slot_release(slot);
        return Status::NoSpace; // first-fit exhausted the bin: a real hint miss
    }

    // Preserve the slot's lifecycle generation across reuse (ABA guard);
    // free() already bumped it.
    uint16_t gen = next_generation(G.objects[slot].generation);
    ObjectDesc& d = G.objects[slot];
    memset(&d, 0, sizeof(d));
    // The payload address is final already (a later split only moves the
    // remainder), so the live-slot link is established BEFORE any block byte or
    // bin membership changes. That link is now a pure O(1) append which cannot
    // fail, so this is the last possible failure point and the rollback is
    // still exactly "release the slot, change nothing else". See order_append()
    // for why the list is no longer kept in address order, and
    // docs/AUDIT_LEDGER.md for the invariant this relocates.
    d.address = reinterpret_cast<uint8_t*>(blk) + BLOCK_HEADER_SIZE;
    order_append(*P, slot);
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

    d.size = size;
    d.block_size = blksize;
    d.user_tag = user_tag;
    d.pool_id = pool_id;
    d.flags = flags;
    d.generation = gen;
    d.address_epoch = 1;
    d.state = ObjState::Live;
    if (flags & PM_ZERO_INIT) memset(d.address, 0, size);

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
// Free (doc section 7.4). Order (round-3 guide 6.1): reference checks ->
// read-only physical verification (own header, prev_size chain, successor
// sanity, free-list membership with reciprocal links) -> destroy callback ->
// re-verification (the callback may legally re-enter for other objects) ->
// mutation. Nothing observable -- no callback, no statistic, no bins write --
// happens before the physical proof succeeds (R24).
// ---------------------------------------------------------------------------
Status free(RawRef const& ref) {
    GlobalState& G = g();
    if (!G.initialized) return Status::NotInitialized;
    // Only a root reference (offset 0) may release an object; sub-object
    // views from pm_ptr::at() must never destroy the parent (task-book 3.7).
    if (ref.offset != 0) return Status::InvalidRef;
    RefCheck rc{};
    Status st = check_ref(ref, 0, 1, rc, /*require_running=*/true, nullptr);
    if (st != Status::Ok) return st;

    Pool& P = *rc.pool;
    ObjectDesc& d = *rc.desc;
    if (d.active_borrows != 0) return Status::Busy;

    uint8_t* poolStart = pool_start(P);
    uint8_t* poolEnd = pool_end(P);
    // check_ref proved the block fully inside the pool, so the header
    // subtraction cannot run out of the zone.
    uint8_t* block = d.address - BLOCK_HEADER_SIZE;

    auto verify_neighbours = [&]() -> Status {
        // Own header must agree with the descriptor and claim "in use".
        if (blk_is_free(block) || blk_size_of(block) != d.block_size)
            return Status::CorruptMetadata;
        // Backward neighbour via prev_size: 0 (pool start / slack) never
        // merges; otherwise the chain must close exactly on a real block,
        // and a free predecessor must genuinely sit in its bin.
        uint32_t psize = load32(block + 4);
        if (psize != 0) {
            if (psize < PM_MIN_BLOCK || (psize & (PM_ALIGNMENT - 1)) != 0)
                return Status::CorruptMetadata;
            if (psize > (uint32_t)(block - poolStart)) return Status::CorruptMetadata;
            uint8_t* prev = block - psize;
            if (blk_size_of(prev) != psize) return Status::CorruptMetadata;
            if (blk_is_free(prev) &&
                !free_block_binned(P, reinterpret_cast<FreeBlock*>(prev)))
                return Status::CorruptMetadata;
        }
        // Forward neighbour: only the free bit decides a merge, but a block
        // that CLAIMS to be free must be a sane, binned free block.
        uint8_t* fwd = block + d.block_size;
        if (fwd < poolEnd && blk_is_free(fwd)) {
            uint32_t nsz = blk_size_of(fwd);
            if (nsz < PM_MIN_BLOCK || (nsz & (PM_ALIGNMENT - 1)) != 0 ||
                nsz > (uint32_t)(poolEnd - fwd))
                return Status::CorruptMetadata;
            if (!free_block_binned(P, reinterpret_cast<FreeBlock*>(fwd)))
                return Status::CorruptMetadata;
        }
        return Status::Ok;
    };
    if (verify_neighbours() != Status::Ok) return Status::CorruptMetadata;

    // Round-11 fix 5: the destroy callback, the post-callback state check and
    // the second verification are one unit -- without a callback no code ran
    // between the two verifications (single-owner contract: no allocator
    // mutation can interleave), so the second proof would be a deterministic
    // replay of the first. R24's "re-prove after the callback" contract is
    // preserved verbatim for every object that HAS a callback, and the
    // failure-output semantics are unchanged on every path (nothing is
    // mutated before this block succeeds either way).
    if (d.destroy_fn) {
        d.state = ObjState::Destroying;
        d.destroy_fn(d.address);
        if (d.state != ObjState::Destroying) return Status::CorruptMetadata;
        // The callback may allocate/free OTHER objects; that can change the
        // physical layout around this block, so the proof is repeated before
        // anything is mutated.
        if (verify_neighbours() != Status::Ok) return Status::CorruptMetadata;
    }

    // --- mutation phase: cannot fail ----------------------------------------
    uint32_t bsize = d.block_size;
    // Backward merge: the predecessor is found via this block's prev_size
    // (already verified above -- membership, chain and bounds).
    uint32_t psize = load32(block + 4);
    if (psize != 0 && blk_is_free(block - psize)) {
        bins_remove(P.bins, reinterpret_cast<FreeBlock*>(block - psize));
        bsize += psize;
        block -= psize;
    }
    // Forward merge: the successor's own header carries its free bit.
    // Sub-minimal slack is poisoned with a 0 header word (finalize_layout);
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
    // Token validation and the counter decrement happen inside ONE critical
    // section (round-3 guide 7.1): the previous implementation read state,
    // generation, pool binding and the counters outside the lock, so two
    // contexts could both pass the checks and double-decrement a one-shot
    // token. On any mismatch NOTHING is modified; Debug asserts the caller
    // bug (R25).
    bool ok;
    PM_LOCK();
    {
        GlobalState& G = g();
        ok = G.initialized != 0 && ref.index < PM_MAX_OBJECTS;
        if (ok) {
            ObjectDesc& d = G.objects[ref.index];
            // The end token must match what begin handed out: same live
            // object, same generation, same pool binding.
            ok = d.state == ObjState::Live && d.generation != 0 &&
                 d.generation == ref.generation;
            if (ok && ref.pool_hint != CROSS_HINT && ref.pool_hint != d.pool_id)
                ok = false;
            if (ok) {
                Pool* P = pool_at((PoolId)d.pool_id);
                ok = P != nullptr && d.active_borrows > 0 && P->borrow_count > 0;
                if (ok) {
                    d.active_borrows--;
                    P->borrow_count--;
                }
            }
        }
    }
    PM_UNLOCK();
    if (!ok) {
        // A mismatched end token is a caller bug. Debug aborts with the reason;
        // Release ignores it -- and either way no counter was touched. Written
        // as an explicit branch rather than a folded string predicate so the
        // message is actually printed and the intent survives in Release.
#if PM_DEBUG
        pm_debug_assert_fail(__FILE__, __LINE__,
                             "borrow_end token does not match its borrow_begin");
#endif
    }
}

Status resolve(RawRef const& ref, uint32_t access_size, uint32_t access_align, void*& out_addr) {
    out_addr = nullptr; // a failed resolve must never leave a stale address
                        // behind for a caller that reuses the variable (P2)
    RefCheck rc{};
    void* addr = nullptr;
    Status st = check_ref(ref, access_size, access_align, rc, /*require_running=*/true, &addr);
    if (st == Status::Ok) out_addr = addr;
    return st;
}

// ---------------------------------------------------------------------------
// Compaction advice (round-6 requirements doc section 6): strictly read-only
// analysis. The per-pool "last advice" cache below is ADVICE state -- it is
// separate fixed storage, never allocator metadata, and is the only thing
// analyze/poll write. The zero-side-effect guarantee covers every Pool,
// ObjectDesc and Auto Zone byte (R31 snapshots prove it).
// ---------------------------------------------------------------------------
CompactionThresholds get_compaction_thresholds() {
    PM_ASSERT(advice_owner_check()); // owner-context API (round-8)
    return s_advice_thresholds;
}

void set_compaction_thresholds(CompactionThresholds const& t) {
    PM_ASSERT(advice_owner_check());
    s_advice_thresholds = t; // single-owner configuration, taken as-is
}

CompactionAdvice analyze_compaction(PoolId pool_id, CompactionRequest const* expected) {
    CompactionAdvice a{};
    a.verdict = CompactionVerdict::INVALID_METADATA;
    a.estimated_moved_objects = COMPACTION_ESTIMATE_UNKNOWN;
    a.estimated_moved_bytes = COMPACTION_ESTIMATE_UNKNOWN;
    a.expected_request_size = expected ? expected->requested_size : 0;
    a.expected_request_alignment = expected ? expected->requested_alignment : 0;
    a.request_flags = expected ? expected->requested_flags : 0;
    a.request_tag = expected ? expected->user_tag : 0;

    // The advice cache holds "what the caller was last told" (advice state,
    // separate fixed storage -- never allocator metadata). EVERY analysis
    // outcome refreshes it so state transitions (damage, recovery, verdict
    // flips) are reported exactly once -- EXCEPT INVALID_REQUEST, which is a
    // caller input error, not pool state, and must never clobber a valid
    // cached advice (round-7 guide section 3).
    auto store = [&](uint32_t epoch) {
        if (pool_id >= PM_MAX_POOLS) return;
        AdviceCache& c = s_advice_cache[pool_id];
        c.valid = 1;
        c.verdict = (uint8_t)a.verdict;
        c.structure_epoch = epoch;
        c.borrow_count = a.borrow_count;
        c.largest_free_block = a.largest_free_block;
        c.fragment_bytes = a.fragment_bytes;
        c.request_size = a.expected_request_size;
        c.request_alignment = a.expected_request_alignment;
        c.pool_state = a.pool_state;
        c.has_pinned = a.has_pinned_objects;
        c.stats_valid = a.stats_valid;
        c.used_bytes = a.used_bytes;
        c.free_bytes = a.free_bytes;
        c.live_objects = a.live_objects;
        c.request_flags = a.request_flags;
        c.request_tag = a.request_tag;
        c.thr_ratio_permille = s_advice_thresholds.fragment_ratio_permille;
        c.thr_min_bytes = s_advice_thresholds.fragment_min_bytes;
    };

    PM_ASSERT(advice_owner_check()); // owner-context API (round-8)
    GlobalState const& G = g();
    if (!G.initialized) {
        store(0);
        return a;
    }
    // Coherent observation point (round-8 guide section 5): under the
    // single-owner contract NO allocator mutation can interleave with this
    // analysis, so the sequential reads below (pool counters, order list,
    // bins via get_stats) form one logical observation. This is NOT a
    // lock-free concurrent-consistent snapshot -- concurrent monitoring is
    // out of v1 scope (header contract).
    // Request validation comes FIRST (round-7 guide section 3): a malformed
    // caller request is INVALID_REQUEST -- distinct from metadata damage --
    // and returns WITHOUT refreshing the advice cache.
    uint32_t need = 0;
    bool have_request = false;
    if (expected) {
        if (expected->requested_size == 0) {
            a.verdict = CompactionVerdict::INVALID_REQUEST;
            return a; // cache untouched by caller errors
        }
        uint32_t align = expected->requested_alignment;
        if (align == 0) align = PM_ALIGNMENT;
        if (align == 0 || (align & (align - 1)) != 0 || align > PM_MAX_ALIGNMENT) {
            a.verdict = CompactionVerdict::INVALID_REQUEST;
            return a;
        }
        if (expected->requested_size > UINT32_MAX - (PM_ALIGNMENT - 1)) {
            a.verdict = CompactionVerdict::INVALID_REQUEST;
            return a;
        }
        uint32_t payload = align_up_u(expected->requested_size, PM_ALIGNMENT);
        if (payload > UINT32_MAX - BLOCK_HEADER_SIZE) {
            a.verdict = CompactionVerdict::INVALID_REQUEST;
            return a;
        }
        need = payload + BLOCK_HEADER_SIZE;
        if (need < PM_MIN_BLOCK) need = PM_MIN_BLOCK;
        if (need >= (1u << PM_FL_MAX)) { // unservable by the allocator: input error
            a.verdict = CompactionVerdict::INVALID_REQUEST;
            return a;
        }
        have_request = true;
    }
    Pool* P = pool_at(pool_id);
    if (!P) {
        store(0);
        return a;
    }

    a.pool_state = (uint8_t)P->state;
    a.capacity = pool_capacity(*P);
    a.used_bytes = P->used_bytes;
    a.free_bytes = P->free_bytes;
    a.live_objects = P->live_objects;
    a.borrow_count = P->borrow_count;
    a.fragment_bytes = P->fragment_bytes;
    a.fragment_ratio_permille =
        a.capacity ? (uint32_t)(((uint64_t)a.fragment_bytes * 1000) / a.capacity) : 0;

    // One address-ordered pass that simultaneously counts the live objects,
    // records the pinned flag and computes the EXACT move estimate by simulating
    // compact's packing (same cursor and barrier rules as compact_impl's plan):
    // a movable object "moves" iff its current block start differs from the
    // packed cursor. Pinned objects never move. The cursor can never overtake a
    // later block (packed prefix <= original prefix, proved in compact_impl), so
    // the simulation is safe on the array. collect_live_sorted is the single
    // sanctioned enumeration and is also where a damaged list is detected.
    uint32_t walked_live = 0;
    uint32_t moved_objects = 0;
    uint64_t moved_bytes = 0;
    uint32_t nslot = 0;
    if (collect_live_sorted(*P, pool_id, s_slots, PM_MAX_OBJECTS, nslot) !=
        Status::Ok) {
        store(P->structure_epoch); // damage is state the caller was told about
        return a;                  // damaged live-slot list: INVALID_METADATA
    }
    uint8_t const* cursor = pool_start(*P);
    for (uint32_t i = 0; i < nslot; ++i) {
        ObjectDesc const& d = G.objects[s_slots[i]];
        ++walked_live;
        uint8_t const* bstart = d.address - BLOCK_HEADER_SIZE;
        if (d.flags & PM_PINNED) {
            a.has_pinned_objects = 1;
            cursor = bstart + d.block_size; // pinned: never moves
            continue;
        }
        if (cursor != bstart) { // would be relocated by compact
            ++moved_objects;
            moved_bytes += d.block_size;
        }
        cursor += d.block_size; // packed end (dst == cursor for movables)
    }
    PoolStats st = get_stats(pool_id);
    a.largest_free_block = st.largest_free_block;
    a.stats_valid = st.valid;
    if (!st.valid) {
        store(P->structure_epoch); // damaged free list: cached, then suppressed
        return a;
    }

    // Counter audit (round-9 guide section 9.2): the verdict arithmetic must
    // never run on damaged counters -- every relation is checked with
    // conditional (uint64) subtraction; any failure is INVALID_METADATA.
    uint64_t const cap = a.capacity;
    uint64_t const used = a.used_bytes;
    uint64_t const free_b = a.free_bytes;
    uint64_t const frag = a.fragment_bytes;
    uint64_t const largest = a.largest_free_block;
    bool const counters_ok =
        used <= cap && free_b <= cap && used + free_b == cap &&
        frag <= free_b && largest <= free_b - frag &&
        walked_live == a.live_objects;
    if (!counters_ok) {
        store(P->structure_epoch); // damaged counters: cached, then suppressed
        return a;                  // damaged counters: INVALID_METADATA
    }

    if (have_request) {
        a.request_can_fit_now = (a.largest_free_block >= need) ? 1 : 0;
        // Honest estimate of the post-compaction largest block: slack
        // (fragment_bytes) cannot be merged by compaction; pinned barriers
        // may reduce it further -- this field is an ESTIMATE, documented.
        uint64_t after = (uint64_t)a.free_bytes - a.fragment_bytes;
        a.request_can_fit_after_compaction_estimate = (after >= need) ? 1 : 0;
    }

    bool const blocked = (P->borrow_count != 0) ||
                         (P->state != PoolState::Running && P->state != PoolState::Paused);
    // Stranded free bytes: free space split across secondary holes that a
    // compaction could consolidate (see CompactionThresholds for the
    // definition; fragment_bytes alone stays near zero in normal operation).
    uint64_t const stranded =
        (uint64_t)a.free_bytes - a.fragment_bytes - a.largest_free_block;
    // Round-11 fix 6: the ratio test is cross-multiplied instead of divided.
    // stranded*1000/capacity >= ratio_permille and
    // stranded*1000 >= ratio_permille*capacity are equivalent for non-negative
    // integers (floor(x/y) >= z  <=>  x >= z*y), and both sides are
    // overflow-free in uint64: stranded < capacity <= zone size < 2^24 (the
    // FL ceiling init() enforces), so the left side is < 2^34, and a
    // caller-set uint32 permille times the capacity stays < 2^56. The
    // division is a 64-bit soft divide (__udivdi3) on Xtensa; the reported
    // fragment_ratio_permille field below keeps its division -- it is a
    // public report value computed once, not a decision.
    bool const fragmented =
        (stranded >= s_advice_thresholds.fragment_min_bytes) &&
        (a.capacity != 0 &&
         stranded * 1000 >=
             (uint64_t)s_advice_thresholds.fragment_ratio_permille * a.capacity);
    if (blocked) {
        a.verdict = CompactionVerdict::COMPACT_BLOCKED;
    } else if (have_request) {
        if (a.request_can_fit_now) {
            a.verdict = CompactionVerdict::NO_ACTION;
        } else if (!a.request_can_fit_after_compaction_estimate) {
            a.verdict = CompactionVerdict::COMPACT_UNLIKELY_TO_HELP;
        } else {
            a.verdict = CompactionVerdict::COMPACT_RECOMMENDED;
            a.caller_must_establish_quiescence = 1; // caller owes the window
        }
    } else if (fragmented) {
        a.verdict = CompactionVerdict::COMPACT_RECOMMENDED;
        a.caller_must_establish_quiescence = 1;
    } else {
        a.verdict = CompactionVerdict::NO_ACTION;
    }
    // Exact move estimate (round-9 guide section 9.1): the simulation above
    // uses the same address order, barrier and cursor rules as compact_impl's
    // plan, so the counts are exact for the "compact succeeds" case -- but
    // this is still NOT the final compact transaction plan.
    if (moved_objects == 0) {
        a.estimated_moved_objects = 0;
        a.estimated_moved_bytes = 0;
    } else {
        a.estimated_moved_objects = moved_objects;
        a.estimated_moved_bytes =
            (moved_bytes <= 0xFFFFFFFEull) ? (uint32_t)moved_bytes
                                           : COMPACTION_ESTIMATE_UNKNOWN;
    }

    store(P->structure_epoch);
    return a;
}

CompactionAdvice poll_compaction_advice(PoolId pool_id, CompactionRequest const* expected,
                                        bool* changed) {
    // Snapshot the PREVIOUS cache first: analyze_compaction refreshes it, and
    // the suppression verdict must compare the fresh result against what the
    // caller was last told, not against the refresh itself. An
    // INVALID_REQUEST result never touches the cache and is reported on
    // every poll (caller errors are not state changes to be suppressed).
    PM_ASSERT(advice_owner_check()); // owner-context API (round-8)
    AdviceCache prev{};
    if (pool_id < PM_MAX_POOLS) prev = s_advice_cache[pool_id];
    CompactionAdvice a = analyze_compaction(pool_id, expected);
    bool is_changed;
    if (a.verdict == CompactionVerdict::INVALID_REQUEST) {
        is_changed = true; // caller errors are reported on every poll
    } else {
        Pool const* P = pool_at(pool_id);
        uint32_t const epoch = P ? P->structure_epoch : 0;
        is_changed = !prev.valid ||
                     prev.verdict != (uint8_t)a.verdict ||
                     prev.pool_state != a.pool_state ||
                     prev.structure_epoch != epoch ||
                     prev.borrow_count != a.borrow_count ||
                     prev.used_bytes != a.used_bytes ||
                     prev.free_bytes != a.free_bytes ||
                     prev.live_objects != a.live_objects ||
                     prev.largest_free_block != a.largest_free_block ||
                     prev.fragment_bytes != a.fragment_bytes ||
                     prev.has_pinned != a.has_pinned_objects ||
                     prev.stats_valid != a.stats_valid ||
                     prev.request_size != a.expected_request_size ||
                     prev.request_alignment != a.expected_request_alignment ||
                     prev.request_flags != a.request_flags ||
                     prev.request_tag != a.request_tag ||
                     prev.thr_ratio_permille != s_advice_thresholds.fragment_ratio_permille ||
                     prev.thr_min_bytes != s_advice_thresholds.fragment_min_bytes;
    }
    if (changed) *changed = is_changed;
    return a;
}

// ---------------------------------------------------------------------------
// Compaction (doc section 8)
// ---------------------------------------------------------------------------
Status compact(PoolId id) {
    return compact(id, nullptr);
}

Status compact(PoolId id, CompactionRequest const* req) {
    GlobalState const& G = g();
    Pool* P = pool_at(id);
    if (!G.initialized) return Status::NotInitialized;
    if (!P) return Status::InvalidPool;
    // Decide the whole state transition under the same lock borrow_begin
    // uses (task-book section 4), then run the maintenance body outside the
    // lock during the quiescent window.
    PM_LOCK();
    Status st;
    bool was_paused = false;
    if (P->state == PoolState::Compacting || P->state == PoolState::Merging ||
        P->state == PoolState::Splitting) {
        st = Status::Busy;
    } else {
        // Doc section 8: pause first, then check borrows. A refused compact
        // leaves the pool Paused; the caller resumes explicitly. A pool that
        // arrives already Paused takes the same path (pool_maintainable).
        was_paused = (P->state == PoolState::Paused);
        if (P->state == PoolState::Running) P->state = PoolState::Paused;
        if (P->state != PoolState::Paused) st = Status::Busy;
        else if (P->borrow_count != 0) st = Status::Busy;
        else { P->state = PoolState::Compacting; st = Status::Ok; }
    }
    PM_UNLOCK();
    if (st != Status::Ok) return st;
    Status r = compact_impl(*P, req);
    // Final commit under the same lock class borrow_begin uses (round-3
    // guide 8): the pool is published Running -- and only then -- after the
    // whole maintenance body succeeded. A planning failure restores the
    // entry state (a caller that paused the pool stays paused).
    PM_LOCK();
    if (r == Status::Ok) {
        P->structure_epoch++;
        P->state = PoolState::Running;
    } else {
        P->state = was_paused ? PoolState::Paused : PoolState::Running;
    }
    PM_UNLOCK();
    return r;
}

// ---------------------------------------------------------------------------
// Pool merge (doc section 10): physically adjacent only; source segments
// transfer to target. Transaction structure (round-3 guide section 3):
//   arming (locked) -> read-only audit + planning (scratch only) -> execution
//   (cannot fail) -> final locked commit. A planning failure leaves both
//   pools byte-identical to the entry state (R22); the previous
//   implementation mutated descriptors, order lists and statistics BEFORE
//   compact_impl's precheck and could only restore the pool states.
// ---------------------------------------------------------------------------
Status merge(PoolId source_id, PoolId target_id) {
    GlobalState& G = g();
    if (!G.initialized) return Status::NotInitialized;
    if (source_id == target_id) return Status::InvalidPool;

    // Arming (validity, adjacency, state, borrows) happens under the same
    // lock borrow_begin uses, so no new borrow can slip in between the check
    // and the Merging state (task-book v2 section 5).
    Pool* S;
    Pool* T;
    bool s_paused;
    bool t_paused;
    bool source_below;
    {
        PM_LOCK();
        S = pool_at(source_id);
        T = pool_at(target_id);
        if (!S || !T) { PM_UNLOCK(); return Status::InvalidPool; }
        if (!pool_maintainable(*S) || !pool_maintainable(*T)) {
            PM_UNLOCK();
            return Status::Busy;
        }
        if (S->borrow_count != 0 || T->borrow_count != 0) {
            PM_UNLOCK();
            return Status::Busy;
        }
        s_paused = (S->state == PoolState::Paused);
        t_paused = (T->state == PoolState::Paused);
        bool const source_above =
            (S->segment_first == (uint32_t)T->segment_first + T->segment_count);
        source_below = (T->segment_first == (uint32_t)S->segment_first + S->segment_count);
        if (!source_above && !source_below) { PM_UNLOCK(); return Status::NoSpace; }
        S->state = PoolState::Merging;
        T->state = PoolState::Merging;
        PM_UNLOCK();
    }

    // ---- read-only audit + planning ----------------------------------------
    // Nothing below writes persistent state until audit AND plan succeed;
    // every failure path restores only the pool states. The lower pool's
    // live blocks all precede the upper pool's (the ranges are adjacent), so
    // the combined address order is (lower list, upper list).
    Pool* const lower = source_below ? S : T;
    Pool* const upper = source_below ? T : S;
    PoolId const lower_id = source_below ? source_id : target_id;
    PoolId const upper_id = source_below ? target_id : source_id;
    uint8_t* const start = pool_start(*lower);
    uint8_t* const end = pool_end(*upper);
    Status st = Status::Ok;
    uint32_t nslot = 0, nbar = 0, nplan = 0;
    uint32_t n_lower = 0, n_upper = 0;

    // Full audit of BOTH pools before any planning: the descriptor audit first
    // (which also yields each pool's address-ordered slots), then the free-list
    // structure. Bounded work throughout.
    //
    // The LOWER pool is collected first on purpose. Its blocks all precede the
    // upper pool's (the ranges are adjacent), so appending upper's sorted slots
    // after lower's leaves the combined array address-ascending -- which is what
    // the plan and finalize_layout both require. That also makes the source's
    // objects one contiguous run of the plan: the prefix when the source is the
    // lower pool, the suffix otherwise. No per-entry pool field is needed.
    st = precheck_pool(*lower, lower_id, pool_start(*lower), pool_end(*lower),
                       s_slots, PM_MAX_OBJECTS, n_lower);
    if (st != Status::Ok) goto fail_restore;
    st = precheck_pool(*upper, upper_id, pool_start(*upper), pool_end(*upper),
                       s_slots + n_lower, PM_MAX_OBJECTS - n_lower, n_upper);
    if (st != Status::Ok) goto fail_restore;
    st = audit_pool_bins(*S, nullptr);
    if (st == Status::Ok) st = audit_pool_bins(*T, nullptr);
    if (st != Status::Ok) goto fail_restore;
    nslot = n_lower + n_upper;

    {
        // Pinned barriers over the combined range (address order).
        for (uint32_t i = 0; i < nslot; ++i) {
            ObjectDesc const& d = G.objects[s_slots[i]];
            if (!(d.flags & PM_PINNED)) continue;
            if (d.address - BLOCK_HEADER_SIZE < start || d.address + d.size > end) {
                st = Status::CorruptMetadata;
                goto fail_restore;
            }
            s_barriers[nbar++] = d.address - BLOCK_HEADER_SIZE;
        }
        // Cursor packing from the combined start (same argument as compact:
        // the packed prefix never exceeds each block's own offset, so the
        // plan cannot fail once precheck passed -- the branches below are
        // defensive and restore the entry states).
        uint8_t* cursor = start;
        uint32_t bar = 0;
        for (uint32_t i = 0; i < nslot; ++i) {
            ObjectDesc const& d = G.objects[s_slots[i]];
            uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
            uint32_t bsize = d.block_size;
            if (d.flags & PM_PINNED) {
                if (cursor > bstart) { st = Status::PinnedConflict; goto fail_restore; }
                // Pinned blocks stay at their address; the cursor jumps them.
                s_plan[nplan].slot = s_slots[i];
                s_plan[nplan].dst_off = off_of(bstart);
                s_plan[nplan].size = bsize;
                nplan++;
                cursor = bstart + bsize;
                bar++;
                continue;
            }
            uint8_t const* barrier = (bar < nbar) ? s_barriers[bar] : end;
            if (cursor + bsize > barrier) {
                st = (bar < nbar) ? Status::PinnedConflict : Status::NoSpace;
                goto fail_restore;
            }
            s_plan[nplan].slot = s_slots[i];
            s_plan[nplan].dst_off = off_of(cursor);
            s_plan[nplan].size = bsize;
            nplan++;
            cursor += bsize;
        }
    }

    // ---- execution (plan A: cannot fail) ------------------------------------
    // Entries ascend by source address and every movable lands at or below
    // its source (packing toward the start), so ascending execution never
    // touches a not-yet-moved source -- the same proof as compact_impl.
    {
        for (uint32_t i = 0; i < nplan; ++i) {
            ObjectDesc& d = G.objects[s_plan[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_plan[i].dst_off;
            if (dst != src) {
                move_block(dst, src, s_plan[i].size);
                d.address = dst + BLOCK_HEADER_SIZE;
                bump_epoch(d.address_epoch);
            }
            bool const from_source =
                source_below ? (i < n_lower) : (i >= n_lower);
            if (from_source) {
                d.pool_id = target_id;
                bump_epoch(d.address_epoch); // even if the address is unchanged
            }
        }
        // Rebuild the target's live-slot list straight from the audited plan.
        // The list carries no ordering guarantee any more (see order_append),
        // but it must still be a well-formed, consistently linked bag: building
        // it by splicing would re-walk a structure nothing re-validated after
        // planning (guide section 4), whereas the plan array is already trusted.
        for (uint32_t i = 0; i < nplan; ++i) {
            ObjectDesc& d = G.objects[s_plan[i].slot];
            d.addr_prev = (i == 0) ? NO_ORDER : s_plan[i - 1].slot;
            d.addr_next = (i + 1 < nplan) ? s_plan[i + 1].slot : NO_ORDER;
        }
        T->order_head = (nplan > 0) ? s_plan[0].slot : NO_ORDER;
        S->order_head = NO_ORDER;

        T->segment_first = source_below ? S->segment_first : T->segment_first;
        T->segment_count = (uint16_t)(S->segment_count + T->segment_count);
        T->live_objects = nplan; // == S.live + T.live, audited by precheck
        // finalize_layout rebuilds headers, bins and the byte statistics for
        // the combined range from the address-ordered slot array. Every entry
        // is inside [start, end) here, so nothing is skipped.
        finalize_layout(*T, start, end, s_slots, nslot);
    }

    // ---- final commit (single locked publish; guide section 3.1) ------------
    PM_LOCK();
    memset(&G.pools[source_id], 0, sizeof(Pool)); // source becomes Empty
    T->structure_epoch++;
    T->state = PoolState::Running;
    PM_UNLOCK();
    return Status::Ok;

fail_restore:
    // Audit or planning failed before anything was written: hand both pools
    // back in the state the caller left them in.
    PM_LOCK();
    S->state = s_paused ? PoolState::Paused : PoolState::Running;
    T->state = t_paused ? PoolState::Paused : PoolState::Running;
    PM_UNLOCK();
    return st;
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
    if (!G.initialized) return Status::NotInitialized;

    // Arming under the borrow lock (task-book v2 section 5): state, borrow
    // count, segment math AND the new pool slot are claimed atomically, so
    // no other API can grab the slot or observe the new pool before the
    // final commit (round-3 guide section 8). A Paused source is accepted
    // (see pool_maintainable) and remembered so a failed split restores it.
    bool was_paused;
    PoolId nid = PM_MAX_POOLS;
    {
        PM_LOCK();
        Pool* Sc = pool_at(source_id);
        if (!Sc) { PM_UNLOCK(); return Status::InvalidPool; }
        if (!pool_maintainable(*Sc)) { PM_UNLOCK(); return Status::Busy; }
        if (Sc->borrow_count != 0) { PM_UNLOCK(); return Status::Busy; }
        if (new_pool_segments == 0 || new_pool_segments >= Sc->segment_count) {
            PM_UNLOCK();
            return Status::NoSpace;
        }
        for (uint32_t i = 0; i < PM_MAX_POOLS; ++i)
            if (G.pools[i].state == PoolState::Empty) { nid = (PoolId)i; break; }
        if (nid == PM_MAX_POOLS) { PM_UNLOCK(); return Status::NoSpace; }
        was_paused = (Sc->state == PoolState::Paused);
        Sc->state = PoolState::Splitting;
        G.pools[nid].state = PoolState::Splitting; // claimed, not yet usable
        PM_UNLOCK();
    }
    Pool* S = pool_at(source_id);
    Pool* N = &G.pools[nid];

    uint8_t* start = pool_start(*S);
    uint8_t* end = pool_end(*S);
    uint32_t keep = S->segment_count - new_pool_segments;
    uint8_t* boundary = seg_base(S->segment_first + keep);
    Status st = Status::Ok;
    uint32_t nslot = 0, nbar = 0, nlow = 0, nup = 0, n_right = 0;

    // Gate every memmove on a full descriptor audit, which also yields the
    // address-ordered slot array. A pre-check failure restores Running with zero
    // side effects.
    st = precheck_pool(*S, source_id, start, end, s_slots, PM_MAX_OBJECTS, nslot);
    if (st != Status::Ok) goto fail_restore;

    // Collect pinned barriers (address order).
    for (uint32_t i = 0; i < nslot; ++i) {
        ObjectDesc const& d = G.objects[s_slots[i]];
        if (!(d.flags & PM_PINNED)) continue;
        s_barriers[nbar++] = d.address - BLOCK_HEADER_SIZE;
    }

    // Plan phase (read-only).
    {
        uint8_t* low_cursor = start;
        uint8_t* up_cursor = boundary;
        uint32_t bar = 0;
        for (uint32_t i = 0; i < nslot; ++i) {
            ObjectDesc const& d = G.objects[s_slots[i]];
            uint8_t* bstart = d.address - BLOCK_HEADER_SIZE;
            uint32_t bsize = d.block_size;
            bool upper = (bstart >= boundary) || (bstart + bsize > boundary); // crossing or above
            if (d.flags & PM_PINNED) {
                if (bstart < boundary && bstart + bsize > boundary) {
                    st = Status::PinnedConflict;
                    goto fail_restore;
                }
                uint8_t const* cur = upper ? up_cursor : low_cursor;
                if (cur > bstart) { st = Status::PinnedConflict; goto fail_restore; }
                if (upper) up_cursor = bstart + bsize;
                else low_cursor = bstart + bsize;
                bar++;
                continue;
            }
            if (!upper) {
                // Pack toward the pool start; never cross the boundary or a
                // pinned barrier below it.
                uint8_t const* barrier = boundary;
                if (bar < nbar && s_barriers[bar] < boundary) barrier = s_barriers[bar];
                if (low_cursor + bsize > barrier) {
                    st = (bar < nbar && s_barriers[bar] < boundary) ? Status::PinnedConflict
                                                                    : Status::NoSpace;
                    goto fail_restore;
                }
                s_plan[nlow].slot = s_slots[i];
                s_plan[nlow].dst_off = off_of(low_cursor);
                s_plan[nlow].size = bsize;
                nlow++;
                low_cursor += bsize;
            } else {
                // Pack from the boundary upward; barriers are pinned objects
                // above the boundary and the pool end.
                uint8_t const* barrier = (bar < nbar) ? s_barriers[bar] : end;
                if (up_cursor + bsize > barrier) {
                    st = (bar < nbar) ? Status::PinnedConflict : Status::NoSpace;
                    goto fail_restore;
                }
                s_upper[nup].slot = s_slots[i];
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
            move_block(dst, src, s_upper[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
        }
        for (uint32_t i = n_right; i < nup; ++i) { // pass 2: leftward, ascending
            ObjectDesc& d = G.objects[s_upper[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_upper[i].dst_off;
            move_block(dst, src, s_upper[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
        }
        // Lower side: ascending, dst <= src, region below the boundary.
        for (uint32_t i = 0; i < nlow; ++i) {
            ObjectDesc& d = G.objects[s_plan[i].slot];
            uint8_t* src = d.address - BLOCK_HEADER_SIZE;
            uint8_t* dst = G.zone + s_plan[i].dst_off;
            move_block(dst, src, s_plan[i].size);
            d.address = dst + BLOCK_HEADER_SIZE;
            bump_epoch(d.address_epoch);
        }
    }

    // Commit: transfer the upper segments and rebuild both order lists from
    // the audited slot array (s_slots is already address-sorted, so straight
    // appends preserve the invariant -- no untrusted traversal, guide
    // section 4). The new pool stays Splitting, invisible to every API that
    // requires Running, until the final locked publish.
    {
        N->segment_first = (uint16_t)(S->segment_first + keep);
        N->segment_count = (uint16_t)new_pool_segments;
        S->order_head = NO_ORDER;
        N->order_head = NO_ORDER;

        uint32_t moved_count = 0;
        uint32_t s_tail = NO_ORDER, n_tail = NO_ORDER;
        for (uint32_t i = 0; i < nslot; ++i) {
            uint32_t idx = s_slots[i];
            ObjectDesc& d = G.objects[idx];
            bool const above = (d.address >= boundary);
            if (above) {
                d.pool_id = nid;
                ++moved_count;
            }
            uint32_t& tail = above ? n_tail : s_tail;
            d.addr_prev = tail;
            d.addr_next = NO_ORDER;
            if (tail != NO_ORDER) G.objects[tail].addr_next = idx;
            else (above ? *N : *S).order_head = idx;
            tail = idx;
        }
        S->live_objects -= moved_count;
        N->live_objects = moved_count;

        S->segment_count = (uint16_t)keep;
        // Execution phase: cannot fail (plan A, task-book 3.6). Both halves get
        // the SAME address-ordered array; finalize_layout skips the entries that
        // fall outside each half's range, so neither half depends on an
        // unverified "below the boundary is a prefix" assumption.
        finalize_layout(*S, start, boundary, s_slots, nslot);
        finalize_layout(*N, boundary, end, s_slots, nslot);

        PM_LOCK();
        S->structure_epoch++;
        S->state = PoolState::Running;
        N->structure_epoch = 1;
        N->state = PoolState::Running; // published with the source
        PM_UNLOCK();
        out_new = nid;
        return Status::Ok;
    }

fail_restore:
    // Plan failure: nothing was moved; restore the entry state and unclaim
    // the new pool slot (a caller that paused the source stays paused).
    PM_LOCK();
    S->state = was_paused ? PoolState::Paused : PoolState::Running;
    memset(&G.pools[nid], 0, sizeof(Pool));
    PM_UNLOCK();
    return st;
}

// ---------------------------------------------------------------------------
// Validation (doc section 15, task-book section 6, v2 section 9).
//
// Every walk is step-limited so corrupted (cyclic) metadata returns
// CorruptMetadata in bounded time instead of hanging, and every range is
// checked as a uint64 ZONE OFFSET before a pointer is formed -- a corrupted
// pointer field must never reach pointer arithmetic, which would be undefined
// behaviour rather than a clean rejection (task-book v2 section 9.1).
//
// Three independent audits, all rooted at the pool's address-ordered live
// list (the only trusted ordering):
//   1) live list: descriptor/block agreement, alignment, bounds and the
//      prev_size chain;
//   2) bins: free-list structure, bitmap agreement, bounds, prev_size chain;
//   3) coverage over the UNION of live and free blocks: no overlaps, and every
//      uncovered byte run must be shorter than PM_MIN_BLOCK (a longer run
//      would have to be a binned free block). Together with the byte sum
//      `live + binned_free + gaps == capacity` this proves the pool has no
//      hidden hole and no double-counted region -- a tampered statistic cannot
//      mask either (v2 section 9.3).
//
// The gap invariant deserves a note, because the naive "every free block
// exactly fills an inter-live gap" formulation is WRONG: free() merges only
// physically adjacent free blocks, so a sub-minimal slack run survives between
// a freed block and its former neighbour, producing two free blocks separated
// by slack. What always holds is the weaker, checkable statement above.
// ---------------------------------------------------------------------------
Status validate(PoolId id) {
    GlobalState const& G = g();
    if (!G.initialized) return Status::NotInitialized;
    Pool const* P = pool_at(id);
    if (!P) return Status::InvalidPool;

    const uint64_t zbase = (uint64_t)(uintptr_t)G.zone;
    const uint32_t capacity = pool_capacity(*P);
    // Pure-integer geometry: validate() is the auditor of last resort and
    // runs on pools whose segment fields are not proven yet (A4-10); only
    // descriptor addresses (real pointers, integer-converted) use zbase.
    const uint64_t start_off = pool_start_off(*P);
    const uint64_t end_off = start_off + capacity;

    // [off, off+len) lies inside the pool byte range. len is added in 64-bit
    // so a hostile offset cannot wrap.
    auto in_pool = [&](uint64_t off, uint64_t len) -> bool {
        return off >= start_off && off <= end_off && len <= end_off - off;
    };

    // prev_size chain: shared with audit_pool_bins (same rule, same proof).
    auto check_prev = [&](uint64_t boff) -> Status {
        return prev_link_ok(boff, start_off);
    };

    // 1) live slots. collect_live_sorted is the single sanctioned enumeration
    //    (bounded walk, index/state/pool/generation checks, addr_prev agreement,
    //    cycle cap) and therefore the sole detection point for a damaged
    //    live-slot list. What remains here is descriptor/block agreement,
    //    strictly increasing non-overlapping placement, header agreement and
    //    the prev_size chain.
    uint32_t nslots = 0;
    if (collect_live_sorted(*P, id, s_slots, PM_MAX_OBJECTS, nslots) !=
        Status::Ok)
        return Status::CorruptMetadata;

    uint32_t used = 0;
    uint64_t prev_end = start_off;
    for (uint32_t i = 0; i < nslots; ++i) {
        ObjectDesc const& d = G.objects[s_slots[i]];
        if (!desc_block_consistent(d)) return Status::CorruptMetadata;

        uint64_t aabs = (uint64_t)(uintptr_t)d.address;
        if (aabs < zbase + BLOCK_HEADER_SIZE) return Status::CorruptMetadata;
        uint64_t aoff = aabs - zbase;
        uint64_t boff = aoff - BLOCK_HEADER_SIZE;
        if (!in_pool(boff, d.block_size) || !in_pool(aoff, d.size))
            return Status::CorruptMetadata;
        if (boff < prev_end) return Status::CorruptMetadata; // ordering/overlap

        if (blk_is_free(G.zone + boff) || blk_size_of(G.zone + boff) != d.block_size)
            return Status::CorruptMetadata; // used block header must agree
        if (check_prev(boff) != Status::Ok) return Status::CorruptMetadata;
        prev_end = boff + d.block_size;
        used += d.block_size;
    }
    if (nslots != P->live_objects || used != P->used_bytes) return Status::CorruptMetadata;
    if (P->free_bytes != capacity - used) return Status::CorruptMetadata;

    // 2) bins: delegated to audit_pool_bins -- bounded walks, bitmap/list
    //    agreement, size classes, reciprocal neighbour links and the
    //    prev_size chain. It also totals the binned free bytes, which must
    //    reconcile with the pool's byte accounting below: a duplicated or
    //    missing free block (e.g. the same block inserted into two bins)
    //    breaks the total.
    uint64_t free_total = 0;
    if (audit_pool_bins(*P, &free_total) != Status::Ok) return Status::CorruptMetadata;

    // Enumerate the binned free blocks. Safe to dereference: phase 2 proved
    // every list node readable and inside the pool.
    auto for_each_free = [&](auto&& fn) {
        for (uint32_t f = 0; f < FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                for (uint32_t o = P->bins.head[f][sl]; o != NULL_OFF;
                     o = ptr_of(o)->next)
                    fn((uint64_t)o, (uint64_t)blk_size_of(ptr_of(o)));
    };

    // 3) Coverage audit over the union. For a block starting at `s`, `prev` is
    //    the highest end address among all blocks that end at or before `s`,
    //    i.e. the byte before the gap that precedes it. `overlap` reports a
    //    block straddling `s`, which would make that value meaningless.
    //
    //    =====================================================================
    //    Both quantities above are properties of the WHOLE block set, not of
    //    the order the blocks are visited in. The shipped code recomputes them
    //    from scratch for every block -- O(live + free) work per block, hence
    //    O((live+free)^2) -- which is why this function is the only remaining
    //    superlinear path in the library (measured exponent 1.70 on device,
    //    bench/RESULTS.md section 2).
    //
    //    Visit the blocks once in ascending address order and keep a running
    //    maximum of end addresses and both quantities fall out for free. Live
    //    blocks are already in address order (collect_live_sorted); the free
    //    blocks are enumerated from the bins and sorted here into
    //    s_free_off[]. That is the whole change.
    //
    //    This is NOT a reinterpretation of the audit. Every predicate below is
    //    the same predicate, and the equal-start behaviour is reproduced
    //    exactly: a block whose start equals `s` never contributes to `prev`
    //    and never counts as a straddle (the shipped loops test `ls < s` /
    //    `fo < s` strictly), so blocks sharing a start are processed as one
    //    group; `slack` still receives `gap` once PER BLOCK (`gap * members`)
    //    and the free-block duplicate rule is still "for each free block, the
    //    number of free blocks sharing its start must be 1".
    //
    //    Bounded scratch, per docs/AUDIT_LEDGER.md item 9, which rejected the
    //    naive linearisation because `free` is not bounded by PM_MAX_OBJECTS
    //    and a capacity-sized array would cost 64-512 KB. The scratch is
    //    O(PM_MAX_OBJECTS) like every other buffer here. A pool whose binned
    //    free count exceeds the capacity is itself corruption -- the library
    //    coalesces physically adjacent free blocks, so every binned free block
    //    is a maximal run and there are at most live+1 of them -- and is
    //    refused (R57); the quadratic fallback this once declined to is gone,
    //    both because the refusal is exact and because a tampered pool could
    //    otherwise hold validate() hostage for a superlinear walk (measured:
    //    11.3 s at 1 MiB on host for a bin-consistent but uncoalesced layout).
    //    =====================================================================
    uint64_t slack = 0, max_end = start_off;
    Status st = Status::Ok;

    auto linear_sweep = [&]() -> Status {
        uint32_t nfree = 0;
        for_each_free([&](uint64_t f0, uint64_t) {
            if (nfree < kFreeOffCap) s_free_off[nfree] = (uint32_t)f0;
            ++nfree;
        });
        // Reachable under corruption: the earlier checks enforce bin
        // structure, not maximality, so a tampered pool can bin more free
        // blocks than live+1 (uniform 16 B blocks pass audit_pool_bins).
        // Refuse in O(nfree); never run a quadratic walk on hostile input.
        if (nfree > kFreeOffCap) return Status::CorruptMetadata;
        sort_u32_asc(s_free_off, nfree);

        uint32_t li = 0, fi = 0;
        uint64_t run_max_end = start_off;
        bool prev_group_free = false;
        constexpr uint64_t kNone = (uint64_t)-1;
        while (li < nslots || fi < nfree) {
            uint64_t const ls =
                (li < nslots)
                    ? (((uint64_t)(uintptr_t)G.objects[s_slots[li]].address -
                        zbase) - BLOCK_HEADER_SIZE)
                    : kNone;
            uint64_t const fs = (fi < nfree) ? (uint64_t)s_free_off[fi] : kNone;
            uint64_t const s = ls < fs ? ls : fs;

            if (run_max_end > s) { st = Status::CorruptMetadata; return st; }
            uint64_t const gap = s - run_max_end;
            if (gap >= PM_MIN_BLOCK) { st = Status::CorruptMetadata; return st; }

            uint32_t members = 0, fmembers = 0;
            uint64_t group_end = run_max_end;
            while (li < nslots) {
                ObjectDesc const& d = G.objects[s_slots[li]];
                uint64_t const a =
                    ((uint64_t)(uintptr_t)d.address - zbase) - BLOCK_HEADER_SIZE;
                if (a != s) break;
                ++members;
                ++li;
                if (a + d.block_size > group_end) group_end = a + d.block_size;
            }
            while (fi < nfree && (uint64_t)s_free_off[fi] == s) {
                ++members;
                ++fmembers;
                uint64_t const e =
                    s + (uint64_t)blk_size_of(ptr_of(s_free_off[fi]));
                if (e > group_end) group_end = e;
                ++fi;
            }
            // Shipped rule: for each free block, exactly one free block shares
            // its start address.
            if (fmembers > 1) { st = Status::CorruptMetadata; return st; }
            // R57: physically adjacent binned free blocks (gap == 0) can never
            // occur -- free() coalesces its physical neighbours, so distinct
            // binned free blocks are separated by live blocks or by the
            // documented sub-minimal slack (gap < PM_MIN_BLOCK is legal here,
            // see the gap-invariant note above; gap == 0 between two free
            // groups is not).
            if (fmembers >= 1 && prev_group_free && gap == 0) {
                st = Status::CorruptMetadata;
                return st;
            }
            slack += gap * (uint64_t)members;
            run_max_end = group_end;
            prev_group_free = fmembers >= 1;
        }
        max_end = run_max_end;
        st = Status::Ok;
        return st;
    };
    st = linear_sweep();
    if (st != Status::Ok) return st;

    // Tail: whatever follows the last block must also be sub-minimal slack.
    if (end_off - max_end >= PM_MIN_BLOCK) return Status::CorruptMetadata;
    slack += end_off - max_end;

    // Whole-pool reconciliation: live bytes + binned free bytes + slack must
    // be exactly the capacity. `used` comes from the live descriptors and
    // `free_bytes` from the live walk, so tampering with either statistic, the
    // bitmap or a block header breaks this identity.
    if (used + free_total + slack != (uint64_t)capacity) return Status::CorruptMetadata;
    if (free_total != (uint64_t)P->free_bytes - (uint64_t)P->fragment_bytes)
        return Status::CorruptMetadata;

    // (the first-level bitmap agreement is part of audit_pool_bins above)
    return Status::Ok;
}

} // namespace pm
