// PondMerge v1 - ESP32-S3 demo (round-6 requirements doc section 8).
// Scripted, fixed-seed scene emitting the same JSON Lines protocol (v1) as
// the Host demo over USB-Serial-JTAG. No ISR work, no screen, no dynamic
// allocation; the scene is repeatable by reset.
#ifndef PM_DEMO_COMMIT
#define PM_DEMO_COMMIT "unknown"
#endif

#include "pondmerge/pondmerge.hpp"
#include "../../../src/internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr uint32_t ZONE_BYTES = 64 * 1024;
constexpr uint32_t SEGMENT = 4096;

struct Entry {
    bool live;
    uint32_t id;
    pm::RawRef ref;
    uint32_t size;
};
Entry g_entries[32];
uint32_t g_next_id = 1;
uint32_t g_seq = 0;
uint8_t g_zone[ZONE_BYTES] __attribute__((aligned(16)));

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

bool alloc_obj(pm::PoolId pool, uint32_t size, uint16_t flags, uint32_t seed) {
    for (auto& e : g_entries) {
        if (e.live) continue;
        pm::RawRef ref{};
        if (pm::alloc(pool, size, 8, flags, 0, ref) != pm::Status::Ok) return false;
        void* p = nullptr;
        if (pm::borrow_begin(ref, size, 1, p) == pm::Status::Ok) {
            memset(p, (int)(seed & 0xFF), size);
            pm::borrow_end(ref);
        }
        e.live = true; e.id = g_next_id++; e.ref = ref; e.size = size;
        return true;
    }
    return false;
}

bool free_obj(uint32_t id) {
    Entry* e = find_entry(id);
    if (!e) return false;
    if (pm::free(e->ref) != pm::Status::Ok) return false;
    e->live = false;
    return true;
}

// One snapshot: pools (with physical blocks: live/free/slack) + objects +
// per-pool advice. Same field set as the Host demo (protocol v1).
void emit_snapshot() {
    using namespace pm::internal;
    GlobalState& G = g();
    printf("{\"t\":\"snapshot\",\"protocol\":1,\"seq\":%u,\"source\":\"ESP32\","
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

        struct Blk { uint32_t off, size, kind, id, gen, epoch; };
        static Blk blk[2 * PM_MAX_OBJECTS + 8];
        uint32_t n = 0;
        for (uint32_t idx = P.order_head; idx != NO_ORDER;
             idx = G.objects[idx].addr_next) {
            ObjectDesc const& d = G.objects[idx];
            blk[n].off = (uint32_t)((uint8_t*)d.address - G.zone) - BLOCK_HEADER_SIZE;
            blk[n].size = d.block_size;
            blk[n].kind = (d.flags & (pm::PM_PINNED | pm::PM_DMA | pm::PM_EXTERNAL)) ? 1 : 0;
            blk[n].id = 0; blk[n].gen = d.generation; blk[n].epoch = d.address_epoch;
            for (auto const& e : g_entries)
                if (e.live && e.ref.index == idx) { blk[n].id = e.id; break; }
            ++n;
        }
        for (uint32_t f = 0; f < FL_COUNT; ++f)
            for (uint32_t sl = 0; sl < SL_COUNT; ++sl)
                for (uint32_t off = P.bins.head[f][sl]; off != NULL_OFF;
                     off = ptr_of(off)->next) {
                    blk[n].off = off;
                    blk[n].size = ptr_of(off)->header & ~BLOCK_FREE_BIT;
                    blk[n].kind = 2; blk[n].id = 0; blk[n].gen = 0; blk[n].epoch = 0;
                    ++n;
                }
        for (uint32_t i = 1; i < n; ++i) {
            Blk k = blk[i];
            uint32_t j = i;
            while (j > 0 && blk[j - 1].off > k.off) { blk[j] = blk[j - 1]; --j; }
            blk[j] = k;
        }
        uint8_t* base = seg_base(P.segment_first);
        uint8_t* pend = seg_base((uint32_t)P.segment_first + P.segment_count);
        uint32_t cursor = (uint32_t)(base - G.zone);
        bool first_blk = true;
        auto emit_block = [&](uint32_t off, uint32_t size, uint32_t kind,
                              uint32_t id, uint32_t gen, uint32_t epoch) {
            printf("%s{\"offset\":%u,\"size\":%u,\"kind\":\"%s\"",
                   first_blk ? "" : ",", (unsigned)off, (unsigned)size,
                   kind == 0 ? "MOVABLE" : kind == 1 ? "PINNED" : kind == 2 ? "FREE" : "SLACK");
            if (kind <= 1)
                printf(",\"object_id\":%u,\"generation\":%u,\"address_epoch\":%u",
                       (unsigned)id, (unsigned)gen, (unsigned)epoch);
            printf("}");
            first_blk = false;
        };
        for (uint32_t i = 0; i < n; ++i) {
            if (blk[i].off > cursor)
                emit_block(cursor, blk[i].off - cursor, 3, 0, 0, 0);
            emit_block(blk[i].off, blk[i].size, blk[i].kind, blk[i].id,
                       blk[i].gen, blk[i].epoch);
            cursor = blk[i].off + blk[i].size;
        }
        if (cursor < (uint32_t)(pend - G.zone))
            emit_block(cursor, (uint32_t)(pend - G.zone) - cursor, 3, 0, 0, 0);
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
               "\"address_epoch\":%u,\"flags\":%u,\"payload_digest\":\"%08x\"}",
               first_obj ? "" : ",", (unsigned)e.id, (unsigned)d.generation,
               (unsigned)d.pool_id, (unsigned)((uint8_t*)d.address - G.zone),
               (unsigned)d.size, (unsigned)d.block_size,
               (unsigned)d.address_epoch, (unsigned)d.flags, (unsigned)digest);
        first_obj = false;
    }
    printf("],\"advice\":[");
    bool first_adv = true;
    for (uint32_t pi = 0; pi < PM_MAX_POOLS; ++pi) {
        if (G.pools[pi].state == PoolState::Empty) continue;
        pm::CompactionAdvice a = pm::analyze_compaction((pm::PoolId)pi);
        printf("%s{\"pool_id\":%u,\"verdict\":%d,\"borrow_count\":%u,"
               "\"has_pinned_objects\":%u,\"external_quiescence_required\":%u}",
               first_adv ? "" : ",", (unsigned)pi, (int)a.verdict,
               (unsigned)a.borrow_count, (unsigned)a.has_pinned_objects,
               (unsigned)a.external_quiescence_required);
        first_adv = false;
    }
    printf("]}\n");
}

