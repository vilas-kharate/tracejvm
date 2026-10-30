#include "file-errors.h"
#include <natives-dsl.h>
#include <tracejvm-host.h>
#ifdef EMSCRIPTEN
#include <fcntl.h>
#else
#include <sys/fcntl.h>
#endif
#include <unistd.h>

DECLARE_NATIVE("java/io", FileOutputStream, initIDs, "()V") { return value_null(); }

DECLARE_NATIVE("java/io", FileOutputStream, open0, "(Ljava/lang/String;Z)V") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }

  heap_string filename = AsHeapString(args[0].handle->obj, on_oom);
  bool append = args[1].i;
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  s32 unix_fd = tracejvm_host_open(filename.chars, flags, 0666);
  StoreFieldInt(fd, "fd", unix_fd);
  if (unix_fd < 0)
    raise_file_not_found(thread, filename, errno);
  free_heap_str(filename);
  return value_null();

on_oom:
  return value_null();
}

// int b, boolean append: one byte (Shiklo fork, 2026-10-30; it was missing, so FileOutputStream.write(int) threw
// UnsatisfiedLinkError: a DataOutputStream or ZipOutputStream straight on a file).
DECLARE_NATIVE("java/io", FileOutputStream, write, "(IZ)V") {
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  s32 unix_fd = LoadFieldInt(fd, "fd");
  char byte = (char)args[0].i;
  if (unix_fd == 1 && thread->vm->write_stdout && !tracejvm_host_routes_fd(unix_fd)) {
    thread->vm->write_stdout(&byte, 1, thread->vm->stdio_override_param);
  } else if (unix_fd == 2 && thread->vm->write_stderr && !tracejvm_host_routes_fd(unix_fd)) {
    thread->vm->write_stderr(&byte, 1, thread->vm->stdio_override_param);
  } else if (tracejvm_host_write(unix_fd, &byte, 1, 0, false) != 1) {
    raise_vm_exception(thread, STR("java/io/IOException"), STR("Error writing file"));
  }
  return value_null();
}

DECLARE_NATIVE("java/io", FileOutputStream, writeBytes, "([BIIZ)V") {
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  s32 unix_fd = LoadFieldInt(fd, "fd");

  obj_header *bytes = args[0].handle->obj;
  s32 offset = args[1].i;
  s32 length = args[2].i;
  [[maybe_unused]] bool append = args[3].i;
  char *data = (char *)ArrayData(bytes);

  if (offset < 0 || length < 0 || (long)offset + length > ArrayLength(bytes)) {
    raise_vm_exception_no_msg(thread, STR("java/lang/ArrayIndexOutOfBoundsException"));
    return value_null();
  }

  char *buf = data + offset;

  if (unix_fd == 1 && thread->vm->write_stdout &&
      !tracejvm_host_routes_fd(unix_fd)) {
    thread->vm->write_stdout(buf, length, thread->vm->stdio_override_param);
  } else if (unix_fd == 2 && thread->vm->write_stderr &&
             !tracejvm_host_routes_fd(unix_fd)) {
    thread->vm->write_stderr(buf, length, thread->vm->stdio_override_param);
  } else {              // do an actual syscall
    if (unix_fd == 2 && !tracejvm_host_routes_fd(unix_fd)) {
      fprintf(stderr, "%.*s", length, buf);
    } else {
      while (length > 0) {
        s32 written = (s32)tracejvm_host_write(
            unix_fd, buf, length, 0, false);

        if (written <= 0) {
          raise_vm_exception(thread, STR("java/io/IOException"), STR("Error writing file"));
          return value_null();
        }

        length -= written;
        buf += written;
      }
    }
  }
  return value_null();
}

DECLARE_NATIVE("java/io", FileOutputStream, close0, "()V") {
  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);

  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd != -1)
    tracejvm_host_close(unix_fd);
  StoreFieldInt(fd, "fd", -1);

  return value_null();
}
