#ifndef DISPLAY_SOCKET_UTILS_H
#define DISPLAY_SOCKET_UTILS_H

#include <stddef.h>
#include <stdint.h>

int64_t socket_deadline_after_ms(int timeout_ms);

int send_fds_deadline(int sock, const void *data, size_t data_len,
                      const int *fds, int fd_count, int64_t deadline_ms);

int recv_fds_deadline(int sock, void *data, size_t data_len,
                      int *fds, int fd_count, int *fds_received,
                      int64_t deadline_ms);

int send_all_deadline(int fd, const void *buf, size_t len, int64_t deadline_ms);

int recv_all_deadline(int fd, void *buf, size_t len, int64_t deadline_ms);

int send_fds(int sock, const void *data, size_t data_len,
             const int *fds, int fd_count);

int recv_fds(int sock, void *data, size_t data_len,
             int *fds, int fd_count, int *fds_received);

int connect_unix(const char *path);

int send_all(int fd, const void *buf, size_t len);

int recv_all(int fd, void *buf, size_t len);

#endif
