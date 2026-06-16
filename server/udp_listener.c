/* udp_listener.c — receives PKT_HEARTBEAT datagrams and refreshes liveness.
 *
 * Why a plain recvfrom loop with a short SO_RCVTIMEO instead of the self-pipe:
 * the pipe has a single byte and a single consumer (the TCP accept loop). This
 * periodic loop just re-checks the running flag on each timeout tick, which is
 * fine for a non-latency-critical path and avoids a multi-reader pipe race.
 */
#define _GNU_SOURCE
#include "server.h"
#include "log.h"
#include "device_table.h"
#include "../common/wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

void *udp_listener_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("udp socket"); return NULL; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(ctx->cfg.udp_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("udp bind");
        close(fd);
        return NULL;
    }

    /* 500ms tick so we notice shutdown promptly */
    struct timeval tv = { 0, 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    LOGF("UDP heartbeat listener up on port %u", ctx->cfg.udp_port);

    uint8_t buf[PL_HDR_SIZE + PL_MAX_PAYLOAD];
    while (atomic_load(&ctx->running)) {
        struct sockaddr_in src;
        socklen_t slen = sizeof src;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
                             (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;                /* tick / signal: re-check running */
            perror("recvfrom");
            continue;
        }
        if ((size_t)n < PL_HDR_SIZE)
            continue;                    /* runt datagram */

        pl_header_t h;
        pl_parse_header(buf, &h);
        if (h.magic != PL_MAGIC || h.version != PL_VERSION)
            continue;
        if ((size_t)n < (size_t)PL_HDR_SIZE + h.payload_len)
            continue;                    /* truncated */
        if (pl_checksum(buf + PL_HDR_SIZE, h.payload_len) != h.checksum)
            continue;                    /* corrupt: silently drop (UDP) */
        if (h.type != PKT_HEARTBEAT)
            continue;

        uint32_t uptime = 0, heap = 0;
        if (h.payload_len >= 8) {
            memcpy(&uptime, buf + PL_HDR_SIZE,     4);
            memcpy(&heap,   buf + PL_HDR_SIZE + 4, 4);
            uptime = ntohl(uptime);
            heap   = ntohl(heap);
        }
        dt_touch(&ctx->devices, h.device_id, (long)time(NULL));
        LOGF("HEARTBEAT dev=0x%08X uptime=%ums heap=%uB", h.device_id, uptime, heap);
    }

    close(fd);
    LOGF("UDP listener shutting down");
    return NULL;
}
