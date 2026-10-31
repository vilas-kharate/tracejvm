#include "unixlike-fs.h"

#include <natives-dsl.h>
#include <tracejvm-host.h>
#ifdef EMSCRIPTEN
#include <fcntl.h>
#else
#include <sys/fcntl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include "unixlike-fs.h"

DECLARE_NATIVE("java/io", UnixFileSystem, initIDs, "()V") { return value_null(); }

// ---- Permissions and times (Shiklo fork, 2026-10-31), as the JDK's UnixFileSystem_md.c: checkAccess0 was always true
// (a missing file could be read), the setters always false. access is FileSystem.ACCESS_READ (4), ACCESS_WRITE (2) or
// ACCESS_EXECUTE (1); the page's files have one user, so the owner's bits decide.

// The file's mode, or -1 when it has none (missing, or out of memory).
static int file_mode(vm_thread *thread, obj_header *file, heap_string *path) {
  obj_header *path_obj = file ? LoadFieldObject(file, "java/lang/String", "path") : nullptr;
  if (!path_obj)
    return -1;
  *path = AsHeapString(path_obj, on_oom);
  struct stat st;
  if (tracejvm_host_stat(path->chars, &st, true) != 0) {
    free_heap_str(*path);
    return -1;
  }
  return (int)st.st_mode;
on_oom:
  return -1;
}

DECLARE_NATIVE("java/io", UnixFileSystem, checkAccess0, "(Ljava/io/File;I)Z") {
  heap_string path;
  int mode = file_mode(thread, args[0].handle->obj, &path);
  if (mode < 0)
    return (stack_value){.i = 0};
  free_heap_str(path);
  return (stack_value){.i = (mode & (args[1].i << 6)) != 0};
}

// File f, int access, boolean enable, boolean owneronly
DECLARE_NATIVE("java/io", UnixFileSystem, setPermission0, "(Ljava/io/File;IZZ)Z") {
  heap_string path;
  int mode = file_mode(thread, args[0].handle->obj, &path);
  if (mode < 0)
    return (stack_value){.i = 0};
  int bits = args[3].i ? args[1].i << 6 : (args[1].i << 6) | (args[1].i << 3) | args[1].i;
  mode = args[2].i ? mode | bits : mode & ~bits;
  bool ok = tracejvm_host_chmod(path.chars, (mode_t)(mode & 07777)) == 0;
  free_heap_str(path);
  return (stack_value){.i = ok};
}

// File f, long time
DECLARE_NATIVE("java/io", UnixFileSystem, setLastModifiedTime0, "(Ljava/io/File;J)Z") {
  heap_string path;
  if (file_mode(thread, args[0].handle->obj, &path) < 0)
    return (stack_value){.i = 0};
  bool ok = tracejvm_host_utimes(path.chars, (double)args[1].l) == 0;
  free_heap_str(path);
  return (stack_value){.i = ok};
}

// File f
DECLARE_NATIVE("java/io", UnixFileSystem, setReadOnly0, "(Ljava/io/File;)Z") {
  heap_string path;
  int mode = file_mode(thread, args[0].handle->obj, &path);
  if (mode < 0)
    return (stack_value){.i = 0};
  bool ok = tracejvm_host_chmod(path.chars, (mode_t)(mode & 07777 & ~0222)) == 0;
  free_heap_str(path);
  return (stack_value){.i = ok};
}

DECLARE_NATIVE("java/io", UnixFileSystem, getNameMax0, "(Ljava/lang/String;)J") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  heap_string path = AsHeapString(args[0].handle->obj, on_oom);
  long name_max = pathconf(path.chars, _PC_NAME_MAX);
  free_heap_str(path);
  return (stack_value){.l = name_max > 0 ? name_max : 255};

on_oom:
  return (stack_value){.l = 255};
}

