// PondMerge v1 - Host demo process (round-6 requirements doc section 7).
//
// Reads one JSON command per stdin line, drives the PUBLIC PondMerge API,
// and answers with one JSON record per stdout line (protocol v1, see
// docs/DEMO_REQUIREMENTS.md). The UI/HTTP layer lives in demo_server.py;
// this process knows nothing about browsers or sockets.
//
// Block-level layout in snapshots is read through src/internal.h (white-box):
// the public API deliberately does not expose free-list internals, but a
// diagnostic demo needs them. The demo never writes allocator metadata.
//
// Build: see QUICKSTART.md. No dynamic allocation, no exceptions.
#include "pondmerge/pondmerge.hpp"
#include "../src/internal.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#ifndef PM_DEMO_COMMIT
#define PM_DEMO_COMMIT "unknown"
#endif

namespace {

constexpr uint32_t ZONE_BYTES = 64 * 1024;
constexpr uint32_t SEGMENT = 4096;
constexpr uint32_t POOL0_SEGS = 6;
constexpr uint32_t POOL1_SEGS = 4;

struct Entry {
    bool live;
    uint32_t id;        // stable demo object id (never reused)
    pm::RawRef ref;
    uint32_t size;
    uint32_t seed;
};
Entry g_entries[64];
uint32_t g_next_id = 1;
pm::PoolId g_pools[2];
uint8_t g_zone[ZONE_BYTES] __attribute__((aligned(16)));
uint32_t g_seq = 0;

// ---------- tiny JSON helpers (numbers/enums only: no escaping needed) ------

void emit_info(char const* event, char const* detail) {
    printf("{\"t\":\"info\",\"protocol\":1,\"event\":\"%s\",\"detail\":\"%s\","
           "\"commit\":\"" PM_DEMO_COMMIT "\"}\n",
           event, detail);
}

uint32_t fnv1a(uint8_t const* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

Entry* find_entry(uint32_t id) {
    for (auto& e : g_entries)
        if (e.live && e.id == id) return &e;
    return nullptr;
}

char const* flags_json(uint16_t flags) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)flags);
    return buf;
}

