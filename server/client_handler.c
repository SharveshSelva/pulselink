/* client_handler.c — one detached worker per TCP connection.
 *
 * State machine:  WAIT_HELLO --(valid HELLO)--> ACTIVE --(EOF/error/3 strikes)--> CLOSING
 *
 * Per-packet loop: read 12-byte header (looping over recv), validate
 * magic/version/length, read payload, verify checksum, then dispatch.
 *
 * M3 additions vs M2:
 *  - registers the device in the rwlock-protected table on HELLO
 *  - updates per-device atomics + writes each reading to the record sink
 *  - cumulative ACK: PKT_ACK with the latest seq every 8th sensor packet
 *  - reaps a silent peer after DEAD_PEER_S, but wakes every WORKER_TICK_S so it
 *    notices shutdown quickly; bumps active_workers so main can drain on exit
 */
#define _GNU_SOURCE
#include "server.h"
#include "log.h"
#include "device_table.h"
#include "sink.h"
#include "../common/wire.h"
#include "../common/netutil.h"
#include "../imaging/frame.h"
#include "../imaging/bmp.h"
#include "../imaging/convolve.h"

#include <errno.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WORKER_TICK_S   2     /* recv wakeup cadence (shutdown responsiveness) */
#define DEAD_PEER_S     30    /* reap a peer silent this long */
#define MAX_STRIKES     3
#define ACK_EVERY       8

static atomic_uint g_next_session = 1;

typedef enum { WAIT_HELLO, ACTIVE, CLOSING } worker_state_t;

static int send_packet(int fd, pkt_type_t type, uint32_t device_id,
                       const void *payload, uint16_t plen)
{
    uint8_t out[PL_HDR_SIZE + 64];
    size_t n = pl_build(out, sizeof out, type, device_id, payload, plen);
    if (n == 0)
        return -1;
    return write_n(fd, out, n) == (ssize_t)n ? 0 : -1;
}

