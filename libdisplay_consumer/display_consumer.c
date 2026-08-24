#define _GNU_SOURCE
#include "display_consumer.h"
#include "../common/socket_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CONTROL_IO_TIMEOUT_MS 1000
#define DATA_IO_TIMEOUT_MS 1000

struct display_ctx {
    int      ctrl_fd;
    int      data_fd;
    int      buf_ready_efd;
    int      fence_fd;        /* read end of the dedicated render-done fence channel */
    int      shm_fd;
    int      audio_fd;        /* local end of the bidirectional audio socketpair (hello slot 4) */
    volatile uint32_t *shm_ptr;
    uint32_t screen_w, screen_h;
    uint32_t pixel_format;
    atomic_bool fallback;
    atomic_bool control_dead;
    atomic_bool aborting;
    uint64_t    transport_generation;
    uint64_t    frame_generation;
    uint64_t    event_generation;
    bool        buffer_pending;

    /* The display lib is called concurrently: the render thread (select_dmabuf /
     * refresh_done / push_dmabufs), the event thread (poll_output_event /
     * handle_resource_request) and JNI input threads (push_input_event*). Two locks
     * with a fixed order (state_lock -> data_lock) tame the resulting races:
     *   - state_lock guards `fallback`, every fd field, `shm_ptr` and `buffer_pending`
     *     (i.e. the whole connection lifecycle mutated by enter_fallback).
     *   - data_lock serialises every complete framed write to data_fd.
     * Invariant: never call enter_fallback() or a user callback while holding either
     * lock (they re-acquire / re-enter). */
    pthread_mutex_t state_lock;
    pthread_mutex_t data_lock;

    int              stored_fds[MAX_BUFS];
    struct buf_info  stored_infos[MAX_BUFS];
    int              stored_count;

    void (*fallback_cb)(void *);
    void (*exit_fallback_cb)(void *);
    void  *fallback_userdata;
    void  *exit_fallback_userdata;
    struct service_info *services;
    int             num_services;
    struct resources *resources;
};

static bool is_fallback(const display_ctx *ctx)
{
    return atomic_load_explicit(&ctx->fallback, memory_order_acquire);
}

static void set_fallback(display_ctx *ctx, bool fallback)
{
    atomic_store_explicit(&ctx->fallback, fallback, memory_order_release);
}

static void set_control_dead(display_ctx *ctx)
{
    atomic_store_explicit(&ctx->control_dead, true, memory_order_release);
}

static int dup_cloexec(int fd)
{
    return fd < 0 ? -1 : fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int recv_exact_progress(int fd, void *buffer, size_t size, int timeout_ms)
{
    uint8_t *cursor = buffer;
    size_t received = 0;
    int64_t deadline = monotonic_ms();
    if (deadline < 0)
        return -1;
    deadline += timeout_ms;

    while (received < size) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0)
            return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0 || (pfd.revents & (POLLERR | POLLNVAL)) ||
            (!(pfd.revents & POLLIN) && (pfd.revents & POLLHUP)))
            return -1;
        if (!(pfd.revents & POLLIN))
            continue;
        ssize_t count = recv(fd, cursor + received, size - received, MSG_DONTWAIT);
        if (count > 0) {
            received += (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return -1;
    }
    return 0;
}

