// PondMerge v1 - public API.
//
// A managed-memory system for MMU-less MCUs: a fixed Auto Zone is split into
// pools; objects are referenced by stable logical ids; physical addresses are
// only handed out through RAII borrows; compaction/merge/split are explicit,
// caller-triggered operations.
//
// C++17 subset: no exceptions, no RTTI, no dynamic allocation.
//
// CONCURRENCY CONTRACT (task-book section 4): v1 is "single owner + quiescent
// maintenance window".
//   * Ordinary alloc/access/free run in ONE owner execution context (thread/
//     task). PondMerge does not make them safe for concurrent callers.
//   * borrow_begin/borrow_end and the pool state flips (pause/resume/compact
//     entry) are internally synchronized, so pausing can never race a new
//     borrow into existence.
//   * Before compact/merge/split, the owner pauses the pool(s); all active
//     borrows must have ended (borrow counters zero). DMA, ISRs and other
//     threads holding raw pointers must be stopped and drained by the CALLER
//     beforehand — PondMerge cannot discover external holders.
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
Status alloc(PoolId pool, uint32_t size, uint32_t alignment, uint16_t flags,
             uint32_t user_tag, RawRef& out);
Status free(RawRef const& ref);
// Sets destroy_fn for a live object (used for pinned non-trivial types).
Status set_destroy_fn(RawRef const& ref, void (*destroy_fn)(void*));

// --- borrow accounting ------------------------------------------------------
// Full validation + borrow. `access_size`/`access_align` describe the sub-object
// being touched (sizeof(T)/alignof(T) plus the ref offset).
Status borrow_begin(RawRef const& ref, uint32_t access_size, uint32_t access_align,
                    void*& out_addr);
void   borrow_end(RawRef const& ref);
// Validation only, no borrow, no address caching guarantee: safe inside a
// critical section or when the pool is quiescent.
Status resolve(RawRef const& ref, uint32_t access_size, uint32_t access_align,
               void*& out_addr);

// --- maintenance (doc sections 8, 10, 11) -----------------------------------
Status compact(PoolId pool);
Status merge(PoolId source, PoolId target);
// Splits `source` after `new_pool_segments` segments; the new pool owns the
// upper range. Crossing movable objects are relocated; crossing pinned objects
// fail with PinnedConflict before anything changes.
Status split(PoolId source, uint32_t new_pool_segments, PoolId& out_new);

// Debug hook used by PM_ASSERT.
void pm_debug_abort(const char* file, int line);

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
    T* operator->() const { PM_ASSERT(ptr_); return ptr_; }
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
// Performance model (task-book 5): there is NO address cache. Every
// borrow/resolve performs the full validation (range, generation, state,
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

    // Validation without a borrow (quiescent use only).
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
