/* main.c — telemetryd entry point.
 *
 *   - parse args (-p tcp -u udp -n max_clients -s mem|dev)
 *   - async-signal-safe SIGINT/SIGTERM (self-pipe) and SIGUSR1 (dump flag)
 *   - spawn TCP listener, UDP heartbeat listener, and the reaper thread
 *   - on shutdown: join those three, drain detached workers, then tidy up
 */
#define _GNU_SOURCE
#include "server.h"
#include "log.h"
#include "device_table.h"
#include "sink.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static server_ctx_t *g_ctx = NULL;

static void on_stop(int sig)
{
    (void)sig;
    if (!g_ctx) return;
    atomic_store(&g_ctx->running, false);
    const char b = 'x';
    ssize_t r = write(g_ctx->shutdown_pipe[1], &b, 1);  /* wakes the listener */
    (void)r;
}

static void on_dump(int sig)
{
    (void)sig;
    if (g_ctx)
        atomic_store(&g_ctx->dump_requested, true);     /* reaper acts on it */
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-p tcp_port] [-u udp_port] [-n max_clients] [-s mem|dev] [-F frame_dir]\n"
        "  -p  TCP telemetry port    (default 9000)\n"
        "  -u  UDP heartbeat port    (default 9001)\n"
        "  -n  max concurrent clients(default 16)\n"
        "  -s  record sink: mem|dev  (default mem; dev arrives in Stage 3)\n"
        "  -T  auto-shutdown after N seconds (0 = run until signal; demo aid)\n",
        prog);
}

int main(int argc, char **argv)
{
    server_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg.tcp_port    = 9000;
    ctx.cfg.udp_port    = 9001;
    ctx.cfg.max_clients = 16;
    atomic_init(&ctx.running, true);
    atomic_init(&ctx.dump_requested, false);
    atomic_init(&ctx.active_workers, 0);
    ctx.listen_fd = -1;
    ctx.sink = &sink_ring_log;
    ctx.frame_dir = NULL;

    int opt;
    int deadline_s = 0;   /* -T: auto-shutdown after N seconds (demo/testing) */
    while ((opt = getopt(argc, argv, "p:u:n:s:F:T:h")) != -1) {
        switch (opt) {
        case 'p': ctx.cfg.tcp_port    = (uint16_t)atoi(optarg); break;
        case 'u': ctx.cfg.udp_port    = (uint16_t)atoi(optarg); break;
        case 'n': ctx.cfg.max_clients = atoi(optarg);           break;
        case 'T': deadline_s          = atoi(optarg);           break;
        case 's':
            if (strcmp(optarg, "mem") == 0) {
                ctx.sink = &sink_ring_log;
            } else if (strcmp(optarg, "dev") == 0) {
                ctx.sink = &sink_chardev;     /* /dev/telemetry (needs the module) */
            } else {
                usage(argv[0]); return 2;
            }
            break;
        case 'F': ctx.frame_dir = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (ctx.cfg.tcp_port == 0 || ctx.cfg.max_clients <= 0) {
        usage(argv[0]); return 2;
    }

    signal(SIGPIPE, SIG_IGN);

    if (pipe(ctx.shutdown_pipe) < 0) { perror("pipe"); return 1; }
    int fl = fcntl(ctx.shutdown_pipe[1], F_GETFL, 0);
    fcntl(ctx.shutdown_pipe[1], F_SETFL, fl | O_NONBLOCK);

    if (sem_init(&ctx.client_slots, 0, (unsigned)ctx.cfg.max_clients) < 0) {
        perror("sem_init"); return 1;
    }
    dt_init(&ctx.devices);
    if (ctx.sink->open() != 0) {
        if (ctx.sink == &sink_chardev) {       /* module not loaded? don't die */
            fprintf(stderr, "dev sink unavailable; falling back to mem sink\n");
            ctx.sink = &sink_ring_log;
            if (ctx.sink->open() != 0) { fprintf(stderr, "sink open failed\n"); return 1; }
        } else {
            fprintf(stderr, "sink open failed\n"); return 1;
        }
    }

    g_ctx = &ctx;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_dump;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_stop;            /* deadline fires the same clean path */
    sigaction(SIGALRM, &sa, NULL);
    if (deadline_s > 0)
        alarm((unsigned)deadline_s);

    LOGF("telemetryd starting: tcp=%u udp=%u max_clients=%d sink=%s (pid %d)",
         ctx.cfg.tcp_port, ctx.cfg.udp_port, ctx.cfg.max_clients,
         ctx.sink->name, (int)getpid());

    pthread_t tcp_tid, udp_tid, reap_tid;
    if (pthread_create(&tcp_tid, NULL, tcp_listener_thread, &ctx) != 0) {
        perror("pthread_create(tcp)"); return 1;
    }
    if (pthread_create(&udp_tid, NULL, udp_listener_thread, &ctx) != 0) {
        perror("pthread_create(udp)"); return 1;
    }
    if (pthread_create(&reap_tid, NULL, reaper_thread, &ctx) != 0) {
        perror("pthread_create(reaper)"); return 1;
    }

    pthread_join(tcp_tid, NULL);
    pthread_join(udp_tid, NULL);
    pthread_join(reap_tid, NULL);

    /* drain detached workers: they wake within WORKER_TICK_S and exit */
    int waited_ms = 0;
    while (atomic_load(&ctx.active_workers) > 0 && waited_ms < 5000) {
        usleep(100 * 1000);
        waited_ms += 100;
    }
    int leftover = atomic_load(&ctx.active_workers);
    if (leftover > 0)
        LOGF("warning: %d worker(s) still active at shutdown", leftover);

    ctx.sink->close();
    dt_destroy(&ctx.devices);
    sem_destroy(&ctx.client_slots);
    close(ctx.shutdown_pipe[0]);
    close(ctx.shutdown_pipe[1]);
    LOGF("telemetryd stopped cleanly");
    return 0;
}
