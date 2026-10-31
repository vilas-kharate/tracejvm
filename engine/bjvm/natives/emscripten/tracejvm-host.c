#include "tracejvm-host.h"

#ifdef EMSCRIPTEN

#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
  TRACEJVM_INOTIFY_FD_BASE = 0x20000000,
  TRACEJVM_INOTIFY_INSTANCE_LIMIT = 64,
  TRACEJVM_INOTIFY_WATCH_LIMIT = 1024,
  TRACEJVM_WATCH_FRAME_HEADER_SIZE = 9,
  TRACEJVM_WATCH_PATH_LIMIT = 16 * 1024,
  TRACEJVM_EPOLL_FD_BASE = 0x30000000,
  TRACEJVM_EPOLL_INSTANCE_LIMIT = 64,
  TRACEJVM_EPOLL_ENTRY_LIMIT = 1024,
  TRACEJVM_EVENTFD_LIMIT = 64,
  TRACEJVM_REMOTE_FD_BASE = 0x40000000,
  TRACEJVM_REMOTE_FD_LIMIT = 0x7fffffff,
};

typedef struct {
  int fd;
  int events;
} tracejvm_epoll_entry;

typedef struct {
  bool used;
  uint32_t context;
  int count;
  int pending_call_id;
  tracejvm_epoll_entry entries[TRACEJVM_EPOLL_ENTRY_LIMIT];
} tracejvm_epoll_instance;

typedef struct {
  bool used;
  uint32_t context;
  int read_fd;
  int write_fd;
} tracejvm_eventfd;

typedef struct {
  bool used;
  uint32_t context;
  int epoll_fd;
  int wake_fd;
  int pending_call_id;
} tracejvm_inotify_instance;

typedef struct {
  bool used;
  int owner;
  int fd;
  int mask;
  bool ready;
  char *path;
} tracejvm_inotify_watch;

static tracejvm_epoll_instance
    tracejvm_epoll_instances[TRACEJVM_EPOLL_INSTANCE_LIMIT];
static tracejvm_eventfd tracejvm_eventfds[TRACEJVM_EVENTFD_LIMIT];
static tracejvm_inotify_instance
    tracejvm_inotify_instances[TRACEJVM_INOTIFY_INSTANCE_LIMIT];
static tracejvm_inotify_watch
    tracejvm_inotify_watches[TRACEJVM_INOTIFY_WATCH_LIMIT];

struct tracejvm_host_dir {
  bool remote;
  union {
    DIR *local;
    int host_handle;
  };
};

EM_JS(bool, tracejvm_host_available_js, (), {
  const available = Module["traceJVMHostAvailable"];
  return typeof available === "function" && available();
});

EM_JS(bool, tracejvm_host_standard_descriptors_js, (), {
  const enabled = Module["traceJVMHostStandardDescriptors"];
  return typeof enabled === "function" && enabled();
});

EM_JS(uint32_t, tracejvm_host_context_js, (), {
  const getContextId = Module["traceJVMHostContextId"];
  const context = typeof getContextId === "function" ? getContextId() : 0;
  return Number.isSafeInteger(context) && context > 0 ? context : 0;
});

EM_JS(int, tracejvm_host_take_errno_js,
      (int eacces, int eagain, int ebadf, int ebusy, int eexist, int efbig,
       int eintr, int einval, int eisdir, int eloop, int emfile, int enametoolong,
       int enfile, int enoent, int enospc, int enosys, int enotdir,
       int enotempty, int enxio, int eoverflow, int eperm, int epipe,
       int erofs, int exdev, int eaddrinuse, int eafnosupport, int ealready,
       int econnrefused, int edestaddrreq, int einprogress, int eisconn,
       int enotconn, int eopnotsupp, int echild, int eproto, int esrch,
       int eio), {
  const error = Module["traceJVMHostLastError"];
  Module["traceJVMHostLastError"] = undefined;
  switch (error && (error.code || error.name)) {
    case "EACCES": return eacces;
    case "EAGAIN": return eagain;
    case "EBADF": return ebadf;
    case "EBUSY": return ebusy;
    case "EEXIST": return eexist;
    case "EFBIG": return efbig;
    case "EINTR": return eintr;
    case "EINVAL": return einval;
    case "EISDIR": return eisdir;
    case "ELOOP": return eloop;
    case "EMFILE": return emfile;
    case "ENAMETOOLONG": return enametoolong;
    case "ENFILE": return enfile;
    case "ENOENT": return enoent;
    case "ENOSPC": return enospc;
    case "ENOSYS": return enosys;
    case "ENOTDIR": return enotdir;
    case "ENOTEMPTY": return enotempty;
    case "ENXIO": return enxio;
    case "EOVERFLOW": return eoverflow;
    case "EPERM": return eperm;
    case "EPIPE": return epipe;
    case "EROFS": return erofs;
    case "EXDEV": return exdev;
    case "EADDRINUSE": return eaddrinuse;
    case "EAFNOSUPPORT": return eafnosupport;
    case "EALREADY": return ealready;
    case "ECONNREFUSED": return econnrefused;
    case "EDESTADDRREQ": return edestaddrreq;
    case "EINPROGRESS": return einprogress;
    case "EISCONN": return eisconn;
    case "ENOTCONN": return enotconn;
    case "EOPNOTSUPP": return eopnotsupp;
    case "ECHILD": return echild;
    case "EPROTO": return eproto;
    case "ESRCH": return esrch;
    default: return eio;
  }
});

static int take_host_errno(void) {
  return tracejvm_host_take_errno_js(
      EACCES, EAGAIN, EBADF, EBUSY, EEXIST, EFBIG, EINTR, EINVAL, EISDIR,
      ELOOP, EMFILE, ENAMETOOLONG, ENFILE, ENOENT, ENOSPC, ENOSYS, ENOTDIR,
      ENOTEMPTY, ENXIO, EOVERFLOW, EPERM, EPIPE, EROFS, EXDEV, EADDRINUSE,
      EAFNOSUPPORT, EALREADY, ECONNREFUSED, EDESTADDRREQ, EINPROGRESS, EISCONN,
      ENOTCONN, EOPNOTSUPP, ECHILD, EPROTO, ESRCH, EIO);
}

static int fail_from_host(void) {
  errno = take_host_errno();
  return -1;
}

static bool path_has_root(char const *path, char const *root) {
  size_t length = strlen(root);
  return strncmp(path, root, length) == 0 &&
         (path[length] == '\0' || path[length] == '/');
}

bool tracejvm_host_routes_path(char const *path) {
  if (!path || !tracejvm_host_available_js())
    return false;
  while (path[0] == '.' && path[1] == '/')
    path += 2;
  return !path_has_root(path, "/tracejvm") &&
         !path_has_root(path, "tracejvm") &&
         !path_has_root(path, "/jdk23") &&
         !path_has_root(path, "jdk23") &&
         strcmp(path, "/compiler-23.jar") != 0 &&
         strcmp(path, "compiler-23.jar") != 0 &&
         strcmp(path, "/jdk23.jar") != 0 &&
         strcmp(path, "jdk23.jar") != 0;
}

bool tracejvm_host_is_remote_fd(int fd) {
  return fd >= TRACEJVM_REMOTE_FD_BASE && fd <= TRACEJVM_REMOTE_FD_LIMIT;
}

bool tracejvm_host_routes_fd(int fd) {
  return tracejvm_host_is_remote_fd(fd) ||
      (fd >= 0 && fd <= 2 &&
       tracejvm_host_available_js() &&
       tracejvm_host_standard_descriptors_js());
}

