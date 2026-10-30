#include "file-errors.h"
#include <natives-dsl.h>
#include <tracejvm-host.h>

#include <fcntl.h>
#include <unistd.h>

DECLARE_NATIVE("java/io", RandomAccessFile, initIDs, "()V") {
  return value_null();
}

static obj_header *random_access_fd(obj_header *obj) {
  obj_header *fd = LoadFieldObject(obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  return fd;
}

static int random_access_native_fd(obj_header *obj) {
  return LoadFieldInt(random_access_fd(obj), "fd");
}

static bool require_open(
    vm_thread *thread, obj_header *obj, int *fd_out) {
  int fd = random_access_native_fd(obj);
  if (fd < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("File not open"));
    return false;
  }
  *fd_out = fd;
  return true;
}

DECLARE_NATIVE("java/io", RandomAccessFile, open0, "(Ljava/lang/String;I)V") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  heap_string filename = AsHeapString(args[0].handle->obj, on_oom);
  int mode = args[1].i;
  int flags = (mode & 2) ? O_RDWR | O_CREAT : O_RDONLY;
  int fd = tracejvm_host_open(filename.chars, flags, 0666);
  StoreFieldInt(random_access_fd(obj->obj), "fd", fd);
  if (fd < 0)
    raise_file_not_found(thread, filename, errno);
  free_heap_str(filename);

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, read0, "()I") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  unsigned char byte;
  ssize_t count = tracejvm_host_read(fd, &byte, 1, 0, false);
  if (count < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error reading file"));
    return value_null();
  }
  return (stack_value){.i = count == 0 ? -1 : byte};
}

DECLARE_NATIVE("java/io", RandomAccessFile, readBytes0, "([BII)I") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  obj_header *array = args[0].handle->obj;
  if (!array) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  s32 offset = args[1].i;
  s32 length = args[2].i;
  if (offset < 0 || length < 0 ||
      (s64)offset + length > ArrayLength(array)) {
    raise_vm_exception_no_msg(
        thread, STR("java/lang/ArrayIndexOutOfBoundsException"));
    return value_null();
  }
  ssize_t count = tracejvm_host_read(
      fd, (char *)ArrayData(array) + offset, length, 0, false);
  if (count < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error reading file"));
    return value_null();
  }
  return (stack_value){.i = count == 0 ? -1 : (s32)count};
}

DECLARE_NATIVE("java/io", RandomAccessFile, write0, "(I)V") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  unsigned char byte = (unsigned char)args[0].i;
  if (tracejvm_host_write(fd, &byte, 1, 0, false) != 1) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error writing file"));
  }
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, writeBytes0, "([BII)V") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  obj_header *array = args[0].handle->obj;
  if (!array) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  s32 offset = args[1].i;
  s32 length = args[2].i;
  if (offset < 0 || length < 0 ||
      (s64)offset + length > ArrayLength(array)) {
    raise_vm_exception_no_msg(
        thread, STR("java/lang/ArrayIndexOutOfBoundsException"));
    return value_null();
  }
  char *buffer = (char *)ArrayData(array) + offset;
  while (length > 0) {
    ssize_t count = tracejvm_host_write(
        fd, buffer, length, 0, false);
    if (count <= 0) {
      raise_vm_exception(
          thread, STR("java/io/IOException"), STR("Error writing file"));
      return value_null();
    }
    buffer += count;
    length -= (s32)count;
  }
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, seek0, "(J)V") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  if (tracejvm_host_seek(fd, (off_t)args[0].l, SEEK_SET) < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error seeking file"));
  }
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, getFilePointer, "()J") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  off_t position = tracejvm_host_seek(fd, 0, SEEK_CUR);
  if (position < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error seeking file"));
    return value_null();
  }
  return (stack_value){.l = (s64)position};
}

DECLARE_NATIVE("java/io", RandomAccessFile, length0, "()J") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  struct stat stat_buffer;
  if (tracejvm_host_fstat(fd, &stat_buffer) < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error stating file"));
    return value_null();
  }
  return (stack_value){.l = (s64)stat_buffer.st_size};
}

DECLARE_NATIVE("java/io", RandomAccessFile, setLength0, "(J)V") {
  int fd;
  if (!require_open(thread, obj->obj, &fd))
    return value_null();
  if (args[0].l < 0 ||
      tracejvm_host_ftruncate(fd, (off_t)args[0].l) < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error truncating file"));
  }
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, close0, "()V") {
  obj_header *fd_object = random_access_fd(obj->obj);
  int fd = LoadFieldInt(fd_object, "fd");
  if (fd >= 0 && tracejvm_host_close(fd) < 0) {
    raise_vm_exception(
        thread, STR("java/io/IOException"), STR("Error closing file"));
    return value_null();
  }
  StoreFieldInt(fd_object, "fd", -1);
  return value_null();
}
