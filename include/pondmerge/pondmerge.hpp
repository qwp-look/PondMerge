// PondMerge v1 - public API.
//
// A managed-memory system for MMU-less MCUs: a fixed Auto Zone is split into
// pools; objects are referenced by stable logical ids; physical addresses are
// only handed out through RAII borrows; compaction/merge/split are explicit,
// caller-triggered operations.
//
// C++17 subset: no exceptions, no RTTI, no dynamic allocation.
//
// CONCURRENCY CONTRACT (task-book section 4; round-5 guide section 4): v1 is
// "single owner + quiescent maintenance window".
//   * alloc / free / resolve / get_stats / validate run in ONE owner
//     execution context (thread/task). PondMerge does not make them safe for
//     concurrent callers.
//   * borrow_begin/borrow_end and the pool state flips (pause/resume/compact
//     entry) are internally synchronized, so pausing can never race a new
//     borrow into existence.
//   * compact/merge/split are ALSO single-owner operations: they must never
//     be called concurrently from two contexts, not even for different pools
//     (the planning scratch is one shared fixed buffer). Within one owner
//     they are internally synchronized at entry and at the final commit.
//   * Before compact/merge/split, the owner pauses the pool(s); all active
//     borrows must have ended (borrow counters zero). DMA, ISRs, other tasks
//     and external code holding raw pointers must be stopped and drained by
//     the CALLER beforehand — PondMerge cannot discover external holders.
//   * SCOPE OF PM_LOCK: it protects only what the library knows -- the borrow
//     counters and the maintenance state publications. It does NOT cover the
//     alloc/free/resolve/get_stats/validate bodies, and it cannot see raw
//     pointers held by DMA, ISRs or external code. Host builds compile
//     PM_LOCK to nothing, so host runs prove NONE of the lock semantics;
//     SMP evidence comes from the dual-core device test only.
//   * Constructors (pm_make) and destroy callbacks must not re-enter the
//     allocator for the object being constructed/destroyed; re-entrancy for
//     OTHER objects is allowed but ordering-sensitive.
// What PondMerge guarantees and what the caller still owes are restated in
// README.md.
#pragma once

#include "pm_config.h"
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <new>
#include <utility>

namespace pm {

// ---------------------------------------------------------------------------
// Status codes (doc section 9/17)
// ---------------------------------------------------------------------------
enum class Status : int8_t {
    Ok = 0,
    Busy,               // active borrow prevents the operation
    NoSpace,            // pool/zone/segment exhaustion
    InvalidPool,
    InvalidRef,         // bad index / stale generation / bad size or offset
    InvalidAlignment,   // alignment > PM_MAX_ALIGNMENT or not a power of two
    PinnedConflict,     // a pinned object blocks the requested layout
    NotRelocatable,     // object not movable but relocation required
    PoolChanged,        // local ref whose object moved to another pool
    AlreadyPaused,
    CorruptMetadata,
};

inline const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "OK";
        case Status::Busy: return "BUSY";
        case Status::NoSpace: return "NO_SPACE";
        case Status::InvalidPool: return "INVALID_POOL";
        case Status::InvalidRef: return "INVALID_REF";
        case Status::InvalidAlignment: return "INVALID_ALIGNMENT";
        case Status::PinnedConflict: return "PINNED_CONFLICT";
        case Status::NotRelocatable: return "NOT_RELOCATABLE";
        case Status::PoolChanged: return "POOL_CHANGED";
        case Status::AlreadyPaused: return "ALREADY_PAUSED";
        case Status::CorruptMetadata: return "CORRUPT_METADATA";
    }
    return "?";
}

using PoolId = uint16_t;
static constexpr uint16_t CROSS_HINT = 0xFFFF; // pool_hint value of a cross-pool ref

// Object flags (doc section 4)
enum ObjectFlags : uint16_t {
    PM_MOVABLE   = 1u << 0,
    PM_PINNED    = 1u << 1,
    PM_DMA       = 1u << 2,  // implies pinned
    PM_EXTERNAL  = 1u << 3,
    PM_ZERO_INIT = 1u << 4,
};

// ---------------------------------------------------------------------------
// Logical reference (doc section 5). object_offset addresses sub-objects.
// ---------------------------------------------------------------------------
struct RawRef {
    uint32_t index;        // descriptor slot
    uint16_t generation;   // lifecycle generation, 0 is never valid
    uint16_t pool_hint;    // owning pool at creation, or CROSS_HINT
    uint32_t offset;       // byte offset inside the payload
};