EM_JS(int, tracejvm_host_getcwd_js, (char *buffer, size_t capacity), {
  try {
    const getWorkingDirectory = Module["traceJVMHostWorkingDirectory"];
    const path = typeof getWorkingDirectory === "function"
      ? getWorkingDirectory()
      : "/";
    if (typeof path !== "string" || !path.startsWith("/")) {
      throw Object.assign(
        new Error("Invalid JVM process working directory."),
        {name: "EINVAL"},
      );
    }
    const required = lengthBytesUTF8(path) + 1;
    if (required > capacity) {
      throw Object.assign(
        new Error("JVM process working directory exceeds the buffer."),
        {name: "ENAMETOOLONG"},
      );
    }
    stringToUTF8(path, buffer, capacity);
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

char *tracejvm_host_getcwd(char *buffer, size_t capacity) {
  if (!buffer || capacity == 0) {
    errno = EINVAL;
    return nullptr;
  }
  if (!tracejvm_host_available_js())
    return getcwd(buffer, capacity);
  if (tracejvm_host_getcwd_js(buffer, capacity) < 0) {
    fail_from_host();
    return nullptr;
  }
  return buffer;
}

static int remote_fd(int tagged_fd) {
  return tracejvm_host_is_remote_fd(tagged_fd)
      ? tagged_fd - TRACEJVM_REMOTE_FD_BASE
      : tagged_fd;
}

static int tag_remote_fd(int fd) {
  if (fd < 0 || fd >= TRACEJVM_REMOTE_FD_BASE) {
    errno = fd < 0 ? EPROTO : EMFILE;
    return -1;
  }
  return TRACEJVM_REMOTE_FD_BASE + fd;
}

static tracejvm_epoll_instance *epoll_instance(int fd) {
  int index = fd - TRACEJVM_EPOLL_FD_BASE;
  if (index < 0 || index >= TRACEJVM_EPOLL_INSTANCE_LIMIT ||
      !tracejvm_epoll_instances[index].used ||
      tracejvm_epoll_instances[index].context != tracejvm_host_context_js())
    return nullptr;
  return &tracejvm_epoll_instances[index];
}

static tracejvm_inotify_instance *inotify_instance(int fd) {
  int index = fd - TRACEJVM_INOTIFY_FD_BASE;
  if (index < 0 || index >= TRACEJVM_INOTIFY_INSTANCE_LIMIT ||
      !tracejvm_inotify_instances[index].used ||
      tracejvm_inotify_instances[index].context != tracejvm_host_context_js())
    return nullptr;
  return &tracejvm_inotify_instances[index];
}

static int inotify_instance_index(tracejvm_inotify_instance const *instance) {
  return (int)(instance - tracejvm_inotify_instances);
}

static tracejvm_inotify_watch *inotify_watch(int owner, int wd) {
  for (int index = 0; index < TRACEJVM_INOTIFY_WATCH_LIMIT; index++) {
    tracejvm_inotify_watch *watch = &tracejvm_inotify_watches[index];
    if (watch->used && watch->owner == owner && watch->fd == wd)
      return watch;
  }
  return nullptr;
}

static ssize_t tracejvm_host_inotify_read(
    tracejvm_inotify_instance *instance, void *buffer, size_t length);

static tracejvm_eventfd *eventfd_instance(int fd) {
  for (int index = 0; index < TRACEJVM_EVENTFD_LIMIT; index++) {
    tracejvm_eventfd *instance = &tracejvm_eventfds[index];
    if (instance->used &&
        instance->context == tracejvm_host_context_js() &&
        instance->read_fd == fd)
      return instance;
  }
  return nullptr;
}

EM_JS(int, tracejvm_host_open_js,
      (char const *path, int access, int create, int exclusive, int truncate,
       int append), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "open",
      payload: {
        path: UTF8ToString(path),
        options: {
          access: access === 0 ? "read" : access === 1 ? "write" : "read-write",
          create: !!create,
          exclusive: !!exclusive,
          truncate: !!truncate,
          append: !!append,
        },
      },
    });
    if (!result || !Number.isSafeInteger(result.fd) || result.fd < 0) {
      throw Object.assign(new Error("Invalid host open response."), {name: "EPROTO"});
    }
    return result.fd;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_open(char const *path, int flags, mode_t mode) {
  if (!tracejvm_host_routes_path(path))
    return open(path, flags, mode);
  int access = (flags & O_RDWR) == O_RDWR ? 2 :
               (flags & O_WRONLY) == O_WRONLY ? 1 : 0;
  int fd = tracejvm_host_open_js(
      path, access, flags & O_CREAT, flags & O_EXCL, flags & O_TRUNC,
      flags & O_APPEND);
  if (fd < 0)
    return fail_from_host();
  return tag_remote_fd(fd);
}

EM_JS(int, tracejvm_host_read_js,
      (int fd, void *buffer, int length, double position, int positioned), {
  try {
    const payload = {fd, maxBytes: length};
    if (positioned) payload.position = position;
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "read",
      payload,
    });
    if (!result || !(result.bytes instanceof Uint8Array)) {
      throw Object.assign(new Error("Invalid host read response."), {name: "EPROTO"});
    }
    const count = Math.min(length, result.bytes.byteLength);
    HEAPU8.set(result.bytes.subarray(0, count), buffer);
    return count;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

ssize_t tracejvm_host_read(
    int fd, void *buffer, size_t length, off_t position, bool positioned) {
  tracejvm_inotify_instance *inotify = inotify_instance(fd);
  if (inotify) {
    if (positioned) {
      errno = ESPIPE;
      return -1;
    }
    return tracejvm_host_inotify_read(inotify, buffer, length);
  }
  if (!tracejvm_host_routes_fd(fd))
    return positioned ? pread(fd, buffer, length, position)
                      : read(fd, buffer, length);
  if (length > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_read_js(
      remote_fd(fd), buffer, (int)length, (double)position, positioned);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_write_js,
      (int fd, void const *buffer, int length, double position, int positioned), {
  try {
    const payload = {
      fd,
      bytes: HEAPU8.slice(buffer, buffer + length),
    };
    if (positioned) payload.position = position;
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "write",
      payload,
    });
    if (!result || !Number.isSafeInteger(result.bytesWritten) ||
        result.bytesWritten < 0 || result.bytesWritten > length ||
        (length > 0 && result.bytesWritten === 0)) {
      throw Object.assign(new Error("Invalid host write response."), {name: "EPROTO"});
    }
    return result.bytesWritten;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

ssize_t tracejvm_host_write(
    int fd, void const *buffer, size_t length, off_t position, bool positioned) {
  if (!tracejvm_host_routes_fd(fd))
    return positioned ? pwrite(fd, buffer, length, position)
                      : write(fd, buffer, length);
  if (length > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_write_js(
      remote_fd(fd), buffer, (int)length, (double)position, positioned);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_close_js, (int fd), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "close",
      payload: {fd},
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_close(int fd) {
  tracejvm_inotify_instance *inotify = inotify_instance(fd);
  if (inotify) {
    int owner = inotify_instance_index(inotify);
    int first_error = 0;
    for (int index = 0; index < TRACEJVM_INOTIFY_WATCH_LIMIT; index++) {
      tracejvm_inotify_watch *watch = &tracejvm_inotify_watches[index];
      if (!watch->used || watch->owner != owner)
        continue;
      if (tracejvm_host_close_js(remote_fd(watch->fd)) < 0 && !first_error)
        first_error = take_host_errno();
      free(watch->path);
      memset(watch, 0, sizeof(*watch));
    }
    if (tracejvm_host_close(inotify->epoll_fd) != 0 && !first_error)
      first_error = errno;
    memset(inotify, 0, sizeof(*inotify));
    if (first_error) {
      errno = first_error;
      return -1;
    }
    return 0;
  }
  tracejvm_epoll_instance *epoll = epoll_instance(fd);
  if (epoll) {
    memset(epoll, 0, sizeof(*epoll));
    return 0;
  }
  tracejvm_eventfd *eventfd = eventfd_instance(fd);
  if (eventfd) {
    int read_result =
        tracejvm_host_close_js(remote_fd(eventfd->read_fd));
    int read_error = read_result < 0 ? take_host_errno() : 0;
    int write_result =
        tracejvm_host_close_js(remote_fd(eventfd->write_fd));
    int write_error = write_result < 0 ? take_host_errno() : 0;
    memset(eventfd, 0, sizeof(*eventfd));
    if (read_error || write_error) {
      errno = read_error ? read_error : write_error;
      return -1;
    }
    return 0;
  }
  if (!tracejvm_host_routes_fd(fd))
    return close(fd);
  return tracejvm_host_close_js(remote_fd(fd)) < 0 ? fail_from_host() : 0;
}

EM_JS(double, tracejvm_host_seek_js,
      (int fd, double offset, int whence), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "seek",
      payload: {
        fd,
        offset,
        whence: whence === 0 ? "set" : whence === 1 ? "current" : "end",
      },
    });
    if (!result || !Number.isSafeInteger(result.offset) || result.offset < 0) {
      throw Object.assign(new Error("Invalid host seek response."), {name: "EPROTO"});
    }
    return result.offset;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

off_t tracejvm_host_seek(int fd, off_t offset, int whence) {
  if (!tracejvm_host_routes_fd(fd))
    return lseek(fd, offset, whence);
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    errno = EINVAL;
    return (off_t)-1;
  }
  double result = tracejvm_host_seek_js(
      remote_fd(fd), (double)offset, whence);
  return result < 0 ? (fail_from_host(), (off_t)-1) : (off_t)result;
}

EM_JS(int, tracejvm_host_stat_js,
      (char const *operation, char const *path, int fd, int *kind_out,
       int *mode_out, int64_t *inode_out, int64_t *nlink_out,
       int64_t *size_out, int64_t *created_out, int64_t *modified_out,
       int64_t *changed_out), {
  try {
    const op = UTF8ToString(operation);
    const payload = path ? {path: UTF8ToString(path)} : {fd};
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: op,
      payload,
    });
    const stat = result && result.stat;
    if (!stat || !Number.isSafeInteger(stat.mode) ||
        !Number.isSafeInteger(stat.inode) || !Number.isSafeInteger(stat.nlink) ||
        !Number.isSafeInteger(stat.size)) {
      throw Object.assign(new Error("Invalid host stat response."), {name: "EPROTO"});
    }
    const kind = stat.kind === "file" ? 1 :
      stat.kind === "directory" ? 2 : stat.kind === "symlink" ? 3 : 0;
    if (!kind) {
      throw Object.assign(new Error("Invalid host stat kind."), {name: "EPROTO"});
    }
    setValue(kind_out, kind, "i32");
    setValue(mode_out, stat.mode, "i32");
    setValue(inode_out, BigInt(stat.inode), "i64");
    setValue(nlink_out, BigInt(stat.nlink), "i64");
    setValue(size_out, BigInt(stat.size), "i64");
    setValue(created_out, BigInt(Math.floor(stat.createdAt || 0)), "i64");
    setValue(modified_out, BigInt(Math.floor(stat.modifiedAt || 0)), "i64");
    setValue(changed_out, BigInt(Math.floor(stat.changedAt || 0)), "i64");
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

static int fill_host_stat(
    char const *operation, char const *path, int fd, struct stat *buffer) {
  int kind;
  int mode;
  int64_t inode;
  int64_t nlink;
  int64_t size;
  int64_t created_at;
  int64_t modified_at;
  int64_t changed_at;
  if (tracejvm_host_stat_js(
          operation, path, fd, &kind, &mode, &inode, &nlink, &size,
          &created_at, &modified_at, &changed_at) < 0)
    return fail_from_host();

  memset(buffer, 0, sizeof(*buffer));
  buffer->st_dev = 1;
  buffer->st_ino = (ino_t)inode;
  buffer->st_mode = (mode_t)mode |
      (kind == 1 ? S_IFREG : kind == 2 ? S_IFDIR : S_IFLNK);
  buffer->st_nlink = (nlink_t)nlink;
  buffer->st_size = (off_t)size;
  buffer->st_atim.tv_sec = modified_at / 1000;
  buffer->st_atim.tv_nsec = (modified_at % 1000) * 1000000;
  buffer->st_mtim.tv_sec = modified_at / 1000;
  buffer->st_mtim.tv_nsec = (modified_at % 1000) * 1000000;
  buffer->st_ctim.tv_sec = changed_at / 1000;
  buffer->st_ctim.tv_nsec = (changed_at % 1000) * 1000000;
  (void)created_at;
  return 0;
}

int tracejvm_host_fstat(int fd, struct stat *stat_buffer) {
  if (!tracejvm_host_routes_fd(fd))
    return fstat(fd, stat_buffer);
  return fill_host_stat("fstat", nullptr, remote_fd(fd), stat_buffer);
}

EM_JS(int, tracejvm_host_ftruncate_js, (int fd, double length), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "ftruncate",
      payload: {fd, length},
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_ftruncate(int fd, off_t length) {
  if (!tracejvm_host_routes_fd(fd))
    return ftruncate(fd, length);
  return tracejvm_host_ftruncate_js(remote_fd(fd), (double)length) < 0
      ? fail_from_host()
      : 0;
}

int tracejvm_host_stat(
    char const *path, struct stat *stat_buffer, bool follow_links) {
  if (!tracejvm_host_routes_path(path))
    return follow_links ? stat(path, stat_buffer) : lstat(path, stat_buffer);
  return fill_host_stat(follow_links ? "stat" : "lstat", path, -1, stat_buffer);
}

EM_JS(int, tracejvm_host_path_call_js,
      (char const *operation, char const *path, int mode), {
  try {
    const op = UTF8ToString(operation);
    const payload = {path: UTF8ToString(path)};
    if (op === "mkdir" || op === "chmod") payload.options = {mode};
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: op,
      payload,
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

static int host_path_call(char const *operation, char const *path, int mode) {
  return tracejvm_host_path_call_js(operation, path, mode) < 0
      ? fail_from_host()
      : 0;
}

int tracejvm_host_mkdir(char const *path, mode_t mode) {
  return tracejvm_host_routes_path(path)
      ? host_path_call("mkdir", path, mode)
      : mkdir(path, mode);
}

int tracejvm_host_rmdir(char const *path) {
  return tracejvm_host_routes_path(path)
      ? host_path_call("rmdir", path, 0)
      : rmdir(path);
}

int tracejvm_host_unlink(char const *path) {
  return tracejvm_host_routes_path(path)
      ? host_path_call("unlink", path, 0)
      : unlink(path);
}

// A file's permission bits and modification time (Shiklo fork, 2026-10-31: File.setReadOnly, setWritable,
// setLastModified and Files.setPosixFilePermissions had nothing to call): posix.chmod {path, options: {mode}} and
// posix.utimes {path, modifiedAt} (milliseconds since 1970).
int tracejvm_host_chmod(char const *path, mode_t mode) {
  return tracejvm_host_routes_path(path)
      ? host_path_call("chmod", path, (int)(mode & 07777))
      : chmod(path, mode);
}

EM_JS(int, tracejvm_host_utimes_js, (char const *path, double modified_ms), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "utimes",
      payload: {path: UTF8ToString(path), modifiedAt: modified_ms},
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_utimes(char const *path, double modified_ms) {
  if (!tracejvm_host_routes_path(path)) { // the runtime's own files
    errno = EROFS;
    return -1;
  }
  return tracejvm_host_utimes_js(path, modified_ms) < 0 ? fail_from_host() : 0;
}

EM_JS(int, tracejvm_host_two_path_call_js,
      (char const *operation, char const *first, char const *second), {
  try {
    const op = UTF8ToString(operation);
    const firstPath = UTF8ToString(first);
    const secondPath = UTF8ToString(second);
    const payload = op === "link"
      ? {existingPath: firstPath, newPath: secondPath}
      : {target: firstPath, linkPath: secondPath};
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: op,
      payload,
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_link(
    char const *existing_path, char const *new_path) {
  bool remote_existing = tracejvm_host_routes_path(existing_path);
  bool remote_new = tracejvm_host_routes_path(new_path);
  if (remote_existing != remote_new) {
    errno = EXDEV;
    return -1;
  }
  if (!remote_existing)
    return link(existing_path, new_path);
  return tracejvm_host_two_path_call_js(
      "link", existing_path, new_path) < 0
      ? fail_from_host()
      : 0;
}

int tracejvm_host_symlink(char const *target, char const *link_path) {
  if (!tracejvm_host_routes_path(link_path))
    return symlink(target, link_path);
  return tracejvm_host_two_path_call_js(
      "symlink", target, link_path) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_readlink_js,
      (char const *path, char *target, int capacity), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "readlink",
      payload: {path: UTF8ToString(path)},
    });
    if (!result || typeof result.target !== "string") {
      throw Object.assign(new Error("Invalid host readlink response."), {name: "EPROTO"});
    }
    const length = lengthBytesUTF8(result.target);
    if (length + 1 > capacity) {
      throw Object.assign(new Error("Link target is too long."), {name: "ENAMETOOLONG"});
    }
    stringToUTF8(result.target, target, capacity);
    return length;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

ssize_t tracejvm_host_readlink(
    char const *path, char *target, size_t capacity) {
  if (!tracejvm_host_routes_path(path))
    return readlink(path, target, capacity);
  if (!target || capacity > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_readlink_js(
      path, target, (int)capacity);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_rename_js,
      (char const *source, char const *destination), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "rename",
      payload: {
        sourcePath: UTF8ToString(source),
        destinationPath: UTF8ToString(destination),
      },
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_rename(char const *source, char const *destination) {
  bool remote_source = tracejvm_host_routes_path(source);
  bool remote_destination = tracejvm_host_routes_path(destination);
  if (remote_source != remote_destination) {
    errno = EXDEV;
    return -1;
  }
  if (!remote_source)
    return rename(source, destination);
  return tracejvm_host_rename_js(source, destination) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_realpath_js,
      (char const *path, char *resolved, int capacity), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "realpath",
      payload: {path: UTF8ToString(path)},
    });
    if (!result || typeof result.path !== "string") {
      throw Object.assign(new Error("Invalid host realpath response."), {name: "EPROTO"});
    }
    const required = lengthBytesUTF8(result.path) + 1;
    if (required > capacity) {
      throw Object.assign(new Error("Resolved path is too long."), {name: "ENAMETOOLONG"});
    }
    stringToUTF8(result.path, resolved, capacity);
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

char *tracejvm_host_realpath(
    char const *path, char *resolved, size_t capacity) {
  if (!tracejvm_host_routes_path(path))
    return realpath(path, resolved);
  if (!resolved || capacity > INT_MAX) {
    errno = EINVAL;
    return nullptr;
  }
  return tracejvm_host_realpath_js(path, resolved, (int)capacity) < 0
      ? (fail_from_host(), nullptr)
      : resolved;
}

EM_JS(int, tracejvm_host_opendir_js, (char const *path), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "readdir",
      payload: {path: UTF8ToString(path)},
    });
    if (!result || !Array.isArray(result.entries) ||
        result.entries.some((entry) =>
          !entry || typeof entry.name !== "string" ||
          entry.name.includes("/") || entry.name.includes("\0"))) {
      throw Object.assign(new Error("Invalid host readdir response."), {name: "EPROTO"});
    }
    const directories =
      Module["traceJVMHostDirectories"] ||= new Map();
    const handle =
      (Module["traceJVMHostNextDirectory"] =
        (Module["traceJVMHostNextDirectory"] || 0) + 1);
    directories.set(handle, {
      entries: result.entries.map((entry) => entry.name),
      index: 0,
    });
    return handle;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

EM_JS(int, tracejvm_host_readdir_js,
      (int handle, char *name, int capacity), {
  try {
    const directory = Module["traceJVMHostDirectories"]?.get(handle);
    if (!directory) {
      throw Object.assign(new Error("Invalid host directory handle."), {name: "EBADF"});
    }
    if (directory.index >= directory.entries.length)
      return 0;
    const entry = directory.entries[directory.index++];
    const required = lengthBytesUTF8(entry) + 1;
    if (required > capacity) {
      throw Object.assign(new Error("Directory entry is too long."), {name: "ENAMETOOLONG"});
    }
    stringToUTF8(entry, name, capacity);
    return 1;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

EM_JS(int, tracejvm_host_closedir_js, (int handle), {
  const directories = Module["traceJVMHostDirectories"];
  if (!directories || !directories.delete(handle)) {
    Module["traceJVMHostLastError"] =
      Object.assign(new Error("Invalid host directory handle."), {name: "EBADF"});
    return -1;
  }
  return 0;
});

tracejvm_host_dir *tracejvm_host_opendir(char const *path) {
  tracejvm_host_dir *directory = calloc(1, sizeof(*directory));
  if (!directory) {
    errno = ENOMEM;
    return nullptr;
  }
  if (!tracejvm_host_routes_path(path)) {
    directory->local = opendir(path);
    if (!directory->local) {
      free(directory);
      return nullptr;
    }
    return directory;
  }
  int handle = tracejvm_host_opendir_js(path);
  if (handle < 0) {
    free(directory);
    fail_from_host();
    return nullptr;
  }
  directory->remote = true;
  directory->host_handle = handle;
  return directory;
}

int tracejvm_host_readdir(
    tracejvm_host_dir *directory, char *name, size_t capacity) {
  if (!directory || !name || capacity == 0 || capacity > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  if (directory->remote) {
    int result = tracejvm_host_readdir_js(
        directory->host_handle, name, (int)capacity);
    return result < 0 ? fail_from_host() : result;
  }
  errno = 0;
  struct dirent *entry = readdir(directory->local);
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

int tracejvm_host_closedir(tracejvm_host_dir *directory) {
  if (!directory) {
    errno = EINVAL;
    return -1;
  }
  int result;
  if (directory->remote) {
    result = tracejvm_host_closedir_js(directory->host_handle);
    if (result < 0)
      result = fail_from_host();
  } else {
    result = closedir(directory->local);
  }
  free(directory);
  return result;
}

EM_JS(int, tracejvm_host_socket_js, (), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "socket",
      payload: {},
    });
    if (!result || !Number.isSafeInteger(result.fd) || result.fd < 0) {
      throw Object.assign(new Error("Invalid host socket response."), {name: "EPROTO"});
    }
    return result.fd;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_socket(void) {
  if (!tracejvm_host_available_js())
    return socket(AF_INET, SOCK_STREAM, 0);
  int fd = tracejvm_host_socket_js();
  return fd < 0 ? fail_from_host() : tag_remote_fd(fd);
}

EM_JS(int, tracejvm_host_bind_js,
      (int fd, char const *host, int port), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "bind",
      payload: {fd, address: {host: UTF8ToString(host), port}},
    });
    const address = result && result.address;
    if (!address || !Number.isSafeInteger(address.port) ||
        address.port < 0 || address.port > 65535) {
      throw Object.assign(new Error("Invalid host bind response."), {name: "EPROTO"});
    }
    return address.port;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_bind(
    int fd, char const *host, int port, int *bound_port) {
  if (!tracejvm_host_is_remote_fd(fd)) {
    errno = ENOSYS;
    return -1;
  }
  int result = tracejvm_host_bind_js(remote_fd(fd), host, port);
  if (result < 0)
    return fail_from_host();
  if (bound_port)
    *bound_port = result;
  return 0;
}

EM_JS(int, tracejvm_host_listen_js, (int fd, int backlog), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "listen",
      payload: {fd, options: {backlog}},
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_listen(int fd, int backlog) {
  if (!tracejvm_host_is_remote_fd(fd))
    return listen(fd, backlog);
  return tracejvm_host_listen_js(remote_fd(fd), backlog) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_accept_js,
      (int fd, char *remote_host, int remote_host_capacity,
       int *remote_port), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "accept",
      payload: {fd},
    });
    const address = result && result.remoteAddress;
    if (!result || !Number.isSafeInteger(result.fd) || result.fd < 0 ||
        !address || typeof address.host !== "string" ||
        !Number.isSafeInteger(address.port) ||
        address.port < 0 || address.port > 65535) {
      throw Object.assign(new Error("Invalid host accept response."), {name: "EPROTO"});
    }
    if (lengthBytesUTF8(address.host) + 1 > remote_host_capacity) {
      throw Object.assign(new Error("Host address exceeds native buffer."), {name: "EOVERFLOW"});
    }
    stringToUTF8(address.host, remote_host, remote_host_capacity);
    setValue(remote_port, address.port, "i32");
    return result.fd;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_accept(
    int fd, char *remote_host, size_t remote_host_capacity, int *remote_port) {
  if (!tracejvm_host_is_remote_fd(fd))
    return accept(fd, nullptr, nullptr);
  if (remote_host_capacity > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  int accepted = tracejvm_host_accept_js(
      remote_fd(fd), remote_host, (int)remote_host_capacity, remote_port);
  return accepted < 0 ? fail_from_host() : tag_remote_fd(accepted);
}

EM_JS(int, tracejvm_host_connect_js,
      (int fd, char const *host, int port), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "connect",
      payload: {fd, address: {host: UTF8ToString(host), port}},
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_connect(int fd, char const *host, int port) {
  if (!tracejvm_host_is_remote_fd(fd)) {
    errno = ENOSYS;
    return -1;
  }
  return tracejvm_host_connect_js(remote_fd(fd), host, port) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_shutdown_js, (int fd, int how), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "shutdown",
      payload: {
        fd,
        how: how === 0 ? "read" : how === 1 ? "write" : "both",
      },
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_shutdown(int fd, int how) {
  if (!tracejvm_host_is_remote_fd(fd))
    return shutdown(fd, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR);
  return tracejvm_host_shutdown_js(remote_fd(fd), how) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_socket_address_js,
      (int fd, int peer, char *host, int host_capacity, int *port), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: peer ? "getpeername" : "getsockname",
      payload: {fd},
    });
    const address = result && result.address;
    if (!address || typeof address.host !== "string" ||
        !Number.isSafeInteger(address.port) ||
        address.port < 0 || address.port > 65535) {
      throw Object.assign(new Error("Invalid host socket address response."), {name: "EPROTO"});
    }
    if (lengthBytesUTF8(address.host) + 1 > host_capacity) {
      throw Object.assign(new Error("Host address exceeds native buffer."), {name: "EOVERFLOW"});
    }
    stringToUTF8(address.host, host, host_capacity);
    setValue(port, address.port, "i32");
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_socket_address(
    int fd, bool peer, char *host, size_t host_capacity, int *port) {
  if (!tracejvm_host_is_remote_fd(fd)) {
    errno = ENOSYS;
    return -1;
  }
  if (host_capacity > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  return tracejvm_host_socket_address_js(
      remote_fd(fd), peer, host, (int)host_capacity, port) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_configure_blocking_js, (int fd, int blocking), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "fcntl",
      payload: {
        fd,
        action: "set-nonblocking",
        nonblocking: !blocking,
      },
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_configure_blocking(int fd, bool blocking) {
  if (inotify_instance(fd))
    return 0;
  if (!tracejvm_host_routes_fd(fd)) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
      return -1;
    return fcntl(fd, F_SETFL, blocking ? flags & ~O_NONBLOCK
                                      : flags | O_NONBLOCK);
  }
  return tracejvm_host_configure_blocking_js(remote_fd(fd), blocking) < 0
      ? fail_from_host()
      : 0;
}

EM_JS(int, tracejvm_host_poll_js,
      (int fd, int read, int write, int timeout_ms), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "poll",
      payload: {
        entries: [{fd, read: !!read, write: !!write}],
        timeoutMs: timeout_ms < 0 ? undefined : timeout_ms,
      },
    });
    const entry = result && result.entries && result.entries[0];
    if (!entry || entry.fd !== fd) {
      throw Object.assign(new Error("Invalid host poll response."), {name: "EPROTO"});
    }
    return (entry.read ? 1 : 0) |
      (entry.write ? 2 : 0) |
      (entry.error ? 4 : 0) |
      (entry.hangup ? 8 : 0) |
      (entry.invalid ? 16 : 0);
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_poll(
    int fd, bool read, bool write, int timeout_ms) {
  if (!tracejvm_host_routes_fd(fd)) {
    errno = ENOSYS;
    return -1;
  }
  int result = tracejvm_host_poll_js(
      remote_fd(fd), read, write, timeout_ms);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_pipe_js,
      (int *descriptors, int blocking, int remote_fd_base), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "pipe",
      payload: {
        options: {
          nonblocking: !blocking,
        },
      },
    });
    if (!result ||
        !Number.isSafeInteger(result.readFd) || result.readFd < 0 ||
        result.readFd >= remote_fd_base ||
        !Number.isSafeInteger(result.writeFd) || result.writeFd < 0 ||
        result.writeFd >= remote_fd_base) {
      throw Object.assign(
        new Error("Invalid host pipe response."),
        {name: "EPROTO"},
      );
    }
    HEAP32[descriptors >> 2] = result.readFd + remote_fd_base;
    HEAP32[(descriptors >> 2) + 1] = result.writeFd + remote_fd_base;
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_pipe(int descriptors[2], bool blocking) {
  if (!descriptors) {
    errno = EINVAL;
    return -1;
  }
  return tracejvm_host_pipe_js(
      descriptors, blocking, TRACEJVM_REMOTE_FD_BASE) < 0
      ? fail_from_host()
      : 0;
}

int tracejvm_host_eventfd(void) {
  int descriptors[2];
  if (tracejvm_host_pipe(descriptors, false) != 0)
    return -1;
  for (int index = 0; index < TRACEJVM_EVENTFD_LIMIT; index++) {
    tracejvm_eventfd *instance = &tracejvm_eventfds[index];
    if (instance->used)
      continue;
    *instance = (tracejvm_eventfd){
        .used = true,
        .context = tracejvm_host_context_js(),
        .read_fd = descriptors[0],
        .write_fd = descriptors[1],
    };
    return descriptors[0];
  }
  int error = EMFILE;
  tracejvm_host_close_js(remote_fd(descriptors[0]));
  tracejvm_host_close_js(remote_fd(descriptors[1]));
  errno = error;
  return -1;
}

int tracejvm_host_eventfd_set(int fd) {
  tracejvm_eventfd *instance = eventfd_instance(fd);
  if (!instance) {
    errno = EBADF;
    return -1;
  }
  uint8_t value = 1;
  return tracejvm_host_write(
      instance->write_fd, &value, sizeof(value), 0, false) < 0
      ? -1
      : 0;
}

int tracejvm_host_epoll_create(void) {
  for (int index = 0; index < TRACEJVM_EPOLL_INSTANCE_LIMIT; index++) {
    tracejvm_epoll_instance *instance = &tracejvm_epoll_instances[index];
    if (instance->used)
      continue;
    memset(instance, 0, sizeof(*instance));
    instance->used = true;
    instance->context = tracejvm_host_context_js();
    return TRACEJVM_EPOLL_FD_BASE + index;
  }
  errno = EMFILE;
  return -1;
}

int tracejvm_host_epoll_ctl(
    int epfd, int operation, int fd, int events) {
  tracejvm_epoll_instance *instance = epoll_instance(epfd);
  if (!instance || !tracejvm_host_routes_fd(fd)) {
    errno = EBADF;
    return -1;
  }
  int index = -1;
  for (int current = 0; current < instance->count; current++) {
    if (instance->entries[current].fd == fd) {
      index = current;
      break;
    }
  }
  if (operation == 1) {
    if (index >= 0) {
      errno = EEXIST;
      return -1;
    }
    if (instance->count >= TRACEJVM_EPOLL_ENTRY_LIMIT) {
      errno = ENOSPC;
      return -1;
    }
    instance->entries[instance->count++] =
        (tracejvm_epoll_entry){.fd = fd, .events = events};
    return 0;
  }
  if (operation == 2) {
    if (index < 0) {
      errno = ENOENT;
      return -1;
    }
    instance->entries[index] = instance->entries[--instance->count];
    return 0;
  }
  if (operation == 3) {
    if (index < 0) {
      errno = ENOENT;
      return -1;
    }
    instance->entries[index].events = events;
    return 0;
  }
  errno = EINVAL;
  return -1;
}

EM_JS(int, tracejvm_host_epoll_wait_begin_js,
      (int *descriptors, int *events, int count, int timeout_ms,
       int remote_fd_base), {
  const dispatch = Module["traceJVMHostDispatchAsync"];
  if (typeof dispatch !== "function") {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error("The asynchronous system host is unavailable."),
      {name: "ENOSYS"},
    );
    return -1;
  }
  const entries = [];
  for (let index = 0; index < count; index += 1) {
    const taggedFd = HEAP32[(descriptors >> 2) + index];
    const requested = HEAP32[(events >> 2) + index];
    if (taggedFd < remote_fd_base) {
      Module["traceJVMHostLastError"] = Object.assign(
        new Error(`Cannot poll VM-local descriptor ${taggedFd}.`),
        {name: "EBADF"},
      );
      return -1;
    }
    entries.push({
      fd: taggedFd - remote_fd_base,
      read: (requested & 0x001) !== 0,
      write: (requested & 0x004) !== 0,
    });
  }
  let pending = Module["traceJVMHostAsyncCalls"];
  if (!pending) {
    pending = new Map();
    Module["traceJVMHostAsyncCalls"] = pending;
  }
  let id = Module["traceJVMHostNextAsyncCallId"] ?? 0;
  do {
    id = id >= 0x7fffffff ? 1 : id + 1;
  } while (pending.has(id));
  Module["traceJVMHostNextAsyncCallId"] = id;
  const getContextId = Module["traceJVMHostContextId"];
  const contextId =
    typeof getContextId === "function" ? getContextId() : 0;
  const call = {settled: false, epollEntries: entries, contextId};
  pending.set(id, call);
  Promise.resolve()
    .then(() => dispatch({
      service: "posix",
      operation: "poll",
      payload: {
        entries,
        timeoutMs: timeout_ms < 0 ? undefined : timeout_ms,
      },
    }, contextId))
    .then(
      (value) => {
        call.settled = true;
        call.value = value;
      },
      (error) => {
        call.settled = true;
        call.error = error;
      },
    );
  return id;
});

int tracejvm_host_epoll_wait_begin(int epfd, int timeout_ms) {
  tracejvm_epoll_instance *instance = epoll_instance(epfd);
  if (!instance) {
    errno = EBADF;
    return -1;
  }
  if (instance->pending_call_id > 0)
    return instance->pending_call_id;
  int descriptors[TRACEJVM_EPOLL_ENTRY_LIMIT];
  int events[TRACEJVM_EPOLL_ENTRY_LIMIT];
  for (int index = 0; index < instance->count; index++) {
    descriptors[index] = instance->entries[index].fd;
    events[index] = instance->entries[index].events;
  }
  int call_id = tracejvm_host_epoll_wait_begin_js(
      descriptors, events, instance->count, timeout_ms,
      TRACEJVM_REMOTE_FD_BASE);
  if (call_id < 0)
    return fail_from_host();
  instance->pending_call_id = call_id;
  return call_id;
}

EM_JS(int, tracejvm_host_epoll_wait_poll_js,
      (int call_id, uint8_t *poll_array, int capacity,
       int remote_fd_base, int pending_value), {
  const pending = Module["traceJVMHostAsyncCalls"];
  const call = pending && pending.get(call_id);
  if (!call) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error(`Unknown asynchronous host call ${call_id}.`),
      {name: "EINVAL"},
    );
    return -1;
  }
  if (!call.settled) return pending_value;
  pending.delete(call_id);
  if (call.error !== undefined) {
    Module["traceJVMHostLastError"] = call.error;
    return -1;
  }
  const returned = call.value && call.value.entries;
  if (!Array.isArray(returned)) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error("Invalid host epoll response."),
      {name: "EPROTO"},
    );
    return -1;
  }
  let ready = 0;
  for (let index = 0; index < returned.length; index += 1) {
    const entry = returned[index];
    const expected = entry && call.epollEntries.find(
      (candidate) => candidate.fd === entry.fd,
    );
    if (!expected) {
      Module["traceJVMHostLastError"] = Object.assign(
        new Error("Host epoll response included an unknown descriptor."),
        {name: "EPROTO"},
      );
      return -1;
    }
    let flags = 0;
    if (entry.read) flags |= 0x001;
    if (entry.write) flags |= 0x004;
    if (entry.error) flags |= 0x008;
    if (entry.hangup || entry.invalid) flags |= 0x010;
    if (!flags) continue;
    if (ready >= capacity) break;
    const eventAddress = poll_array + ready * 12;
    HEAP32[eventAddress >> 2] = flags;
    HEAP32[(eventAddress + 4) >> 2] = entry.fd + remote_fd_base;
    ready += 1;
  }
  return ready;
});

int tracejvm_host_epoll_wait_poll(
    int call_id, void *poll_array, int capacity) {
  if (capacity < 0) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_epoll_wait_poll_js(
      call_id, poll_array, capacity, TRACEJVM_REMOTE_FD_BASE,
      TRACEJVM_HOST_ASYNC_PENDING);
  if (result == TRACEJVM_HOST_ASYNC_PENDING)
    return result;
  for (int index = 0; index < TRACEJVM_EPOLL_INSTANCE_LIMIT; index++) {
    if (tracejvm_epoll_instances[index].pending_call_id == call_id) {
      tracejvm_epoll_instances[index].pending_call_id = 0;
      break;
    }
  }
  return result == -1 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_watch_js,
      (char const *path, int remote_fd_base), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "watch",
      payload: {
        path: UTF8ToString(path),
        options: {recursive: false},
      },
    });
    if (!result || !Number.isSafeInteger(result.fd) ||
        result.fd < 0 || result.fd >= remote_fd_base) {
      throw Object.assign(
        new Error("Invalid host watch response."),
        {name: "EPROTO"},
      );
    }
    return result.fd + remote_fd_base;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_inotify_init(void) {
  int epoll_fd = tracejvm_host_epoll_create();
  if (epoll_fd < 0)
    return -1;
  for (int index = 0; index < TRACEJVM_INOTIFY_INSTANCE_LIMIT; index++) {
    tracejvm_inotify_instance *instance = &tracejvm_inotify_instances[index];
    if (instance->used)
      continue;
    *instance = (tracejvm_inotify_instance){
        .used = true,
        .context = tracejvm_host_context_js(),
        .epoll_fd = epoll_fd,
        .wake_fd = -1,
    };
    return TRACEJVM_INOTIFY_FD_BASE + index;
  }
  tracejvm_host_close(epoll_fd);
  errno = EMFILE;
  return -1;
}

int tracejvm_host_inotify_add_watch(
    int ifd, char const *path, int mask) {
  tracejvm_inotify_instance *instance = inotify_instance(ifd);
  if (!instance || !path) {
    errno = !instance ? EBADF : EINVAL;
    return -1;
  }
  char resolved[PATH_MAX];
  if (!tracejvm_host_realpath(path, resolved, sizeof(resolved)))
    return -1;
  int owner = inotify_instance_index(instance);
  for (int index = 0; index < TRACEJVM_INOTIFY_WATCH_LIMIT; index++) {
    tracejvm_inotify_watch *watch = &tracejvm_inotify_watches[index];
    if (watch->used && watch->owner == owner &&
        strcmp(watch->path, resolved) == 0) {
      watch->mask = mask;
      return watch->fd;
    }
  }
  int slot = -1;
  for (int index = 0; index < TRACEJVM_INOTIFY_WATCH_LIMIT; index++) {
    if (!tracejvm_inotify_watches[index].used) {
      slot = index;
      break;
    }
  }
  if (slot < 0) {
    errno = ENOSPC;
    return -1;
  }
  char *stored_path = strdup(resolved);
  if (!stored_path) {
    errno = ENOMEM;
    return -1;
  }
  int fd = tracejvm_host_watch_js(resolved, TRACEJVM_REMOTE_FD_BASE);
  if (fd < 0) {
    free(stored_path);
    return fail_from_host();
  }
  if (tracejvm_host_epoll_ctl(instance->epoll_fd, 1, fd, 0x001) != 0) {
    int error = errno;
    tracejvm_host_close_js(remote_fd(fd));
    free(stored_path);
    errno = error;
    return -1;
  }
  tracejvm_inotify_watches[slot] = (tracejvm_inotify_watch){
      .used = true,
      .owner = owner,
      .fd = fd,
      .mask = mask,
      .path = stored_path,
  };
  return fd;
}

int tracejvm_host_inotify_rm_watch(int ifd, int wd) {
  tracejvm_inotify_instance *instance = inotify_instance(ifd);
  if (!instance) {
    errno = EBADF;
    return -1;
  }
  tracejvm_inotify_watch *watch =
      inotify_watch(inotify_instance_index(instance), wd);
  if (!watch) {
    errno = EINVAL;
    return -1;
  }
  int first_error = 0;
  if (tracejvm_host_epoll_ctl(instance->epoll_fd, 2, wd, 0) != 0)
    first_error = errno;
  if (tracejvm_host_close_js(remote_fd(wd)) < 0 && !first_error)
    first_error = take_host_errno();
  free(watch->path);
  memset(watch, 0, sizeof(*watch));
  if (first_error) {
    errno = first_error;
    return -1;
  }
  return 0;
}

int tracejvm_host_inotify_poll_begin(int ifd, int wake_fd) {
  tracejvm_inotify_instance *instance = inotify_instance(ifd);
  if (!instance || !tracejvm_host_routes_fd(wake_fd)) {
    errno = EBADF;
    return -1;
  }
  if (instance->pending_call_id > 0)
    return instance->pending_call_id;
  if (instance->wake_fd != wake_fd) {
    if (instance->wake_fd >= 0 &&
        tracejvm_host_epoll_ctl(
            instance->epoll_fd, 2, instance->wake_fd, 0) != 0 &&
        errno != ENOENT)
      return -1;
    if (tracejvm_host_epoll_ctl(
            instance->epoll_fd, 1, wake_fd, 0x001) != 0)
      return -1;
    instance->wake_fd = wake_fd;
  }
  int call_id = tracejvm_host_epoll_wait_begin(instance->epoll_fd, -1);
  if (call_id < 0)
    return -1;
  instance->pending_call_id = call_id;
  return call_id;
}

int tracejvm_host_inotify_poll_poll(int call_id) {
  tracejvm_inotify_instance *instance = nullptr;
  for (int index = 0; index < TRACEJVM_INOTIFY_INSTANCE_LIMIT; index++) {
    if (tracejvm_inotify_instances[index].used &&
        tracejvm_inotify_instances[index].pending_call_id == call_id) {
      instance = &tracejvm_inotify_instances[index];
      break;
    }
  }
  if (!instance) {
    errno = EINVAL;
    return -1;
  }
  uint8_t events[
      (TRACEJVM_INOTIFY_WATCH_LIMIT + 1) * 12];
  int ready = tracejvm_host_epoll_wait_poll(
      call_id, events, TRACEJVM_INOTIFY_WATCH_LIMIT + 1);
  if (ready == TRACEJVM_HOST_ASYNC_PENDING)
    return ready;
  instance->pending_call_id = 0;
  if (ready < 0)
    return -1;
  int result = 0;
  int owner = inotify_instance_index(instance);
  for (int index = 0; index < ready; index++) {
    int fd;
    memcpy(&fd, events + index * 12 + 4, sizeof(fd));
    if (fd == instance->wake_fd) {
      result |= 2;
      continue;
    }
    tracejvm_inotify_watch *watch = inotify_watch(owner, fd);
    if (!watch) {
      errno = EPROTO;
      return -1;
    }
    watch->ready = true;
    result |= 1;
  }
  return result;
}

static bool tracejvm_watch_frame(
    uint8_t const *frame,
    size_t length,
    int *type,
    char *path,
    size_t path_capacity) {
  static uint8_t const magic[4] = {0x54, 0x4b, 0x57, 0x31};
  if (length < TRACEJVM_WATCH_FRAME_HEADER_SIZE ||
      memcmp(frame, magic, sizeof(magic)) != 0)
    return false;
  uint32_t path_length;
  memcpy(&path_length, frame + 5, sizeof(path_length));
  if (path_length > TRACEJVM_WATCH_PATH_LIMIT ||
      length != TRACEJVM_WATCH_FRAME_HEADER_SIZE + path_length ||
      path_length + 1 > path_capacity ||
      (frame[4] < 1 || frame[4] > 5))
    return false;
  memcpy(path, frame + TRACEJVM_WATCH_FRAME_HEADER_SIZE, path_length);
  path[path_length] = '\0';
  *type = frame[4];
  return true;
}

static size_t tracejvm_inotify_record(
    uint8_t *target,
    size_t capacity,
    int wd,
    uint32_t mask,
    char const *name) {
  size_t name_length = name ? strlen(name) : 0;
  size_t padded_name_length =
      name_length == 0 ? 0 : (name_length + 1 + 3) & ~(size_t)3;
  size_t record_length = 16 + padded_name_length;
  if (record_length > capacity)
    return 0;
  uint32_t cookie = 0;
  uint32_t native_wd = (uint32_t)wd;
  uint32_t native_name_length = (uint32_t)padded_name_length;
  memcpy(target, &native_wd, 4);
  memcpy(target + 4, &mask, 4);
  memcpy(target + 8, &cookie, 4);
  memcpy(target + 12, &native_name_length, 4);
  if (padded_name_length > 0) {
    memset(target + 16, 0, padded_name_length);
    memcpy(target + 16, name, name_length);
  }
  return record_length;
}

static ssize_t tracejvm_host_inotify_read(
    tracejvm_inotify_instance *instance, void *buffer, size_t length) {
  enum {
    IN_MODIFY = 0x00000002,
    IN_CREATE = 0x00000100,
    IN_DELETE = 0x00000200,
    IN_Q_OVERFLOW = 0x00004000,
    IN_IGNORED = 0x00008000,
  };
  if (!buffer && length > 0) {
    errno = EINVAL;
    return -1;
  }
  uint8_t frame[
      TRACEJVM_WATCH_FRAME_HEADER_SIZE + TRACEJVM_WATCH_PATH_LIMIT];
  char event_path[TRACEJVM_WATCH_PATH_LIMIT + 1];
  uint8_t *output = buffer;
  size_t written = 0;
  int owner = inotify_instance_index(instance);
  for (int index = 0; index < TRACEJVM_INOTIFY_WATCH_LIMIT; index++) {
    tracejvm_inotify_watch *watch = &tracejvm_inotify_watches[index];
    if (!watch->used || watch->owner != owner || !watch->ready)
      continue;
    int count = tracejvm_host_read_js(
        remote_fd(watch->fd), frame, sizeof(frame), 0, false);
    watch->ready = false;
    if (count < 0)
      return written > 0 ? (ssize_t)written : fail_from_host();
    int type = 0;
    if (!tracejvm_watch_frame(
            frame, (size_t)count, &type, event_path, sizeof(event_path))) {
      errno = EPROTO;
      return written > 0 ? (ssize_t)written : -1;
    }
    int wd = watch->fd;
    uint32_t mask;
    char const *name = nullptr;
    if (type == 3) {
      wd = -1;
      mask = IN_Q_OVERFLOW;
    } else if (strcmp(event_path, watch->path) == 0) {
      if (type == 5) {
        mask = IN_IGNORED;
      } else if (type != 2) {
        continue;
      } else {
        /*
         * Code 2 is the original TKW1 ambiguous rename event. Retain its
         * stat-based interpretation for older hosts; codes 4 and 5 carry exact
         * create/delete semantics and never take this racy compatibility path.
         */
        struct stat status;
        if (tracejvm_host_stat(event_path, &status, true) == 0)
          continue;
        if (errno != ENOENT) {
          mask = IN_Q_OVERFLOW;
          wd = -1;
        } else {
          mask = IN_IGNORED;
        }
      }
    } else {
      char const *slash = strrchr(event_path, '/');
      name = slash ? slash + 1 : event_path;
      if (type == 1) {
        mask = IN_MODIFY;
      } else if (type == 4) {
        mask = IN_CREATE;
      } else if (type == 5) {
        mask = IN_DELETE;
      } else {
        /* Compatibility with the original ambiguous TKW1 rename code. */
        struct stat status;
        if (tracejvm_host_stat(event_path, &status, true) == 0) {
          mask = IN_CREATE;
        } else if (errno == ENOENT) {
          mask = IN_DELETE;
        } else {
          mask = IN_Q_OVERFLOW;
          wd = -1;
          name = nullptr;
        }
      }
      if (wd >= 0 && (watch->mask & (int)mask) == 0)
        continue;
    }
    size_t record_length = tracejvm_inotify_record(
        output + written, length - written, wd, mask, name);
    if (record_length == 0) {
      if (written > 0)
        break;
      record_length = tracejvm_inotify_record(
          output, length, -1, IN_Q_OVERFLOW, nullptr);
      if (record_length == 0) {
        errno = EINVAL;
        return -1;
      }
    }
    written += record_length;
  }
  if (written == 0) {
    errno = EAGAIN;
    return -1;
  }
  return (ssize_t)written;
}

EM_JS(int, tracejvm_host_spawn_js,
      (char const *program, char const *argument_block,
       size_t argument_block_length, int argument_count,
       char const *environment_block, size_t environment_block_length,
       int environment_count, char const *directory, int *descriptors,
       int redirect_error_stream, int remote_fd_base), {
  const parseBlock = (pointer, length, count) => {
    if (!pointer || count === 0) return [];
    const bytes = HEAPU8.subarray(pointer, pointer + length);
    const decoder = new TextDecoder();
    const values = [];
    let offset = 0;
    for (let index = 0; index < count; index += 1) {
      let end = offset;
      while (end < bytes.length && bytes[end] !== 0) end += 1;
      if (end >= bytes.length) {
        throw Object.assign(
          new Error("Invalid null-delimited process block."),
          {name: "EINVAL"},
        );
      }
      values.push(decoder.decode(bytes.subarray(offset, end)));
      offset = end + 1;
    }
    return values;
  };
  const runtimeForCommand = (command) => {
    const name = command.split("/").at(-1).toLowerCase();
    if (name === "node" || name === "nodejs") return "javascript";
    if (name === "python" || name === "python3") return "python";
    if (name === "java") return "java";
    if (name === "dotnet") return "csharp";
    return "cpp";
  };
  try {
    const command = UTF8ToString(program);
    const args = parseBlock(
      argument_block,
      argument_block_length,
      argument_count,
    );
    const environmentEntries = parseBlock(
      environment_block,
      environment_block_length,
      environment_count,
    );
    const env = Object.fromEntries(environmentEntries.map((entry) => {
      const separator = entry.indexOf("=");
      if (separator <= 0) {
        throw Object.assign(
          new Error("Invalid process environment entry."),
          {name: "EINVAL"},
        );
      }
      return [entry.slice(0, separator), entry.slice(separator + 1)];
    }));
    const stdio = {};
    const descriptorMappings = [];
    for (let index = 0; index < 3; index += 1) {
      const descriptor = HEAP32[(descriptors >> 2) + index];
      const name = index === 0 ? "stdin" : index === 1 ? "stdout" : "stderr";
      if (descriptor === -1) {
        stdio[name] = "pipe";
      } else if (descriptor === index) {
        stdio[name] = "inherit";
      } else if (descriptor >= remote_fd_base) {
        descriptorMappings.push({
          parentFd: descriptor - remote_fd_base,
          childFd: index,
        });
      } else {
        throw Object.assign(
          new Error(
            `Cannot inherit VM-local descriptor ${descriptor} into a host child.`,
          ),
          {name: "EBADF"},
        );
      }
    }
    const descriptorActions = [];
    if (redirect_error_stream) {
      stdio.stderr = "ignore";
      descriptorActions.push({op: "dup2", fd: 1, targetFd: 2});
    }
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "spawn",
      payload: {
        runtime: runtimeForCommand(command),
        command,
        args,
        ...(directory ? {cwd: UTF8ToString(directory)} : {}),
        ...(environmentEntries.length > 0 ? {env} : {}),
        ...(descriptorMappings.length > 0 ? {descriptorMappings} : {}),
        ...(descriptorActions.length > 0 ? {descriptorActions} : {}),
        stdio,
      },
    });
    if (!result || !Number.isSafeInteger(result.pid) || result.pid <= 0) {
      throw Object.assign(
        new Error("Invalid host spawn response."),
        {name: "EPROTO"},
      );
    }
    const returned = result.stdio || {};
    const parentDescriptors = [
      returned.stdinFd,
      returned.stdoutFd,
      returned.stderrFd,
    ];
    for (let index = 0; index < 3; index += 1) {
      const descriptor = parentDescriptors[index];
      HEAP32[(descriptors >> 2) + index] =
        Number.isSafeInteger(descriptor) && descriptor >= 0
          ? descriptor + remote_fd_base
          : -1;
    }
    return result.pid;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

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
    bool redirect_error_stream) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  int pid = tracejvm_host_spawn_js(
      program, argument_block, argument_block_length, argument_count,
      environment_block, environment_block_length, environment_count,
      directory, descriptors, redirect_error_stream, TRACEJVM_REMOTE_FD_BASE);
  return pid < 0 ? fail_from_host() : pid;
}

enum {
  TRACEJVM_ASYNC_WAIT = 1,
  TRACEJVM_ASYNC_READ = 2,
  TRACEJVM_ASYNC_ACCEPT = 3,
};

EM_JS(int, tracejvm_host_async_begin_js,
      (int operation, double argument, int length), {
  const dispatch = Module["traceJVMHostDispatchAsync"];
  if (typeof dispatch !== "function") {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error("The asynchronous system host is unavailable."),
      {name: "ENOSYS"},
    );
    return -1;
  }
  let pending = Module["traceJVMHostAsyncCalls"];
  if (!pending) {
    pending = new Map();
    Module["traceJVMHostAsyncCalls"] = pending;
  }
  let id = Module["traceJVMHostNextAsyncCallId"] ?? 0;
  do {
    id = id >= 0x7fffffff ? 1 : id + 1;
  } while (pending.has(id));
  Module["traceJVMHostNextAsyncCallId"] = id;
  const getContextId = Module["traceJVMHostContextId"];
  const contextId =
    typeof getContextId === "function" ? getContextId() : 0;
  const call = {settled: false, contextId};
  pending.set(id, call);
  const request = operation === 1
    ? {service: "posix", operation: "wait", payload: {pid: argument}}
    : operation === 2
      ? {
          service: "posix",
          operation: "read",
          payload: {fd: argument, maxBytes: length},
        }
      : operation === 3
        ? {service: "posix", operation: "accept", payload: {fd: argument}}
        : undefined;
  if (!request) {
    pending.delete(id);
    Module["traceJVMHostLastError"] = Object.assign(
      new Error(`Unknown asynchronous host operation ${operation}.`),
      {name: "EINVAL"},
    );
    return -1;
  }
  Promise.resolve()
    .then(() => dispatch(request, contextId))
    .then(
      (value) => {
        call.settled = true;
        call.value = value;
      },
      (error) => {
        call.settled = true;
        call.error = error;
      },
    );
  return id;
});

int tracejvm_host_wait_begin(int64_t pid) {
  int call_id = tracejvm_host_async_begin_js(
      TRACEJVM_ASYNC_WAIT, (double)pid, 0);
  return call_id < 0 ? fail_from_host() : call_id;
}

EM_JS(int, tracejvm_host_wait_poll_js, (int call_id, int *exit_code), {
  const pending = Module["traceJVMHostAsyncCalls"];
  const call = pending && pending.get(call_id);
  if (!call) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error(`Unknown asynchronous host call ${call_id}.`),
      {name: "EINVAL"},
    );
    return -1;
  }
  if (!call.settled) return 0;
  pending.delete(call_id);
  if (call.error !== undefined) {
    Module["traceJVMHostLastError"] = call.error;
    return -1;
  }
  const termination = call.value && call.value.termination;
  if (
    !termination ||
    !Number.isSafeInteger(termination.exitCode) ||
    termination.exitCode < -0x80000000 ||
    termination.exitCode > 0x7fffffff
  ) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error("Invalid host wait response."),
      {name: "EPROTO"},
    );
    return -1;
  }
  HEAP32[exit_code >> 2] = termination.exitCode;
  return 1;
});

int tracejvm_host_wait_poll(int call_id, int *exit_code) {
  int status = tracejvm_host_wait_poll_js(call_id, exit_code);
  return status < 0 ? fail_from_host() : status;
}

int tracejvm_host_read_begin(int fd, size_t length) {
  if (!tracejvm_host_routes_fd(fd) || length > INT_MAX) {
    errno = !tracejvm_host_routes_fd(fd) ? EBADF : EINVAL;
    return -1;
  }
  int call_id = tracejvm_host_async_begin_js(
      TRACEJVM_ASYNC_READ, (double)remote_fd(fd), (int)length);
  return call_id < 0 ? fail_from_host() : call_id;
}

EM_JS(int, tracejvm_host_read_poll_js,
      (int call_id, void *buffer, int capacity, int pending_value), {
  const pending = Module["traceJVMHostAsyncCalls"];
  const call = pending && pending.get(call_id);
  if (!call) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error(`Unknown asynchronous host call ${call_id}.`),
      {name: "EINVAL"},
    );
    return -1;
  }
  if (!call.settled) return pending_value;
  pending.delete(call_id);
  if (call.error !== undefined) {
    Module["traceJVMHostLastError"] = call.error;
    return -1;
  }
  const bytes = call.value && call.value.bytes;
  if (!(bytes instanceof Uint8Array) || bytes.byteLength > capacity) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error("Invalid host read response."),
      {name: "EPROTO"},
    );
    return -1;
  }
  HEAPU8.set(bytes, buffer);
  return bytes.byteLength;
});

ssize_t tracejvm_host_read_poll(
    int call_id, void *buffer, size_t capacity) {
  if (capacity > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_read_poll_js(
      call_id, buffer, (int)capacity, TRACEJVM_HOST_ASYNC_PENDING);
  return result == -1 ? fail_from_host() : result;
}

int tracejvm_host_accept_begin(int fd) {
  if (!tracejvm_host_is_remote_fd(fd)) {
    errno = EBADF;
    return -1;
  }
  int call_id = tracejvm_host_async_begin_js(
      TRACEJVM_ASYNC_ACCEPT, (double)remote_fd(fd), 0);
  return call_id < 0 ? fail_from_host() : call_id;
}

EM_JS(int, tracejvm_host_accept_poll_js,
      (int call_id, char *remote_host, int remote_host_capacity,
       int *remote_port, int remote_fd_base), {
  const pending = Module["traceJVMHostAsyncCalls"];
  const call = pending && pending.get(call_id);
  if (!call) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error(`Unknown asynchronous host call ${call_id}.`),
      {name: "EINVAL"},
    );
    return -1;
  }
  if (!call.settled) return 0;
  pending.delete(call_id);
  if (call.error !== undefined) {
    Module["traceJVMHostLastError"] = call.error;
    return -1;
  }
  const value = call.value;
  const address = value && value.remoteAddress;
  if (
    !value ||
    !Number.isSafeInteger(value.fd) ||
    value.fd < 0 ||
    value.fd >= remote_fd_base ||
    !address ||
    typeof address.host !== "string" ||
    !Number.isSafeInteger(address.port) ||
    address.port < 0 ||
    address.port > 65535 ||
    lengthBytesUTF8(address.host) + 1 > remote_host_capacity
  ) {
    Module["traceJVMHostLastError"] = Object.assign(
      new Error("Invalid host accept response."),
      {name: "EPROTO"},
    );
    return -1;
  }
  stringToUTF8(address.host, remote_host, remote_host_capacity);
  HEAP32[remote_port >> 2] = address.port;
  return value.fd + remote_fd_base;
});

