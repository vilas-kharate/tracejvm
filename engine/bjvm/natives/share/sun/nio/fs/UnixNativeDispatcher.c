
#include <natives-dsl.h>
#include <tracejvm-host.h>
#ifdef EMSCRIPTEN
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#else
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

/*
 * The pinned OpenJDK image contains Linux UnixConstants, while Emscripten's
 * libc exposes WASI errno numbers. Never leak the host ABI across this native
 * boundary: Java branches on errno identity to select its public exception
 * types (for example ENOENT -> NoSuchFileException).
 */
static int openjdk_linux_errno(int host_errno) {
#ifdef EMSCRIPTEN
  switch (host_errno) {
    case EACCES: return 13;
    case EAGAIN: return 11;
    case EEXIST: return 17;
    case EINVAL: return 22;
    case EISDIR: return 21;
    case ELOOP: return 40;
    case EMFILE: return 24;
    case ENAMETOOLONG: return 36;
    case ENOENT: return 2;
    case ENOSPC: return 28;
    case ENOSYS: return 38;
    case ENOTDIR: return 20;
    case ENOTEMPTY: return 39;
    case ENXIO: return 6;
    case ERANGE: return 34;
    case EROFS: return 30;
    case EXDEV: return 18;
    default: return host_errno;
  }
#else
  return host_errno;
#endif
}

static int host_errno_from_openjdk_linux(int openjdk_errno) {
#ifdef EMSCRIPTEN
  switch (openjdk_errno) {
    case 13: return EACCES;
    case 11: return EAGAIN;
    case 17: return EEXIST;
    case 22: return EINVAL;
    case 21: return EISDIR;
    case 40: return ELOOP;
    case 24: return EMFILE;
    case 36: return ENAMETOOLONG;
    case 2: return ENOENT;
    case 28: return ENOSPC;
    case 38: return ENOSYS;
    case 20: return ENOTDIR;
    case 39: return ENOTEMPTY;
    case 6: return ENXIO;
    case 34: return ERANGE;
    case 30: return EROFS;
    case 18: return EXDEV;
    default: return openjdk_errno;
  }
#else
  return openjdk_errno;
#endif
}

static obj_header *create_unix_exception(vm_thread *thread, int errno_code) {
  classdesc *classdesc = cached_classes(thread->vm)->unix_exception;
  obj_header *obj = new_object(thread, classdesc);

  cp_method *method = method_lookup(classdesc, STR("<init>"), STR("(I)V"), true, false);
  call_interpreter_synchronous(thread, method,
                               (stack_value[]){{.obj = obj}, {.i = openjdk_linux_errno(errno_code)}}); // constructor is void method

  return obj;
}

static obj_header *create_unix_exception_message(vm_thread *thread, char const *message) {
  classdesc *classdesc = cached_classes(thread->vm)->unix_exception;
  obj_header *obj = new_object(thread, classdesc);
  obj_header *java_message = MakeJStringFromCString(thread, message, false);

  cp_method *method =
      method_lookup(classdesc, STR("<init>"), STR("(Ljava/lang/String;)V"), true, false);
  call_interpreter_synchronous(
      thread, method, (stack_value[]){{.obj = obj}, {.obj = java_message}});

  return obj;
}

static obj_header *create_unix_path_exception(vm_thread *thread, char const *operation,
                                              char const *path, int errno_code) {
  char message[PATH_MAX + 128];
  snprintf(message, sizeof(message), "%s(%s): %s", operation, path, strerror(errno_code));
  return create_unix_exception_message(thread, message);
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, init, "()I") { return value_null(); }

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getcwd, "()[B") {
  INIT_STACK_STRING(cwd, 1024);
  char *p = tracejvm_host_getcwd(cwd.chars, 1024);
  if (p == nullptr) {
    return value_null();
  }
  cwd.len = strlen(cwd.chars);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, cwd.len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), cwd.chars, cwd.len);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, realpath0, "(J)[B") {
  if (!args[0].l) {
    thread->current_exception = create_unix_exception(thread, EINVAL);
    return value_null();
  }

  char resolved[PATH_MAX];
  if (!tracejvm_host_realpath(
          (char const *)args[0].l, resolved, sizeof(resolved))) {
    thread->current_exception = create_unix_path_exception(
        thread, "realpath", (char const *)args[0].l, errno);
    return value_null();
  }

  size_t len = strlen(resolved);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), resolved, len);
  return (stack_value){.obj = array};
}