void allocate_services(struct display_ctx *ctx, struct service_info *services, int num_services){
    ctx->services = services;
    ctx->num_services = num_services;
    ctx->resources = (struct resources*)malloc(sizeof(struct resources) * num_services);
    if (!ctx->resources) {
        ctx->num_services = 0;
        return;
    }
    for(int i=0;i<num_services;i++){
        ctx->resources[i].service_type = services[i].type;
        ctx->resources[i].type = -1;//unallocated
        ctx->resources[i].num = 0;
        ctx->resources[i].fds = NULL;
    }
}
void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event, int* fds, int fd_count);
void handle_resource_request(struct display_ctx *ctx, struct OutputEvent *event){
    uint32_t service_type = event->resources_request.type;
    uint8_t found = 0;
    int i;
    for(i=0;i<ctx->num_services;i++){
        if(ctx->services[i].type == service_type){
            //if failed fds=NULL, num=0
            uint32_t args[3];
            memcpy(args, event->resources_request.args, sizeof(args));
            struct resources res = ctx->services[i].allocate_resource(
                args, ctx->services[i].userdata);
            if (ctx->resources[i].type != -1) {
                // free previous resource if it was allocated
                ctx->services[i].free_resource(ctx->resources[i], ctx->services[i].userdata);
            }
            ctx->resources[i] = res;
            found = 1;
            break;
        }
    }
    if(!found)
        return; //do nothing if the service type is not found, the producer will not enable the service if it not received the resources back
    //send resources back to producer
    struct InputEvent input_event;
    input_event.type = INPUT_TYPE_RESOURCE;
    input_event.resource.type = service_type;
    input_event.resource.fdnum = ctx->resources[i].num;
    push_input_event_with_fds(ctx, &input_event, ctx->resources[i].fds, ctx->resources[i].num);

}
void free_resources(struct display_ctx *ctx){//释放资源，保留服务信息
    for(int i=0;i<ctx->num_services;i++){
        if(ctx->resources[i].type != -1){
            ctx->services[i].free_resource(ctx->resources[i], ctx->services[i].userdata);
            ctx->resources[i].type = -1;
            ctx->resources[i].num = 0;
            ctx->resources[i].fds = NULL;
        }
    }
}
static int create_shm(display_ctx *ctx)
{
    ctx->shm_fd = memfd_create("buf_select", MFD_CLOEXEC);
    if (ctx->shm_fd < 0)
        return -1;
    if (ftruncate(ctx->shm_fd, sizeof(uint32_t)) < 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        ctx->shm_ptr = NULL;
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    *ctx->shm_ptr = 0;
    return 0;
}

static int send_hello_fds(display_ctx *ctx)
{
    /* Three dedicated socketpairs:
     *   - data:  consumer->producer input/bufs (reverse direction reserved for future)
     *   - fence: producer->consumer render-done messages; the message itself is the
     *            "frame rendered" signal (no separate eventfd, no cross-channel ordering).
     *   - audio: full-duplex PCM -- producer writes playback, consumer writes mic.
     * We keep one end of each and hand the other to the producer. The fd slot order
     * must match the producer's pickup_fds(): { buf_ready, fence, data, shm, audio }. */
    int sv[2], fv[2], av[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fv) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    /* SEQPACKET: each PCM/format message is one atomic datagram, so neither end can
     * desync mid-frame the way a byte stream could on a partial send/recv. */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, av) < 0) {
        close(sv[0]);
        close(sv[1]);
        close(fv[0]);
        close(fv[1]);
        return -1;
    }
    ctx->data_fd  = sv[0];
    ctx->fence_fd = fv[0];
    ctx->audio_fd = av[0];
    struct timeval send_timeout = { .tv_sec = 1, .tv_usec = 0 };
    if (setsockopt(ctx->data_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &send_timeout, sizeof(send_timeout)) < 0) {
        close(sv[1]);
        close(fv[1]);
        close(av[1]);
        return -1;
    }

    struct ctrl_msg hdr = { .type = CTRL_MSG_CONSUMER_HELLO, .size = 0 };
    int fds[5] = { ctx->buf_ready_efd, fv[1], sv[1], ctx->shm_fd, av[1] };
    int64_t deadline = socket_deadline_after_ms(CONTROL_IO_TIMEOUT_MS);
    int ret = deadline < 0 ? -1 : send_fds_deadline(
        ctx->ctrl_fd, &hdr, sizeof(hdr), fds, 5, deadline);
    close(sv[1]);
    close(fv[1]);
    close(av[1]);
    return ret;
}

static void enter_fallback(display_ctx *ctx);
static void enter_fallback_if_generation(display_ctx *ctx, uint64_t generation);
static void reset_transport_locked(display_ctx *ctx);

