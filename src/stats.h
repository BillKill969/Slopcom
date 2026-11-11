#ifndef SLOPCOM_STATS_H
#define SLOPCOM_STATS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define STATS_SAMPLE_CAP 2048

typedef struct {
    uint64_t samples[STATS_SAMPLE_CAP];
    size_t count;
    uint64_t total_samples;
} TimingBucket;

typedef struct Stats {
    uint64_t x_events, damage_events;
    uint64_t render_attempts, successful_swaps;
    uint64_t full_repaints, partial_repaints;
    uint64_t skipped_presentations;
    uint64_t skipped_occluded_damage;
    uint64_t poll_wakeups;
    uint64_t imports, reimports, x_round_trips;
    uint64_t live_pixmaps, live_images, live_textures;
    uint64_t live_damage, live_regions;

    TimingBucket event_reconcile_ns;
    TimingBucket draw_ns;
    TimingBucket swap_ns;
} Stats;

void stats_init(Stats *s);
bool stats_monotonic_ns(uint64_t *out);

void stats_record_event_reconcile(Stats *s, uint64_t duration_ns);
void stats_record_draw(Stats *s, uint64_t duration_ns);
void stats_record_swap(Stats *s, uint64_t duration_ns);

void stats_dump(const Stats *s, const char *kind, bool detailed, int fd);
void stats_dump_human(const Stats *s, int exit_status, int fd);

#endif /* SLOPCOM_STATS_H */
