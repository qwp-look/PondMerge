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

#if PM_DEBUG
#define PM_ASSERT(x) do { if (!(x)) pm_debug_abort(__FILE__, __LINE__); } while (0)
#else
#define PM_ASSERT(x) ((void)0)
#endif
