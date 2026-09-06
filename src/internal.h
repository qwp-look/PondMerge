// PondMerge v1 - internal structures. Not part of the public API.
#pragma once

#include "pondmerge/pm_config.h"
#include "pondmerge/pm_port.h"
#include <stdint.h>
#include <stddef.h>

namespace pm {
namespace internal {

// --- block header -----------------------------------------------------------
// Every block (used or free) starts with an 8-byte header:
//   [0..4)  this block's size | BLOCK_FREE_BIT (set when this block is free)
//   [4..8)  physical predecessor's size (0 = pool start or unmanaged slack)
// Blocks are PM_ALIGNMENT-aligned, so the low 3 bits of the sizes are free
// for flags. Free-list links are u32 offsets from the zone base so the block
// format is identical on 32- and 64-bit targets.
static constexpr uint32_t BLOCK_HEADER_SIZE = 8;
static constexpr uint32_t BLOCK_FREE_BIT    = 1u;
static constexpr uint32_t NULL_OFF          = 0xFFFFFFFFu;

struct FreeBlock {
    uint32_t header;     // own size | BLOCK_FREE_BIT (meaningful when free)
    uint32_t prev_size;  // predecessor's size
    uint32_t prev;       // bin list links as zone offsets (NULL_OFF = none)
    uint32_t next;
};

static_assert(sizeof(FreeBlock) <= PM_MIN_BLOCK, "min block too small for links");

// --- TLSF bins --------------------------------------------------------------
static constexpr uint32_t SL_COUNT = PM_SL_COUNT;
static constexpr uint32_t MIN_FL   = 4; // floor(log2(PM_MIN_BLOCK)) = log2(16)
static constexpr uint32_t FL_COUNT = PM_FL_MAX - MIN_FL;
static_assert(FL_COUNT <= 32, "enlarge fl_bitmap for larger zones");

struct TlsfBins {
    uint32_t fl_bitmap;                 // bit f: first level MIN_FL+f non-empty
    uint16_t sl_bitmap[FL_COUNT];       // bit s: bin (f,s) non-empty
    uint32_t head[FL_COUNT][SL_COUNT];  // list heads as zone offsets

    void reset();
};

// --- object descriptor (doc section 4) --------------------------------------
enum class ObjState : uint8_t { Free, Live, Destroying };

struct ObjectDesc {
    uint8_t* address;              // payload start
    uint32_t size;                 // user-visible payload size
    uint32_t block_size;           // full physical block incl. header
    uint32_t user_tag;             // caller-defined id for diagnostics
    uint16_t pool_id;
    uint16_t flags;
    uint16_t generation;           // lifecycle generation; 0 invalid
    uint32_t address_epoch;
    uint32_t active_borrows;
    uint16_t next_free_slot;       // free-slot list when state == Free
    uint32_t addr_prev;            // address_order links (NO_ORDER = none)
    uint32_t addr_next;
    void (*destroy_fn)(void*);
    ObjState state;
};

static constexpr uint32_t NO_ORDER = 0xFFFFFFFFu;
static constexpr uint16_t NO_SLOT  = 0xFFFFu;

// --- pool (doc section 3) ---------------------------------------------------
enum class PoolState : uint8_t {
    Empty,
    Running,
    Paused,
    Compacting,
    Merging,
    Splitting,
};

// Kept trivial (no default member initializers) so memset-based resets in
// core.cpp are well-defined.
struct Pool {
    PoolState state;
    uint16_t segment_first;
    uint16_t segment_count;
    uint32_t borrow_count;
    uint32_t structure_epoch;
    uint32_t used_bytes;       // sum of live block_size
    uint32_t free_bytes;
    uint32_t fragment_bytes;   // sub-minimal gaps absorbed/slack
    uint32_t live_objects;
    uint32_t objects_moved;    // last compaction statistics
    uint32_t bytes_moved;
    uint64_t compact_time_us;
    TlsfBins bins;
    uint32_t order_head;       // address_order list head (descriptor idx)
};

// --- global state -----------------------------------------------------------
struct GlobalState {
    uint8_t*  zone;
    uint32_t  zone_size;
    uint32_t  segment_size;
    uint32_t  segment_count;

    Pool pools[PM_MAX_POOLS];
    ObjectDesc objects[PM_MAX_OBJECTS];
    uint16_t free_slot_head;
    uint32_t live_object_count;

    // High-water statistics (doc section 18).
    uint32_t max_live_objects;
    uint32_t max_borrow_count;
    uint32_t max_fragment_bytes;
    uint32_t max_bytes_moved;
    uint64_t max_compact_time_us;
    uint32_t initialized;
};

GlobalState& g();
uint32_t metadata_scratch_bytes(); // compaction plan scratch size

// --- helpers implemented in core.cpp ----------------------------------------
uint32_t fl_index(uint32_t size);
uint32_t sl_index(uint32_t size, uint32_t fl);
void bins_insert(TlsfBins& b, FreeBlock* blk);
void bins_remove(TlsfBins& b, FreeBlock* blk);
FreeBlock* bins_find(TlsfBins& b, uint32_t need);

inline uint8_t* seg_base(uint32_t first) {
    return g().zone + (uint64_t)first * g().segment_size;
}
inline uint32_t off_of(void* p) { return (uint32_t)((uint8_t*)p - g().zone); }
inline FreeBlock* ptr_of(uint32_t off) { return (FreeBlock*)(g().zone + off); }

} // namespace internal
} // namespace pm