int tracejvm_host_accept_poll(
    int call_id,
    char *remote_host,
    size_t remote_host_capacity,
    int *remote_port) {
  if (remote_host_capacity > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_accept_poll_js(
      call_id, remote_host, (int)remote_host_capacity, remote_port,
      TRACEJVM_REMOTE_FD_BASE);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(double, tracejvm_host_identity_js, (double requested_pid, int parent), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "identity",
      payload: requested_pid > 0 ? {pid: requested_pid} : {},
    });
    const value = parent ? result && result.ppid : result && result.pid;
    if (!Number.isSafeInteger(value) || value <= 0) {
      throw Object.assign(
        new Error("Invalid host process identity response."),
        {name: "EPROTO"},
      );
    }
    return value;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int64_t tracejvm_host_current_pid(void) {
  if (!tracejvm_host_available_js())
    return 1;
  double pid = tracejvm_host_identity_js(0, false);
  if (pid < 0) {
    fail_from_host();
    return -1;
  }
  return (int64_t)pid;
}

int64_t tracejvm_host_parent_pid(int64_t pid) {
  if (!tracejvm_host_available_js())
    return pid == 1 ? 0 : -1;
  double parent = tracejvm_host_identity_js((double)pid, true);
  if (parent < 0) {
    fail_from_host();
    return -1;
  }
  return (int64_t)parent;
}

EM_JS(int, tracejvm_host_identity_snapshot_js, (int64_t *identity), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "identity",
      payload: {},
    });
    const values = [
      result && result.pid,
      result && result.ppid,
      result && result.pgid,
      result && result.sid,
    ];
    if (!values.every((value) => Number.isSafeInteger(value) && value >= 0)) {
      throw Object.assign(
        new Error("Invalid host process identity response."),
        {name: "EPROTO"},
      );
    }
    for (let index = 0; index < values.length; index += 1)
      setValue(identity + index * 8, BigInt(values[index]), "i64");
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_identity_snapshot(int64_t identity[4]) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (!identity) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_identity_snapshot_js(identity);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_watchdog_js,
      (int action, double timeout_ms, int signal, int64_t *status), {
  try {
    const actions = ["arm", "pet", "disarm", "status"];
    if (action < 1 || action > actions.length)
      throw Object.assign(new Error("Invalid watchdog action."), {name: "EINVAL"});
    const payload = {action: actions[action - 1]};
    if (action === 1) {
      if (!Number.isSafeInteger(timeout_ms) || timeout_ms <= 0)
        throw Object.assign(new Error("Invalid watchdog timeout."), {name: "EINVAL"});
      payload.timeoutMs = timeout_ms;
      payload.signal = signal === 0
        ? "SIGTERM"
        : signal === 1
          ? "SIGKILL"
          : (() => {
              throw Object.assign(
                new Error("Invalid watchdog signal."),
                {name: "EINVAL"},
              );
            })();
    }
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "watchdog",
      payload,
    });
    if (!result || typeof result.armed !== "boolean")
      throw Object.assign(
        new Error("Invalid host watchdog response."),
        {name: "EPROTO"},
      );
    const values = result.armed
      ? [
          1,
          result.timeoutMs,
          result.deadlineAt,
          result.signal === "SIGTERM"
            ? 0
            : result.signal === "SIGKILL"
              ? 1
              : -1,
        ]
      : [0, 0, 0, -1];
    if (!values.every((value) => Number.isSafeInteger(value)) ||
        (result.armed &&
          (values[1] <= 0 || values[2] <= 0 || values[3] < 0))) {
      throw Object.assign(
        new Error("Invalid host watchdog status."),
        {name: "EPROTO"},
      );
    }
    for (let index = 0; index < values.length; index += 1)
      setValue(status + index * 8, BigInt(values[index]), "i64");
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_watchdog(
    int action,
    int64_t timeout_ms,
    int signal,
    int64_t status[4]) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (!status || timeout_ms < 0 || timeout_ms > 9007199254740991LL) {
    errno = EINVAL;
    return -1;
  }
  int result =
      tracejvm_host_watchdog_js(action, (double)timeout_ms, signal, status);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_setsid_js, (int64_t *identity), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "setsid",
      payload: {},
    });
    if (
      !result ||
      !Number.isSafeInteger(result.sid) ||
      result.sid <= 0 ||
      !Number.isSafeInteger(result.pgid) ||
      result.pgid <= 0
    ) {
      throw Object.assign(
        new Error("Invalid host setsid response."),
        {name: "EPROTO"},
      );
    }
    setValue(identity, BigInt(result.sid), "i64");
    setValue(identity + 8, BigInt(result.pgid), "i64");
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_setsid(int64_t identity[2]) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (!identity) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_setsid_js(identity);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(double, tracejvm_host_process_group_js,
      (int operation, double pid, double pgid, int fd), {
  try {
    const names = ["setpgid", "tcgetpgrp", "tcsetpgrp"];
    if (operation < 1 || operation > names.length)
      throw Object.assign(
        new Error("Invalid process-group operation."),
        {name: "EINVAL"},
      );
    const payload = operation === 1
      ? {pid, pgid}
      : operation === 2
        ? {fd}
        : {fd, pgid};
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: names[operation - 1],
      payload,
    });
    const value = result && result.pgid;
    if (!Number.isSafeInteger(value) || value <= 0)
      throw Object.assign(
        new Error("Invalid host process-group response."),
        {name: "EPROTO"},
      );
    return value;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int64_t tracejvm_host_setpgid(int64_t pid, int64_t pgid) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (
      pid < 0 || pgid < 0 ||
      pid > 9007199254740991LL || pgid > 9007199254740991LL) {
    errno = EINVAL;
    return -1;
  }
  double result =
      tracejvm_host_process_group_js(1, (double)pid, (double)pgid, -1);
  if (result < 0) {
    fail_from_host();
    return -1;
  }
  return (int64_t)result;
}

