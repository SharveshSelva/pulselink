/* fake_client.c — load/test rig for telemetryd.
 *
 *   ./fake_client -h host -p port -d device_id -r rate_hz -n count
 *                 [--bad-checksum] [--flood]
 *
 * M2 behaviour: TCP connect -> HELLO handshake (prints the assigned session) ->
 * stream `count` synthetic sine-wave sensor readings at `rate_hz`, printing any
 * server replies (PKT_ERROR / future PKT_ACK). --bad-checksum corrupts every
 * packet's checksum to prove the server's 3-strike error handling and drop.
 *
 * UDP heartbeats + the second heartbeat thread arrive in M3.
 */
#define _GNU_SOURCE
#include "../common/wire.h"
#include "../common/netutil.h"
#include "../imaging/image.h"
#include "../imaging/frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *type_name(uint8_t t)
{
    switch (t) {
    case PKT_HELLO_ACK: return "HELLO_ACK";
    case PKT_ACK:       return "ACK";
    case PKT_ERROR:     return "ERROR";
    default:            return "?";
    }
}

static const char *err_name(uint16_t c)
{
    switch (c) {
    case PL_ERR_BAD_MAGIC:    return "BAD_MAGIC";
    case PL_ERR_BAD_VERSION:  return "BAD_VERSION";
    case PL_ERR_BAD_CHECKSUM: return "BAD_CHECKSUM";
    case PL_ERR_BAD_TYPE:     return "BAD_TYPE";
    case PL_ERR_TOO_BIG:      return "TOO_BIG";
    case PL_ERR_NO_HELLO:     return "NO_HELLO";
    default:                  return "?";
    }
}

/* Receive one packet. Returns:
 *   >0  : packet type (payload copied to buf, *plen set)
 *    0  : server closed the connection (EOF)
 *   -1  : error/timeout (errno == EAGAIN/EWOULDBLOCK on SO_RCVTIMEO) */
static int recv_packet(int fd, uint8_t *buf, uint16_t *plen, pl_header_t *h)
{
    uint8_t hdr[PL_HDR_SIZE];
    ssize_t r = read_n(fd, hdr, PL_HDR_SIZE);
    if (r == 0) return 0;
    if (r != PL_HDR_SIZE) return -1;

    pl_parse_header(hdr, h);
    if (h->payload_len > PL_MAX_PAYLOAD) return -1;
    if (h->payload_len) {
        r = read_n(fd, buf, h->payload_len);
        if (r != (ssize_t)h->payload_len) return (r == 0) ? 0 : -1;
    }
    *plen = h->payload_len;
    return h->type;
}


/* ---- UDP heartbeat thread (spec: heartbeats from a second thread) ---- */
typedef struct {
    const char *host;
    uint16_t    udp_port;
    uint32_t    devid;
    atomic_bool running;
} hb_arg_t;

static void *heartbeat_thread(void *arg)
{
    hb_arg_t *hb = (hb_arg_t *)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(hb->udp_port);
    inet_pton(AF_INET, hb->host, &dst.sin_addr);

    uint8_t out[PL_HDR_SIZE + 8];
    uint32_t uptime = 0;
    while (atomic_load(&hb->running)) {
        pl_heartbeat_payload_t p = { .uptime_ms = htonl(uptime),
                                     .free_heap = htonl(180000) };
        size_t n = pl_build(out, sizeof out, PKT_HEARTBEAT, hb->devid, &p, sizeof p);
        sendto(fd, out, n, 0, (struct sockaddr *)&dst, sizeof dst);
        uptime += 1000;
        /* 1s heartbeat for a snappy demo (spec cadence is 5s) */
        for (int i = 0; i < 10 && atomic_load(&hb->running); i++)
            usleep(100 * 1000);
    }
    close(fd);
    return NULL;
}

