#include <arrays.h>
#include <monitors.h>
#include <natives-dsl.h>
#include <roundrobin_scheduler.h>

DECLARE_NATIVE("java/lang", Thread, registerNatives, "()V") { return value_null(); }

DECLARE_NATIVE("java/lang", Thread, currentThread, "()Ljava/lang/Thread;") {
  return (stack_value){.obj = (void *)thread->thread_obj};
}

DECLARE_NATIVE("java/lang", Thread, getThreads, "()[Ljava/lang/Thread;") {
  int count = 0;
  for (int index = 0; index < arrlen(thread->vm->active_threads); ++index) {
    if (thread->vm->active_threads[index]->thread_obj)
      ++count;
  }

  obj_header *result = CreateObjectArray1D(thread, cached_classes(thread->vm)->thread, count);
  if (!result)
    return value_null();

  struct native_Thread **threads = ArrayData(result);
  int output = 0;
  for (int index = 0; index < arrlen(thread->vm->active_threads); ++index) {
    vm_thread *candidate = thread->vm->active_threads[index];
    if (candidate->thread_obj)
      threads[output++] = candidate->thread_obj;
  }
  return (stack_value){.obj = result};
}

obj_header *make_stack_trace_elements(vm_thread *thread, stack_frame *top, stack_frame *base); // Throwable.c