int64_t tracejvm_host_tcgetpgrp(int fd) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (fd < 0) {
    errno = EBADF;
    return -1;
  }
  double result = tracejvm_host_process_group_js(2, 0, 0, fd);
  if (result < 0) {
    fail_from_host();
    return -1;
  }
  return (int64_t)result;
}

int64_t tracejvm_host_tcsetpgrp(int fd, int64_t pgid) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (fd < 0 || pgid <= 0 || pgid > 9007199254740991LL) {
    errno = fd < 0 ? EBADF : EINVAL;
    return -1;
  }
  double result =
      tracejvm_host_process_group_js(3, 0, (double)pgid, fd);
  if (result < 0) {
    fail_from_host();
    return -1;
  }
  return (int64_t)result;
}

EM_JS(int, tracejvm_host_terminal_window_size_js,
      (int operation, int fd, double rows, double columns, int64_t *size), {
  try {
    const name = operation === 1
      ? "tcgetwinsize"
      : operation === 2
        ? "tcsetwinsize"
        : (() => {
            throw Object.assign(
              new Error("Invalid terminal window-size operation."),
              {name: "EINVAL"},
            );
          })();
    const payload = operation === 1 ? {fd} : {fd, rows, columns};
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: name,
      payload,
    });
    if (
      !result ||
      !Number.isSafeInteger(result.rows) ||
      result.rows <= 0 ||
      !Number.isSafeInteger(result.columns) ||
      result.columns <= 0
    ) {
      throw Object.assign(
        new Error("Invalid host terminal window-size response."),
        {name: "EPROTO"},
      );
    }
    setValue(size, BigInt(result.rows), "i64");
    setValue(size + 8, BigInt(result.columns), "i64");
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_tcgetwinsize(int fd, int64_t size[2]) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (fd < 0 || !size) {
    errno = fd < 0 ? EBADF : EINVAL;
    return -1;
  }
  int result =
      tracejvm_host_terminal_window_size_js(1, fd, 0, 0, size);
  return result < 0 ? fail_from_host() : result;
}

