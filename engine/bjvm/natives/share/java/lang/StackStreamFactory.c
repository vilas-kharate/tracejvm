#include <linkage.h>
#include <natives-dsl.h>
#include <stdlib.h>

/*
 * OpenJDK uses this bootstrap check to assert that the VM's StackWalker mode
 * constants agree with java.lang.StackStreamFactory. This engine consumes
 * the OpenJDK constants directly and has no second, independently numbered
 * mode table, so the values cannot diverge.
 */
DECLARE_NATIVE("java/lang", StackStreamFactory, checkStackWalkModes, "()Z") {
  return (stack_value){.i = 1};
}

// StackWalker (Shiklo fork, 2026-10-30). callStackWalk and fetchStackFrames were missing, so every StackWalker threw
// UnsatisfiedLinkError, and with it every java.util.logging call (LogRecord finds its caller with a StackWalker).
// As HotSpot's stackwalk.cpp: the walk starts below the stack walker's own frames (StackWalker, AbstractStackWalker and
// its subclasses), skips skipframes more, leaves out hidden frames (lambda adapters, LambdaForms) unless
// SHOW_HIDDEN_FRAMES, fills the Java side's buffer from startIndex up to bufferSize, and calls back doStackWalk, which
// asks for more batches with fetchStackFrames. A StackFrameInfo is filled whole at once: its ResolvedMethodName (the
// declaring class), modifiers, bci, name, descriptor and StackTraceElement, so the VM needs no expandStackFrameInfo or
// initStackTraceElement. The anchor is the address of the walk's cursor, valid while callStackWalk runs.

enum { CLASS_INFO_ONLY = 0x2, SHOW_HIDDEN_FRAMES = 0x20 };
enum { MN_HIDDEN_MEMBER = 0x00400000, MEMBER_INFO_FLAGS = 0x00FFFFFF };
enum { STACK_WALK_MAGIC = 0x57616c6b };

obj_header *make_stack_trace_element(vm_thread *thread, stack_frame *frame); // Throwable.c

typedef struct {
  u32 magic;
  vm_thread *thread;
  stack_frame *next; // the next frame to hand out, or null at the bottom
} stack_walk;

static stack_frame *visible_frame(stack_frame *frame, int mode) {
  while (frame && !(mode & SHOW_HIDDEN_FRAMES) && is_frame_hidden(frame))
    frame = frame->prev;
  return frame;
}

static bool is_stack_walker_frame(const stack_frame *frame) {
  const classdesc *cd = frame->method->my_class;
  slice walker = STR("java/lang/StackStreamFactory$AbstractStackWalker");
  return utf8_equals(cd->name, "java/lang/StackWalker") || utf8_equals_utf8(cd->name, walker) ||
         (cd->super_class && utf8_equals_utf8(cd->super_class->classdesc->name, walker));
}

// Fills frames[index] (a ClassFrameInfo, or a StackFrameInfo unless CLASS_INFO_ONLY) for frame; false when out of
// memory (the exception is pending).
static bool fill_frame(vm_thread *thread, handle *frames, int index, stack_frame *frame, int mode) {
  cp_method *method = frame->method;
  classdesc *cd = method->my_class;
  obj_header *mirror = (obj_header *)get_class_mirror(thread, cd);
  if (!mirror)
    return false;
  handle *declaring = make_handle(thread, mirror);
  handle *resolved = nullptr, *name = nullptr, *type = nullptr, *element = nullptr;
  bool full = !(mode & CLASS_INFO_ONLY), ok = false;
  if (full) {
    classdesc *ResolvedMethodName = bootstrap_lookup_class(thread, STR("java/lang/invoke/ResolvedMethodName"));
    if (!ResolvedMethodName)
      goto done;
    link_class(thread, ResolvedMethodName);
    resolved = make_handle(thread, new_object(thread, ResolvedMethodName));
    if (!resolved->obj)
      goto done;
    StoreFieldObject(resolved->obj, "java/lang/Class", "vmholder", declaring->obj);
    name = make_handle(thread, MakeJStringFromModifiedUTF8(thread, method->name, true));
    if (!name->obj)
      goto done;
    type = make_handle(thread, MakeJStringFromModifiedUTF8(thread, method->unparsed_descriptor, true));
    if (!type->obj)
      goto done;
    element = make_handle(thread, make_stack_trace_element(thread, frame));
    if (!element->obj)
      goto done;
  }
  obj_header *info = *((obj_header **)ArrayData(frames->obj) + index);
  if (!info) {
    raise_null_pointer_exception(thread);
    goto done;
  }
  int member = (method->access_flags & 0xFFFF) | (cd->is_hidden ? MN_HIDDEN_MEMBER : 0);
  StoreFieldInt(info, "flags", (LoadFieldInt(info, "flags") & ~MEMBER_INFO_FLAGS) | member);
  if (full) {
    StoreFieldObject(info, "java/lang/Object", "classOrMemberName", resolved->obj);
    StoreFieldObject(info, "java/lang/String", "name", name->obj);
    StoreFieldObject(info, "java/lang/Object", "type", type->obj);
    StoreFieldInt(info, "bci", is_frame_native(frame) || !method->code ? -1
                                   : method->code->code[frame->program_counter].original_pc);
    StoreFieldObject(info, "java/lang/StackTraceElement", "ste", element->obj);
  } else {
    StoreFieldObject(info, "java/lang/Object", "classOrMemberName", declaring->obj);
  }
  ok = true;

done:
  if (element)
    drop_handle(thread, element);
  if (type)
    drop_handle(thread, type);
  if (name)
    drop_handle(thread, name);
  if (resolved)
    drop_handle(thread, resolved);
  drop_handle(thread, declaring);
  return ok;
}