static bool push_dmabufs_locked(display_ctx *ctx)
{
    if (ctx->stored_count <= 0)
        return true;

    struct data_msg dhdr = {
        .type = DATA_MSG_BUFS_READY,
        .size = ctx->stored_count * sizeof(struct buf_info),
    };
    int fd = ctx->data_fd;
    int64_t deadline = socket_deadline_after_ms(DATA_IO_TIMEOUT_MS);
    return fd >= 0 && deadline >= 0 &&
           send_fds_deadline(fd, &dhdr, sizeof(dhdr), ctx->stored_fds,
                             ctx->stored_count, deadline) >= 0 &&
           send_all_deadline(fd, ctx->stored_infos,
                             ctx->stored_count * sizeof(struct buf_info), deadline) == 0;
}

static int push_dmabufs_internal(display_ctx *ctx)
{
    pthread_mutex_lock(&ctx->data_lock);
    if (is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->data_lock);
        return 0;
    }
    bool ok = push_dmabufs_locked(ctx);
    pthread_mutex_unlock(&ctx->data_lock);

    if (!ok) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}

/* Self-contained fallback->active transition: safe to call from any thread/site (the
 * state flip is under state_lock; only one caller wins). Currently driven by the
 * render thread via select_dmabuf, but the locking keeps future call sites correct. */