DECLARE_NATIVE("java/io", UnixFileSystem, createFileExclusively0, "(Ljava/lang/String;)Z") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  heap_string path = AsHeapString(args[0].handle->obj, on_oom);
  int fd = tracejvm_host_open(
      path.chars, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (fd >= 0) tracejvm_host_close(fd);
  free_heap_str(path);
  return (stack_value){.i = fd >= 0};

on_oom:
  return (stack_value){.i = 0};
}
DECLARE_ASYNC_NATIVE("java/io", UnixFileSystem, getBooleanAttributes0, "(Ljava/io/File;)I", locals(),
                     invoked_methods()) {
  obj_header *file_obj = args[0].handle->obj;
  obj_header *path = LoadFieldObject(file_obj, "java/lang/String", "path");

  heap_string path_str = AsHeapString(path, on_oom);

  struct stat st;
  int result = tracejvm_host_stat(path_str.chars, &st, true) != 0
      ? 0
      : BA_EXISTS | (S_ISDIR(st.st_mode) ? BA_DIRECTORY : BA_REGULAR);
  free_heap_str(path_str);

  ASYNC_RETURN((stack_value){.i = result});

on_oom:
  ASYNC_END(value_null());
}

static heap_string canonicalize_path(slice path) {
  slice *components = nullptr;

  u32 i = 0;
  for (u32 j = 0; j <= path.len; ++j) {
    if (path.chars[j] == '/' || j == path.len) {
      slice slc = (slice){path.chars + i, j - i};
      if (utf8_equals(slc, "..")) {
        if (arrlen(components)) {
          arrpop(components);
        }
      } else if (!utf8_equals(slc, ".") && i < j) {
        arrput(components, slc);
      }
      i = j + 1;
    }
  }

  i = 0;
  heap_string result = make_heap_str(path.len);
  for (u32 component_i = 0; component_i < arrlen(components); ++component_i) {
    result.chars[i++] = '/';
    for (u32 j = 0; j < components[component_i].len; ++j) {
      result.chars[i++] = components[component_i].chars[j];
    }
  }
  result.len = i;
  arrfree(components);
  return result;
}

DECLARE_NATIVE("java/io", UnixFileSystem, canonicalize0, "(Ljava/lang/String;)Ljava/lang/String;") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }

  // Concatenate the current working directory with the given path
  object raw = RawStringData(thread, args[0].handle->obj);
  slice data = (slice){.chars = ArrayData(raw), .len = ArrayLength(raw)};

  heap_string canonical = canonicalize_path(data); // todo: deal with oom here

  // canonicalize_path only is looking for sequences of .. and /, whcih look the same regardless of the coding.
  // It passes other elements on as-is, meaning the canonicalized string has the same encoding as the input string
  string_coder_kind coder = ((struct native_String *)args[0].handle->obj)->coder;
  obj_header *result = MakeJStringFromData(thread, hslc(canonical), coder);

  free_heap_str(canonical);

  return (stack_value){.obj = result};
}