// One snapshot record per call: pools with their physical block layout
// (live + free + slack), objects with stable ids and payload digests, and
// per-pool compaction advice.
void emit_snapshot() {
    using namespace pm::internal;
    GlobalState& G = g();
    printf("{\"t\":\"snapshot\",\"protocol\":1,\"seq\":%u,\"source\":\"HOST\","
           "\"zone_size\":%u,\"segment_size\":%u,\"pools\":[",
           (unsigned)++g_seq, (unsigned)G.zone_size, (unsigned)G.segment_size);
    bool first_pool = true;
    for (uint32_t pi = 0; pi < PM_MAX_POOLS; ++pi) {
        Pool const& P = G.pools[pi];
        if (P.state == PoolState::Empty) continue;
        printf("%s{\"pool_id\":%u,\"state\":%u,\"segment_first\":%u,"
               "\"segment_count\":%u,\"capacity\":%u,\"used_bytes\":%u,"
               "\"free_bytes\":%u,\"largest_free_block\":%u,"
               "\"fragment_bytes\":%u,\"live_objects\":%u,\"borrow_count\":%u,"
               "\"structure_epoch\":%u,\"blocks\":[",
               first_pool ? "" : ",", (unsigned)pi, (unsigned)P.state,
               (unsigned)P.segment_first, (unsigned)P.segment_count,
               (unsigned)((uint32_t)P.segment_count * G.segment_size),
               (unsigned)P.used_bytes, (unsigned)P.free_bytes,
               (unsigned)pm::get_stats((pm::PoolId)pi).largest_free_block,
               (unsigned)P.fragment_bytes, (unsigned)P.live_objects,
               (unsigned)P.borrow_count, (unsigned)P.structure_epoch);
        first_pool = false;

        // Layout = live blocks (order list, address-sorted) merged with the
        // free blocks (bins); everything between is slack.
        struct Blk { uint32_t off, size, kind, id, flags, gen, epoch; };
        static Blk blk[2 * PM_MAX_OBJECTS + 8]; // demo-only scratch
        uint32_t n = 0;
        for (uint32_t idx = P.order_head; idx != NO_ORDER;
             idx = G.objects[idx].addr_next) {
            ObjectDesc const& d = G.objects[idx];
            blk[n].off = (uint32_t)((uint8_t*)d.address - G.zone) - BLOCK_HEADER_SIZE;
            blk[n].size = d.block_size;
            blk[n].kind = (d.flags & (pm::PM_PINNED | pm::PM_DMA | pm::PM_EXTERNAL)) ? 1 : 0;
            blk[n].id = 0; blk[n].flags = d.flags; blk[n].gen = d.generation;
            blk[n].epoch = d.address_epoch;
            for (auto const& e : g_entries)
                if (e.live && e.ref.index == idx) { blk[n].id = e.id; break; }
            ++n;
        }
        for (uint32_t f = 0; f < FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                for (uint32_t off = P.bins.head[f][sl]; off != NULL_OFF;
                     off = ptr_of(off)->next) {
                    blk[n].off = off; blk[n].size = pm::internal::ptr_of(off)->header & ~pm::internal::BLOCK_FREE_BIT;
                    blk[n].kind = 2; blk[n].id = 0; blk[n].flags = 0;
                    blk[n].gen = 0; blk[n].epoch = 0;
                    ++n;
                }
        // insertion sort by offset (small n)
        for (uint32_t i = 1; i < n; ++i) {
            Blk k = blk[i];
            uint32_t j = i;
            while (j > 0 && blk[j - 1].off > k.off) { blk[j] = blk[j - 1]; --j; }
            blk[j] = k;
        }
        uint32_t cursor = (uint32_t)((uint8_t*)seg_base(P.segment_first) - G.zone);
        bool first_blk = true;
        auto emit_block = [&](uint32_t off, uint32_t size, uint32_t kind,
                              uint32_t id, uint32_t flags, uint32_t gen,
                              uint32_t epoch) {
            printf("%s{\"offset\":%u,\"size\":%u,\"kind\":\"%s\"", first_blk ? "" : ",",
                   (unsigned)off, (unsigned)size,
                   kind == 0 ? "MOVABLE" : kind == 1 ? "PINNED" : kind == 2 ? "FREE" : "SLACK");
            if (kind <= 1)
                printf(",\"object_id\":%u,\"flags\":%u,\"generation\":%u,"
                       "\"address_epoch\":%u",
                       (unsigned)id, (unsigned)flags, (unsigned)gen, (unsigned)epoch);
            printf("}");
            first_blk = false;
        };
        for (uint32_t i = 0; i < n; ++i) {
            if (blk[i].off > cursor) // gap = slack run
                emit_block(cursor, blk[i].off - cursor, 3, 0, 0, 0, 0);
            emit_block(blk[i].off, blk[i].size, blk[i].kind, blk[i].id,
                       blk[i].flags, blk[i].gen, blk[i].epoch);
            cursor = blk[i].off + blk[i].size;
        }
        if (cursor < (uint32_t)((uint8_t*)seg_base(P.segment_first + P.segment_count) - G.zone))
            emit_block(cursor,
                       (uint32_t)((uint8_t*)seg_base(P.segment_first + P.segment_count) - G.zone) - cursor,
                       3, 0, 0, 0, 0);
        printf("]}");
    }
    printf("],\"objects\":[");
    bool first_obj = true;
    for (auto const& e : g_entries) {
        if (!e.live) continue;
        ObjectDesc const& d = G.objects[e.ref.index];
        void* p = nullptr;
        uint32_t digest = 0;
        pm::RawRef x = e.ref;
        x.pool_hint = pm::CROSS_HINT; // objects may have moved pools (merge/split)
        if (pm::borrow_begin(x, e.size, 1, p) == pm::Status::Ok) {
            digest = fnv1a(static_cast<uint8_t const*>(p), e.size);
            pm::borrow_end(x);
        }
        printf("%s{\"object_id\":%u,\"generation\":%u,\"pool_id\":%u,"
               "\"address_offset\":%u,\"size\":%u,\"block_size\":%u,"
               "\"address_epoch\":%u,\"flags\":%s,\"payload_digest\":\"%08x\"}",
               first_obj ? "" : ",", (unsigned)e.id, (unsigned)d.generation,
               (unsigned)d.pool_id,
               (unsigned)((uint8_t*)d.address - G.zone), (unsigned)d.size,
               (unsigned)d.block_size, (unsigned)d.address_epoch,
               flags_json(d.flags), (unsigned)digest);
        first_obj = false;
    }
    printf("],\"advice\":[");
    bool first_adv = true;
    for (uint32_t pi = 0; pi < PM_MAX_POOLS; ++pi) {
        if (G.pools[pi].state == PoolState::Empty) continue;
        pm::CompactionAdvice a = pm::analyze_compaction((pm::PoolId)pi);
        printf("%s{\"pool_id\":%u,\"verdict\":%d,\"fragment_ratio_permille\":%u,"
               "\"borrow_count\":%u,\"has_pinned_objects\":%u,"
               "\"external_quiescence_required\":%u,"
               "\"estimated_moved_bytes\":%u}",
               first_adv ? "" : ",", (unsigned)pi, (int)a.verdict,
               (unsigned)a.fragment_ratio_permille, (unsigned)a.borrow_count,
               (unsigned)a.has_pinned_objects,
               (unsigned)a.external_quiescence_required,
               (unsigned)a.estimated_moved_bytes);
        first_adv = false;
    }
    printf("]}\n");
    fflush(stdout);
}

void emit_result(char const* op, pm::Status st) {
    printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"%s\",\"status\":\"%s\"}\n",
           op, pm::status_name(st));
    fflush(stdout);
    emit_snapshot();
}