void emit_result(char const* op, pm::Status st) {
    printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"%s\",\"status\":\"%s\"}\n",
           op, pm::status_name(st));
}

void emit_advice(pm::PoolId pool, uint32_t size) {
    pm::CompactionRequest req{size, 8, 0, 0};
    pm::CompactionAdvice a = pm::analyze_compaction(pool, size ? &req : nullptr);
    printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"advice\",\"status\":\"OK\","
           "\"pool_id\":%u,\"verdict\":%d,\"request_can_fit_now\":%u,"
           "\"request_can_fit_after_compaction_estimate\":%u,"
           "\"external_quiescence_required\":%u}\n",
           (unsigned)pool, (int)a.verdict, (unsigned)a.request_can_fit_now,
           (unsigned)a.request_can_fit_after_compaction_estimate,
           (unsigned)a.external_quiescence_required);
}

} // namespace

extern "C" void app_main(void);
void pondmerge_run_demo_scene(void);

void pondmerge_run_demo_scene(void) {
    // fixed-seed scene: 2 pools, seeded objects, fragmentation, advice,
    // compact, merge, split -- every step emits a snapshot or a result.
    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) {
        printf("{\"t\":\"info\",\"protocol\":1,\"event\":\"error\","
               "\"detail\":\"init failed\"}\n");
        return;
    }
    pm::PoolId p0{}, p1{};
    pm::create_pool(p0, 6);
    pm::create_pool(p1, 4);
    uint32_t const seeds0[4] = {1200, 2400, 800, 1600};
    for (uint32_t i = 0; i < 4; ++i) alloc_obj(p0, seeds0[i], 0, 0xA0 + i);
    alloc_obj(p1, 900, 0, 0xB0);
    emit_snapshot();

    // fragment: free every second object of pool 0, refill half-size so the
    // remainder holes stay stranded.
    free_obj(2);
    free_obj(4);
    alloc_obj(p0, 400, 0, 0xC0);
    emit_result("fragment", pm::Status::Ok);
    emit_snapshot();

    emit_advice(p0, 2000);   // targeted: needs compaction
    emit_advice(p0, 0);      // generic

    printf("{\"t\":\"result\",\"protocol\":1,\"op\":\"compact_before\","
           "\"status\":\"OK\",\"objects\":[");  // before-state object digests
                              // snapshot; the UI diffs those automatically
    bool first = true;
    for (auto const& e : g_entries) {
        if (!e.live) continue;
        void* p = nullptr;
        uint32_t digest = 0;
        pm::RawRef x = e.ref;
        x.pool_hint = pm::CROSS_HINT;
        if (pm::borrow_begin(x, e.size, 1, p) == pm::Status::Ok) {
            digest = fnv1a(static_cast<uint8_t const*>(p), e.size);
            pm::borrow_end(x);
        }
        printf("%s{\"object_id\":%u,\"pool_id\":%u,\"address_offset\":%u,"
               "\"address_epoch\":%u,\"payload_digest\":\"%08x\"}",
               first ? "" : ",", (unsigned)e.id,
               (unsigned)pm::internal::g().objects[e.ref.index].pool_id,
               (unsigned)((uint8_t*)pm::internal::g().objects[e.ref.index].address - pm::internal::g().zone),
               (unsigned)pm::internal::g().objects[e.ref.index].address_epoch,
               (unsigned)digest);
        first = false;
    }
    printf("]}\n");
    emit_result("compact", pm::compact(p0));
    emit_snapshot();

    emit_result("merge", pm::merge(p1, p0));
    emit_snapshot();
    pm::PoolId nid{};
    emit_result("split", pm::split(p0, 2, nid));
    emit_snapshot();
    emit_advice(p0, 0);

    printf("{\"t\":\"info\",\"protocol\":1,\"event\":\"done\",\"detail\":\"scene "
           "complete\"}\n");
}

extern "C" void app_main(void) {
    printf("{\"t\":\"info\",\"protocol\":1,\"event\":\"ready\","
           "\"source\":\"ESP32\",\"commit\":\"" PM_DEMO_COMMIT "\"}\n");
    pondmerge_run_demo_scene();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