int tracejvm_host_tcsetwinsize(
    int fd, int64_t rows, int64_t columns, int64_t size[2]) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (
      fd < 0 || !size || rows <= 0 || columns <= 0 ||
      rows > 9007199254740991LL || columns > 9007199254740991LL) {
    errno = fd < 0 ? EBADF : EINVAL;
    return -1;
  }
  int result = tracejvm_host_terminal_window_size_js(
      2, fd, (double)rows, (double)columns, size);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_poll_signal_js, (), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "signal",
      operation: "poll",
    });
    if (!Number.isSafeInteger(result) || result < 0) {
      throw Object.assign(
        new Error("Invalid host signal-poll response."),
        {name: "EPROTO"},
      );
    }
    return result;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_poll_signal(void) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  int result = tracejvm_host_poll_signal_js();
  return result < 0 ? fail_from_host() : result;
}

EM_JS(double, tracejvm_host_process_start_time_js, (double pid), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "processInfo",
      payload: {pid},
    });
    const process = result && result.process;
    if (
      !process ||
      !Number.isSafeInteger(process.pid) ||
      process.pid !== pid ||
      typeof process.phase !== "string"
    ) {
      throw Object.assign(
        new Error("Invalid host process info response."),
        {name: "EPROTO"},
      );
    }
    if (process.phase === "exited") return -1;
    return Number.isSafeInteger(process.startedAt) && process.startedAt >= 0
      ? process.startedAt
      : 0;
  } catch (error) {
    if (error && (error.code === "ESRCH" || error.name === "ESRCH"))
      return -1;
    Module["traceJVMHostLastError"] = error;
    return -2;
  }
});

