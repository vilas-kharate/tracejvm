#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef EMSCRIPTEN

enum { TRACEJVM_HOST_ASYNC_PENDING = -2 };

bool tracejvm_host_routes_path(char const *path);
bool tracejvm_host_is_remote_fd(int fd);
bool tracejvm_host_routes_fd(int fd);
char *tracejvm_host_getcwd(char *buffer, size_t capacity);
void tracejvm_host_dispose_active_context(void);

typedef struct tracejvm_host_dir tracejvm_host_dir;

int tracejvm_host_open(char const *path, int flags, mode_t mode);
ssize_t tracejvm_host_read(
    int fd, void *buffer, size_t length, off_t position, bool positioned);
ssize_t tracejvm_host_write(
    int fd, void const *buffer, size_t length, off_t position, bool positioned);
int tracejvm_host_close(int fd);
off_t tracejvm_host_seek(int fd, off_t offset, int whence);
int tracejvm_host_fstat(int fd, struct stat *stat_buffer);
int tracejvm_host_ftruncate(int fd, off_t length);
int tracejvm_host_stat(
    char const *path, struct stat *stat_buffer, bool follow_links);
int tracejvm_host_mkdir(char const *path, mode_t mode);
int tracejvm_host_rmdir(char const *path);
int tracejvm_host_unlink(char const *path);
int tracejvm_host_chmod(char const *path, mode_t mode);
int tracejvm_host_utimes(char const *path, double modified_ms);
int tracejvm_host_link(char const *existing_path, char const *new_path);
int tracejvm_host_symlink(char const *target, char const *link_path);
ssize_t tracejvm_host_readlink(
    char const *path, char *target, size_t capacity);
int tracejvm_host_rename(char const *source, char const *destination);
char *tracejvm_host_realpath(
    char const *path, char *resolved, size_t capacity);
tracejvm_host_dir *tracejvm_host_opendir(char const *path);
int tracejvm_host_readdir(
    tracejvm_host_dir *directory, char *name, size_t capacity);
int tracejvm_host_closedir(tracejvm_host_dir *directory);
int tracejvm_host_socket(void);
int tracejvm_host_bind(int fd, char const *host, int port, int *bound_port);
int tracejvm_host_listen(int fd, int backlog);
int tracejvm_host_accept(
    int fd, char *remote_host, size_t remote_host_capacity, int *remote_port);
int tracejvm_host_connect(int fd, char const *host, int port);
int tracejvm_host_shutdown(int fd, int how);
int tracejvm_host_socket_address(
    int fd, bool peer, char *host, size_t host_capacity, int *port);
int tracejvm_host_configure_blocking(int fd, bool blocking);
int tracejvm_host_poll(int fd, bool read, bool write, int timeout_ms);
int tracejvm_host_pipe(int descriptors[2], bool blocking);
int tracejvm_host_eventfd(void);
int tracejvm_host_eventfd_set(int fd);
int tracejvm_host_epoll_create(void);
int tracejvm_host_epoll_ctl(int epfd, int operation, int fd, int events);
int tracejvm_host_epoll_wait_begin(int epfd, int timeout_ms);
int tracejvm_host_epoll_wait_poll(
    int call_id, void *poll_array, int capacity);
int tracejvm_host_inotify_init(void);
int tracejvm_host_inotify_add_watch(
    int ifd, char const *path, int mask);
int tracejvm_host_inotify_rm_watch(int ifd, int wd);
int tracejvm_host_inotify_poll_begin(int ifd, int wake_fd);
int tracejvm_host_inotify_poll_poll(int call_id);
int tracejvm_host_spawn(
    char const *program,
    char const *argument_block,
    size_t argument_block_length,
    int argument_count,
    char const *environment_block,
    size_t environment_block_length,
    int environment_count,
    char const *directory,
    int descriptors[3],
    bool redirect_error_stream);
int tracejvm_host_wait_begin(int64_t pid);
int tracejvm_host_wait_poll(int call_id, int *exit_code);
int tracejvm_host_read_begin(int fd, size_t length);
ssize_t tracejvm_host_read_poll(
    int call_id, void *buffer, size_t capacity);
int tracejvm_host_accept_begin(int fd);
int tracejvm_host_accept_poll(
    int call_id,
    char *remote_host,
    size_t remote_host_capacity,
    int *remote_port);
int64_t tracejvm_host_current_pid(void);
int64_t tracejvm_host_parent_pid(int64_t pid);
int64_t tracejvm_host_process_start_time(int64_t pid);
int tracejvm_host_process_list(
    int64_t parent_pid,
    int64_t *pids,
    int64_t *parent_pids,
    int64_t *start_times,
    int capacity);
