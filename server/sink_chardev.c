/* sink_chardev.c — the "dev" record sink: writes each reading into the
 * /dev/telemetry kernel character device (Stage 3). The sensor_record_t layout
 * is identical to the kernel's struct telemetry_record, so a record goes
 * straight through write(2) with no repacking. Same machine on both ends, so
 * host byte order is correct (the big-endian wire format is only for the network
 * hop, not this local IPC).
 */
#define _GNU_SOURCE
#include "sink.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_PATH "/dev/telemetry"

static int g_fd = -1;

static int cd_open(void)
{
    const char *path = getenv("PULSELINK_DEV");   /* override for testing */
    if (!path) path = DEV_PATH;
    g_fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (g_fd < 0) {
        LOGF("sink dev: open %s failed: %s (is pulselink_telemetry.ko loaded?)",
             path, strerror(errno));
        return -1;
    }
    LOGF("sink dev: streaming records to %s", path);
    return 0;
}

static int cd_write_record(const sensor_record_t *rec)
{
    ssize_t n = write(g_fd, rec, sizeof *rec);
    if (n != (ssize_t)sizeof *rec) {
        LOGF("sink dev: short/failed write (%zd): %s", n, strerror(errno));
        return -1;
    }
    return 0;
}

static void cd_dump(FILE *out)
{
    fprintf(out, "==== sink dev: %s ====\n", DEV_PATH);
    FILE *p = fopen("/proc/pulselink", "r");   /* mirror the kernel's view */
    if (p) {
        char line[256];
        while (fgets(line, sizeof line, p))
            fputs(line, out);
        fclose(p);
    } else {
        fprintf(out, "  (/proc/pulselink unavailable)\n");
    }
    fprintf(out, "=====================================\n");
    fflush(out);
}

static void cd_close(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
}

const sink_t sink_chardev = {
    .name         = "dev(/dev/telemetry)",
    .open         = cd_open,
    .write_record = cd_write_record,
    .dump         = cd_dump,
    .close        = cd_close,
};