int64_t tracejvm_host_process_start_time(int64_t pid) {
  if (!tracejvm_host_available_js())
    return pid == 1 ? 0 : -1;
  double result = tracejvm_host_process_start_time_js((double)pid);
  if (result == -2) {
    fail_from_host();
    return -1;
  }
  return (int64_t)result;
}

EM_JS(int, tracejvm_host_process_list_js,
      (double parent_pid, int64_t *pids, int64_t *parent_pids,
       int64_t *start_times, int capacity), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "processList",
    });
    if (!result || !Array.isArray(result.processes)) {
      throw Object.assign(
        new Error("Invalid host process list response."),
        {name: "EPROTO"},
      );
    }
    const processes = result.processes.filter((process) =>
      process &&
      process.phase !== "exited" &&
      (parent_pid <= 0 || process.ppid === parent_pid)
    );
    const written = Math.min(capacity, processes.length);
    for (let index = 0; index < written; index += 1) {
      const process = processes[index];
      if (
        !Number.isSafeInteger(process.pid) ||
        !Number.isSafeInteger(process.ppid)
      ) {
        throw Object.assign(
          new Error("Invalid process entry in host process list."),
          {name: "EPROTO"},
        );
      }
      if (pids)
        setValue(pids + index * 8, BigInt(process.pid), "i64");
      if (parent_pids)
        setValue(parent_pids + index * 8, BigInt(process.ppid), "i64");
      if (start_times) {
        const startedAt =
          Number.isSafeInteger(process.startedAt) && process.startedAt >= 0
            ? process.startedAt
            : 0;
        setValue(start_times + index * 8, BigInt(startedAt), "i64");
      }
    }
    return processes.length;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_process_list(
    int64_t parent_pid,
    int64_t *pids,
    int64_t *parent_pids,
    int64_t *start_times,
    int capacity) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (capacity < 0) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_process_list_js(
      (double)parent_pid, pids, parent_pids, start_times, capacity);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_process_info_js,
      (double pid, char *command, int command_capacity,
       char *argument_block, int argument_block_capacity,
       size_t *command_length, size_t *argument_block_length,
       int *argument_count, int64_t *start_time), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "processInfo",
      payload: {pid},
    });
    const process = result && result.process;
    if (
      !process ||
      !Number.isSafeInteger(process.pid) ||
      process.pid !== pid ||
      typeof process.command !== "string" ||
      !Array.isArray(process.args) ||
      !process.args.every((argument) => typeof argument === "string")
    ) {
      throw Object.assign(
        new Error("Invalid host process metadata response."),
        {name: "EPROTO"},
      );
    }

    const modifiedUtf8Length = (value) => {
      let length = 0;
      for (let index = 0; index < value.length; index += 1) {
        const codeUnit = value.charCodeAt(index);
        length += codeUnit === 0
          ? 2
          : codeUnit <= 0x7f
            ? 1
            : codeUnit <= 0x7ff
              ? 2
              : 3;
      }
      return length;
    };
    const writeModifiedUtf8 = (value, destination) => {
      let offset = destination;
      for (let index = 0; index < value.length; index += 1) {
        const codeUnit = value.charCodeAt(index);
        if (codeUnit !== 0 && codeUnit <= 0x7f) {
          HEAPU8[offset++] = codeUnit;
        } else if (codeUnit <= 0x7ff) {
          HEAPU8[offset++] = 0xc0 | (codeUnit >> 6);
          HEAPU8[offset++] = 0x80 | (codeUnit & 0x3f);
        } else {
          HEAPU8[offset++] = 0xe0 | (codeUnit >> 12);
          HEAPU8[offset++] = 0x80 | ((codeUnit >> 6) & 0x3f);
          HEAPU8[offset++] = 0x80 | (codeUnit & 0x3f);
        }
      }
      return offset;
    };

    const requiredCommandLength = modifiedUtf8Length(process.command);
    const requiredArgumentBlockLength = process.args.reduce(
      (length, argument) => length + modifiedUtf8Length(argument) + 1,
      0,
    );
    setValue(command_length, requiredCommandLength, "i32");
    setValue(argument_block_length, requiredArgumentBlockLength, "i32");
    setValue(argument_count, process.args.length, "i32");
    const startedAt =
      Number.isSafeInteger(process.startedAt) && process.startedAt >= 0
        ? process.startedAt
        : 0;
    setValue(start_time, BigInt(startedAt), "i64");

    if (
      command &&
      command_capacity > requiredCommandLength &&
      argument_block_capacity >= requiredArgumentBlockLength
    ) {
      let commandEnd = writeModifiedUtf8(process.command, command);
      HEAPU8[commandEnd] = 0;
      let argumentOffset = argument_block;
      for (const argument of process.args) {
        argumentOffset = writeModifiedUtf8(argument, argumentOffset);
        HEAPU8[argumentOffset++] = 0;
      }
      return 0;
    }
    return 1;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_process_info(
    int64_t pid,
    char *command,
    size_t command_capacity,
    char *argument_block,
    size_t argument_block_capacity,
    size_t *command_length,
    size_t *argument_block_length,
    int *argument_count,
    int64_t *start_time) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (
    command_capacity > INT_MAX ||
    argument_block_capacity > INT_MAX ||
    !command_length ||
    !argument_block_length ||
    !argument_count ||
    !start_time
  ) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_process_info_js(
      (double)pid,
      command,
      (int)command_capacity,
      argument_block,
      (int)argument_block_capacity,
      command_length,
      argument_block_length,
      argument_count,
      start_time);
  return result < 0 ? fail_from_host() : result;
}

