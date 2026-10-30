#pragma once

// The FileNotFoundException of a file that could not be opened, with the system's reason as the JDK's io_util_md.c
// gives it: "notes.txt (No such file or directory)" (Shiklo fork, 2026-10-30; the message was the path alone).

#include <errno.h>
#include <exceptions.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void raise_file_not_found(vm_thread *thread, heap_string path, int error) {
  char const *reason = strerror(error ? error : ENOENT);
  size_t capacity = path.len + strlen(reason) + 4;
  char *message = malloc(capacity);
  if (!message) {
    raise_vm_exception(thread, STR("java/io/FileNotFoundException"), hslc(path));
    return;
  }
  int length = snprintf(message, capacity, "%.*s (%s)", (int)path.len, path.chars, reason);
  raise_vm_exception(thread, STR("java/io/FileNotFoundException"), (slice){.chars = message, .len = length});
  free(message);
}