struct Config {
    uint8_t* zone;         // start of the Auto Zone (>= 8 byte aligned)
    uint32_t zone_size;    // total bytes, multiple of segment_size preferred
    uint32_t segment_size; // pool resize granularity (power of two, >= 4 KiB)
};

struct PoolStats {
    uint8_t  state;
    uint8_t  valid;            // 1 = free-list walk completed; 0 = a corrupted
                               // list was refused, largest_free_block is then
                               // meaningless (0) -- the two "zero" cases are
                               // distinguishable (round-4 task book section 11)
    uint16_t segment_first;
    uint16_t segment_count;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t largest_free_block;
    uint32_t fragment_bytes;   // sub-minimal-block gaps absorbed into blocks
    uint32_t object_count;
    uint32_t borrow_count;
    uint32_t structure_epoch;
    uint32_t objects_moved;    // last compaction
    uint32_t bytes_moved;
    uint64_t compact_time_us;
};

struct GlobalStats {
    uint32_t max_live_objects;
    uint32_t max_borrow_count;
    uint32_t max_fragment_bytes;
    uint32_t max_bytes_moved;
    uint64_t max_compact_time_us;
    uint32_t metadata_bytes;   // size of all static metadata
};

// --- lifecycle --------------------------------------------------------------
Status init(Config const& cfg);
Status deinit();
GlobalStats global_stats();

// --- pools ------------------------------------------------------------------
Status create_pool(PoolId& out, uint32_t segment_count);
Status destroy_pool(PoolId pool);
Status pause(PoolId pool);
Status resume(PoolId pool);
PoolStats get_stats(PoolId pool);
Status validate(PoolId pool);

// --- objects ----------------------------------------------------------------
// alloc: first-fit inside the bin the TLSF bitmap selects, so it is
// O(bin chain length) -- see the complexity note in the maintenance section.
// Requests at or above 2^PM_FL_MAX are refused with NoSpace rather than being
// clamped into the top bin.
Status alloc(PoolId pool, uint32_t size, uint32_t alignment, uint16_t flags,
             uint32_t user_tag, RawRef& out);
Status free(RawRef const& ref);
// Sets destroy_fn for a live object. Pinned objects only: a movable object
// would have its destructor run at a moved address after a byte-wise memmove,
// so movable refs are refused with NotRelocatable (task-book v2 section 10).
Status set_destroy_fn(RawRef const& ref, void (*destroy_fn)(void*));

// --- borrow accounting ------------------------------------------------------
// Full validation + borrow. `access_size`/`access_align` describe the sub-object
// being touched (sizeof(T)/alignof(T) plus the ref offset). On failure
// out_addr is written as nullptr, never left stale.
Status borrow_begin(RawRef const& ref, uint32_t access_size, uint32_t access_align,
                    void*& out_addr);
// Ends exactly one borrow_begin. The token (index, generation, pool binding)
// and the counters are validated and decremented inside ONE critical
// section: a duplicate, stale or wrong-pool end can never move a counter
// (Debug asserts such tokens as caller bugs; Release ignores them).
void   borrow_end(RawRef const& ref);
// ADVANCED, NON-BORROWING validation (round-5 guide section 6): resolve
// returns a raw pointer WITHOUT incrementing any borrow counter. That pointer
// is valid only while the pool is quiescent (no maintenance entry, no
// concurrent mutation -- see the concurrency contract) and must NEVER be
// carried across a compact/merge/split call or stored. The supported way to
// touch an object is try_borrow()/pm_access or the expression-level RAII
// operator-> of pm_ptr, which hold a real borrow for their lifetime. There
// is no address cache behind resolve: the protection comes from the borrow
// counters and the quiescent window, not from epoch checks. On failure
// out_addr is ALWAYS nullptr, so a caller that reuses the variable cannot
// keep a stale address (round-3 guide P2).
Status resolve(RawRef const& ref, uint32_t access_size, uint32_t access_align,
               void*& out_addr);