// Thread.getAllStackTraces and another thread's getStackTrace (Shiklo fork, 2026-10-28; both natives were missing:
// UnsatisfiedLinkError). Each thread's frames come from its own VM stack; a thread that has not started or has ended
// gets an empty trace, as in HotSpot.
DECLARE_NATIVE("java/lang", Thread, dumpThreads, "([Ljava/lang/Thread;)[[Ljava/lang/StackTraceElement;") {
  if (!args[0].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  classdesc *trace_type = get_or_create_array_classdesc(thread, cached_classes(thread->vm)->stack_trace_element);
  if (!trace_type)
    return value_null();
  int count = ArrayLength(args[0].handle->obj);
  handle *result = make_handle(thread, CreateObjectArray1D(thread, trace_type, count));
  if (!result->obj) {
    drop_handle(thread, result);
    return value_null();
  }
  for (int i = 0; i < count; ++i) {
    struct native_Thread *t = ((struct native_Thread **)ArrayData(args[0].handle->obj))[i];
    vm_thread *target = t ? (vm_thread *)t->eetop : nullptr;
    obj_header *trace = make_stack_trace_elements(thread, target ? target->stack.top : nullptr, target ? target->program_base : nullptr);
    if (!trace)
      break; // out of memory: the exception is pending
    ((obj_header **)ArrayData(result->obj))[i] = trace;
  }
  obj_header *traces = result->obj;
  drop_handle(thread, result);
  return (stack_value){.obj = traces};
}

// Thread.asyncGetStackTrace (another thread's getStackTrace): this thread's trace; empty means none (JDK 23 does not
// fall back to dumpThreads).
DECLARE_NATIVE("java/lang", Thread, getStackTrace0, "()Ljava/lang/Object;") {
  vm_thread *target = (vm_thread *)((struct native_Thread *)obj->obj)->eetop;
  return (stack_value){.obj = make_stack_trace_elements(thread, target ? target->stack.top : nullptr, target ? target->program_base : nullptr)};
}

DECLARE_NATIVE("java/lang", Thread, setPriority0, "(I)V") { return value_null(); }

DECLARE_NATIVE("java/lang", Thread, holdsLock, "(Ljava/lang/Object;)Z") {
  assert(argc == 1);
  handle *lock_obj = args[0].handle;
  if (!lock_obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }

  u32 hold_count = current_thread_hold_count(thread, lock_obj->obj);
  return (stack_value){.i = hold_count > 0};
}

DECLARE_NATIVE("java/lang", Thread, start0, "()V") {
  rr_scheduler *scheduler = thread->vm->scheduler;
  if (!scheduler)
    return value_null(); // TODO throw error instead

#define this_thread ((struct native_Thread *)obj->obj)
  vm_thread *wrapped_thread = create_vm_thread(thread->vm, thread, this_thread, default_thread_options());

  cp_method *run = method_lookup(obj->obj->descriptor, STR("run"), STR("()V"), false, false);
  stack_value argz[1] = {{.obj = obj->obj}};

  this_thread->eetop = (intptr_t)wrapped_thread;
  set_thread_status(wrapped_thread, THREAD_STATUS_RUNNABLE);
  rr_scheduler_run(scheduler, (call_interpreter_t){{wrapped_thread, run, argz}});
#undef this_thread

  return value_null();
}

DECLARE_NATIVE("java/lang", Thread, ensureMaterializedForStackWalk, "(Ljava/lang/Object;)V") { return value_null(); }

DECLARE_NATIVE("java/lang", Thread, getNextThreadIdOffset, "()J") {
  return (stack_value){.l = (intptr_t)&thread->vm->next_thread_id};
}

DECLARE_NATIVE("java/lang", Thread, currentCarrierThread, "()Ljava/lang/Thread;") {
  return (stack_value){.obj = (void *)thread->thread_obj};
}

DECLARE_NATIVE("java/lang", Thread, interrupt0, "()V") {
  StoreFieldBoolean(obj->obj, "interrupted", true);

  [[maybe_unused]] rr_scheduler *scheduler = thread->vm->scheduler;
  // todo: inform scheduler of interrupt, cause thread to potentially awake if yielded
  // todo: maybe also only do this if previously wasn't interrupted already?

  return value_null();
}

DECLARE_ASYNC_NATIVE("java/lang", Thread, sleepNanos0, "(J)V", locals(rr_wakeup_info wakeup_info), invoked_methods()) {

  DEBUG_PEDANTIC_YIELD(self->wakeup_info);

  assert(argc == 1);
  s64 nanos = args[0].l;
  assert(nanos >= 0); // this is always checked before calling this private method

  if (thread->thread_obj->interrupted) {
    thread->thread_obj->interrupted = false; // throw and reset flag
    raise_vm_exception(thread, STR("java/lang/InterruptedException"), STR("Thread interrupted before sleeping"));
    ASYNC_RETURN_VOID();
  }

  u64 start_us = get_unix_us(), sleep_us = (u64)nanos / 1000, end;
  if (__builtin_add_overflow(start_us, sleep_us, &end)) {
    end = 0; // overflow, just sleep forever
  }

  self->wakeup_info.kind = RR_WAKEUP_SLEEP;
  self->wakeup_info.wakeup_us = end;
  set_thread_status(thread, THREAD_STATUS_SLEEPING);
  ASYNC_YIELD((void *)&self->wakeup_info);

  DEBUG_PEDANTIC_YIELD(self->wakeup_info);
  set_thread_status(thread, THREAD_STATUS_RUNNABLE);

  // re-check interrupt status after wakeup
  if (thread->thread_obj->interrupted) {
    thread->thread_obj->interrupted = false; // throw and reset flag
    raise_vm_exception(thread, STR("java/lang/InterruptedException"), STR("Thread interrupted while sleeping"));
    ASYNC_RETURN_VOID();
  }

  ASYNC_END_VOID();
}

DECLARE_NATIVE("java/lang", Thread, clearInterruptEvent, "()V") {
  return value_null(); // openjdk only does something here on Windows devices
}

DECLARE_NATIVE("java/lang", Thread, setNativeName, "(Ljava/lang/String;)V") { return value_null(); }

DECLARE_ASYNC_NATIVE("java/lang", Thread, yield0, "()V", locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  // Thread.yield has no synchronization semantics (JLS 17.9);
  // the JVM is free to implement it as a no-op or treat it as a scheduling hint.
  // This is usually used to "encourage" more context switches to improve throughput, but
  // "It is rarely appropriate to use this method" (OpenJDK).

  self->wakeup_info.kind = RR_WAKEUP_YIELDING;
  ASYNC_YIELD((void *)&self->wakeup_info);

  DEBUG_PEDANTIC_YIELD(self->wakeup_info);

  ASYNC_END_VOID();
}
