/* device_table.c — see device_table.h */
#define _GNU_SOURCE
#include "device_table.h"

#include <stdlib.h>
#include <time.h>

static inline unsigned dt_hash(uint32_t id)
{
    /* Knuth multiplicative hash (overflow mod 2^32 is intentional). */
    return (unsigned)((id * 2654435761u) % TABLE_BUCKETS);
}

void dt_init(device_table_t *t)
{
    for (int i = 0; i < TABLE_BUCKETS; i++)
        t->buckets[i] = NULL;
    pthread_rwlock_init(&t->lock, NULL);
    atomic_init(&t->device_count, 0);
}

void dt_destroy(device_table_t *t)
{
    pthread_rwlock_wrlock(&t->lock);
    for (int i = 0; i < TABLE_BUCKETS; i++) {
        device_t *d = t->buckets[i];
        while (d) {
            device_t *next = d->next;
            free(d);
            d = next;
        }
        t->buckets[i] = NULL;
    }
    pthread_rwlock_unlock(&t->lock);
    pthread_rwlock_destroy(&t->lock);
}

static device_t *find_locked(device_table_t *t, unsigned h, uint32_t id)
{
    for (device_t *d = t->buckets[h]; d; d = d->next)
        if (d->id == id)
            return d;
    return NULL;
}

device_t *dt_get_or_create(device_table_t *t, uint32_t id, uint32_t session_id)
{
    unsigned h = dt_hash(id);

    /* fast path: read-locked lookup */
    pthread_rwlock_rdlock(&t->lock);
    device_t *d = find_locked(t, h, id);
    pthread_rwlock_unlock(&t->lock);
    if (d)
        return d;

    /* slow path: upgrade to write lock, re-check (someone may have raced us) */
    pthread_rwlock_wrlock(&t->lock);
    d = find_locked(t, h, id);
    if (d) {
        pthread_rwlock_unlock(&t->lock);
        return d;
    }
    d = calloc(1, sizeof *d);
    if (!d) {
        pthread_rwlock_unlock(&t->lock);
        return NULL;
    }
    d->id         = id;
    d->session_id = session_id;
    atomic_init(&d->state,       DEV_ONLINE);
    atomic_init(&d->last_seq,    0);
    atomic_init(&d->last_seen,   (long long)time(NULL));
    atomic_init(&d->packets_ok,  0);
    atomic_init(&d->packets_bad, 0);
    atomic_init(&d->last_temp,   0);
    atomic_init(&d->last_humid,  0);
    d->next        = t->buckets[h];
    t->buckets[h]  = d;
    atomic_fetch_add(&t->device_count, 1);
    pthread_rwlock_unlock(&t->lock);
    return d;
}

void dt_record_sensor(device_t *d, uint32_t seq,
                      int32_t temp, int32_t humid, long now)
{
    atomic_store(&d->last_seq,  seq);
    atomic_store(&d->last_temp, temp);
    atomic_store(&d->last_humid, humid);
    atomic_store(&d->last_seen, (long long)now);
    atomic_store(&d->state,     DEV_ONLINE);
    atomic_fetch_add(&d->packets_ok, 1);
}

void dt_record_bad(device_t *d)
{
    if (d)
        atomic_fetch_add(&d->packets_bad, 1);
}

void dt_touch(device_table_t *t, uint32_t id, long now)
{
    device_t *d = dt_get_or_create(t, id, 0);
    if (d) {
        atomic_store(&d->last_seen, (long long)now);
        atomic_store(&d->state, DEV_ONLINE);
    }
}

void dt_sweep_offline(device_table_t *t, long now, int timeout_s)
{
    pthread_rwlock_rdlock(&t->lock);   /* read lock: we only traverse + set atomics */
    for (int i = 0; i < TABLE_BUCKETS; i++)
        for (device_t *d = t->buckets[i]; d; d = d->next)
            if (now - (long)atomic_load(&d->last_seen) > timeout_s)
                atomic_store(&d->state, DEV_OFFLINE);
    pthread_rwlock_unlock(&t->lock);
}

void dt_dump(device_table_t *t, FILE *out)
{
    pthread_rwlock_rdlock(&t->lock);
    fprintf(out, "==== device table (%u devices) ====\n",
            atomic_load(&t->device_count));
    fprintf(out, "  %-10s %-7s %-7s %-8s %-8s %-7s %-7s\n",
            "device", "session", "state", "ok", "bad", "temp", "humid");
    for (int i = 0; i < TABLE_BUCKETS; i++) {
        for (device_t *d = t->buckets[i]; d; d = d->next) {
            int temp = atomic_load(&d->last_temp);
            int hum  = atomic_load(&d->last_humid);
            fprintf(out,
                "  0x%08X %-7u %-7s %-8llu %-8llu %3d.%02d %3d.%02d\n",
                d->id, d->session_id,
                atomic_load(&d->state) == DEV_ONLINE ? "ONLINE" : "OFFLINE",
                (unsigned long long)atomic_load(&d->packets_ok),
                (unsigned long long)atomic_load(&d->packets_bad),
                temp / 100, abs(temp % 100), hum / 100, abs(hum % 100));
        }
    }
    fprintf(out, "===================================\n");
    fflush(out);
    pthread_rwlock_unlock(&t->lock);
}