static bool try_exit_fallback(display_ctx *ctx)
{
    if (!is_fallback(ctx) || atomic_load_explicit(&ctx->aborting, memory_order_acquire))
        return false;

    pthread_mutex_lock(&ctx->state_lock);
    int ctrl_fd = dup_cloexec(ctx->ctrl_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (ctrl_fd < 0) {
        set_control_dead(ctx);
        return false;
    }

    struct pollfd pfd = { .fd = ctrl_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 0);
    if (ret < 0) {
        close(ctrl_fd);
        if (errno != EINTR)
            set_control_dead(ctx);
        return false;
    }
    if (ret == 0) {
        close(ctrl_fd);
        return false;
    }
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(ctrl_fd);
        set_control_dead(ctx);
        return false;
    }
    if (!(pfd.revents & POLLIN)) {
        close(ctrl_fd);
        return false;
    }

    struct ctrl_msg hdr;
    bool valid = recv_exact_progress(ctrl_fd, &hdr, sizeof(hdr), 1000) == 0 &&
                 hdr.type == CTRL_MSG_FDS_READY && hdr.size == 0;
    close(ctrl_fd);
    if (!valid) {
        set_control_dead(ctx);
        return false;
    }

    /* Hold state_lock -> data_lock while publishing the new session and replaying
     * BUFS_READY. Input writers cannot observe active state until the producer has
     * received the complete dmabuf descriptor frame. */
    pthread_mutex_lock(&ctx->state_lock);
    if (!is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return false;
    }
    pthread_mutex_lock(&ctx->data_lock);
    bool ready = push_dmabufs_locked(ctx);
    if (ready)
        set_fallback(ctx, false);
    else
        reset_transport_locked(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    pthread_mutex_unlock(&ctx->state_lock);

    if (!ready)
        return false;
    if (ctx->exit_fallback_cb)
        ctx->exit_fallback_cb(ctx->exit_fallback_userdata);
    return true;
}

static void reset_transport_locked(display_ctx *ctx)
{
    ctx->transport_generation++;
    if (ctx->transport_generation == 0)
        ctx->transport_generation = 1;
    ctx->frame_generation = 0;
    ctx->event_generation = 0;
    if (ctx->data_fd >= 0)         { int fd = ctx->data_fd; ctx->data_fd = -1; shutdown(fd, SHUT_RDWR); close(fd); }
    if (ctx->buf_ready_efd >= 0)   { close(ctx->buf_ready_efd);   ctx->buf_ready_efd = -1; }
    if (ctx->fence_fd >= 0)        { shutdown(ctx->fence_fd, SHUT_RDWR); close(ctx->fence_fd); ctx->fence_fd = -1; }
    if (ctx->audio_fd >= 0)        { shutdown(ctx->audio_fd, SHUT_RDWR); close(ctx->audio_fd); ctx->audio_fd = -1; }
    if (ctx->shm_ptr) { volatile uint32_t *p = ctx->shm_ptr; ctx->shm_ptr = NULL; munmap((void *)p, sizeof(uint32_t)); }
    if (ctx->shm_fd >= 0)         { close(ctx->shm_fd);           ctx->shm_fd = -1; }

    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
    if (ctx->buf_ready_efd < 0 || create_shm(ctx) < 0 || send_hello_fds(ctx) < 0)
        set_control_dead(ctx);
}

static void enter_fallback_for_generation(display_ctx *ctx, uint64_t generation)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (is_fallback(ctx) || (generation != 0 && generation != ctx->transport_generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    set_fallback(ctx, true);
    free_resources(ctx);
    ctx->buffer_pending = false;
    bool aborting = atomic_load_explicit(&ctx->aborting, memory_order_acquire);
    if (!aborting) {
        pthread_mutex_lock(&ctx->data_lock);
        reset_transport_locked(ctx);
        pthread_mutex_unlock(&ctx->data_lock);
    }
    pthread_mutex_unlock(&ctx->state_lock);

    if (ctx->fallback_cb)
        ctx->fallback_cb(ctx->fallback_userdata);
}

static void enter_fallback(display_ctx *ctx)
{
    enter_fallback_for_generation(ctx, 0);
}

static void enter_fallback_if_generation(display_ctx *ctx, uint64_t generation)
{
    enter_fallback_for_generation(ctx, generation);
}
int connect_to_deamon(display_ctx **out, const char *socket_path){
    return connect_to_deamon_with_fd(out, connect_unix(socket_path));
}
int connect_to_deamon_with_fd(display_ctx **out, int ctrl_fd)
{
    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (ctrl_fd >= 0)
            close(ctrl_fd);
        return -1;
    }

    pthread_mutex_init(&ctx->state_lock, NULL);
    pthread_mutex_init(&ctx->data_lock, NULL);

    ctx->ctrl_fd = -1;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->audio_fd = -1;
    ctx->shm_ptr = NULL;
    atomic_init(&ctx->fallback, true);
    atomic_init(&ctx->control_dead, false);
    atomic_init(&ctx->aborting, false);
    ctx->transport_generation = 1;
    ctx->frame_generation = 0;
    ctx->event_generation = 0;

    ctx->ctrl_fd = ctrl_fd;
    if (ctx->ctrl_fd < 0)
        goto fail;

    /* buf_ready_efd is the consumer->producer pacing eventfd; fence_fd is created as a
     * socketpair inside send_hello_fds(). */
    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
    if (ctx->buf_ready_efd < 0)
        goto fail;

    if (create_shm(ctx) < 0)
        goto fail;

    if (send_hello_fds(ctx) < 0)
        goto fail;

    *out = ctx;
    return 0;

fail:
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
    return -1;
}

void disconnect(display_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    free_resources(ctx);
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
}

int set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh)
{
    ctx->screen_w = width;
    ctx->screen_h = height;
    ctx->pixel_format = format;

    struct ctrl_msg hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) };
    struct screen_info si = { .width = width, .height = height, .format = format, .refresh = refresh };
    uint8_t msg[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), &si, sizeof(si));
    int64_t deadline = socket_deadline_after_ms(CONTROL_IO_TIMEOUT_MS);
    return deadline < 0 ? -1 :
           send_all_deadline(ctx->ctrl_fd, msg, sizeof(msg), deadline);
}

int push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count)
{
    if (!ctx || !fds || !infos || count <= 0 || count > MAX_BUFS)
        return -1;
    memcpy(ctx->stored_fds, fds, (size_t)count * sizeof(int));
    memcpy(ctx->stored_infos, infos, (size_t)count * sizeof(struct buf_info));
    ctx->stored_count = count;

    if (is_fallback(ctx))
        return 0;

    int ret = push_dmabufs_internal(ctx);
    if (ret < 0)
        enter_fallback(ctx);
    return ret;
}

