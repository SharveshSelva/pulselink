/* telemetry_stat.c — query or reset /dev/telemetry ring stats via ioctl.
 *
 *   telemetry_stat            print stats
 *   telemetry_stat --reset    reset the counters
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "telemetry_uapi.h"

int main(int argc, char **argv)
{
    int do_reset = (argc > 1 && strcmp(argv[1], "--reset") == 0);

    int fd = open("/dev/" PL_TELEMETRY_DEVNAME, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    if (do_reset) {
        if (ioctl(fd, PL_IOC_RESET) < 0) { perror("ioctl RESET"); close(fd); return 1; }
        printf("ring counters reset\n");
        close(fd);
        return 0;
    }

    struct pl_stats s;
    if (ioctl(fd, PL_IOC_GET_STATS, &s) < 0) { perror("ioctl GET_STATS"); close(fd); return 1; }
    printf("produced=%llu dropped=%llu in_ring=%u/%u\n",
           (unsigned long long)s.produced, (unsigned long long)s.dropped,
           s.in_ring, s.capacity);
    close(fd);
    return 0;
}