EM_JS(int, tracejvm_host_environment_js,
      (char *entry_block, int entry_block_capacity,
       size_t *entry_block_length, int *entry_count), {
  try {
    const result = Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "environment",
    });
    const env = result && result.env;
    if (!env || typeof env !== "object" || Array.isArray(env)) {
      throw Object.assign(
        new Error("Invalid host process environment response."),
        {name: "EPROTO"},
      );
    }
    const entries = Object.entries(env).map(([name, value]) => {
      if (
        name.length === 0 ||
        name.includes("=") ||
        name.includes("\0") ||
        typeof value !== "string" ||
        value.includes("\0")
      ) {
        throw Object.assign(
          new Error("Invalid entry in host process environment."),
          {name: "EPROTO"},
        );
      }
      return [name, value];
    });
    const requiredLength = entries.reduce(
      (length, [name, value]) =>
        length + lengthBytesUTF8(name) + lengthBytesUTF8(value) + 2,
      0,
    );
    setValue(entry_block_length, requiredLength, "i32");
    setValue(entry_count, entries.length, "i32");
    if (requiredLength === 0) return 0;
    if (!entry_block || entry_block_capacity < requiredLength) return 1;

    let offset = entry_block;
    for (const [name, value] of entries) {
      const nameLength = lengthBytesUTF8(name);
      stringToUTF8(name, offset, nameLength + 1);
      offset += nameLength + 1;
      const valueLength = lengthBytesUTF8(value);
      stringToUTF8(value, offset, valueLength + 1);
      offset += valueLength + 1;
    }
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_environment(
    char *entry_block,
    size_t entry_block_capacity,
    size_t *entry_block_length,
    int *entry_count) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  if (
    entry_block_capacity > INT_MAX ||
    !entry_block_length ||
    !entry_count
  ) {
    errno = EINVAL;
    return -1;
  }
  int result = tracejvm_host_environment_js(
      entry_block,
      (int)entry_block_capacity,
      entry_block_length,
      entry_count);
  return result < 0 ? fail_from_host() : result;
}