int select_dmabuf(display_ctx *ctx, int idx)
{
    if (is_fallback(ctx)) {
        try_exit_fallback(ctx);
        if (is_fallback(ctx))
            return 0;
    }

    if (idx < 0 || idx >= ctx->stored_count)
        return -1;

    pthread_mutex_lock(&ctx->state_lock);
    if (is_fallback(ctx) || !ctx->shm_ptr || ctx->buf_ready_efd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    *ctx->shm_ptr = (uint32_t)idx;
    eventfd_t val = 1;
    int result = eventfd_write(ctx->buf_ready_efd, val);
    if (result == 0) {
        ctx->buffer_pending = true;
        ctx->frame_generation = ctx->transport_generation;
    }
    pthread_mutex_unlock(&ctx->state_lock);
    if (result < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}

/* Wait for the producer to finish the frame, then return its render-done fence so
 * the caller can hand it to ANativeWindow_queueBuffer (SurfaceFlinger waits on it
 * GPU-side before scanout). The producer sends exactly one message per frame on the
 * dedicated fence channel; the message itself is the "frame rendered" signal (no
 * separate eventfd, no cross-channel ordering) and the optional fence rides as
 * SCM_RIGHTS ancillary data. Returns the fence fd (caller owns it), or -1 if none /
 * on error. */
int refresh_done(display_ctx *ctx)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (!ctx->buffer_pending || is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }
    uint64_t generation = ctx->frame_generation;
    int fence_fd = dup_cloexec(ctx->fence_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (fence_fd < 0) {
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }

    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 5000);
    bool failed = ret <= 0 || (pfd.revents & (POLLERR | POLLNVAL)) ||
                  !(pfd.revents & POLLIN);

    int rfence = -1;
    if (!failed) {
        char byte;
        struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
        union {
            char buf[CMSG_SPACE(sizeof(int) * 8)];
            struct cmsghdr align;
        } cmsg;
        memset(&cmsg, 0, sizeof(cmsg));
        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = cmsg.buf,
            .msg_controllen = sizeof(cmsg.buf),
        };
        ssize_t count = recvmsg(fence_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (count != 1) {
            failed = true;
        } else {
            int received_fds[8];
            int received_count = 0;
            int control_count = 0;
            bool control_valid = !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
            for (struct cmsghdr *control = CMSG_FIRSTHDR(&msg); control;
                 control = CMSG_NXTHDR(&msg, control)) {
                control_count++;
                if (control->cmsg_level != SOL_SOCKET ||
                    control->cmsg_type != SCM_RIGHTS ||
                    control->cmsg_len < CMSG_LEN(0)) {
                    control_valid = false;
                    continue;
                }
                size_t bytes = control->cmsg_len - CMSG_LEN(0);
                if (bytes % sizeof(int) != 0 ||
                    bytes / sizeof(int) > (size_t)(8 - received_count)) {
                    control_valid = false;
                    continue;
                }
                int fds = (int)(bytes / sizeof(int));
                memcpy(received_fds + received_count, CMSG_DATA(control), bytes);
                received_count += fds;
            }
            control_valid = control_valid &&
                ((control_count == 0 && received_count == 0) ||
                 (control_count == 1 && received_count == 1));
            if (control_valid && received_count == 1) {
                rfence = received_fds[0];
            } else {
                for (int i = 0; i < received_count; i++)
                    close(received_fds[i]);
                failed = !control_valid;
            }
        }
    }
    close(fence_fd);

    pthread_mutex_lock(&ctx->state_lock);
    bool current = !is_fallback(ctx) && generation != 0 &&
                   generation == ctx->transport_generation &&
                   generation == ctx->frame_generation;
    if (current)
        ctx->buffer_pending = false;
    pthread_mutex_unlock(&ctx->state_lock);

    if (!current) {
        if (rfence >= 0)
            close(rfence);
        return -1;
    }
    if (failed) {
        if (rfence >= 0)
            close(rfence);
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }
    return rfence;
}

int push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    if (is_fallback(ctx))
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    pthread_mutex_lock(&ctx->data_lock);
    if (is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->data_lock);
        return 0;
    }
    int fd = ctx->data_fd;
    int64_t deadline = socket_deadline_after_ms(DATA_IO_TIMEOUT_MS);
    int r = (fd >= 0 && deadline >= 0) ?
            send_all_deadline(fd, msg, sizeof(msg), deadline) : -1;
    pthread_mutex_unlock(&ctx->data_lock);

    if (r < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}
