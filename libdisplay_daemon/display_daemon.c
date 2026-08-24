#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "display_daemon.h"
#include "../common/protocol.h"
#include "../common/socket_utils.h"

#define MAX_EVENTS 16
/* Hello fd set: { buf_ready, fence, data, shm, audio }. The daemon only relays
 * the fds; it never interprets the slots. */
#define MAX_FDS    5
#define CTRL_IO_TIMEOUT_MS 1000

struct client {
    int  ctrl_fd;
    bool is_consumer;
};

struct daemon_ctx {
    struct client *consumer;
    struct client *producer;
    int epoll_fd;
    int listen_fd;
    volatile bool running;

    struct screen_info stored_screen;
    bool has_screen_info;

    int deposited_fds[MAX_FDS];
    int deposited_fd_count;

    bool producer_waiting_screen;
    bool producer_waiting_fds;

    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static void client_free(daemon_ctx *ctx, struct client *c)
{
    if (!c) return;
    if (c->ctrl_fd >= 0) {
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, c->ctrl_fd, NULL);
        close(c->ctrl_fd);
    }
    free(c);
}

static void close_fd_array(int *fds, int count)
{
    for (int i = 0; i < count; i++)
        close(fds[i]);
}

static void clear_deposited_fds(daemon_ctx *ctx)
{
    close_fd_array(ctx->deposited_fds, ctx->deposited_fd_count);
    ctx->deposited_fd_count = 0;
}

static int send_ctrl(int fd, uint32_t type)
{
    struct ctrl_msg msg = { .type = type, .size = 0 };
    int64_t deadline = socket_deadline_after_ms(CTRL_IO_TIMEOUT_MS);
    return deadline < 0 ? -1 :
           send_all_deadline(fd, &msg, sizeof(msg), deadline);
}

static void drop_client(daemon_ctx *ctx, struct client *c);

static int send_screen_info_msg(daemon_ctx *ctx, int fd)
{
    struct ctrl_msg hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) };
    uint8_t buf[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &ctx->stored_screen, sizeof(ctx->stored_screen));
    int64_t deadline = socket_deadline_after_ms(CTRL_IO_TIMEOUT_MS);
    return deadline < 0 ? -1 : send_all_deadline(fd, buf, sizeof(buf), deadline);
}

static bool try_deliver_fds(daemon_ctx *ctx)
{
    if (!ctx->producer || !ctx->consumer || ctx->deposited_fd_count != MAX_FDS) {
        ctx->producer_waiting_fds = true;
        return true;
    }

    struct client *producer = ctx->producer;
    struct client *consumer = ctx->consumer;
    struct ctrl_msg msg = { .type = CTRL_MSG_FDS_READY, .size = 0 };
    int64_t deadline = socket_deadline_after_ms(CTRL_IO_TIMEOUT_MS);
    bool delivered = deadline >= 0 && send_fds_deadline(
        producer->ctrl_fd, &msg, sizeof(msg), ctx->deposited_fds,
        ctx->deposited_fd_count, deadline) == 0;
    bool acknowledged = delivered &&
        send_ctrl(consumer->ctrl_fd, CTRL_MSG_FDS_READY) == 0;
    if (!acknowledged) {
        fprintf(stderr, "daemon: fd delivery failed; resetting both peers\n");
        if (ctx->producer == producer)
            drop_client(ctx, producer);
        if (ctx->consumer == consumer)
            drop_client(ctx, consumer);
        return false;
    }

    clear_deposited_fds(ctx);
    ctx->producer_waiting_fds = false;
    fprintf(stderr, "daemon: fds delivered to producer\n");
    return true;
}

/*
 * Tear down whichever role this client holds, reset the associated state, then free
 * it. Used both for real disconnects (EPOLLHUP / recv error) and to evict a stale
 * client when a new one takes over the same role -- whether the old client is still
 * alive or already a ghost, finding one is reason enough to drop it.
 *
 * The role pointer is cleared BEFORE client_free() so that any event still queued for
 * this client in the current epoll batch fails the "is it a current role?" guard in
 * the main loop and is skipped instead of dereferencing freed memory.
 */
