/* integ_test.c — in-process end-to-end test of the worker pipeline.
 *
 * Why in-process: it exercises the REAL client_worker() over socketpairs with
 * no network/backgrounding, so it is deterministic and also runnable under
 * ThreadSanitizer (make integ_tsan) to satisfy the Stage-2 race-check.
 *
 * It spins up NCONN concurrent connections sharing one device table + sink.
 * Two of them use the SAME device id, so dt_get_or_create() and the per-device
 * atomic counters get hammered concurrently — exactly the rwlock contention we
 * claim is safe.
 */
#define _GNU_SOURCE
#include "server.h"
#include "device_table.h"
#include "sink.h"
#include "../common/wire.h"
#include "../common/netutil.h"
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NCONN     4
#define NSENSORS  16
#define ACK_EVERY 8

static int g_fail = 0;
#define CHECK(c, ...) do { if(!(c)){ g_fail++; \
    fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__); \
    fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); } } while(0)

/* read exactly one framed packet; returns type or -1 on EOF/timeout */
static int read_packet(int fd, pl_header_t *h, uint8_t *payload)
{
    uint8_t hdr[PL_HDR_SIZE];
    if (read_n(fd, hdr, PL_HDR_SIZE) != PL_HDR_SIZE)
        return -1;
    pl_parse_header(hdr, h);
    if (h->payload_len) {
        if (read_n(fd, payload, h->payload_len) != (ssize_t)h->payload_len)
            return -1;
    }
    return (int)h->type;
}

typedef struct {
    int      fd;            /* device end of the pair */
    uint32_t devid;
    int      acks_got;
    int      hello_acked;
} conn_t;

static void *device_side(void *arg)
{
    conn_t *c = arg;
    struct timeval tv = { 0, 200000 };          /* 200ms read timeout */
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    uint8_t out[PL_HDR_SIZE + 64], pl[PL_MAX_PAYLOAD];
    pl_header_t h;

    /* HELLO */
    pl_hello_payload_t hello = { .firmware_version = htonl(0x00010002),
                                 .capabilities = htonl(0x1) };
    size_t n = pl_build(out, sizeof out, PKT_HELLO, c->devid, &hello, sizeof hello);
    write_n(c->fd, out, n);
    if (read_packet(c->fd, &h, pl) == PKT_HELLO_ACK)
        c->hello_acked = 1;

    /* stream sensors (don't block on replies mid-stream) */
    for (int i = 1; i <= NSENSORS; i++) {
        pl_sensor_payload_t s = {
            .seq          = htonl((uint32_t)i),
            .timestamp_ms = htonl((uint32_t)(i * 100)),
            .temperature  = (int32_t)htonl((uint32_t)(2500 + i)),  /* 25.xx C */
            .humidity     = (int32_t)htonl((uint32_t)(4800 + i)),
        };
        n = pl_build(out, sizeof out, PKT_SENSOR, c->devid, &s, sizeof s);
        write_n(c->fd, out, n);
    }
    /* now collect the ACKs the worker queued back to us */
    while (read_packet(c->fd, &h, pl) == PKT_ACK)
        c->acks_got++;

    return NULL;
}

int main(void)
{
    server_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg.max_clients = NCONN;
    atomic_init(&ctx.running, true);
    atomic_init(&ctx.dump_requested, false);
    atomic_init(&ctx.active_workers, 0);
    sem_init(&ctx.client_slots, 0, NCONN);
    dt_init(&ctx.devices);
    ctx.sink = &sink_ring_log;
    ctx.sink->open();

    /* two connections share device 0x1001 to force concurrent updates */
    uint32_t ids[NCONN] = { 0x1001, 0x1001, 0x2002, 0x3003 };

    pthread_t   workers[NCONN], devs[NCONN];
    conn_t      conns[NCONN];

    for (int i = 0; i < NCONN; i++) {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

        worker_arg_t *wa = calloc(1, sizeof *wa);
        wa->ctx = &ctx;
        wa->fd  = sv[0];
        snprintf(wa->peer, sizeof wa->peer, "conn%d", i);
        pthread_create(&workers[i], NULL, client_worker, wa);  /* real worker */

        conns[i] = (conn_t){ .fd = sv[1], .devid = ids[i] };
        pthread_create(&devs[i], NULL, device_side, &conns[i]);
    }

    for (int i = 0; i < NCONN; i++)
        pthread_join(devs[i], NULL);

    /* devices done sending: close their ends so workers see EOF and exit */
    for (int i = 0; i < NCONN; i++)
        close(conns[i].fd);
    atomic_store(&ctx.running, false);
    for (int i = 0; i < NCONN; i++)
        pthread_join(workers[i], NULL);

    /* ---- assertions -------------------------------------------------- */
    for (int i = 0; i < NCONN; i++) {
        CHECK(conns[i].hello_acked, "conn%d got HELLO_ACK", i);
        CHECK(conns[i].acks_got == NSENSORS / ACK_EVERY,
              "conn%d acks=%d (want %d)", i, conns[i].acks_got, NSENSORS / ACK_EVERY);
    }

    unsigned long long total_ok = 0;
    for (int b = 0; b < TABLE_BUCKETS; b++)
        for (device_t *d = ctx.devices.buckets[b]; d; d = d->next)
            total_ok += atomic_load(&d->packets_ok);
    CHECK(total_ok == (unsigned long long)NCONN * NSENSORS,
          "total packets_ok=%llu (want %d)", total_ok, NCONN * NSENSORS);

    /* 4 connections, 3 distinct device ids (0x1001 shared) */
    CHECK(atomic_load(&ctx.devices.device_count) == 3,
          "distinct devices=%u (want 3)", atomic_load(&ctx.devices.device_count));

    fprintf(stderr, "\n--- final state ---\n");
    dt_dump(&ctx.devices, stderr);
    ctx.sink->dump(stderr);

    ctx.sink->close();
    dt_destroy(&ctx.devices);
    sem_destroy(&ctx.client_slots);

    if (g_fail == 0) { printf("\ninteg_test: ALL PASS\n"); return 0; }
    printf("\ninteg_test: %d FAILURES\n", g_fail);
    return 1;
}