int push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void* payload, size_t size)
{
    if (is_fallback(ctx))
        return 0;

    if ((size > 0 && !payload) ||
        size > SIZE_MAX - sizeof(struct data_msg) - sizeof(struct InputEvent))
        return -1;
    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    size_t total = sizeof(struct data_msg) + sizeof(struct InputEvent) + size;
    uint8_t *msg = malloc(total);
    if (!msg)
        return -1;
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));
    if (size > 0)
        memcpy(msg + sizeof(hdr) + sizeof(struct InputEvent), payload, size);

    pthread_mutex_lock(&ctx->data_lock);
    if (is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->data_lock);
        free(msg);
        return 0;
    }
    int fd = ctx->data_fd;
    int64_t deadline = socket_deadline_after_ms(DATA_IO_TIMEOUT_MS);
    int r = (fd >= 0 && deadline >= 0) ?
            send_all_deadline(fd, msg, total, deadline) : -1;
    pthread_mutex_unlock(&ctx->data_lock);

    free(msg);
    if (r < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}
int poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    uint64_t generation = ctx->transport_generation;
    int data_fd = dup_cloexec(ctx->data_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (data_fd < 0) {
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }

    struct pollfd pfd = { .fd = data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret == 0) {
        close(data_fd);
        return 0;
    }
    if (ret < 0) {
        close(data_fd);
        if (errno == EINTR)
            return 0;
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }
    if (pfd.revents & (POLLERR | POLLNVAL)) {
        close(data_fd);
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }
    if (!(pfd.revents & POLLIN)) {
        close(data_fd);
        if (pfd.revents & POLLHUP) {
            enter_fallback_if_generation(ctx, generation);
            return -1;
        }
        return 0;
    }

    uint8_t message[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    bool valid = (pfd.revents & POLLIN) &&
                 recv_exact_progress(data_fd, message, sizeof(message), 1000) == 0;
    close(data_fd);
    if (valid) {
        struct data_msg header;
        memcpy(&header, message, sizeof(header));
        valid = header.type == DATA_MSG_OUTPUT_EVENT &&
                header.size == sizeof(struct OutputEvent);
    }

    pthread_mutex_lock(&ctx->state_lock);
    bool current = !is_fallback(ctx) && generation == ctx->transport_generation;
    if (valid && current)
        ctx->event_generation = generation;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!current)
        return 0;
    if (!valid) {
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }
    memcpy(event, message + sizeof(struct data_msg), sizeof(*event));
    return 1;
}

int poll_output_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms)
{
    pthread_mutex_lock(&ctx->state_lock);
    uint64_t generation = ctx->event_generation;
    if (is_fallback(ctx) || generation == 0 || generation != ctx->transport_generation) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    int data_fd = dup_cloexec(ctx->data_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (data_fd < 0) {
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }

    bool received = size == 0 || recv_exact_progress(data_fd, payload, size, timeout_ms) == 0;
    close(data_fd);
    pthread_mutex_lock(&ctx->state_lock);
    bool current = !is_fallback(ctx) && generation == ctx->transport_generation &&
                   generation == ctx->event_generation;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!current)
        return 0;
    if (!received) {
        enter_fallback_if_generation(ctx, generation);
        return -1;
    }
    return 1;
}
int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata)
{
    ctx->fallback_cb = on_fallback;
    ctx->fallback_userdata = userdata;
    return 0;
}

