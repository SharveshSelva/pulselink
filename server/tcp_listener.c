/* tcp_listener.c — accept loop.
 *
 * Design notes (interview talking points):
 *  - poll() waits on BOTH the listening socket and the self-pipe read end, so a
 *    SIGINT turns into a pollable event and we shut down immediately instead of
 *    being stuck in accept(). No work is done in the signal handler itself.
 *  - Concurrency is capped with a counting semaphore. We sem_trywait AFTER
 *    accept and *reject at the door* when full, rather than sem_wait-blocking
 *    the accept loop. That keeps the listener responsive to shutdown and avoids
 *    hiding overload inside the kernel's listen backlog.
 *  - Each connection gets a DETACHED worker thread: no one joins it, it cleans
 *    up after itself (closes fd, sem_post, frees its arg).
 */
#define _GNU_SOURCE
#include "server.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_listen_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {   /* backlog 16 per spec */
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

void *tcp_listener_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    ctx->listen_fd = make_listen_socket(ctx->cfg.tcp_port);
    if (ctx->listen_fd < 0) {
        atomic_store(&ctx->running, false);
        return NULL;
    }
    LOGF("TCP listener up on port %u (backlog 16)", ctx->cfg.tcp_port);

    struct pollfd fds[2];
    fds[0].fd = ctx->listen_fd;        fds[0].events = POLLIN;
    fds[1].fd = ctx->shutdown_pipe[0]; fds[1].events = POLLIN;

    while (atomic_load(&ctx->running)) {
        int pr = poll(fds, 2, -1);     /* block until a conn arrives or we're told to stop */
        if (pr < 0) {
            if (errno == EINTR)
                continue;              /* signal hit poll directly; loop re-checks running */
            perror("poll");
            break;
        }
        if (fds[1].revents & POLLIN)   /* shutdown byte arrived */
            break;
        if (!(fds[0].revents & POLLIN))
            continue;

        struct sockaddr_in cli;
        socklen_t clilen = sizeof cli;
        int cfd = accept(ctx->listen_fd, (struct sockaddr *)&cli, &clilen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            perror("accept");
            continue;
        }

        /* cap concurrency: reject (don't block) when full */
        if (sem_trywait(&ctx->client_slots) != 0) {
            LOGF("at capacity (%d clients), rejecting connection",
                 ctx->cfg.max_clients);
            close(cfd);
            continue;
        }

        worker_arg_t *wa = malloc(sizeof *wa);
        if (!wa) {
            LOGF("OOM allocating worker arg, dropping connection");
            sem_post(&ctx->client_slots);
            close(cfd);
            continue;
        }
        wa->ctx = ctx;
        wa->fd  = cfd;
        snprintf(wa->peer, sizeof wa->peer, "%s:%u",
                 inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&tid, &attr, client_worker, wa);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            LOGF("pthread_create failed (%d), dropping connection", rc);
            sem_post(&ctx->client_slots);
            close(cfd);
            free(wa);
            continue;
        }
        LOGF("accepted %s (worker spawned)", wa->peer);
    }

    close(ctx->listen_fd);
    ctx->listen_fd = -1;
    LOGF("TCP listener shutting down");
    return NULL;
}
