// PondMerge v1 - compile-time configuration
// All limits are compile-time constants; metadata lives in static storage,
// never inside the Auto Zone.
#pragma once

#ifndef PM_MAX_POOLS
#define PM_MAX_POOLS 16
#endif

#ifndef PM_MAX_OBJECTS
#define PM_MAX_OBJECTS 1024
#endif

#ifndef PM_MAX_SEGMENTS
#define PM_MAX_SEGMENTS 64
#endif

// v1 alignment contract: 8 bytes everywhere, larger requests fail.
#ifndef PM_ALIGNMENT
#define PM_ALIGNMENT 8
#endif

#ifndef PM_MAX_ALIGNMENT
#define PM_MAX_ALIGNMENT 8
#endif

// Minimum block: 8 byte header + 8 byte payload (two free-list pointers).
#ifndef PM_MIN_BLOCK
#define PM_MIN_BLOCK 16
#endif

// TLSF second level count (SLI = 2 -> 4 bins per first level).
#ifndef PM_SL_COUNT
#define PM_SL_COUNT 4
#endif

// First-level index ceiling: block sizes up to 2^(PM_FL_MAX-1).
#ifndef PM_FL_MAX
#define PM_FL_MAX 24
#endif

#ifndef PM_DEBUG
#define PM_DEBUG 1
#endif

// internal.h defines MIN_FL = 4; keep the FL contract checkable here without
// pulling internal headers into a public file.
#ifndef MIN_FL_HINT
#define MIN_FL_HINT 4
#endif

// Compile-time configuration contract (task-book section 7).
static_assert(PM_ALIGNMENT == 8, "v1 block format assumes 8-byte alignment");
static_assert((PM_MIN_BLOCK & (PM_MIN_BLOCK - 1)) == 0 && PM_MIN_BLOCK >= 16,
              "PM_MIN_BLOCK must be a power of two >= 16 (header + 2 links)");
static_assert((PM_SL_COUNT & (PM_SL_COUNT - 1)) == 0 && PM_SL_COUNT >= 2 &&
                  PM_SL_COUNT <= 16,
              "PM_SL_COUNT must be a power of two in [2, 16]: the second-level "
              "bitmap is uint16_t, so 32 would silently truncate (task-book v2 7.1)");
static_assert(PM_MAX_OBJECTS < 0xFFFF,
              "descriptor slot indices and NO_SLOT share a 16-bit space");
static_assert(PM_FL_MAX > MIN_FL_HINT && PM_FL_MAX <= 31,
              "1u << PM_FL_MAX must stay representable and FL needs a range");
static_assert(PM_MAX_SEGMENTS <= 0xFFFF,
              "Pool segment fields are 16-bit (task-book v2 7.2)");
static_assert(PM_MAX_POOLS >= 1 && PM_MAX_SEGMENTS >= 1, "degenerate limits");
static_assert(PM_FL_MAX > 4, "no first-level range to search");

#if PM_DEBUG
#define PM_ASSERT(x) do { if (!(x)) pm_debug_abort(__FILE__, __LINE__); } while (0)
#else
#define PM_ASSERT(x) ((void)0)
#endif
