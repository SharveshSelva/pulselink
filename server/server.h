/* server.h — shared server state and thread entry points. */
#ifndef PULSELINK_SERVER_H
#define PULSELINK_SERVER_H

#include <stdatomic.h>
#include <semaphore.h>
#include <stdint.h>

#include "device_table.h"
#include "sink.h"

typedef struct {
    uint16_t tcp_port;     /* -p */
    uint16_t udp_port;     /* -u */
    int      max_clients;  /* -n */
} server_config_t;

typedef struct {
    server_config_t cfg;
    atomic_bool     running;          /* cleared on SIGINT/SIGTERM */
    atomic_bool     dump_requested;   /* set by SIGUSR1, acted on by reaper */
    atomic_int      active_workers;   /* live worker threads (drain on exit) */
    sem_t           client_slots;     /* counting sem, init = max_clients */
    int             listen_fd;        /* TCP listening socket */
    int             shutdown_pipe[2]; /* self-pipe: [1] from handler, [0] polled */
    device_table_t  devices;
    const sink_t   *sink;             /* record sink (mem ring_log in M3) */
    const char     *frame_dir;        /* -F: where to write frame BMPs (NULL=off) */
} server_ctx_t;

/* Per-connection argument handed to a detached worker (heap-owned). */
typedef struct {
    server_ctx_t *ctx;
    int           fd;
    char          peer[64];   /* "ip:port" for logging */
} worker_arg_t;

void *tcp_listener_thread(void *arg); /* arg: server_ctx_t*  */
void *udp_listener_thread(void *arg); /* arg: server_ctx_t*  */
void *reaper_thread(void *arg);       /* arg: server_ctx_t*  */
void *client_worker(void *arg);       /* arg: worker_arg_t* (frees it) */

#endif /* PULSELINK_SERVER_H */
