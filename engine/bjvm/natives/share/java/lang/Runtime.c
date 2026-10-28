#include <natives-dsl.h>
#include <stdio.h>
#include <tracejvm-host.h>

DECLARE_NATIVE("java/lang", Runtime, availableProcessors, "()I") { return (stack_value){.i = 1}; }

// The Java heap has one fixed size: totalMemory and maxMemory are its capacity, freeMemory the part not in use, as
// their JDK meanings (Shiklo fork, 2026-10-28). maxMemory was Long.MAX_VALUE, and totalMemory / freeMemory were missing
// (UnsatisfiedLinkError, also from SecureRandom's seed generator, so UUID.randomUUID failed).
DECLARE_NATIVE("java/lang", Runtime, maxMemory, "()J") { return (stack_value){.l = (s64)thread->vm->heap_capacity}; }

DECLARE_NATIVE("java/lang", Runtime, totalMemory, "()J") { return (stack_value){.l = (s64)thread->vm->heap_capacity}; }

DECLARE_NATIVE("java/lang", Runtime, freeMemory, "()J") {
  return (stack_value){.l = (s64)(thread->vm->heap_capacity - thread->vm->heap_used)};
}

DECLARE_NATIVE("java/lang", Runtime, gc, "()V") {
  major_gc(thread->vm);
  return value_null();
}

DECLARE_NATIVE("java/lang", Shutdown, beforeHalt, "()V") { return value_null(); }

// Runtime.exit and Runtime.halt end the program (Shiklo fork, 2026-10-28). halt0 returned, so System.exit called from
// code the page does not rewrite (the JDK's own tools such as jar and javap, Runtime.getRuntime().exit / halt,
// System::exit) went on with the program. When the host routes the error stream, halt0 writes the page's exit signal
// to it (ESC ] lmpb-exit;<status> BEL, as the launcher's $.Sys$.exit does); the host never answers that write, so the
// VM stops there and the page ends the run with that status. Shutdown hooks have run by then, as in HotSpot.
DECLARE_NATIVE("java/lang", Shutdown, halt0, "(I)V") {
  if (tracejvm_host_routes_fd(2)) {
    char mark[32];
    int length = snprintf(mark, sizeof(mark), "\x1b]lmpb-exit;%d\x07", args[0].i);
    tracejvm_host_write(2, mark, (size_t)length, 0, false);
  }
  return value_null();
}