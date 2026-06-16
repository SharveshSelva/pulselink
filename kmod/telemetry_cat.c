/* telemetry_cat.c — read live records from /dev/telemetry and print them.
 *
 *   telemetry_cat [device]      (default /dev/telemetry)
 *
 * The kernel side does the blocking, so this is a plain read() loop: each call
 * returns one or more whole records once any are available.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "telemetry_uapi.h"

#define REC_SZ (sizeof(struct telemetry_record))

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/" PL_TELEMETRY_DEVNAME;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "is the module loaded?  sudo insmod pulselink_telemetry.ko\n");
        return 1;
    }

    struct telemetry_record recs[32];
    for (;;) {
        ssize_t n = read(fd, recs, sizeof recs);
        if (n < 0) { perror("read"); break; }
        if (n == 0) continue;

        size_t cnt = (size_t)n / REC_SZ;
        for (size_t i = 0; i < cnt; i++) {
            struct telemetry_record *r = &recs[i];
            int t = r->temperature, h = r->humidity;
            printf("dev=0x%08X seq=%-6u  %4d.%02d C  %4d.%02d %%  ts=%llu\n",
                   r->device_id, r->seq,
                   t / 100, abs(t % 100), h / 100, abs(h % 100),
                   (unsigned long long)r->server_timestamp_ns);
        }
        fflush(stdout);
    }

    close(fd);
    return 0;
}
