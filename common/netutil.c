/* netutil.c — see netutil.h */
#define _GNU_SOURCE
#include "netutil.h"

#include <errno.h>
#include <sys/socket.h>

ssize_t read_n(int fd, void *buf, size_t n)
{
    size_t got = 0;
    char *p = (char *)buf;

    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;          /* interrupted by a signal: retry */
            return -1;             /* real error or SO_RCVTIMEO timeout */
        }
        if (r == 0)
            break;                 /* orderly shutdown by peer (EOF) */
        got += (size_t)r;
    }
    return (ssize_t)got;
}

ssize_t write_n(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    const char *p = (const char *)buf;

    while (sent < n) {
        ssize_t w = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return (ssize_t)sent;
}
