// FL capacity contract (task-book v2 section 7.2, test-plan item 11.1.7).
//
// fl_index() saturates at PM_FL_MAX-1, so a zone whose largest possible block
// reaches 2^PM_FL_MAX cannot be represented by the bins. init() must refuse
// such a configuration instead of silently clamping large blocks into the top
// bin -- and a zone just below the ceiling must be accepted AND usable.
//
// One binary serves both expectations:
//   config_limits refuse   -> declared zone at the FL ceiling: NO_SPACE
//   config_limits accept   -> declared zone below the ceiling: OK + usable
#include "pondmerge/pondmerge.hpp"

#include <cstdio>
#include <cstring>

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

// Keeps the segment count legal (<= PM_MAX_SEGMENTS) for every PM_FL_MAX the
// configuration matrix builds.
static uint32_t pick_segment(uint32_t zone_bytes) {
    uint32_t seg = zone_bytes / 64u;
    if (seg < 4096u) seg = 4096u;
    return seg;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const bool refuse = strcmp(argv[1], "refuse") == 0;
    // init() validates the CONFIGURATION, not the caller's backing store, so
    // declaring a zone larger than `zone` is a legitimate configuration test.
    const uint32_t zone_bytes =
        refuse ? (1u << PM_FL_MAX) : ((1u << PM_FL_MAX) / 2u);
    pm::Config cfg{zone, zone_bytes, pick_segment(zone_bytes)};

    pm::Status st = pm::init(cfg);
    if (refuse) {
        if (st != pm::Status::NoSpace) {
            printf("FAIL: zone at the FL ceiling accepted (%s)\n", pm::status_name(st));
            return 1;
        }
        printf("FL ceiling refused: zone=%u FL_MAX=%d -> NO_SPACE\n",
               (unsigned)zone_bytes, PM_FL_MAX);
        return 0;
    }
    if (st != pm::Status::Ok) {
        printf("FAIL: zone below the FL ceiling refused (%s)\n", pm::status_name(st));
        return 1;
    }
    // Accepted must mean usable, not merely tolerated.
    pm::PoolId p{};
    if (pm::create_pool(p, 8) != pm::Status::Ok) return 1;
    pm::RawRef r{};
    if (pm::alloc(p, 1000, 8, 0, 1, r) != pm::Status::Ok) return 1;
    if (pm::validate(p) != pm::Status::Ok) return 1;
    if (pm::free(r) != pm::Status::Ok) return 1;
    if (pm::deinit() != pm::Status::Ok) return 1;
    printf("FL ceiling accepted and usable: zone=%u FL_MAX=%d\n",
           (unsigned)zone_bytes, PM_FL_MAX);
    return 0;
}
