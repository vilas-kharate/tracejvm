#include "file-errors.h"
#include <errno.h>
#include <limits.h>
#include <natives-dsl.h>
#include <tracejvm-host.h>
#ifdef EMSCRIPTEN
#include <fcntl.h>
#else
#include <sys/fcntl.h>
#endif
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

DECLARE_NATIVE("java/io", FileInputStream, initIDs, "()V") { return value_null(); }

DECLARE_NATIVE("java/io", FileInputStream, open0, "(Ljava/lang/String;)V") {
  if (!args[0].handle->obj)
    return value_null();

  heap_string filename = AsHeapString(args[0].handle->obj, on_oom);

  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  s32 unix_fd = tracejvm_host_open(filename.chars, O_RDONLY, 0);
  StoreFieldInt(fd, "fd", unix_fd);

  if (unix_fd < 0)
    raise_file_not_found(thread, filename, errno);

  free_heap_str(filename);
  return value_null();

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/io", FileInputStream, read0, "()I") {
  DCHECK(argc == 0);
  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd < 0) {
    raise_vm_exception(thread, STR("java/io/FileNotFoundException"), STR("File not found"));
    return value_null();
  }

  int length = 1;
  char res;

  int bytes_read;
  if (unix_fd == 0 && thread->vm->read_stdin &&
      !tracejvm_host_routes_fd(unix_fd)) {
    bytes_read = thread->vm->read_stdin(&res, length, thread->vm->stdio_override_param);
  } else {
    bytes_read = (s32)tracejvm_host_read(
        unix_fd, &res, length, 0, false);
  }

  if (bytes_read < 0) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("Error reading file"));
    return value_null();
  }

  return (stack_value){.i = bytes_read == 0 ? -1 : (int)res};
}

DECLARE_NATIVE("java/io", FileInputStream, readBytes, "([BII)I") {
  DCHECK(argc == 3);
  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd < 0) {
    raise_vm_exception(thread, STR("java/io/FileNotFoundException"), STR("File not found"));
    return value_null();
  }

  obj_header *array = args[0].handle->obj;
  if (!array) {
    raise_null_pointer_exception(thread);
    return value_null();
  }

  s32 offset = args[1].i;
  s32 length = args[2].i;

  if (offset < 0 || length < 0 || (s64)offset + length > ArrayLength(array)) {
    raise_vm_exception_no_msg(thread, STR("java/lang/ArrayIndexOutOfBoundsException"));
    return value_null();
  }

  char *buf = ArrayData(array) + offset;

  s32 bytes_read;
  if (unix_fd == 0 && thread->vm->read_stdin &&
      !tracejvm_host_routes_fd(unix_fd)) {
    bytes_read = thread->vm->read_stdin(buf, length, thread->vm->stdio_override_param);
  } else {
    bytes_read = (s32)tracejvm_host_read(
        unix_fd, buf, length, 0, false);
  }

  if (bytes_read < 0) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("Error reading file"));
    return value_null();
  }
  return (stack_value){.i = bytes_read == 0 ? -1 : bytes_read};
}

DECLARE_NATIVE("java/io", FileInputStream, close0, "()V") {
  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);

  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd != -1)
    tracejvm_host_close(unix_fd);
  StoreFieldInt(fd, "fd", -1);

  return value_null();
}

DECLARE_NATIVE("java/io", FileInputStream, available0, "()I") {
  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd == -1) {
    return (stack_value){.i = 0};
  }

  int available;

  if (unix_fd == 0 && thread->vm->poll_available_stdin &&
      !tracejvm_host_routes_fd(unix_fd)) {
    available = thread->vm->poll_available_stdin(thread->vm->stdio_override_param);
  } else if (tracejvm_host_is_remote_fd(unix_fd)) {
    available = 0;
  } else {
    struct stat st;
    if (fstat(unix_fd, &st) == 0 && S_ISREG(st.st_mode)) {
      off_t position = lseek(unix_fd, 0, SEEK_CUR);
      if (position < 0) {
        raise_vm_exception(thread, STR("java/io/IOException"),
                           STR("Error getting current file position"));
        return value_null();
      }
      off_t remaining = st.st_size > position ? st.st_size - position : 0;
      available = remaining > INT_MAX ? INT_MAX : (int)remaining;
    } else {
      int err = ioctl(unix_fd, FIONREAD, &available);
      if (err < 0) {
        raise_vm_exception(thread, STR("java/io/IOException"),
                           STR("Error getting available bytes"));
        return value_null();
      }
    }
  }

  return (stack_value){.i = (s32)available};
}

// length0 / position0: used by readAllBytes, readNBytes, skip and transferTo (JDK 23). Missing before, so
// FileInputStream.readAllBytes threw UnsatisfiedLinkError (Shiklo fork, 2026-10-28). As the JDK: a closed stream is
// "Stream Closed"; a stream that is not a regular file (keyboard, pipe) has length and position 0, which makes the
// callers fall back to reading until the end.
DECLARE_NATIVE("java/io", FileInputStream, length0, "()J") {
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd == -1) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("Stream Closed"));
    return value_null();
  }
  struct stat st;
  if (tracejvm_host_fstat(unix_fd, &st) != 0) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("GetLength failed"));
    return value_null();
  }
  return (stack_value){.l = S_ISREG(st.st_mode) ? (s64)st.st_size : 0};
}

DECLARE_NATIVE("java/io", FileInputStream, position0, "()J") {
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd == -1) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("Stream Closed"));
    return value_null();
  }
  struct stat st;
  if (tracejvm_host_fstat(unix_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    return (stack_value){.l = 0};
  }
  off_t position = tracejvm_host_seek(unix_fd, 0, SEEK_CUR);
  if (position < 0) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("Seek failed"));
    return value_null();
  }
  return (stack_value){.l = (s64)position};
}