static void drop_client(daemon_ctx *ctx, struct client *c)
{
    if (!c)
        return;
    if (c == ctx->consumer) {
        fprintf(stderr, "daemon: consumer disconnected\n");
        ctx->consumer = NULL;
        /* The deposited fds belong to the consumer that just left; a future producer
         * must never be handed this stale set. Drop them together with the consumer. */
        clear_deposited_fds(ctx);
    } else if (c == ctx->producer) {
        fprintf(stderr, "daemon: producer disconnected\n");
        ctx->producer = NULL;
        ctx->producer_waiting_screen = false;
        ctx->producer_waiting_fds = false;
    }
    client_free(ctx, c);
}

static void handle_client_data(daemon_ctx *ctx, struct client *c)
{
    struct ctrl_msg hdr;
    int fds[MAX_FDS];
    int fd_count = 0;

    int64_t deadline = socket_deadline_after_ms(CTRL_IO_TIMEOUT_MS);
    int n = deadline < 0 ? -1 : recv_fds_deadline(
        c->ctrl_fd, &hdr, sizeof(hdr), fds, MAX_FDS, &fd_count, deadline);
    if (n != (int)sizeof(hdr)) {
        drop_client(ctx, c);
        return;
    }

    uint8_t payload[sizeof(struct screen_info)];
    if (hdr.size > sizeof(payload) ||
        (hdr.size > 0 && recv_all_deadline(
            c->ctrl_fd, payload, hdr.size, deadline) < 0)) {
        close_fd_array(fds, fd_count);
        drop_client(ctx, c);
        return;
    }

    bool accepted_fds = false;
    bool valid = true;
    switch (hdr.type) {
    case CTRL_MSG_CONSUMER_HELLO:
        valid = c == ctx->consumer && hdr.size == 0 && fd_count == MAX_FDS;
        if (valid) {
            clear_deposited_fds(ctx);
            memcpy(ctx->deposited_fds, fds, sizeof(int) * fd_count);
            ctx->deposited_fd_count = fd_count;
            accepted_fds = true;
            fprintf(stderr, "daemon: consumer re-deposited %d fds\n", fd_count);
            if (ctx->producer_waiting_fds && !try_deliver_fds(ctx))
                return;
        }
        break;

    case CTRL_MSG_SCREEN_INFO:
        valid = c == ctx->consumer && hdr.size == sizeof(struct screen_info) &&
                fd_count == 0;
        if (valid) {
            struct screen_info si;
            memcpy(&si, payload, sizeof(si));
            /* Always accept the consumer's screen info, even if it differs from a
             * previous connection — the Android display may have rotated or switched
             * resolution. Overwrite and forward to any waiting producer. */
            ctx->stored_screen = si;
            ctx->has_screen_info = true;
            fprintf(stderr, "daemon: screen info %ux%u fmt=%u\n",
                    si.width, si.height, si.format);
            if (ctx->producer_waiting_screen && ctx->producer) {
                struct client *producer = ctx->producer;
                if (send_screen_info_msg(ctx, producer->ctrl_fd) < 0)
                    drop_client(ctx, producer);
                else
                    ctx->producer_waiting_screen = false;
            }
        }
        break;

    case CTRL_MSG_PICKUP_FDS:
        valid = c == ctx->producer && hdr.size == 0 && fd_count == 0;
        if (valid && !try_deliver_fds(ctx))
            return;
        break;

    default:
        valid = false;
        break;
    }
    if (!accepted_fds)
        close_fd_array(fds, fd_count);
    if (!valid)
        drop_client(ctx, c);
}

