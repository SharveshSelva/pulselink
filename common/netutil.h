/* netutil.h — robust stream I/O helpers shared by the server and tools.
 *
 * TCP is a byte stream, not a message stream: one recv() can return a partial
 * header, two headers, or anything in between. These helpers loop until the
 * exact byte count is satisfied so the rest of the code can think in messages.
 */
#ifndef PULSELINK_NETUTIL_H
#define PULSELINK_NETUTIL_H

#include <stddef.h>
#include <sys/types.h>   /* ssize_t */

/* read_n: read exactly n bytes into buf.
 *   returns n            -> success (all bytes read)
 *   returns 0..n-1       -> peer closed the connection (short read / EOF)
 *   returns -1           -> error; errno is set.
 *                           errno == EAGAIN/EWOULDBLOCK means an SO_RCVTIMEO
 *                           timeout fired (caller treats as a dead peer).
 * EINTR is retried internally. */
ssize_t read_n(int fd, void *buf, size_t n);

/* write_n: write exactly n bytes from buf (loops over send, retries EINTR).
 * Uses MSG_NOSIGNAL so a vanished peer yields EPIPE instead of killing us.
 *   returns n   -> success
 *   returns -1  -> error; errno is set. */
ssize_t write_n(int fd, const void *buf, size_t n);

#endif /* PULSELINK_NETUTIL_H */