// ---------- scenario helpers -------------------------------------------------
void do_reset() {
    for (auto& e : g_entries)
        if (e.live) { pm::free(e.ref); e.live = false; }
    memset(g_entries, 0, sizeof(g_entries));
    g_next_id = 1;
    g_seq = 0;
    if (pm::deinit() != pm::Status::Ok) { /* first run: not initialized */ }
    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) { emit_info("error", "init failed"); return; }
    pm::create_pool(g_pools[0], POOL0_SEGS);
    pm::create_pool(g_pools[1], POOL1_SEGS);
    // Seed scene: 4 movable objects in pool 0, 1 in pool 1.
    uint32_t const sizes0[4] = {1200, 2400, 800, 1600};
    for (uint32_t i = 0; i < 4; ++i) {
        Entry& e = g_entries[i];
        e.live = true; e.id = g_next_id++; e.size = sizes0[i]; e.seed = 100 + i;
        pm::alloc(g_pools[0], e.size, 8, 0, e.id, e.ref);
        void* p = nullptr;
        if (pm::borrow_begin(e.ref, e.size, 1, p) == pm::Status::Ok) {
            memset(p, (int)e.seed, e.size);
            pm::borrow_end(e.ref);
        }
    }
    Entry& e = g_entries[4];
    e.live = true; e.id = g_next_id++; e.size = 900; e.seed = 200;
    pm::alloc(g_pools[1], e.size, 8, 0, e.id, e.ref);
    void* p = nullptr;
    if (pm::borrow_begin(e.ref, e.size, 1, p) == pm::Status::Ok) {
        memset(p, (int)e.seed, e.size);
        pm::borrow_end(e.ref);
    }
    emit_info("reset", "2 pools, 5 objects");
    emit_snapshot();
}

void do_alloc(uint32_t pool_id, uint32_t size, uint16_t flags) {
    Entry* slot = nullptr;
    for (auto& e : g_entries)
        if (!e.live) { slot = &e; break; }
    if (!slot) { emit_result("alloc", pm::Status::NoSpace); return; }
    pm::RawRef ref{};
    pm::Status st = pm::alloc((pm::PoolId)pool_id, size, 8, flags, 0, ref);
    if (st == pm::Status::Ok) {
        slot->live = true; slot->id = g_next_id++; slot->ref = ref;
        slot->size = size; slot->seed = 1 + (g_next_id & 0xFF);
        void* p = nullptr;
        if (pm::borrow_begin(ref, size, 1, p) == pm::Status::Ok) {
            memset(p, (int)slot->seed, size);
            pm::borrow_end(ref);
        }
        printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"alloc\","
               "\"status\":\"OK\",\"object_id\":%u}\n", (unsigned)slot->id);
        fflush(stdout);
        emit_snapshot();
    } else {
        emit_result("alloc", st);
    }
}

void do_free(uint32_t id) {
    Entry* e = find_entry(id);
    if (!e) { emit_result("free", pm::Status::InvalidRef); return; }
    pm::Status st = pm::free(e->ref);
    if (st == pm::Status::Ok) e->live = false;
    emit_result("free", st);
}

// Classic fragmentation maker: free every second live object of the lowest
// pool, then allocate one small object so the freed holes stay stranded.
void do_fragment() {
    uint32_t lowest = 0xFFFF, freed = 0, pos = 0;
    for (auto const& e : g_entries)
        if (e.live && e.ref.pool_hint < lowest) lowest = e.ref.pool_hint;
    for (auto& e : g_entries) {
        if (e.live && e.ref.pool_hint == lowest) {
            if ((pos & 1) == 0 && pm::free(e.ref) == pm::Status::Ok) {
                e.live = false;
                ++freed;
            }
            ++pos;
        }
    }
    if (freed != 0) do_alloc(lowest, 400, 0);
    emit_result("fragment", pm::Status::Ok);
}