// Fills frames[start_index, buffer_size) from the walk's cursor; the number of frames filled, or -1 on an exception.
static int fill_batch(vm_thread *thread, stack_walk *walk, int mode, handle *frames, int start_index, int buffer_size) {
  int end = buffer_size < ArrayLength(frames->obj) ? buffer_size : ArrayLength(frames->obj);
  int count = 0;
  for (int i = start_index; i < end && walk->next; ++i, ++count) {
    if (!fill_frame(thread, frames, i, walk->next, mode))
      return -1;
    walk->next = visible_frame(walk->next->prev, mode);
    if (walk->next == thread->program_base) // the program's launcher and what ran it are not shown
      walk->next = nullptr;
  }
  return count;
}

// int mode, int skipframes, ContinuationScope contScope, Continuation continuation, int bufferSize, int startIndex,
// T[] frames
DECLARE_ASYNC_NATIVE("java/lang", StackStreamFactory$AbstractStackWalker, callStackWalk,
                     "(IILjdk/internal/vm/ContinuationScope;Ljdk/internal/vm/Continuation;II[Ljava/lang/Object;)"
                     "Ljava/lang/Object;",
                     locals(stack_walk *walk; stack_value call[6];), invoked_methods(invoked_method(call_interpreter))) {
  int mode = args[0].i, skip = args[1].i, buffer_size = args[4].i, start_index = args[5].i;
  if (!args[6].handle->obj) {
    raise_null_pointer_exception(thread);
    ASYNC_RETURN(value_null());
  }
  self->walk = calloc(1, sizeof(stack_walk));
  if (!self->walk) {
    out_of_memory(thread);
    ASYNC_RETURN(value_null());
  }
  self->walk->magic = STACK_WALK_MAGIC;
  self->walk->thread = thread;
  stack_frame *frame = thread->stack.top;
  while (frame && is_stack_walker_frame(frame))
    frame = frame->prev;
  frame = visible_frame(frame, mode);
  for (int n = 0; n < skip && frame; ++n)
    frame = visible_frame(frame->prev, mode);
  self->walk->next = frame == thread->program_base ? nullptr : frame;

  int count = fill_batch(thread, self->walk, mode, args[6].handle, start_index, buffer_size);
  if (count < 0) {
    free(self->walk);
    ASYNC_RETURN(value_null());
  }
  cp_method *walk_method =
      method_lookup(obj->obj->descriptor, STR("doStackWalk"), STR("(JIIII)Ljava/lang/Object;"), true, false);
  DCHECK(walk_method);
  self->call[0] = (stack_value){.obj = obj->obj};
  self->call[1] = (stack_value){.l = (s64)(intptr_t)self->walk};
  self->call[2] = (stack_value){.i = skip};
  self->call[3] = (stack_value){.i = count};
  self->call[4] = (stack_value){.i = start_index};
  self->call[5] = (stack_value){.i = start_index + count};
  AWAIT(call_interpreter, thread, walk_method, self->call);

  self->walk->magic = 0; // the anchor is no longer valid
  free(self->walk);
  ASYNC_END(get_async_result(call_interpreter));
}

// int mode, long anchor, int lastBatchFrameCount, int bufferSize, int startIndex, T[] frames
DECLARE_NATIVE("java/lang", StackStreamFactory$AbstractStackWalker, fetchStackFrames, "(IJIII[Ljava/lang/Object;)I") {
  stack_walk *walk = (stack_walk *)(intptr_t)args[1].l;
  if (!walk || walk->magic != STACK_WALK_MAGIC || walk->thread != thread) {
    raise_vm_exception(thread, STR("java/lang/InternalError"), STR("doStackWalk: corrupted buffers on stack"));
    return value_null();
  }
  if (!args[5].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  int count = fill_batch(thread, walk, args[0].i, args[5].handle, args[4].i, args[3].i);
  return (stack_value){.i = count < 0 ? 0 : count};
}

// long anchor, T[] frames, Continuation cont: walking into a parent continuation; this VM's stacks are walked whole.
DECLARE_NATIVE("java/lang", StackStreamFactory$AbstractStackWalker, setContinuation,
               "(J[Ljava/lang/Object;Ljdk/internal/vm/Continuation;)V") {
  return value_null();
}