stack_value stat_impl(value *args, bool follow_links) {
  struct stat st;

  if (!args[1].handle)
    return value_null();

  uintptr_t buf = args[0].l;
  int result = tracejvm_host_stat((char *)buf, &st, follow_links);
  if (result)
    return (stack_value){.i = openjdk_linux_errno(errno)};

  obj_header *attrs = args[1].handle->obj;

#define MapAttrLong(name, value) StoreFieldLong(attrs, (#name), value)
#define MapAttrInt(name, value) StoreFieldInt(attrs, (#name), value)
  MapAttrInt(st_mode, st.st_mode);
  MapAttrLong(st_ino, st.st_ino);
  MapAttrLong(st_dev, st.st_dev);
  MapAttrLong(st_rdev, st.st_rdev);
  MapAttrInt(st_nlink, st.st_nlink);
  MapAttrInt(st_uid, st.st_uid);
  MapAttrInt(st_gid, st.st_gid);
  MapAttrLong(st_size, st.st_size);

#ifdef __APPLE__
  MapAttrLong(st_atime_sec, st.st_atime);
  MapAttrLong(st_atime_nsec, st.st_atimensec);

  MapAttrLong(st_mtime_sec, st.st_mtime);
  MapAttrLong(st_mtime_nsec, st.st_mtimensec);

  MapAttrLong(st_ctime_sec, st.st_ctime);
  MapAttrLong(st_ctime_nsec, st.st_ctimensec);
#else
  MapAttrLong(st_atime_sec, st.st_atim.tv_sec);
  MapAttrLong(st_atime_nsec, st.st_atim.tv_nsec);

  MapAttrLong(st_mtime_sec, st.st_mtim.tv_sec);
  MapAttrLong(st_mtime_nsec, st.st_mtim.tv_nsec);

  MapAttrLong(st_ctime_sec, st.st_ctim.tv_sec);
  MapAttrLong(st_ctime_nsec, st.st_ctim.tv_nsec);
#endif

#ifdef __APPLE__
  MapAttrLong(st_birthtime_sec, st.st_birthtime);
  MapAttrLong(st_birthtime_nsec, st.st_birthtimensec);
#endif

#undef MapAttrLong
#undef MapAttrInt

  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, stat0, "(JLsun/nio/fs/UnixFileAttributes;)I") {
  return stat_impl(args, true);
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, lstat0, "(JLsun/nio/fs/UnixFileAttributes;)V") {
  stack_value result = stat_impl(args, false);
  if (result.i != 0) {
    thread->current_exception = create_unix_exception(thread, errno);
  }
  return value_null();
}

// DIR *opendir(const char* dirname)
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, opendir0, "(J)J") {
  tracejvm_host_dir *dir =
      tracejvm_host_opendir((char const *)args[0].l);
  if (!dir) {
    thread->current_exception =
        create_unix_path_exception(thread, "opendir", (char const *)args[0].l, errno);
    return value_null();
  }
  return (stack_value){.l = (s64)dir};
}