void do_advice(uint32_t pool_id, uint32_t size, uint32_t align) {
    pm::CompactionRequest req{size, align, 0, 0};
    pm::CompactionAdvice a =
        pm::analyze_compaction((pm::PoolId)pool_id, size ? &req : nullptr);
    printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"advice\",\"status\":\"OK\","
           "\"pool_id\":%u,\"verdict\":%d,\"capacity\":%u,\"used_bytes\":%u,"
           "\"free_bytes\":%u,\"largest_free_block\":%u,\"fragment_bytes\":%u,"
           "\"fragment_ratio_permille\":%u,\"live_objects\":%u,"
           "\"borrow_count\":%u,\"has_pinned_objects\":%u,"
           "\"request_can_fit_now\":%u,"
           "\"request_can_fit_after_compaction_estimate\":%u,"
           "\"external_quiescence_required\":%u,"
           "\"estimated_moved_bytes\":%u}\n",
           (unsigned)pool_id, (int)a.verdict, (unsigned)a.capacity,
           (unsigned)a.used_bytes, (unsigned)a.free_bytes,
           (unsigned)a.largest_free_block, (unsigned)a.fragment_bytes,
           (unsigned)a.fragment_ratio_permille, (unsigned)a.live_objects,
           (unsigned)a.borrow_count, (unsigned)a.has_pinned_objects,
           (unsigned)a.request_can_fit_now,
           (unsigned)a.request_can_fit_after_compaction_estimate,
           (unsigned)a.external_quiescence_required,
           (unsigned)a.estimated_moved_bytes);
    fflush(stdout);
    emit_snapshot();
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    emit_info("ready", "host demo process");
    do_reset();

    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        uint32_t id = 0, size = 0, align = 0, pool_idx = 0;
        uint32_t source = 0, target = 1, segments = 2, ratio = 0, minb = 0;
        if (strstr(line, "\"reset\"")) { do_reset(); continue; }
        if (strstr(line, "\"alloc\"")) {
            unsigned uflags = 0;
            sscanf(line,
                   "{\"cmd\":\"alloc\",\"pool\":%u,\"size\":%u,\"align\":%u,"
                   "\"flags\":%u}", &pool_idx, &size, &align, &uflags);
            do_alloc(pool_idx, size, (uint16_t)uflags);
            continue;
        }
        if (strstr(line, "\"free\"")) {
            sscanf(line, "{\"cmd\":\"free\",\"id\":%u}", &id);
            do_free(id);
            continue;
        }
        if (strstr(line, "\"fragment\"")) { do_fragment(); continue; }
        if (strstr(line, "\"advice\"")) {
            sscanf(line, "{\"cmd\":\"advice\",\"pool\":%u,\"size\":%u,\"align\":%u}",
                   &pool_idx, &size, &align);
            do_advice(pool_idx, size, align);
            continue;
        }
        if (strstr(line, "\"compact\"")) {
            sscanf(line, "{\"cmd\":\"compact\",\"pool\":%u}", &pool_idx);
            emit_result("compact", pm::compact((pm::PoolId)pool_idx));
            continue;
        }
        if (strstr(line, "\"merge\"")) {
            sscanf(line, "{\"cmd\":\"merge\",\"source\":%u,\"target\":%u}",
                   &source, &target);
            emit_result("merge",
                        pm::merge((pm::PoolId)source, (pm::PoolId)target));
            continue;
        }
        if (strstr(line, "\"split\"")) {
            sscanf(line, "{\"cmd\":\"split\",\"source\":%u,\"segments\":%u}",
                   &source, &segments);
            pm::PoolId nid{};
            emit_result("split", pm::split((pm::PoolId)source, segments, nid));
            continue;
        }
        if (strstr(line, "\"thresholds\"")) {
            int n = sscanf(line,
                           "{\"cmd\":\"thresholds\",\"ratio\":%u,\"min\":%u}",
                           &ratio, &minb);
            if (n == 2) pm::set_compaction_thresholds({ratio, minb});
            pm::CompactionThresholds t = pm::get_compaction_thresholds();
            printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"thresholds\","
                   "\"status\":\"OK\",\"fragment_ratio_permille\":%u,"
                   "\"fragment_min_bytes\":%u}\n",
                   (unsigned)t.fragment_ratio_permille,
                   (unsigned)t.fragment_min_bytes);
            fflush(stdout);
            continue;
        }
        if (strstr(line, "\"quit\"")) break;
        emit_info("error", "unknown command");
    }
    emit_info("bye", "");
    return 0;
}