int tracejvm_host_process_exists(int64_t pid) {
  return tracejvm_host_process_start_time(pid) >= 0 ? 1 : 0;
}

EM_JS(int, tracejvm_host_kill_js, (double pid, int force), {
  try {
    Module["traceJVMHostDispatchSync"]({
      service: "posix",
      operation: "kill",
      payload: {pid, signal: force ? "SIGKILL" : "SIGTERM"},
    });
    return 0;
  } catch (error) {
    Module["traceJVMHostLastError"] = error;
    return -1;
  }
});

int tracejvm_host_kill(int64_t pid, bool force) {
  if (!tracejvm_host_available_js()) {
    errno = ENOSYS;
    return -1;
  }
  int result = tracejvm_host_kill_js((double)pid, force);
  return result < 0 ? fail_from_host() : 0;
}

EM_JS(void, tracejvm_host_cancel_context_calls_js, (uint32_t context), {
  const pending = Module["traceJVMHostAsyncCalls"];
  if (!pending) return;
  for (const [id, call] of pending) {
    if (call && call.contextId === context) pending.delete(id);
  }
});

void tracejvm_host_dispose_active_context(void) {
  uint32_t context = tracejvm_host_context_js();
  if (context == 0)
    return;
  for (int index = 0; index < TRACEJVM_INOTIFY_INSTANCE_LIMIT; index++) {
    tracejvm_inotify_instance *instance = &tracejvm_inotify_instances[index];
    if (instance->used && instance->context == context)
      (void)tracejvm_host_close(TRACEJVM_INOTIFY_FD_BASE + index);
  }
  for (int index = 0; index < TRACEJVM_EVENTFD_LIMIT; index++) {
    tracejvm_eventfd *instance = &tracejvm_eventfds[index];
    if (instance->used && instance->context == context)
      (void)tracejvm_host_close(instance->read_fd);
  }
  for (int index = 0; index < TRACEJVM_EPOLL_INSTANCE_LIMIT; index++) {
    tracejvm_epoll_instance *instance = &tracejvm_epoll_instances[index];
    if (instance->used && instance->context == context)
      memset(instance, 0, sizeof(*instance));
  }
  tracejvm_host_cancel_context_calls_js(context);
}

#endif