static void handle_new_connection(daemon_ctx *ctx, int listen_fd)
{
    int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client_fd < 0)
        return;

    struct ctrl_msg hdr;
    int fds[MAX_FDS];
    int fd_count = 0;
    int64_t deadline = socket_deadline_after_ms(CTRL_IO_TIMEOUT_MS);
    int n = deadline < 0 ? -1 : recv_fds_deadline(
        client_fd, &hdr, sizeof(hdr), fds, MAX_FDS, &fd_count, deadline);
    bool consumer_hello = n == (int)sizeof(hdr) &&
                          hdr.type == CTRL_MSG_CONSUMER_HELLO && hdr.size == 0 &&
                          fd_count == MAX_FDS;
    bool producer_hello = n == (int)sizeof(hdr) &&
                          hdr.type == CTRL_MSG_PRODUCER_HELLO && hdr.size == 0 &&
                          fd_count == 0;
    if (!consumer_hello && !producer_hello) {
        close_fd_array(fds, fd_count);
        close(client_fd);
        return;
    }

    struct client *c = calloc(1, sizeof(*c));
    if (!c) {
        close_fd_array(fds, fd_count);
        close(client_fd);
        return;
    }
    c->ctrl_fd = client_fd;

    if (consumer_hello) {
        /* Evict any prior consumer (alive or ghost) and its stale deposit before this
         * one takes over the role. */
        if (ctx->consumer)
            drop_client(ctx, ctx->consumer);
        c->is_consumer = true;
        ctx->consumer = c;

        clear_deposited_fds(ctx);
        memcpy(ctx->deposited_fds, fds, sizeof(int) * fd_count);
        ctx->deposited_fd_count = fd_count;
        fprintf(stderr, "daemon: consumer connected, %d fds\n", fd_count);

        if (ctx->producer_waiting_fds && !try_deliver_fds(ctx))
            return;

    } else if (producer_hello) {
        /* Evict any prior producer (alive or ghost) before this one takes over. */
        if (ctx->producer)
            drop_client(ctx, ctx->producer);
        c->is_consumer = false;
        ctx->producer = c;
        ctx->producer_waiting_screen = false;
        ctx->producer_waiting_fds = false;
        fprintf(stderr, "daemon: producer connected\n");

        if (ctx->has_screen_info) {
            if (send_screen_info_msg(ctx, client_fd) < 0) {
                drop_client(ctx, c);
                return;
            }
        } else {
            ctx->producer_waiting_screen = true;
        }

    } else {
        close(client_fd);
        free(c);
        return;
    }

    struct epoll_event ev = { .events = EPOLLIN | EPOLLHUP | EPOLLERR, .data.ptr = c };
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
}

int daemon_create(daemon_ctx **out, const char *sock_path)
{
    daemon_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->epoll_fd = -1;
    ctx->listen_fd = -1;
    ctx->running = true;
    strncpy(ctx->sock_path, sock_path, sizeof(ctx->sock_path) - 1);

    unlink(sock_path);
    ctx->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        perror("socket");
        daemon_destroy(ctx);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        daemon_destroy(ctx);
        return -1;
    }
    if (listen(ctx->listen_fd, 4) < 0) {
        perror("listen");
        daemon_destroy(ctx);
        return -1;
    }

    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd < 0) {
        perror("epoll_create1");
        daemon_destroy(ctx);
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);

    fprintf(stderr, "daemon: listening on %s\n", sock_path);

    *out = ctx;
    return 0;
}

void daemon_run(daemon_ctx *ctx)
{
    struct epoll_event events[MAX_EVENTS];
    while (ctx->running) {
        int nfds = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, 1000);
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                handle_new_connection(ctx, ctx->listen_fd);
            } else {
                struct client *c = events[i].data.ptr;
                /* A client freed earlier in this same batch -- a real disconnect, or
                 * one evicted by a new connection taking over its role -- leaves a
                 * stale event behind. Skip anything that is no longer a current role
                 * so we never touch freed memory. */
                if (c != ctx->consumer && c != ctx->producer)
                    continue;
                if (events[i].events & (EPOLLHUP | EPOLLERR))
                    drop_client(ctx, c);
                else
                    handle_client_data(ctx, c);
            }
        }
    }
}

void daemon_stop(daemon_ctx *ctx)
{
    if (ctx)
        ctx->running = false;
}

void daemon_destroy(daemon_ctx *ctx)
{
    if (!ctx)
        return;

    clear_deposited_fds(ctx);
    client_free(ctx, ctx->consumer);
    client_free(ctx, ctx->producer);
    if (ctx->listen_fd >= 0)
        close(ctx->listen_fd);
    if (ctx->epoll_fd >= 0)
        close(ctx->epoll_fd);
    if (ctx->sock_path[0])
        unlink(ctx->sock_path);
    fprintf(stderr, "daemon: shutdown\n");
    free(ctx);
}