int set_exit_fallback_callback(display_ctx *ctx, void (*on_exit_fallback)(void *), void *userdata)
{
    ctx->exit_fallback_cb = on_exit_fallback;
    ctx->exit_fallback_userdata = userdata;
    return 0;
}
int get_data_fd(display_ctx *ctx)
{
    return ctx->data_fd;
}
/* Current local end of the audio socketpair, or -1 in fallback. The value changes
 * across reconnects (each hello creates a fresh socketpair), so callers must re-fetch
 * it rather than cache it. */
int get_audio_fd(display_ctx *ctx)
{
    return is_fallback(ctx) ? -1 : ctx->audio_fd;
}

int dup_audio_fd(display_ctx *ctx, uint64_t *generation)
{
    if (!ctx || !generation)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    int fd = is_fallback(ctx) ? -1 : dup_cloexec(ctx->audio_fd);
    *generation = fd >= 0 ? ctx->transport_generation : 0;
    pthread_mutex_unlock(&ctx->state_lock);
    return fd;
}

void display_consumer_fail_transport(display_ctx *ctx)
{
    if (ctx)
        enter_fallback(ctx);
}

void display_consumer_abort_io(display_ctx *ctx)
{
    if (!ctx)
        return;
    atomic_store_explicit(&ctx->aborting, true, memory_order_release);
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->data_fd >= 0)
        shutdown(ctx->data_fd, SHUT_RDWR);
    if (ctx->fence_fd >= 0)
        shutdown(ctx->fence_fd, SHUT_RDWR);
    if (ctx->audio_fd >= 0)
        shutdown(ctx->audio_fd, SHUT_RDWR);
    pthread_mutex_unlock(&ctx->state_lock);
}

int display_consumer_needs_reconnect(display_ctx *ctx)
{
    if (!ctx)
        return 1;
    if (atomic_load_explicit(&ctx->control_dead, memory_order_acquire))
        return 1;

    struct pollfd pfd = { .fd = ctx->ctrl_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 0);
    if (ret < 0) {
        if (errno != EINTR)
            set_control_dead(ctx);
    } else if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        set_control_dead(ctx);
    }
    return atomic_load_explicit(&ctx->control_dead, memory_order_acquire) ? 1 : 0;
}
//用于处理未处理的变长payload事件
void handle_unhandled_event(display_ctx *ctx, const struct OutputEvent *event)
{
    switch (event->type)
    {
    case OUTPUT_TYPE_CLIPBOARD:
        //客户端发送了一个剪贴板事件，后续会有变长数据跟随，但是库调用者没有处理这个事件，所以我们需要把后续的变长数据读掉，避免阻塞
        if (event->clipboard.size > 0) {
            void* payload = malloc(event->clipboard.size);
            if (!payload) {
                display_consumer_fail_transport(ctx);
                return;
            }
            poll_output_event_extend_data(ctx, payload, event->clipboard.size, 1000);
            free(payload);
        }
        break;
    default:
        break;
    }
}

void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event, int* fds, int fd_count)
{
    if (is_fallback(ctx))
        return;

    /* This is the ONLY fd-carrying writer, and it is two framed sends (RESOURCE event
     * + EXTEND_FDS ancillary). Hold data_lock across BOTH so a concurrent input write
     * can't wedge between them and desync the producer's framed stream. Sent inline
     * here (not via push_input_event) to avoid re-locking data_lock. */
    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    pthread_mutex_lock(&ctx->data_lock);
    if (is_fallback(ctx)) {
        pthread_mutex_unlock(&ctx->data_lock);
        return;
    }
    int fd = ctx->data_fd;
    int64_t deadline = socket_deadline_after_ms(DATA_IO_TIMEOUT_MS);
    bool ok = fd >= 0 && deadline >= 0 &&
              send_all_deadline(fd, msg, sizeof(msg), deadline) == 0;
    if (ok && fd_count > 0) {
        struct data_msg fhdr = { .type = DATA_MSG_INPUT_EXTEND_FDS, .size = 0 };
        ok = send_fds_deadline(fd, &fhdr, sizeof(fhdr), fds, fd_count,
                               deadline) >= 0;
    }
    pthread_mutex_unlock(&ctx->data_lock);

    if (!ok)
        enter_fallback(ctx);   /* outside the lock */
}
