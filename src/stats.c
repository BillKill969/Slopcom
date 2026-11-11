#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>

#include "stats.h"

static uint64_t s_stats_seq = 0;

void stats_init(Stats *s) {
    memset(s, 0, sizeof(*s));
}

bool stats_monotonic_ns(uint64_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return false;
    *out = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return true;
}

static void record_timing(TimingBucket *b, uint64_t duration_ns) {
    if (b->count < STATS_SAMPLE_CAP) {
        b->samples[b->count++] = duration_ns;
    } else {
        b->samples[b->total_samples % STATS_SAMPLE_CAP] = duration_ns;
    }
    b->total_samples++;
}

void stats_record_event_reconcile(Stats *s, uint64_t duration_ns) {
    record_timing(&s->event_reconcile_ns, duration_ns);
}

void stats_record_draw(Stats *s, uint64_t duration_ns) {
    record_timing(&s->draw_ns, duration_ns);
}

void stats_record_swap(Stats *s, uint64_t duration_ns) {
    record_timing(&s->swap_ns, duration_ns);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void calc_percentiles(const TimingBucket *b, uint64_t *out_p50, uint64_t *out_p95, uint64_t *out_p99) {
    if (b->count == 0) {
        *out_p50 = *out_p95 = *out_p99 = 0;
        return;
    }
    uint64_t sorted[STATS_SAMPLE_CAP];
    memcpy(sorted, b->samples, b->count * sizeof(uint64_t));
    qsort(sorted, b->count, sizeof(uint64_t), cmp_u64);
    *out_p50 = sorted[b->count * 50 / 100];
    *out_p95 = sorted[b->count * 95 / 100];
    *out_p99 = sorted[(b->count >= 100) ? (b->count * 99 / 100) : (b->count - 1)];
}

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n > 0) {
            buf += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

static void write_json_string(FILE *stream, const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    fputc('"', stream);
    for (; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", stream); break;
        case '\\': fputs("\\\\", stream); break;
        case '\b': fputs("\\b", stream); break;
        case '\f': fputs("\\f", stream); break;
        case '\n': fputs("\\n", stream); break;
        case '\r': fputs("\\r", stream); break;
        case '\t': fputs("\\t", stream); break;
        default:
            if (*p < 0x20) fprintf(stream, "\\u%04x", (unsigned int)*p);
            else fputc(*p, stream);
        }
    }
    fputc('"', stream);
}

void stats_dump(const Stats *s, const char *kind, bool detailed, int fd) {
    uint64_t now_ns = 0;
    stats_monotonic_ns(&now_ns);
    uint64_t seq = s_stats_seq++;

    char *buf = NULL;
    size_t len = 0;
    FILE *stream = open_memstream(&buf, &len);
    if (!stream) return;

    fputs("{\"schema_version\":1,\"kind\":", stream);
    write_json_string(stream, kind ? kind : "snapshot");
    fprintf(stream,
        ",\"seq\":%" PRIu64 ",\"pid\":%d,\"time_ns\":%" PRIu64
        ",\"x_events\":%" PRIu64 ",\"damage_events\":%" PRIu64
        ",\"render_attempts\":%" PRIu64 ",\"successful_swaps\":%" PRIu64
        ",\"full_repaints\":%" PRIu64 ",\"partial_repaints\":%" PRIu64
        ",\"skipped_presentations\":%" PRIu64 ",\"skipped_occluded_damage\":%" PRIu64
        ",\"poll_wakeups\":%" PRIu64
        ",\"imports\":%" PRIu64 ",\"reimports\":%" PRIu64
        ",\"x_round_trips\":%" PRIu64
        ",\"live_pixmaps\":%" PRIu64 ",\"live_images\":%" PRIu64
        ",\"live_textures\":%" PRIu64 ",\"live_damage\":%" PRIu64
        ",\"live_regions\":%" PRIu64,
        seq, (int)getpid(), now_ns,
        s->x_events, s->damage_events,
        s->render_attempts, s->successful_swaps,
        s->full_repaints, s->partial_repaints,
        s->skipped_presentations, s->skipped_occluded_damage,
        s->poll_wakeups,
        s->imports, s->reimports,
        s->x_round_trips,
        s->live_pixmaps, s->live_images,
        s->live_textures, s->live_damage,
        s->live_regions);

    if (detailed) {
        uint64_t er_p50 = 0, er_p95 = 0, er_p99 = 0;
        uint64_t dr_p50 = 0, dr_p95 = 0, dr_p99 = 0;
        uint64_t sw_p50 = 0, sw_p95 = 0, sw_p99 = 0;
        calc_percentiles(&s->event_reconcile_ns, &er_p50, &er_p95, &er_p99);
        calc_percentiles(&s->draw_ns, &dr_p50, &dr_p95, &dr_p99);
        calc_percentiles(&s->swap_ns, &sw_p50, &sw_p95, &sw_p99);

        fprintf(stream,
            ",\"timing_cpu_ns\":{"
            "\"event_reconcile\":{\"count\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 "},"
            "\"draw\":{\"count\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 "},"
            "\"swap\":{\"count\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 "}"
            "}}",
            (uint64_t)s->event_reconcile_ns.total_samples, er_p50, er_p95, er_p99,
            (uint64_t)s->draw_ns.total_samples, dr_p50, dr_p95, dr_p99,
            (uint64_t)s->swap_ns.total_samples, sw_p50, sw_p95, sw_p99);
    } else {
        fputc('}', stream);
    }
    fputc('\n', stream);
    if (fclose(stream) != 0) {
        free(buf);
        return;
    }
    write_all(fd, buf, len);
    free(buf);
}

void stats_dump_human(const Stats *s, int exit_status, int fd) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "slopcom: Exiting (exit status %d). Final stats: render_attempts=%" PRIu64
        ", swap_successes=%" PRIu64 ", live_pixmaps=%" PRIu64 ", live_textures=%" PRIu64 "\n",
        exit_status, s->render_attempts, s->successful_swaps, s->live_pixmaps, s->live_textures);
    if (len > 0 && (size_t)len < sizeof(buf)) {
        write_all(fd, buf, (size_t)len);
    } else if (len < 0 || (size_t)len >= sizeof(buf)) {
        static const char fallback[] = "slopcom: Final stats formatting failed\n";
        write_all(fd, fallback, sizeof(fallback) - 1);
    }
}
