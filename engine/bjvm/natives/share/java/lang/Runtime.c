#include <natives-dsl.h>

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

DECLARE_NATIVE("java/lang", Shutdown, halt0, "(I)V") { return value_null(); }