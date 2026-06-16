/* ring_log.c — v1 record sink: a fixed in-memory circular buffer.
 *
 * Stands in for the Stage-3 kernel device so the rest of the server is testable
 * end-to-end now. Multiple worker threads write concurrently, so the buffer is
 * mutex-protected. (The kernel version will use a spinlock; the reasons differ
 * and are noted in the kmod README.)
 */
#define _GNU_SOURCE
#include "sink.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define RING_SLOTS 1024

static sensor_record_t  g_ring[RING_SLOTS];
static size_t           g_head;            /* next write index */
static atomic_ullong    g_written;         /* total records written */
static atomic_ullong    g_overwritten;     /* records lost to wraparound */
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;

static int rl_open(void)
{
    pthread_mutex_lock(&g_lock);
    g_head = 0;
    memset(g_ring, 0, sizeof g_ring);
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_written, 0);
    atomic_store(&g_overwritten, 0);
    return 0;
}

static int rl_write(const sensor_record_t *rec)
{
    pthread_mutex_lock(&g_lock);
    g_ring[g_head % RING_SLOTS] = *rec;
    g_head++;
    pthread_mutex_unlock(&g_lock);

    unsigned long long w = atomic_fetch_add(&g_written, 1) + 1;
    if (w > RING_SLOTS)
        atomic_fetch_add(&g_overwritten, 1);
    return 0;
}

static void rl_dump(FILE *out)
{
    unsigned long long written = atomic_load(&g_written);
    unsigned long long lost    = atomic_load(&g_overwritten);

    pthread_mutex_lock(&g_lock);
    size_t fill = g_head < RING_SLOTS ? g_head : RING_SLOTS;
    fprintf(out, "==== ring_log: written=%llu  overwritten=%llu  fill=%zu/%d ====\n",
            written, lost, fill, RING_SLOTS);
    /* show the most recent few records */
    size_t show = fill < 5 ? fill : 5;
    for (size_t i = 0; i < show; i++) {
        size_t idx = (g_head - 1 - i) % RING_SLOTS;
        const sensor_record_t *r = &g_ring[idx];
        fprintf(out, "  dev=0x%08X seq=%u temp=%d humid=%d ts=%llu\n",
                r->device_id, r->seq, r->temperature, r->humidity,
                (unsigned long long)r->server_timestamp_ns);
    }
    pthread_mutex_unlock(&g_lock);
    fflush(out);
}

static void rl_close(void) { /* nothing to release for the in-memory buffer */ }

const sink_t sink_ring_log = {
    .name         = "mem(ring_log)",
    .open         = rl_open,
    .write_record = rl_write,
    .dump         = rl_dump,
    .close        = rl_close,
};