static int connect_to(const char *host, uint16_t port)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char **argv)
{
    const char *host  = "127.0.0.1";
    uint16_t    port  = 9000;
    uint16_t    uport = 9001;
    uint32_t    devid = 0x1001;
    double      rate  = 1.0;     /* Hz */
    int         count = 10;
    bool        bad   = false;
    bool        flood = false;

    static struct option lo[] = {
        {"bad-checksum", no_argument, 0, 'B'},
        {"flood",        no_argument, 0, 'F'},
        {"frame",        no_argument, 0, 'M'},
        {0, 0, 0, 0}
    };
    bool        send_frame = false;
    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:u:d:r:n:", lo, NULL)) != -1) {
        switch (opt) {
        case 'h': host  = optarg;                       break;
        case 'p': port  = (uint16_t)atoi(optarg);       break;
        case 'u': uport = (uint16_t)atoi(optarg);       break;
        case 'd': devid = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'r': rate  = atof(optarg);                 break;
        case 'n': count = atoi(optarg);                 break;
        case 'B': bad   = true;                         break;
        case 'F': flood = true;                         break;
        case 'M': send_frame = true;                    break;
        default:
            fprintf(stderr, "usage: %s -h host -p port -d id -r hz -n count"
                            " [--bad-checksum] [--flood] [--frame]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);   /* server drop mid-send -> EPIPE, not death */

    int fd = connect_to(host, port);
    if (fd < 0) {
        fprintf(stderr, "connect to %s:%u failed\n", host, port);
        return 1;
    }
    printf("connected to %s:%u as device 0x%08X%s\n",
           host, port, devid, bad ? "  [bad-checksum mode]" : "");

    uint8_t out[PL_HDR_SIZE + PL_MAX_PAYLOAD];
    uint8_t in[PL_MAX_PAYLOAD];
    uint16_t plen;
    pl_header_t rh;

    /* ---- HELLO handshake -------------------------------------------- */
    pl_hello_payload_t hello = { .firmware_version = htonl(0x00010000),
                                 .capabilities     = htonl(0x1) };
    size_t n = pl_build(out, sizeof out, PKT_HELLO, devid, &hello, sizeof hello);
    if (write_n(fd, out, n) != (ssize_t)n) { perror("send HELLO"); close(fd); return 1; }

    /* HELLO_ACK must arrive within 2 s (spec rule) */
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int t = recv_packet(fd, in, &plen, &rh);
    if (t == PKT_HELLO_ACK && plen == 4) {
        uint32_t sid;
        memcpy(&sid, in, 4);
        printf("HELLO_ACK: session_id=%u\n", ntohl(sid));
    } else if (t == 0) {
        printf("server closed before HELLO_ACK\n"); close(fd); return 1;
    } else {
        printf("no/invalid HELLO_ACK (t=%d)\n", t); close(fd); return 1;
    }

    /* ---- one-shot frame mode (Stage 4): send a synthetic frame, exit ---- */
    if (send_frame) {
        image_t img;
        if (image_alloc(&img, 160, 120) != 0) { close(fd); return 1; }
        image_test_pattern(&img);
        uint32_t total = (uint32_t)img.w * img.h;
        uint16_t cc = frame_chunk_count(total);
        for (uint16_t i = 0; i < cc; i++) {
            uint32_t off = (uint32_t)i * FRAME_CHUNK_MAX;
            uint32_t len = total - off; if (len > FRAME_CHUNK_MAX) len = FRAME_CHUNK_MAX;
            uint8_t pkt[PL_HDR_SIZE + 12 + FRAME_CHUNK_MAX];
            size_t m = frame_chunk_build(pkt, sizeof pkt, devid, (uint16_t)img.w,
                                         (uint16_t)img.h, 1, i, cc,
                                         img.pix + off, (uint16_t)len);
            if (write_n(fd, pkt, m) != (ssize_t)m) { perror("send FRAME"); break; }
        }
        printf("sent a %dx%d frame as %u PKT_FRAME chunks\n", img.w, img.h, cc);
        image_free(&img);
        close(fd);
        return 0;
    }

    /* ---- stream sensor readings ------------------------------------- */
    tv.tv_sec = 0; tv.tv_usec = 150000;          /* 150ms poll for replies */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    hb_arg_t hb = { .host = host, .udp_port = uport, .devid = devid };
    atomic_init(&hb.running, true);
    pthread_t hb_tid;
    pthread_create(&hb_tid, NULL, heartbeat_thread, &hb);
    printf("UDP heartbeats -> %s:%u every 1s\n", host, uport);

    int sent = 0, errors = 0;
    for (uint32_t seq = 1; (int)seq <= count; seq++) {
        double rad = 2.0 * M_PI * ((double)seq / 20.0);
        int32_t temp = (int32_t)(2500.0 + 500.0 * sin(rad));   /* centi-degC */
        int32_t hum  = (int32_t)(5000.0 + 1000.0 * cos(rad));  /* centi-%    */

        pl_sensor_payload_t s = {
            .seq          = htonl(seq),
            .timestamp_ms = htonl(seq * (rate > 0 ? (uint32_t)(1000.0 / rate) : 1)),
            .temperature  = (int32_t)htonl((uint32_t)temp),
            .humidity     = (int32_t)htonl((uint32_t)hum),
        };
        n = pl_build(out, sizeof out, PKT_SENSOR, devid, &s, sizeof s);
        if (bad)
            out[10] ^= 0xFF;   /* corrupt the checksum field (offset 10..11) */

        if (write_n(fd, out, n) != (ssize_t)n) {
            printf("server dropped us mid-send after %d packets\n", sent);
            break;
        }
        sent++;

        /* drain any server replies (errors now, ACKs in M3) */
        int rt = recv_packet(fd, in, &plen, &rh);
        if (rt == 0) { printf("server closed connection after %d packets\n", sent); break; }
        if (rt == PKT_ERROR && plen == 2) {
            uint16_t code = ntohs(*(uint16_t *)in);
            printf("<- %s %s\n", type_name((uint8_t)rt), err_name(code));
            errors++;
        } else if (rt > 0) {
            printf("<- %s\n", type_name((uint8_t)rt));
        }

        if (!flood && rate > 0)
            usleep((useconds_t)(1e6 / rate));
    }

    atomic_store(&hb.running, false);
    pthread_join(hb_tid, NULL);
    printf("done: sent=%d errors=%d\n", sent, errors);
    close(fd);
    return 0;
}