DECLARE_NATIVE("java/io", UnixFileSystem, getLastModifiedTime0, "(Ljava/io/File;)J") {
  obj_header *file_obj = args[0].handle->obj;
  obj_header *path = LoadFieldObject(file_obj, "java/lang/String", "path");

  heap_string path_str = AsHeapString(path, on_oom);

  struct stat st;
  stack_value result = tracejvm_host_stat(path_str.chars, &st, true) != 0
      ? value_null()
      : (stack_value){
            .l = (s64)st.st_mtim.tv_sec * 1000 +
                 st.st_mtim.tv_nsec / 1000000};
  free_heap_str(path_str);
  return result;

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/io", UnixFileSystem, getLength0, "(Ljava/io/File;)J") {
  obj_header *file_obj = args[0].handle->obj;
  obj_header *path = LoadFieldObject(file_obj, "java/lang/String", "path");

  heap_string path_str = AsHeapString(path, on_oom);
  struct stat st;
  stack_value result =
      tracejvm_host_stat(path_str.chars, &st, true) == 0
          ? (stack_value){.l = st.st_size}
          : (stack_value){.l = 0};
  free_heap_str(path_str);
  return result;

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/io", UnixFileSystem, delete0, "(Ljava/io/File;)Z") {
  obj_header *file_obj = args[0].handle->obj;
  obj_header *path = LoadFieldObject(file_obj, "java/lang/String", "path");
  heap_string path_str = AsHeapString(path, on_oom);
  struct stat st;
  int result = 0;
  if (tracejvm_host_stat(path_str.chars, &st, true) == 0) {
    result = S_ISDIR(st.st_mode)
      ? tracejvm_host_rmdir(path_str.chars) == 0
      : tracejvm_host_unlink(path_str.chars) == 0;
  }
  free_heap_str(path_str);
  return (stack_value){.i = result};

on_oom:
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("java/io", UnixFileSystem, createDirectory0, "(Ljava/io/File;)Z") {
  obj_header *file_obj = args[0].handle->obj;
  if (!file_obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }

  obj_header *path = LoadFieldObject(file_obj, "java/lang/String", "path");
  heap_string path_str = AsHeapString(path, on_oom);
  int result = tracejvm_host_mkdir(path_str.chars, 0777) == 0;
  free_heap_str(path_str);
  return (stack_value){.i = result};

on_oom:
  return (stack_value){.i = 0};
}

// The java.io.File natives this VM lacked (Shiklo fork, 2026-10-29): list0 (File.list, listFiles), rename0
// (renameTo), getSpace0, and the ones that change a file's permissions or time. The page's file system keeps neither
// permissions nor settable times, so those report failure (false), as the JDK does when it cannot make the change.

// File f → its entries' names without "." and "..", or null when it is not a readable folder.
DECLARE_NATIVE("java/io", UnixFileSystem, list0, "(Ljava/io/File;)[Ljava/lang/String;") {
  obj_header *file_obj = args[0].handle->obj;
  if (!file_obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  obj_header *path = LoadFieldObject(file_obj, "java/lang/String", "path");
  heap_string path_str = AsHeapString(path, on_oom);
  tracejvm_host_dir *dir = tracejvm_host_opendir(path_str.chars);
  free_heap_str(path_str);
  if (!dir)
    return value_null();
  char **names = nullptr;
  char name[PATH_MAX];
  while (tracejvm_host_readdir(dir, name, sizeof(name)) > 0) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    arrput(names, strdup(name));
  }
  tracejvm_host_closedir(dir);

  handle *result = make_handle(thread, CreateObjectArray1D(thread, cached_classes(thread->vm)->string, arrlen(names)));
  for (int i = 0; result->obj && i < arrlen(names); ++i) {
    obj_header *entry = MakeJStringFromModifiedUTF8(thread, str_to_utf8(names[i]), false);
    if (!entry)
      break; // out of memory: the exception is pending
    ReferenceArrayStore(result->obj, i, entry);
  }
  obj_header *list = thread->current_exception ? nullptr : result->obj;
  drop_handle(thread, result);
  for (int i = 0; i < arrlen(names); ++i)
    free(names[i]);
  arrfree(names);
  return (stack_value){.obj = list};

on_oom:
  return value_null();
}

// File f1, File f2
DECLARE_NATIVE("java/io", UnixFileSystem, rename0, "(Ljava/io/File;Ljava/io/File;)Z") {
  if (!args[0].handle->obj || !args[1].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  obj_header *from = LoadFieldObject(args[0].handle->obj, "java/lang/String", "path");
  heap_string from_str = AsHeapString(from, on_oom);
  obj_header *to = LoadFieldObject(args[1].handle->obj, "java/lang/String", "path");
  heap_string to_str = AsHeapString(to, on_oom_from);
  int result = tracejvm_host_rename(from_str.chars, to_str.chars) == 0;
  free_heap_str(to_str);
  free_heap_str(from_str);
  return (stack_value){.i = result};

on_oom_from:
  free_heap_str(from_str);
on_oom:
  return (stack_value){.i = 0};
}

// File f, int t (0 total, 1 free, 2 usable): the page's files are described as 1 GiB with 768 MiB free, as statvfs0.
DECLARE_NATIVE("java/io", UnixFileSystem, getSpace0, "(Ljava/io/File;I)J") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  return (stack_value){.l = args[1].i == 0 ? (s64)1 << 30 : (s64)768 << 20};
}