int tracejvm_host_process_info(
    int64_t pid,
    char *command,
    size_t command_capacity,
    char *argument_block,
    size_t argument_block_capacity,
    size_t *command_length,
    size_t *argument_block_length,
    int *argument_count,
    int64_t *start_time);
int tracejvm_host_environment(
    char *entry_block,
    size_t entry_block_capacity,
    size_t *entry_block_length,
    int *entry_count);
int tracejvm_host_identity_snapshot(int64_t identity[4]);
int tracejvm_host_watchdog(
    int action,
    int64_t timeout_ms,
    int signal,
    int64_t status[4]);
int tracejvm_host_setsid(int64_t identity[2]);
int64_t tracejvm_host_setpgid(int64_t pid, int64_t pgid);
int64_t tracejvm_host_tcgetpgrp(int fd);
int64_t tracejvm_host_tcsetpgrp(int fd, int64_t pgid);
int tracejvm_host_tcgetwinsize(int fd, int64_t size[2]);
int tracejvm_host_tcsetwinsize(
    int fd, int64_t rows, int64_t columns, int64_t size[2]);
int tracejvm_host_poll_signal(void);
int tracejvm_host_process_exists(int64_t pid);
int tracejvm_host_kill(int64_t pid, bool force);

#else

enum { TRACEJVM_HOST_ASYNC_PENDING = -2 };

static inline bool tracejvm_host_routes_path(char const *path) {
  (void)path;
  return false;
}

static inline bool tracejvm_host_is_remote_fd(int fd) {
  (void)fd;
  return false;
}

static inline bool tracejvm_host_routes_fd(int fd) {
  (void)fd;
  return false;
}

static inline char *tracejvm_host_getcwd(char *buffer, size_t capacity) {
  return getcwd(buffer, capacity);
}

static inline void tracejvm_host_dispose_active_context(void) {}

typedef DIR tracejvm_host_dir;

static inline int tracejvm_host_open(
    char const *path, int flags, mode_t mode) {
  return open(path, flags, mode);
}

static inline ssize_t tracejvm_host_read(
    int fd, void *buffer, size_t length, off_t position, bool positioned) {
  return positioned ? pread(fd, buffer, length, position)
                    : read(fd, buffer, length);
}

static inline ssize_t tracejvm_host_write(
    int fd, void const *buffer, size_t length, off_t position, bool positioned) {
  return positioned ? pwrite(fd, buffer, length, position)
                    : write(fd, buffer, length);
}

static inline int tracejvm_host_close(int fd) {
  return close(fd);
}

static inline off_t tracejvm_host_seek(int fd, off_t offset, int whence) {
  return lseek(fd, offset, whence);
}

static inline int tracejvm_host_fstat(int fd, struct stat *stat_buffer) {
  return fstat(fd, stat_buffer);
}

static inline int tracejvm_host_ftruncate(int fd, off_t length) {
  return ftruncate(fd, length);
}

static inline int tracejvm_host_stat(
    char const *path, struct stat *stat_buffer, bool follow_links) {
  return follow_links ? stat(path, stat_buffer) : lstat(path, stat_buffer);
}

static inline int tracejvm_host_mkdir(char const *path, mode_t mode) {
  return mkdir(path, mode);
}

static inline int tracejvm_host_rmdir(char const *path) {
  return rmdir(path);
}

static inline int tracejvm_host_unlink(char const *path) {
  return unlink(path);
}

static inline int tracejvm_host_link(
    char const *existing_path, char const *new_path) {
  return link(existing_path, new_path);
}

static inline int tracejvm_host_symlink(
    char const *target, char const *link_path) {
  return symlink(target, link_path);
}

static inline ssize_t tracejvm_host_readlink(
    char const *path, char *target, size_t capacity) {
  return readlink(path, target, capacity);
}

static inline int tracejvm_host_rename(
    char const *source, char const *destination) {
  return rename(source, destination);
}

static inline char *tracejvm_host_realpath(
    char const *path, char *resolved, size_t capacity) {
  (void)capacity;
  return realpath(path, resolved);
}

static inline tracejvm_host_dir *tracejvm_host_opendir(char const *path) {
  return opendir(path);
}

static inline int tracejvm_host_readdir(
    tracejvm_host_dir *directory, char *name, size_t capacity) {
  errno = 0;
  struct dirent *entry = readdir(directory);
  if (!entry)
    return errno == 0 ? 0 : -1;
  size_t length = strlen(entry->d_name);
  if (length + 1 > capacity) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(name, entry->d_name, length + 1);
  return 1;
}

static inline int tracejvm_host_closedir(tracejvm_host_dir *directory) {
  return closedir(directory);
}

