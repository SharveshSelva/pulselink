/* frame_e2e_test.c — drive a chunked frame through the REAL client_worker over a
 * socketpair and verify it reassembles the frame and writes the edge BMP. This
 * proves the server's PKT_FRAME path, not just the imaging library in isolation.
 */
#define _GNU_SOURCE
#include "server.h"
#include "sink.h"
#include "../common/wire.h"
#include "../common/netutil.h"
#include "../imaging/image.h"
#include "../imaging/bmp.h"
#include "../imaging/frame.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)){ fails++; \
    fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__); \
    fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); } } while(0)

static int read_packet(int fd, pl_header_t *h, uint8_t *pl)
{
    uint8_t hb[PL_HDR_SIZE];
    if (read_n(fd, hb, PL_HDR_SIZE) != PL_HDR_SIZE) return -1;
    pl_parse_header(hb, h);
    if (h->payload_len && read_n(fd, pl, h->payload_len) != (ssize_t)h->payload_len)
        return -1;
    return (int)h->type;
}

int main(void)
{
    server_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    atomic_init(&ctx.running, true);
    atomic_init(&ctx.active_workers, 0);
    atomic_init(&ctx.dump_requested, false);
    sem_init(&ctx.client_slots, 0, 1);
    dt_init(&ctx.devices);
    ctx.sink = &sink_ring_log;
    ctx.sink->open();
    ctx.listen_fd = -1;
    ctx.frame_dir = "/tmp";

    const char *in_path = "/tmp/dev00001001_in.bmp";
    const char *ed_path = "/tmp/dev00001001_edges.bmp";
    remove(in_path); remove(ed_path);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    worker_arg_t *wa = malloc(sizeof *wa);
    wa->ctx = &ctx; wa->fd = sv[0];
    snprintf(wa->peer, sizeof wa->peer, "e2e");
    pthread_t worker;
    pthread_create(&worker, NULL, client_worker, wa);

    int fd = sv[1];
    uint8_t out[PL_HDR_SIZE + 1100], pl[1100];
    pl_header_t h;

    /* HELLO -> HELLO_ACK */
    pl_hello_payload_t hello = { htonl(0x00010008), htonl(1) };
    size_t n = pl_build(out, sizeof out, PKT_HELLO, 0x1001, &hello, sizeof hello);
    write_n(fd, out, n);
    CHECK(read_packet(fd, &h, pl) == PKT_HELLO_ACK, "HELLO_ACK received");

    /* send a synthetic frame as PKT_FRAME chunks */
    image_t src;
    image_alloc(&src, 40, 30);          /* 1200 bytes -> 2 chunks */
    image_test_pattern(&src);
    uint32_t total = (uint32_t)src.w * src.h;
    uint16_t cc = frame_chunk_count(total);
    for (uint16_t i = 0; i < cc; i++) {
        uint32_t off = (uint32_t)i * FRAME_CHUNK_MAX;
        uint32_t len = total - off; if (len > FRAME_CHUNK_MAX) len = FRAME_CHUNK_MAX;
        uint8_t pkt[PL_HDR_SIZE + 12 + FRAME_CHUNK_MAX];
        size_t m = frame_chunk_build(pkt, sizeof pkt, 0x1001, (uint16_t)src.w,
                                     (uint16_t)src.h, 1, i, cc,
                                     src.pix + off, (uint16_t)len);
        write_n(fd, pkt, m);
    }

    usleep(300000);                      /* let the worker process + write */
    atomic_store(&ctx.running, false);
    close(fd);                           /* worker's read returns EOF -> exits */
    pthread_join(worker, NULL);

    /* the worker should have reassembled the frame and written both BMPs */
    image_t in;
    if (bmp_read(&in, in_path) == 0) {
        CHECK(in.w == src.w && in.h == src.h, "reassembled dims %dx%d", in.w, in.h);
        CHECK(memcmp(in.pix, src.pix, total) == 0,
              "server-reassembled frame is byte-identical to source");
        image_free(&in);
    } else {
        CHECK(0, "input BMP not written by the worker");
    }
    image_t ed;
    CHECK(bmp_read(&ed, ed_path) == 0, "edges BMP written by the worker");
    if (ed.pix) image_free(&ed);

    remove(in_path); remove(ed_path);
    image_free(&src);
    ctx.sink->close();

    if (fails == 0) {
        printf("frame_e2e_test: ALL PASS (worker reassembled a frame -> edge BMP)\n");
        return 0;
    }
    printf("frame_e2e_test: %d FAILURES\n", fails);
    return 1;
}