// --- maintenance (doc sections 8, 10, 11) -----------------------------------
//
// MAINTAINABILITY CONTRACT (task-book v2 section 5.1). All three operations
// take the same entry states and follow the same rules, so a caller may either
// open a quiescent window explicitly (pause -> operate -> resume) or just call
// the operation:
//
//   entry state    Running or Paused both accepted. A pool already inside
//                  Compacting/Merging/Splitting is refused with Busy.
//   borrows        any non-zero borrow counter -> Busy, before anything moves.
//                  The library cannot see DMA/ISR/other-thread holders; the
//                  caller must stop those first.
//   plan failure   the ENTRY state is restored and nothing was moved. The one
//                  exception is compact()'s borrow refusal: doc section 8 makes
//                  compact enter Paused before checking borrows, so a refused
//                  compact leaves the pool Paused for the caller to resume().
//   success        the resulting pool(s) end up Running (merge: the target is
//                  Running and the source becomes Empty).
//
// COMPLEXITY (task-book v2 section 7.3; worst case, not amortised). alloc is
// NOT O(1): the TLSF bitmap locates a bin, and several block sizes share one
// SL bin, so the allocator walks that bin for a first fit. The honest bounds
// are: alloc O(bin chain length + live objects) -- the TLSF bitmap locates a
// bin and walks it first-fit, then the descriptor is linked into the
// address-order list (bounded walk, round-4 task book section 7);
// free O(1 + the bin chain lengths of its free neighbours), upper bound
// O(zone_size / PM_MIN_BLOCK) -- free proves the neighbours' free-list
// membership before merging instead of trusting their headers (round-3
// guide 6.2); pause/resume O(1); compact/merge/split O(objects + moved
// bytes) plus the read-only audits (merge audits both pools' order lists,
// descriptors, statistics and bins: O(objects + free blocks)); validate
// O((live + free)^2); get_stats O(free_blocks) with a step cap (a refused
// walk on a corrupted list is reported via PoolStats::valid == 0).
Status compact(PoolId pool);
Status merge(PoolId source, PoolId target);
// Splits `source` after `new_pool_segments` segments; the new pool owns the
// upper range. Crossing movable objects are relocated; crossing pinned objects
// fail with PinnedConflict before anything changes. A side that cannot hold
// its objects fails with NoSpace, also before anything changes.
Status split(PoolId source, uint32_t new_pool_segments, PoolId& out_new);

// Debug hook used by PM_ASSERT.
void pm_debug_abort(const char* file, int line);

// ---------------------------------------------------------------------------
// Compaction advice (round-6 requirements doc section 6): a READ-ONLY
// analysis answering "is compaction worth trying now / would my next
// allocation of size X fit". It never moves objects, never changes pool
// state, generation, address_epoch or any allocator byte, and never calls
// compact() itself. Executing a compaction stays an explicit caller action
// under the full maintenance contract (state, borrows, external quiescence).
// ---------------------------------------------------------------------------
enum class CompactionVerdict : uint8_t {
    NO_ACTION = 0,            // nothing to gain right now
    COMPACT_RECOMMENDED,      // fragmentation and/or the expected request
                              // would benefit from a compaction
    COMPACT_BLOCKED,          // borrows active or pool not Running/Paused
                              // (see borrow_count / pool_state diagnostics)
    COMPACT_UNLIKELY_TO_HELP, // even a successful compaction could not satisfy
                              // the expected request, or there is nothing to
                              // move (no fragmentation)
    INVALID_METADATA,         // invalid pool id or damaged metadata detected
                              // by the advice's bounded audit
};

// What the caller intends to allocate next (all optional). size 0 = "no
// specific request": the advice then judges general fragmentation only.
struct CompactionRequest {
    uint32_t requested_size;
    uint32_t requested_alignment; // 0 = default PM_ALIGNMENT
    uint16_t requested_flags;     // informational (PM_MOVABLE / PM_PINNED / ...)
    uint32_t user_tag;
};

// Tunable advice thresholds. Defaults are documented in
// docs/COMPACTION_POLICY.md; query them, do not assume them. Both apply to
// the STRANDED free bytes -- free_bytes - fragment_bytes - largest_free_block,
// i.e. the bytes stuck in secondary holes that a compaction could consolidate
// (fragment_bytes alone is only the sub-minimal slack and stays near zero in
// normal operation).
struct CompactionThresholds {
    uint32_t fragment_ratio_permille; // fragmented when stranded*1000 /
                                      // capacity >= this (default 100 = 10%)
    uint32_t fragment_min_bytes;      // ... AND stranded >= this (default 512 B)
};

CompactionThresholds get_compaction_thresholds();
void set_compaction_thresholds(CompactionThresholds const& t);