static inline int tracejvm_host_socket(void) {
  return socket(AF_INET, SOCK_STREAM, 0);
}

static inline int tracejvm_host_bind(
    int fd, char const *host, int port, int *bound_port) {
  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)port),
  };
  if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    return -1;
  socklen_t length = sizeof(address);
  if (getsockname(fd, (struct sockaddr *)&address, &length) != 0)
    return -1;
  if (bound_port)
    *bound_port = ntohs(address.sin_port);
  return 0;
}

static inline int tracejvm_host_listen(int fd, int backlog) {
  return listen(fd, backlog);
}

static inline int tracejvm_host_accept(
    int fd, char *remote_host, size_t remote_host_capacity, int *remote_port) {
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  int accepted = accept(fd, (struct sockaddr *)&address, &length);
  if (accepted < 0)
    return -1;
  if (!inet_ntop(AF_INET, &address.sin_addr, remote_host,
                 (socklen_t)remote_host_capacity)) {
    close(accepted);
    return -1;
  }
  if (remote_port)
    *remote_port = ntohs(address.sin_port);
  return accepted;
}

static inline int tracejvm_host_connect(
    int fd, char const *host, int port) {
  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)port),
  };
  if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  return connect(fd, (struct sockaddr *)&address, sizeof(address));
}

static inline int tracejvm_host_shutdown(int fd, int how) {
  return shutdown(fd, how);
}

static inline int tracejvm_host_socket_address(
    int fd, bool peer, char *host, size_t host_capacity, int *port) {
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  int result = peer
      ? getpeername(fd, (struct sockaddr *)&address, &length)
      : getsockname(fd, (struct sockaddr *)&address, &length);
  if (result != 0)
    return -1;
  if (!inet_ntop(AF_INET, &address.sin_addr, host, (socklen_t)host_capacity))
    return -1;
  if (port)
    *port = ntohs(address.sin_port);
  return 0;
}

static inline int tracejvm_host_configure_blocking(int fd, bool blocking) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1;
  return fcntl(fd, F_SETFL, blocking ? flags & ~O_NONBLOCK
                                    : flags | O_NONBLOCK);
}

static inline int tracejvm_host_poll(
    int fd, bool read_ready, bool write_ready, int timeout_ms) {
  struct pollfd descriptor = {
      .fd = fd,
      .events = (short)((read_ready ? POLLIN : 0) |
                        (write_ready ? POLLOUT : 0)),
  };
  int result = poll(&descriptor, 1, timeout_ms);
  if (result <= 0)
    return result;
  return ((descriptor.revents & POLLIN) ? 1 : 0) |
      ((descriptor.revents & POLLOUT) ? 2 : 0) |
      ((descriptor.revents & POLLERR) ? 4 : 0) |
      ((descriptor.revents & POLLHUP) ? 8 : 0) |
      ((descriptor.revents & POLLNVAL) ? 16 : 0);
}

static inline int tracejvm_host_pipe(int descriptors[2], bool blocking) {
  if (pipe(descriptors) != 0)
    return -1;
  if (!blocking) {
    for (int index = 0; index < 2; index++) {
      int flags = fcntl(descriptors[index], F_GETFL);
      if (flags < 0 ||
          fcntl(descriptors[index], F_SETFL, flags | O_NONBLOCK) != 0) {
        int error = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        errno = error;
        return -1;
      }
    }
  }
  return 0;
}