// struct dirent* readdir(DIR *dirp), return  dirent->d_name
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, readdir0, "(J)[B") {
  char name[PATH_MAX];
  int result = tracejvm_host_readdir(
      (tracejvm_host_dir *)args[0].l, name, sizeof(name));
  if (result < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  if (result == 0) {
    return value_null();
  }
  size_t len = strlen(name);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), name, len);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, closedir, "(J)V") {
  tracejvm_host_closedir((tracejvm_host_dir *)args[0].l);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, open0, "(JII)I") {
  int result = tracejvm_host_open(
      (char const *)args[0].l, args[1].i, args[2].i);
  if (result >= 0)
    return (stack_value){.i = result};

  thread->current_exception =
      create_unix_path_exception(thread, "open", (char const *)args[0].l, errno);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, access0, "(JI)I") {
  char const *path = (char const *)args[0].l;
  int result;
  if (tracejvm_host_routes_path(path)) {
    struct stat st;
    result = tracejvm_host_stat(path, &st, true);
  } else {
    result = access(path, args[1].i);
  }
  if (result == 0)
    return (stack_value){.i = 0};

  return (stack_value){.i = openjdk_linux_errno(errno)};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, mkdir0, "(JI)V") {
  char const *path = (char const *)args[0].l;
  if (tracejvm_host_mkdir(path, (mode_t)args[1].i) == 0)
    return value_null();

  // OpenJDK's Java layer owns the path-specific IOException translation.
  // Preserve errno here so ENOENT becomes NoSuchFileException and
  // Files.createDirectories can walk upward to the first existing parent.
  thread->current_exception = create_unix_exception(thread, errno);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, unlink0, "(J)V") {
  char const *path = (char const *)args[0].l;
  if (!path || tracejvm_host_unlink(path) != 0) {
    thread->current_exception = create_unix_path_exception(
        thread, "unlink", path ? path : "", path ? errno : EINVAL);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, rmdir0, "(J)V") {
  char const *path = (char const *)args[0].l;
  if (!path || tracejvm_host_rmdir(path) != 0) {
    thread->current_exception = create_unix_path_exception(
        thread, "rmdir", path ? path : "", path ? errno : EINVAL);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, rename0, "(JJ)V") {
  char const *source = (char const *)args[0].l;
  char const *destination = (char const *)args[1].l;
  if (!source || !destination ||
      tracejvm_host_rename(source, destination) != 0) {
    thread->current_exception = create_unix_path_exception(
        thread, "rename", source ? source : "",
        source && destination ? errno : EINVAL);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, link0, "(JJ)V") {
  char const *existing_path = (char const *)args[0].l;
  char const *new_path = (char const *)args[1].l;
  if (!existing_path || !new_path ||
      tracejvm_host_link(existing_path, new_path) != 0) {
    thread->current_exception = create_unix_path_exception(
        thread, "link", existing_path ? existing_path : "",
        existing_path && new_path ? errno : EINVAL);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, symlink0, "(JJ)V") {
  char const *target = (char const *)args[0].l;
  char const *link_path = (char const *)args[1].l;
  if (!target || !link_path ||
      tracejvm_host_symlink(target, link_path) != 0) {
    thread->current_exception = create_unix_path_exception(
        thread, "symlink", link_path ? link_path : "",
        target && link_path ? errno : EINVAL);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, readlink0, "(J)[B") {
  char const *path = (char const *)args[0].l;
  char target[PATH_MAX];
  ssize_t length = path
      ? tracejvm_host_readlink(path, target, sizeof(target))
      : -1;
  if (length < 0) {
    thread->current_exception = create_unix_path_exception(
        thread, "readlink", path ? path : "", path ? errno : EINVAL);
    return value_null();
  }
  obj_header *array =
      CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, (s32)length);
  if (!array)
    return value_null();
  memcpy(ArrayData(array), target, (size_t)length);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, close0, "(I)V") {
  if (tracejvm_host_close(args[0].i) != 0) {
    thread->current_exception = create_unix_exception(thread, errno);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, read, "(IJI)I") {
  ssize_t result = tracejvm_host_read(
      args[0].i,
      (void *)(uintptr_t)args[1].l,
      (size_t)args[2].i,
      0,
      false);
  if (result < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = (s32)result};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, write, "(IJI)I") {
  ssize_t result = tracejvm_host_write(
      args[0].i,
      (void const *)(uintptr_t)args[1].l,
      (size_t)args[2].i,
      0,
      false);
  if (result < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = (s32)result};
}

/*
 * Temurin's extracted runtime image uses the private read0/write0 symbols,
 * while the b-jvm compatibility jar exposes the un-suffixed form. Keep both
 * native spellings at this ABI boundary; they intentionally share semantics.
 */
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, read0, "(IJI)I") {
  ssize_t result = tracejvm_host_read(
      args[0].i,
      (void *)(uintptr_t)args[1].l,
      (size_t)args[2].i,
      0,
      false);
  if (result < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = (s32)result};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, write0, "(IJI)I") {
  ssize_t result = tracejvm_host_write(
      args[0].i,
      (void const *)(uintptr_t)args[1].l,
      (size_t)args[2].i,
      0,
      false);
  if (result < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = (s32)result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, size0, "(Ljava/io/FileDescriptor;)J") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  struct stat st;
  int result = tracejvm_host_fstat(fd, &st);
  if (result) {
    int error = errno;
    char message[128];
    snprintf(message, sizeof(message), "size(fd=%d): %s", fd,
             strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  return (stack_value){.l = st.st_size};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, seek0, "(Ljava/io/FileDescriptor;J)J") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  off_t requested = (off_t)args[1].l;
  off_t result = requested < 0
      ? tracejvm_host_seek(fd, 0, SEEK_CUR)
      : tracejvm_host_seek(fd, requested, SEEK_SET);
  if (result == (off_t)-1) {
    int error = errno;
    char message[160];
    snprintf(message, sizeof(message), "seek(fd=%d, offset=%lld): %s", fd,
             (long long)args[1].l, strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  return (stack_value){.l = (s64)result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, read0, "(Ljava/io/FileDescriptor;JI)I") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  ssize_t result = tracejvm_host_read(
      fd, (void *)args[1].l, (size_t)args[2].i, 0, false);
  if (result < 0) {
    int error = errno;
    char message[160];
    snprintf(message, sizeof(message), "read(fd=%d, length=%d): %s", fd,
             args[2].i, strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  // End of file is IOStatus.EOF (-1), as the JDK's convertReturnVal(n, reading) gives; a plain 0 made readers fail
  // with "Underlying input stream returned zero bytes" (Shiklo fork, 2026-10-28).
  return (stack_value){.i = result == 0 ? -1 : (s32)result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, pread0,
               "(Ljava/io/FileDescriptor;JIJ)I") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  size_t length = (size_t)args[2].i;
  off_t position = (off_t)args[3].l;
  ssize_t result = tracejvm_host_read(
      fd, (void *)args[1].l, length, position, true);
  if (result < 0) {
    int error = errno;
    char message[192];
    snprintf(message, sizeof(message),
             "pread(fd=%d, length=%zu, position=%lld): %s", fd, length,
             (long long)position, strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  // End of file is IOStatus.EOF (-1), as in read0 above.
  return (stack_value){.i = result == 0 ? -1 : (s32)result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, write0, "(Ljava/io/FileDescriptor;JI)I") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  ssize_t result = tracejvm_host_write(
      fd, (void const *)args[1].l, (size_t)args[2].i, 0, false);
  if (result < 0) {
    int error = errno;
    char message[160];
    snprintf(message, sizeof(message), "write(fd=%d, length=%d): %s", fd,
             args[2].i, strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  return (stack_value){.i = (s32)result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, allocationGranularity0, "()J") { return (stack_value){.l = 4096}; }

// (fd: FileDescriptor, prot:Int, pos: Long, len: Long, isSync: Boolean) -> Long
DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, map0, "(Ljava/io/FileDescriptor;IJJZ)J") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  int map_mode = args[1].i;
  off_t pos = args[2].l;
  off_t len = args[3].l;
  bool is_sync = args[4].i;
  int protections;
  int flags;

  // These values are the JDK's UnixFileDispatcherImpl map modes, not
  // operating-system protection flags:
  //   0 = read-only, 1 = read/write, 2 = private copy-on-write.
  switch (map_mode) {
  case 0:
    protections = PROT_READ;
    flags = MAP_SHARED;
    break;
  case 1:
    protections = PROT_READ | PROT_WRITE;
    flags = MAP_SHARED;
    break;
  case 2:
    protections = PROT_READ | PROT_WRITE;
    flags = MAP_PRIVATE;
    break;
  default: {
    char message[160];
    snprintf(message, sizeof(message),
             "map(fd=%d, mode=%d, offset=%lld, length=%lld, sync=%d): "
             "invalid Java map mode",
             fd, map_mode, (long long)pos, (long long)len, is_sync);
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  }

#if defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE)
  if (is_sync)
    flags |= MAP_SYNC | MAP_SHARED_VALIDATE;
#else
  if (is_sync) {
    char message[160];
    snprintf(message, sizeof(message),
             "map(fd=%d, mode=%d, offset=%lld, length=%lld, sync=1): "
             "synchronous mappings are unsupported",
             fd, map_mode, (long long)pos, (long long)len);
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
#endif

  void *result;
  if (tracejvm_host_is_remote_fd(fd)) {
    errno = ENOSYS;
    result = MAP_FAILED;
  } else {
    result = mmap(nullptr, len, protections, flags, fd, pos);
  }
  if (result == MAP_FAILED) {
    int error = errno;
    char message[192];
    snprintf(message, sizeof(message),
             "map(fd=%d, mode=%d, offset=%lld, length=%lld, sync=%d): %s",
             fd, map_mode, (long long)pos, (long long)len, is_sync,
             strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
    return value_null();
  }
  arrput(thread->vm->mmap_allocations, ((mmap_allocation){result, len}));
  return (stack_value){.l = (s64)result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, unmap0, "(JJ)V") {
  void *addr = (void *)args[0].l;
  size_t len = args[1].l;
  for (size_t i = 0; i < arrlenu(thread->vm->mmap_allocations); ++i) {
    if (thread->vm->mmap_allocations[i].ptr == addr) {
      arrdelswap(thread->vm->mmap_allocations, i);
      break;
    }
  }
  int result = munmap(addr, len);
  if (result) {
    int error = errno;
    char message[160];
    snprintf(message, sizeof(message), "unmap(address=%p, length=%zu): %s",
             addr, len, strerror(error));
    thread->current_exception = create_unix_exception_message(thread, message);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, strerror, "(I)[B") {
  char *msg = strerror(host_errno_from_openjdk_linux(args[0].i));
  size_t len = strlen(msg);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), msg, len);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, init0, "()V") { return value_null(); }
DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, init, "()V") { return value_null(); }

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, closeIntFD, "(I)V") {
  if (args[0].i >= 0 && tracejvm_host_close(args[0].i) != 0) {
    thread->current_exception = create_unix_exception_message(
        thread, strerror(errno));
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, closeIntFD, "(I)V") {
  if (args[0].i >= 0 && tracejvm_host_close(args[0].i) != 0) {
    thread->current_exception = create_unix_exception_message(
        thread, strerror(errno));
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, close0,
               "(Ljava/io/FileDescriptor;)V") {
  if (!args[0].handle || !args[0].handle->obj)
    return value_null();
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  if (fd >= 0 && tracejvm_host_close(fd) != 0) {
    thread->current_exception = create_unix_exception_message(
        thread, strerror(errno));
  } else {
    StoreFieldInt(args[0].handle->obj, "fd", -1);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, preClose0,
               "(Ljava/io/FileDescriptor;)V") {
  return value_null();
}