// Marker for diagnostic fields the implementation cannot estimate honestly.
static constexpr uint32_t COMPACTION_ESTIMATE_UNKNOWN = 0xFFFFFFFFu;

struct CompactionAdvice {
    CompactionVerdict verdict;
    // ---- diagnostics (informational; the verdict alone is sufficient for
    // ---- callers that do not want to reason about internals)
    uint8_t  pool_state;            // raw PoolState value (doc section 3)
    uint32_t capacity;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t largest_free_block;    // 0 when the walk was refused (valid == 0)
    uint32_t fragment_bytes;
    uint32_t fragment_ratio_permille; // fragment_bytes * 1000 / capacity
    uint32_t live_objects;
    uint32_t borrow_count;
    uint8_t  has_pinned_objects;
    uint32_t estimated_moved_objects; // COMPACTION_ESTIMATE_UNKNOWN unless the
                                      // estimate is trivially exact (see
                                      // docs/COMPACTION_POLICY.md)
    uint32_t estimated_moved_bytes;   // same honesty rule as above
    uint8_t  stats_valid;             // 0 = free-list walk refused
    // ---- expected-request echo (0 when no request was given)
    uint32_t expected_request_size;
    uint32_t expected_request_alignment;
    uint8_t  request_can_fit_now;     // largest_free_block >= need
    uint8_t  request_can_fit_after_compaction_estimate; // free - fragment >= need
    uint8_t  external_quiescence_required; // 1 = stop DMA/ISR/external users
                                           // before calling compact()
};

// Read-only analysis; may be called in any pool state. Returns INVALID_METADATA
// for an unknown pool id or when the bounded audit detects damage.
CompactionAdvice analyze_compaction(PoolId pool,
                                    CompactionRequest const* expected = nullptr);

// Same analysis plus repeat-prompt suppression: the per-pool "last advice"
// cache (separate fixed storage, never allocator metadata) is compared against
// the fresh result; *changed is false when verdict, structure_epoch,
// borrow_count, largest_free_block and fragment_bytes are all unchanged.
// Suppression is the ONLY supported auto-prompt mechanism: polling this from
// a monitor loop is safe, calling it from an ISR is not, and nothing here
// ever executes a compaction.
CompactionAdvice poll_compaction_advice(PoolId pool,
                                        CompactionRequest const* expected,
                                        bool* changed);

// ---------------------------------------------------------------------------
// Generic result wrapper
// ---------------------------------------------------------------------------
template <class T>
struct Result {
    Status status = Status::InvalidRef;
    T      value{};
    Result() = default;
    Result(Status s, T v) : status(s), value(std::move(v)) {}
    explicit operator bool() const { return status == Status::Ok; }
    bool ok() const { return status == Status::Ok; }
    T&       operator*()       { return value; }
    T const& operator*() const { return value; }
    T*       operator->()      { return &value; }
};

// ---------------------------------------------------------------------------
// RAII borrow (doc section 6)
// ---------------------------------------------------------------------------
template <class T>
class pm_access {
    RawRef ref_{};
    T*     ptr_ = nullptr;
    explicit pm_access(RawRef r, T* p) : ref_(r), ptr_(p) {}
    template <class U, bool Cross> friend class pm_ptr_impl;
    template <class U> friend class pm_access_proxy;

public:
    // Null access: only produced inside a failed Result; dereferencing asserts.
    pm_access() noexcept = default;

    pm_access(pm_access const&) = delete;
    pm_access& operator=(pm_access const&) = delete;
    pm_access(pm_access&& o) noexcept : ref_(o.ref_), ptr_(o.ptr_) {
        o.ptr_ = nullptr;
    }
    pm_access& operator=(pm_access&&) = delete;
    ~pm_access() {
        if (ptr_) borrow_end(ref_);
    }
    // PM_ASSERT documents the contract: a null access only exists inside a
    // failed Result (see the default constructor above), and dereferencing one
    // is a programming error. In Release the assert compiles out, so the null
    // dereference is deliberate, not redundant.
    // cppcheck-suppress nullPointerRedundantCheck
    T* operator->() const { PM_ASSERT(ptr_); return ptr_; }
    // cppcheck-suppress nullPointerRedundantCheck
    T& operator*()  const { PM_ASSERT(ptr_); return *ptr_; }
    T* get()        const { return ptr_; }
};

