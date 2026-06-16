/* reaper.c — periodic maintenance thread.
 *   - every ~5s: mark devices OFFLINE if no traffic for HEARTBEAT_TIMEOUT_S
 *   - on each 500ms tick: if a SIGUSR1 set dump_requested, dump table + sink stats
 *
 * Doing the SIGUSR1 dump HERE (not in the handler) keeps the handler trivially
 * async-signal-safe: it only sets an atomic flag. The dump itself runs on a
 * normal thread, takes the table READ lock, and so proves rwlock correctness —
 * workers keep updating different devices while we read a consistent snapshot.
 */
#define _GNU_SOURCE
#include "server.h"
#include "log.h"
#include "device_table.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_TIMEOUT_S 15
#define SWEEP_PERIOD_TICKS  10      /* 10 * 500ms = 5s */

void *reaper_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    int ticks = 0;

    while (atomic_load(&ctx->running)) {
        usleep(500 * 1000);          /* 500ms tick */

        if (atomic_exchange(&ctx->dump_requested, false)) {
            LOGF("SIGUSR1: dumping stats");
            dt_dump(&ctx->devices, stderr);
            if (ctx->sink && ctx->sink->dump)
                ctx->sink->dump(stderr);
        }

        if (++ticks >= SWEEP_PERIOD_TICKS) {
            ticks = 0;
            dt_sweep_offline(&ctx->devices, (long)time(NULL), HEARTBEAT_TIMEOUT_S);
        }
    }
    LOGF("reaper shutting down");
    return NULL;
}
