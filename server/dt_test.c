/* dt_test.c — device table unit + concurrency tests.
 * Build: make dt_test    Run: ./dt_test   (also run the tsan build for races)
 */
#define _GNU_SOURCE
#include "device_table.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_fail = 0;
#define CHECK(c, ...) do { if(!(c)){ g_fail++; \
    fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__); \
    fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); } } while(0)

/* ---- single-threaded correctness ---------------------------------------- */
static void test_basic(void)
{
    device_table_t t;
    dt_init(&t);

    device_t *a = dt_get_or_create(&t, 0x1001, 7);
    CHECK(a && a->id == 0x1001, "create 0x1001");
    CHECK(a->session_id == 7, "session stored");

    /* idempotent: same id returns the SAME pointer (stable for lifetime) */
    device_t *a2 = dt_get_or_create(&t, 0x1001, 99);
    CHECK(a == a2, "get_or_create idempotent (same pointer)");
    CHECK(a2->session_id == 7, "session not overwritten on re-get");

    dt_record_sensor(a, 42, 2654, 4810, (long)time(NULL));
    CHECK(atomic_load(&a->last_seq) == 42, "last_seq updated");
    CHECK(atomic_load(&a->last_temp) == 2654, "last_temp updated");
    CHECK(atomic_load(&a->packets_ok) == 1, "packets_ok incremented");

    /* many distinct devices land and are all findable */
    for (uint32_t i = 0; i < 500; i++)
        dt_get_or_create(&t, 0x4000 + i, i);
    CHECK(atomic_load(&t.device_count) == 501, "count = 501 (got %u)",
          atomic_load(&t.device_count));
    for (uint32_t i = 0; i < 500; i++) {
        device_t *d = dt_get_or_create(&t, 0x4000 + i, 0);
        CHECK(d && d->id == 0x4000 + i, "lookup 0x%X", 0x4000 + i);
    }

    /* offline sweep: force last_seen into the past */
    atomic_store(&a->last_seen, (long long)(time(NULL) - 100));
    dt_sweep_offline(&t, (long)time(NULL), 15);
    CHECK(atomic_load(&a->state) == DEV_OFFLINE, "stale device -> OFFLINE");

    dt_destroy(&t);
}

/* ---- concurrency: many threads, overlapping ids ------------------------- */
#define NTHREADS 8
#define NOPS     20000

static device_table_t g_t;

static void *worker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < NOPS; i++) {
        uint32_t id = 0x8000 + (rand_r(&seed) % 64);   /* heavy collisions */
        device_t *d = dt_get_or_create(&g_t, id, 1);
        if (d)
            dt_record_sensor(d, (uint32_t)i, i % 5000, i % 100,
                             (long)time(NULL));
    }
    return NULL;
}

static void test_concurrent(void)
{
    dt_init(&g_t);
    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; i++)
        pthread_create(&th[i], NULL, worker, (void *)(i + 1));
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);

    /* at most 64 distinct ids should have been created */
    unsigned cnt = atomic_load(&g_t.device_count);
    CHECK(cnt <= 64, "distinct devices <= 64 (got %u)", cnt);

    /* total packets_ok across all devices must equal NTHREADS*NOPS exactly */
    unsigned long long total = 0;
    for (int b = 0; b < TABLE_BUCKETS; b++)
        for (device_t *d = g_t.buckets[b]; d; d = d->next)
            total += atomic_load(&d->packets_ok);
    CHECK(total == (unsigned long long)NTHREADS * NOPS,
          "no lost updates: total=%llu expected=%llu",
          total, (unsigned long long)NTHREADS * NOPS);

    dt_destroy(&g_t);
}

int main(void)
{
    test_basic();
    test_concurrent();
    if (g_fail == 0) { printf("device_table: ALL PASS\n"); return 0; }
    printf("device_table: %d FAILURES\n", g_fail);
    return 1;
}