static int send_error(int fd, uint32_t device_id, uint16_t code)
{
    uint16_t be = htons(code);
    return send_packet(fd, PKT_ERROR, device_id, &be, sizeof be);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void *client_worker(void *arg)
{
    worker_arg_t *wa  = (worker_arg_t *)arg;
    server_ctx_t *ctx = wa->ctx;
    int fd            = wa->fd;

    atomic_fetch_add(&ctx->active_workers, 1);

    struct timeval tv = { WORKER_TICK_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    worker_state_t state = WAIT_HELLO;
    uint32_t  session_id = 0;
    device_t *dev        = NULL;
    int       strikes    = 0;
    int       idle_ticks = 0;
    uint32_t  sensor_count = 0;
    uint8_t   payload[PL_MAX_PAYLOAD];
    frame_asm_t fa;                       /* reassembles PKT_FRAME chunks */
    frame_asm_init(&fa);

    while (atomic_load(&ctx->running) && state != CLOSING) {

        /* ---- header ------------------------------------------------- */
        uint8_t hdrbuf[PL_HDR_SIZE];
        ssize_t r = read_n(fd, hdrbuf, PL_HDR_SIZE);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++idle_ticks * WORKER_TICK_S >= DEAD_PEER_S) {
                    LOGF("%s idle > %ds, reaping", wa->peer, DEAD_PEER_S);
                    break;
                }
                continue;                 /* tick: re-check running, keep waiting */
            }
            LOGF("%s header recv error: %s", wa->peer, strerror(errno));
            break;
        }
        if (r == 0) { LOGF("%s closed connection", wa->peer); break; }
        if (r != PL_HDR_SIZE) {
            LOGF("%s closed mid-header (%zd/%u)", wa->peer, r, PL_HDR_SIZE);
            break;
        }
        idle_ticks = 0;

        pl_header_t h;
        pl_parse_header(hdrbuf, &h);

        /* ---- framing validation ------------------------------------- */
        if (h.magic != PL_MAGIC) {
            LOGF("%s bad magic 0x%04X (strike %d)", wa->peer, h.magic, strikes + 1);
            send_error(fd, h.device_id, PL_ERR_BAD_MAGIC);
            dt_record_bad(dev);
            if (++strikes >= MAX_STRIKES) break;
            continue;
        }
        if (h.version != PL_VERSION) {
            LOGF("%s bad version 0x%02X (strike %d)", wa->peer, h.version, strikes + 1);
            send_error(fd, h.device_id, PL_ERR_BAD_VERSION);
            dt_record_bad(dev);
            if (++strikes >= MAX_STRIKES) break;
            continue;
        }
        if (h.payload_len > PL_MAX_PAYLOAD) {
            LOGF("%s payload too big (%u, strike %d)", wa->peer, h.payload_len, strikes + 1);
            send_error(fd, h.device_id, PL_ERR_TOO_BIG);
            dt_record_bad(dev);
            if (++strikes >= MAX_STRIKES) break;
            continue;
        }

        /* ---- payload ------------------------------------------------ */
        if (h.payload_len > 0) {
            r = read_n(fd, payload, h.payload_len);
            if (r != (ssize_t)h.payload_len) {
                LOGF("%s closed/err mid-payload", wa->peer);
                break;
            }
        }

        /* ---- checksum ----------------------------------------------- */
        if (pl_checksum(payload, h.payload_len) != h.checksum) {
            LOGF("%s bad checksum (strike %d)", wa->peer, strikes + 1);
            send_error(fd, h.device_id, PL_ERR_BAD_CHECKSUM);
            dt_record_bad(dev);
            if (++strikes >= MAX_STRIKES) break;
            continue;
        }

        /* ---- dispatch ----------------------------------------------- */
        if (state == WAIT_HELLO) {
            if (h.type != PKT_HELLO) {
                LOGF("%s sent type %u before HELLO, dropping", wa->peer, h.type);
                send_error(fd, h.device_id, PL_ERR_NO_HELLO);
                break;
            }
            session_id = atomic_fetch_add(&g_next_session, 1);
            dev = dt_get_or_create(&ctx->devices, h.device_id, session_id);
            uint32_t sid_be = htonl(session_id);
            if (send_packet(fd, PKT_HELLO_ACK, h.device_id, &sid_be, sizeof sid_be) != 0) {
                LOGF("%s failed to send HELLO_ACK", wa->peer);
                break;
            }
            state   = ACTIVE;
            strikes = 0;
            LOGF("%s HELLO from device 0x%08X -> session %u (ACTIVE)",
                 wa->peer, h.device_id, session_id);
            continue;
        }

        /* state == ACTIVE */
        switch (h.type) {
        case PKT_SENSOR: {
            pl_sensor_payload_t s;
            memcpy(&s, payload, sizeof s);
            uint32_t seq  = ntohl(s.seq);
            int32_t  temp = (int32_t)ntohl((uint32_t)s.temperature);
            int32_t  hum  = (int32_t)ntohl((uint32_t)s.humidity);

            dt_record_sensor(dev, seq, temp, hum, (long)time(NULL));

            sensor_record_t rec = {
                .device_id           = h.device_id,
                .seq                 = seq,
                .temperature         = temp,
                .humidity            = hum,
                .server_timestamp_ns = now_ns(),
            };
            ctx->sink->write_record(&rec);

            if (++sensor_count % ACK_EVERY == 0) {
                uint32_t acked = htonl(seq);
                send_packet(fd, PKT_ACK, h.device_id, &acked, sizeof acked);
                LOGF("%s ACK seq=%u (every %d)", wa->peer, seq, ACK_EVERY);
            }
            strikes = 0;
            break;
        }
        case PKT_FRAME: {
            image_t img;
            int fr = frame_asm_feed(&fa, payload, h.payload_len, &img);
            if (fr < 0) {
                LOGF("%s bad FRAME chunk (strike %d)", wa->peer, strikes + 1);
                send_error(fd, h.device_id, PL_ERR_TOO_BIG);
                dt_record_bad(dev);
                if (++strikes >= MAX_STRIKES) state = CLOSING;
            } else {
                strikes = 0;
                if (fr == 1) {
                    LOGF("%s FRAME complete %dx%d", wa->peer, img.w, img.h);
                    if (ctx->frame_dir) {
                        image_t edges;
                        if (image_alloc(&edges, img.w, img.h) == 0) {
                            sobel(&img, &edges);
                            char p1[512], p2[512];
                            snprintf(p1, sizeof p1, "%s/dev%08X_in.bmp",
                                     ctx->frame_dir, h.device_id);
                            snprintf(p2, sizeof p2, "%s/dev%08X_edges.bmp",
                                     ctx->frame_dir, h.device_id);
                            if (bmp_write(&img, p1) == 0 && bmp_write(&edges, p2) == 0)
                                LOGF("%s wrote %s (+ _edges)", wa->peer, p1);
                            else
                                LOGF("%s frame BMP write failed", wa->peer);
                            image_free(&edges);
                        }
                    }
                    image_free(&img);
                }
            }
            break;
        }
        case PKT_HELLO:
            LOGF("%s duplicate HELLO ignored", wa->peer);
            break;
        default:
            LOGF("%s unexpected type %u in ACTIVE", wa->peer, h.type);
            send_error(fd, h.device_id, PL_ERR_BAD_TYPE);
            dt_record_bad(dev);
            if (++strikes >= MAX_STRIKES) state = CLOSING;
            break;
        }
    }

    /* ---- cleanup: worker owns fd, the semaphore slot, and wa --------- */
    frame_asm_reset(&fa);
    close(fd);
    sem_post(&ctx->client_slots);
    atomic_fetch_sub(&ctx->active_workers, 1);
    LOGF("%s worker exit", wa->peer);
    free(wa);
    return NULL;
}
