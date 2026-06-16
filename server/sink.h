/* sink.h — the record-sink abstraction.
 *
 * Defining one interface NOW means the Stage-3 kernel char device (sink_chardev)
 * slots in behind the same v-table with zero changes to the worker code. The
 * record layout deliberately matches what /dev/telemetry will store.
 */
#ifndef PULSELINK_SINK_H
#define PULSELINK_SINK_H

#include <stdint.h>
#include <stdio.h>

/* Mirrors the planned kernel struct telemetry_record (host order here; the
 * chardev sink will pack/convert as needed in Stage 3). */
typedef struct {
    uint32_t device_id;
    uint32_t seq;
    int32_t  temperature;          /* centi-degC */
    int32_t  humidity;             /* centi-%    */
    uint64_t server_timestamp_ns;
} sensor_record_t;

typedef struct sink {
    const char *name;
    int  (*open)(void);
    int  (*write_record)(const sensor_record_t *rec);
    void (*dump)(FILE *out);       /* stats dump (SIGUSR1); may be NULL */
    void (*close)(void);
} sink_t;

extern const sink_t sink_ring_log;   /* -s mem (default) */
extern const sink_t sink_chardev;    /* -s dev (/dev/telemetry, Stage 3) */

#endif /* PULSELINK_SINK_H */