// Short-lived proxy: holds the borrow for the duration of one full expression.
template <class T>
class pm_access_proxy {
    pm_access<T> access_;
    explicit pm_access_proxy(pm_access<T>&& a) : access_(std::move(a)) {}
    template <class U, bool Cross> friend class pm_ptr_impl;

public:
    T* operator->() const { return access_.get(); }
};

// ---------------------------------------------------------------------------
// Logical pointer (doc section 5). local (pool_hint = owning pool) and cross
// (pool_hint = CROSS_HINT) share the implementation but not the construction
// path: only pm_as_cross() / pm_cross_ref() produce cross refs, and a local
// pointer can never be bootstrapped from a CROSS_HINT ref (the constructor
// invalidates such refs; task-book 3.4).
//
// BINDING BOUNDARY (round-3 guide 7.2, pinned by R27): a local binding is
// enforced at RESOLUTION time, not at construction time. pm_local_ptr can be
// constructed from any caller-copied RawRef -- that is a deliberate low-level
// capability, and it cannot grant access: a forged concrete pool hint is
// refused with PoolChanged on every borrow/resolve/free, exactly like a
// genuine pointer whose object changed pools. RawRef itself is therefore an
// unsafe, forgeable handle by design; pm_cross_ref(raw) is the documented
// unsafe cross-pool constructor. Nothing beyond the resolution-time checks
// (range, generation, state, pool) protects against forged refs.
//
// Performance model (task-book 5): there is NO address cache. Every
// borrow/resolve performs the full validation (pool range, generation, state,
// pool) plus exactly one descriptor read to fetch the current payload
// address — a single indexed load, so caching would save nothing. The lazy
// update of an object's address after compaction happens through the
// descriptor on the next use, which is the "lazy re-resolution" the
// architecture doc promises.
// ---------------------------------------------------------------------------
template <class T, bool Cross>
class pm_ptr_impl {
    RawRef ref_{};

public:
    pm_ptr_impl() = default;
    explicit pm_ptr_impl(RawRef r) : ref_(r) {
        if constexpr (!Cross) {
            // A local pointer must be bound to a concrete pool. A raw ref
            // carrying CROSS_HINT cannot be trusted as a local binding
            // (it would bypass the pool check at resolution time), so the
            // constructed pointer is invalidated instead.
            if (ref_.pool_hint == CROSS_HINT) ref_.generation = 0;
        }
    }

    bool valid() const { return ref_.generation != 0; }
    RawRef raw() const { return ref_; }  // value copy; callers cannot mutate us
    PoolId pool_hint() const { return ref_.pool_hint; }

    // Safe path: validates, borrows, and reports errors.
    Result<pm_access<T>> try_borrow() const {
        void* addr = nullptr;
        Status st = borrow_begin(ref_, sizeof(T), alignof(T), addr);
        if (st != Status::Ok) return {st, pm_access<T>()};
        return {Status::Ok, pm_access<T>(ref_, static_cast<T*>(addr))};
    }

    // Advanced, non-borrowing peek: same boundary as resolve() -- the
    // pointer is for immediate use in a quiescent window only, never stored
    // or carried across maintenance. Prefer try_borrow()/pm_access.
    Result<T*> peek() const {
        void* addr = nullptr;
        Status st = resolve(ref_, sizeof(T), alignof(T), addr);
        if (st != Status::Ok) return {st, nullptr};
        return {Status::Ok, static_cast<T*>(addr)};
    }

    // C++ syntax sugar; failure is a programming error (Debug asserts,
    // Release yields a null deref). Prefer try_borrow() on cold paths.
    pm_access_proxy<T> operator->() const {
        auto r = try_borrow();
        PM_ASSERT(r.ok());
        return pm_access_proxy<T>(std::move(r.value));
    }

    // Typed view onto a sub-object. The offset is ABSOLUTE from the object
    // start (replace semantics, not cumulative): chaining at() calls keeps
    // addressing relative to the root object (task-book 11). Bounds and
    // alignment are re-checked at resolution time.
    template <class U>
    pm_ptr_impl<U, Cross> at(uint32_t byte_offset) const {
        RawRef r = ref_;
        r.offset = byte_offset;
        return pm_ptr_impl<U, Cross>(r);
    }
};

template <class T> using pm_local_ptr = pm_ptr_impl<T, false>;
template <class T> using pm_cross_ptr = pm_ptr_impl<T, true>;

