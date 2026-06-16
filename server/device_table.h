/* device_table.h — the device registry.
 *
 * Concurrency design (the rwlock talking point):
 *   - The rwlock guards only the TABLE STRUCTURE (the bucket chains). Inserting
 *     a new device takes the WRITE lock; looking one up takes the READ lock.
 *   - Devices are NEVER removed (going quiet just flips state to OFFLINE), so a
 *     device_t* is stable for the whole process lifetime. That lets every hot
 *     per-device field be a C11 atomic updated WITHOUT the table lock.
 *   => Inserts (write-lock) are rare (once per new device); lookups + the stats
 *      dump (read-lock) are frequent. That asymmetry is exactly when a rwlock
 *      beats a plain mutex: N workers update N different devices in parallel and
 *      a SIGUSR1 stats reader doesn't block any of them.
 */
#ifndef PULSELINK_DEVICE_TABLE_H
#define PULSELINK_DEVICE_TABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { DEV_ONLINE = 0, DEV_OFFLINE = 1 } dev_state_t;

typedef struct device {
    uint32_t id;            /* immutable after insert            */
    uint32_t session_id;    /* set once at insert                */
    atomic_int       state; /* dev_state_t                       */
    atomic_uint      last_seq;
    atomic_llong     last_seen;       /* unix seconds            */
    atomic_ullong    packets_ok;
    atomic_ullong    packets_bad;
    atomic_int       last_temp;       /* centi-degC              */
    atomic_int       last_humid;      /* centi-%                 */
    struct device   *next;            /* chain (guarded by rwlock) */
} device_t;

#define TABLE_BUCKETS 64

typedef struct {
    device_t        *buckets[TABLE_BUCKETS];
    pthread_rwlock_t lock;
    atomic_uint      device_count;
} device_table_t;

void      dt_init(device_table_t *t);
void      dt_destroy(device_table_t *t);

/* Find device by id, creating it (state ONLINE) if absent. Never returns NULL
 * except on allocation failure. session_id is used only when creating. */
device_t *dt_get_or_create(device_table_t *t, uint32_t id, uint32_t session_id);

/* Hot-path field updates (no table lock needed — pointer is stable, fields are
 * atomic). */
void      dt_record_sensor(device_t *d, uint32_t seq,
                           int32_t temp, int32_t humid, long now);
void      dt_record_bad(device_t *d);
void      dt_touch(device_table_t *t, uint32_t id, long now); /* UDP heartbeat */

/* Maintenance / reporting (take the read lock). */
void      dt_sweep_offline(device_table_t *t, long now, int timeout_s);
void      dt_dump(device_table_t *t, FILE *out);

#endif /* PULSELINK_DEVICE_TABLE_H */
