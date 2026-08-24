#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "socket_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t socket_deadline_after_ms(int timeout_ms)
{
    int64_t now = monotonic_ms();
    if (now < 0 || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (now > INT64_MAX - timeout_ms) {
        errno = EOVERFLOW;
        return -1;
    }
    return now + timeout_ms;
}

static int wait_ready(int fd, short events, int64_t deadline_ms)
{
    for (;;) {
        int timeout = -1;
        if (deadline_ms != INT64_MAX) {
            int64_t now = monotonic_ms();
            if (now < 0)
                return -1;
            int64_t remaining = deadline_ms - now;
            if (remaining <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        }

        struct pollfd pfd = { .fd = fd, .events = events };
        int result = poll(&pfd, 1, timeout);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (pfd.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (pfd.revents & POLLERR) {
            errno = EIO;
            return -1;
        }
        if (pfd.revents & events)
            return 0;
        if (pfd.revents & POLLHUP) {
            errno = ECONNRESET;
            return -1;
        }
    }
}

int send_all_deadline(int fd, const void *buf, size_t len, int64_t deadline_ms)
{
    const uint8_t *cursor = buf;
    size_t sent = 0;
    while (sent < len) {
        if (wait_ready(fd, POLLOUT, deadline_ms) < 0)
            return -1;
        ssize_t count = send(fd, cursor + sent, len - sent,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) {
            sent += (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count == 0)
            errno = EPIPE;
        return -1;
    }
    return 0;
}

int recv_all_deadline(int fd, void *buf, size_t len, int64_t deadline_ms)
{
    uint8_t *cursor = buf;
    size_t received = 0;
    while (received < len) {
        if (wait_ready(fd, POLLIN, deadline_ms) < 0)
            return -1;
        ssize_t count = recv(fd, cursor + received, len - received, MSG_DONTWAIT);
        if (count > 0) {
            received += (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count == 0)
            errno = ECONNRESET;
        return -1;
    }
    return 0;
}

int send_fds_deadline(int sock, const void *data, size_t data_len,
                      const int *fds, int fd_count, int64_t deadline_ms)
{
    if (!data || data_len == 0 || !fds || fd_count <= 0 || data_len > SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }

    size_t fd_bytes = sizeof(int) * (size_t)fd_count;
    size_t control_size = CMSG_SPACE(fd_bytes);
    char *control = calloc(1, control_size);
    if (!control)
        return -1;

    struct iovec iov = { .iov_base = (void *)data, .iov_len = data_len };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = control_size,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fd_bytes);
    memcpy(CMSG_DATA(cmsg), fds, fd_bytes);

    int result = -1;
    for (;;) {
        if (wait_ready(sock, POLLOUT, deadline_ms) < 0)
            break;
        ssize_t count = sendmsg(sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count == (ssize_t)data_len) {
            result = 0;
            break;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count >= 0)
            errno = EIO;
        break;
    }
    free(control);
    return result;
}

static void close_received_fds(int *fds, int count)
{
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0)
            close(fds[i]);
    }
}

int recv_fds_deadline(int sock, void *data, size_t data_len,
                      int *fds, int fd_count, int *fds_received,
                      int64_t deadline_ms)
{
    if (!data || data_len == 0 || data_len > INT_MAX || !fds || fd_count <= 0 ||
        !fds_received) {
        errno = EINVAL;
        return -1;
    }
    *fds_received = 0;

    size_t control_size = CMSG_SPACE(sizeof(int) * (size_t)fd_count);
    char *control = calloc(1, control_size);
    if (!control)
        return -1;

    struct iovec iov = { .iov_base = data, .iov_len = data_len };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = control_size,
    };

    ssize_t first = -1;
    for (;;) {
        if (wait_ready(sock, POLLIN, deadline_ms) < 0)
            goto fail;
        first = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (first > 0)
            break;
        if (first < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (first == 0)
            errno = ECONNRESET;
        goto fail;
    }

    int received = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) {
            errno = EPROTO;
            close_received_fds(fds, received);
            goto fail;
        }
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0) {
            errno = EPROTO;
            close_received_fds(fds, received);
            goto fail;
        }
        int count = (int)(bytes / sizeof(int));
        if (count > fd_count - received) {
            int *extra = (int *)CMSG_DATA(cmsg);
            close_received_fds(fds, received);
            close_received_fds(extra, count);
            errno = EMSGSIZE;
            goto fail;
        }
        memcpy(fds + received, CMSG_DATA(cmsg), bytes);
        received += count;
    }
    if (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) {
        close_received_fds(fds, received);
        errno = EMSGSIZE;
        goto fail;
    }
    *fds_received = received;

    if ((size_t)first < data_len &&
        recv_all_deadline(sock, (uint8_t *)data + first, data_len - (size_t)first,
                          deadline_ms) < 0) {
        close_received_fds(fds, received);
        *fds_received = 0;
        goto fail;
    }

    free(control);
    return (int)data_len;

fail:
    free(control);
    return -1;
}

int send_fds(int sock, const void *data, size_t data_len,
             const int *fds, int fd_count)
{
    return send_fds_deadline(sock, data, data_len, fds, fd_count, INT64_MAX);
}

int recv_fds(int sock, void *data, size_t data_len,
             int *fds, int fd_count, int *fds_received)
{
    return recv_fds_deadline(sock, data, data_len, fds, fd_count, fds_received,
                             INT64_MAX);
}

int connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int send_all(int fd, const void *buf, size_t len)
{
    return send_all_deadline(fd, buf, len, INT64_MAX);
}

int recv_all(int fd, void *buf, size_t len)
{
    return recv_all_deadline(fd, buf, len, INT64_MAX);
}