// Explicit local -> cross conversion (doc section 5).
template <class T>
pm_cross_ptr<T> pm_as_cross(pm_local_ptr<T> const& p) {
    RawRef r = p.raw();
    r.pool_hint = CROSS_HINT;
    return pm_cross_ptr<T>(r);
}

// Explicit cross construction from a raw ref.
template <class T>
pm_cross_ptr<T> pm_cross_ref(RawRef r) {
    r.pool_hint = CROSS_HINT;
    return pm_cross_ptr<T>(r);
}

// ---------------------------------------------------------------------------
// Relocatability trait (task-book 3.5): OPT-IN ONLY.
//
// "Relocatable" is a PROJECT-defined contract: the object may be moved by a
// plain byte-wise memmove and remains logically valid afterwards. It is NOT
// implied by std::is_trivially_copyable — a trivially copyable struct can
// still hold an Auto Zone raw address, a DMA/device register address, a
// self-pointer, an external owner or a sync primitive, none of which survive
// relocation. Types default to NOT relocatable; a project must explicitly
// specialize pm_is_relocatable<T> as true after auditing T.
//
// Note: set_destroy_fn() only supplies destruction for pinned objects; it
// never makes a type safe to relocate. Raw allocations with PM_MOVABLE
// bypass this trait — callers of the low-level alloc() own that audit.
// ---------------------------------------------------------------------------
template <class T>
struct pm_is_relocatable : std::false_type {};

namespace detail {

template <class T>
void destroy_thunk(void* p) {
    static_cast<T*>(p)->~T();
}

} // namespace detail

// Movable make: requires the project relocatability trait.
template <class T, class... Args>
Result<pm_local_ptr<T>> pm_make(PoolId pool, Args&&... args) {
    static_assert(pm_is_relocatable<T>::value,
                  "pm_make requires a trivially relocatable type; "
                  "use pm_make_pinned for non-trivial types");
    if (alignof(T) > PM_MAX_ALIGNMENT) return {Status::InvalidAlignment, {}};
    RawRef r{};
    Status st = alloc(pool, sizeof(T), alignof(T), PM_MOVABLE, 0, r);
    if (st != Status::Ok) return {st, {}};
    T* p = nullptr;
    st = resolve(r, sizeof(T), alignof(T), reinterpret_cast<void*&>(p));
    if (st != Status::Ok) { free(r); return {st, {}}; }
    ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
    return {Status::Ok, pm_local_ptr<T>(r)};
}

// Pinned make: for non-trivial types; a destroy thunk runs the destructor.
template <class T, class... Args>
Result<pm_local_ptr<T>> pm_make_pinned(PoolId pool, Args&&... args) {
    if (alignof(T) > PM_MAX_ALIGNMENT) return {Status::InvalidAlignment, {}};
    uint16_t flags = PM_PINNED;
    RawRef r{};
    Status st = alloc(pool, sizeof(T), alignof(T), flags, 0, r);
    if (st != Status::Ok) return {st, {}};
    st = set_destroy_fn(r, &detail::destroy_thunk<T>);
    if (st != Status::Ok) {
        free(r);
        return {st, {}};
    }
    T* p = nullptr;
    st = resolve(r, sizeof(T), alignof(T), reinterpret_cast<void*&>(p));
    if (st != Status::Ok) { free(r); return {st, {}}; }
    ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
    return {Status::Ok, pm_local_ptr<T>(r)};
}

// Destroys the referenced object. The pointer is cleared ONLY on success:
// on Busy (active borrow), PoolChanged (a local pointer whose object moved
// pools — the object still exists) or any other error the pointer is kept so
// the caller can retry, rebind via pm_as_cross(), or report the error.
template <class T, bool Cross>
Status pm_destroy(pm_ptr_impl<T, Cross>& p) {
    Status st = free(p.raw());
    if (st == Status::Ok) p = pm_ptr_impl<T, Cross>();
    return st;
}

// Typed raw allocation for byte buffers (flags may include PM_ZERO_INIT).
inline Result<pm_local_ptr<uint8_t>> pm_alloc_buffer(PoolId pool, uint32_t size,
                                                     uint16_t flags) {
    if (size == 0) return {Status::InvalidRef, {}};
    RawRef r{};
    Status st = alloc(pool, size, 1, flags, 0, r);
    if (st != Status::Ok) return {st, {}};
    return {Status::Ok, pm_local_ptr<uint8_t>(r)};
}

} // namespace pm