static inline int tracejvm_host_eventfd(void) {
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_eventfd_set(int fd) {
  (void)fd;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_epoll_create(void) {
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_epoll_ctl(
    int epfd, int operation, int fd, int events) {
  (void)epfd;
  (void)operation;
  (void)fd;
  (void)events;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_epoll_wait_begin(
    int epfd, int timeout_ms) {
  (void)epfd;
  (void)timeout_ms;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_epoll_wait_poll(
    int call_id, void *poll_array, int capacity) {
  (void)call_id;
  (void)poll_array;
  (void)capacity;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_inotify_init(void) {
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_inotify_add_watch(
    int ifd, char const *path, int mask) {
  (void)ifd;
  (void)path;
  (void)mask;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_inotify_rm_watch(int ifd, int wd) {
  (void)ifd;
  (void)wd;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_inotify_poll_begin(
    int ifd, int wake_fd) {
  (void)ifd;
  (void)wake_fd;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_inotify_poll_poll(int call_id) {
  (void)call_id;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_spawn(
    char const *program,
    char const *argument_block,
    size_t argument_block_length,
    int argument_count,
    char const *environment_block,
    size_t environment_block_length,
    int environment_count,
    char const *directory,
    int descriptors[3],
    bool redirect_error_stream) {
  (void)program;
  (void)argument_block;
  (void)argument_block_length;
  (void)argument_count;
  (void)environment_block;
  (void)environment_block_length;
  (void)environment_count;
  (void)directory;
  (void)descriptors;
  (void)redirect_error_stream;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_wait_begin(int64_t pid) {
  (void)pid;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_wait_poll(int call_id, int *exit_code) {
  (void)call_id;
  (void)exit_code;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_read_begin(int fd, size_t length) {
  (void)fd;
  (void)length;
  errno = ENOSYS;
  return -1;
}

static inline ssize_t tracejvm_host_read_poll(
    int call_id, void *buffer, size_t capacity) {
  (void)call_id;
  (void)buffer;
  (void)capacity;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_accept_begin(int fd) {
  (void)fd;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_accept_poll(
    int call_id,
    char *remote_host,
    size_t remote_host_capacity,
    int *remote_port) {
  (void)call_id;
  (void)remote_host;
  (void)remote_host_capacity;
  (void)remote_port;
  errno = ENOSYS;
  return -1;
}

static inline int64_t tracejvm_host_current_pid(void) {
  return (int64_t)getpid();
}

static inline int64_t tracejvm_host_parent_pid(int64_t pid) {
  (void)pid;
  return (int64_t)getppid();
}

static inline int64_t tracejvm_host_process_start_time(int64_t pid) {
  return kill((pid_t)pid, 0) == 0 ? 0 : -1;
}

static inline int tracejvm_host_process_list(
    int64_t parent_pid,
    int64_t *pids,
    int64_t *parent_pids,
    int64_t *start_times,
    int capacity) {
  (void)parent_pid;
  (void)pids;
  (void)parent_pids;
  (void)start_times;
  (void)capacity;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_process_info(
    int64_t pid,
    char *command,
    size_t command_capacity,
    char *argument_block,
    size_t argument_block_capacity,
    size_t *command_length,
    size_t *argument_block_length,
    int *argument_count,
    int64_t *start_time) {
  (void)pid;
  (void)command;
  (void)command_capacity;
  (void)argument_block;
  (void)argument_block_capacity;
  (void)command_length;
  (void)argument_block_length;
  (void)argument_count;
  (void)start_time;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_environment(
    char *entry_block,
    size_t entry_block_capacity,
    size_t *entry_block_length,
    int *entry_count) {
  (void)entry_block;
  (void)entry_block_capacity;
  (void)entry_block_length;
  (void)entry_count;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_identity_snapshot(int64_t identity[4]) {
  (void)identity;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_watchdog(
    int action,
    int64_t timeout_ms,
    int signal,
    int64_t status[4]) {
  (void)action;
  (void)timeout_ms;
  (void)signal;
  (void)status;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_setsid(int64_t identity[2]) {
  (void)identity;
  errno = ENOSYS;
  return -1;
}

static inline int64_t tracejvm_host_setpgid(int64_t pid, int64_t pgid) {
  (void)pid;
  (void)pgid;
  errno = ENOSYS;
  return -1;
}

static inline int64_t tracejvm_host_tcgetpgrp(int fd) {
  (void)fd;
  errno = ENOSYS;
  return -1;
}

static inline int64_t tracejvm_host_tcsetpgrp(int fd, int64_t pgid) {
  (void)fd;
  (void)pgid;
  errno = ENOSYS;
  return -1;
}

static inline int tracejvm_host_tcgetwinsize(int fd, int64_t size[2]) {
  struct winsize window;
  if (!size || ioctl(fd, TIOCGWINSZ, &window) != 0)
    return -1;
  size[0] = window.ws_row;
  size[1] = window.ws_col;
  return 0;
}

static inline int tracejvm_host_tcsetwinsize(
    int fd, int64_t rows, int64_t columns, int64_t size[2]) {
  if (!size || rows <= 0 || rows > UINT16_MAX ||
      columns <= 0 || columns > UINT16_MAX) {
    errno = EINVAL;
    return -1;
  }
  struct winsize window = {
      .ws_row = (unsigned short)rows,
      .ws_col = (unsigned short)columns,
  };
  if (ioctl(fd, TIOCSWINSZ, &window) != 0)
    return -1;
  size[0] = window.ws_row;
  size[1] = window.ws_col;
  return 0;
}

static inline int tracejvm_host_poll_signal(void) {
  return 0;
}

static inline int tracejvm_host_process_exists(int64_t pid) {
  return kill((pid_t)pid, 0) == 0 ? 1 : 0;
}

static inline int tracejvm_host_kill(int64_t pid, bool force) {
  return kill((pid_t)pid, force ? SIGKILL : SIGTERM);
}

#endif
