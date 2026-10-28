#include "analysis.h"

#include <linkage.h>
#include <natives-dsl.h>

// Returns true if the frame is currently constructing the given object.
static bool is_frame_part_of_throwable_construction(stack_frame *frame, object obj) {
  if (utf8_equals(frame->method->name, "fillInStackTrace")) { // hack for now
    return true;
  }
  if (!frame->method->is_ctor || is_frame_native(frame) || frame->num_locals < 1) {
    return false;
  }
  return frame_locals(frame)[0].obj == obj;
}

// Implementation-dependent field where we can store the stack trace
obj_header **backtrace_object(obj_header *throwable) { return &((struct native_Throwable *)throwable)->backtrace; }

// The stack trace elements of the frames [top, top->prev, ...] of any thread's stack, allocated by thread. As HotSpot,
// frames of hidden classes (lambda bodies' adapters, LambdaForms) are not shown. Null when the array could not be
// allocated; when an element or its strings could not be, the array holds the elements made so far. Used by
// Throwable.fillInStackTrace and Thread.dumpThreads.
obj_header *make_stack_trace_elements(vm_thread *thread, stack_frame *top) {
  classdesc *StackTraceElement = cached_classes(thread->vm)->stack_trace_element;
  stack_frame *frame = top;

  int n_frames = 0;
  for (stack_frame *tmp = frame; tmp; tmp = tmp->prev) {
    if (!tmp->method->my_class->is_hidden)
      ++n_frames;
  }

  // Create stack trace of the appropriate height
  handle *stack_trace = make_handle(thread, CreateObjectArray1D(thread, StackTraceElement, n_frames));
  if (!stack_trace->obj) { // Failed to allocate
    drop_handle(thread, stack_trace);
    return nullptr;
  }

  for (int j = 0; n_frames > 0; --n_frames, ++j) {
    while (frame && frame->method->my_class->is_hidden)
      frame = frame->prev;
    DCHECK(frame);

    // Create the stack trace element
    handle *e = make_handle(thread, new_object(thread, StackTraceElement));
    if (!e->obj) { // Failed to allocate StackTraceElement
      drop_handle(thread, e);
      goto done;
    }

    cp_method *method = frame->method;

#define E ((struct native_StackTraceElement *)e->obj)
    // As HotSpot's java_lang_StackTraceElement::fill_in (Shiklo fork, 2026-10-28): the class's binary name with dots,
    // its module's name ("java.base/java.lang.Integer.parseInt"), no file name when the class has no SourceFile
    // attribute ("Unknown Source"; it showed the method's name), and -2 for a native method ("Native Method").
    int line = is_frame_native(frame) ? -2 : get_line_number(method->code, frame->program_counter);
    object o = (void *)get_class_mirror(thread, method->my_class);
    if (!o)
      goto element_oom;
    E->declaringClassObject = o;
    obj_header *module = ((struct native_Class *)o)->module;
    if (module)
      E->moduleName = LoadFieldObject(module, "java/lang/String", "name"); // null for the unnamed module
    INIT_STACK_STRING(binary_name, 1024);
    exchange_slashes_and_dots(&binary_name, method->my_class->name);
    o = MakeJStringFromModifiedUTF8(thread, binary_name, true);
    if (!o)
      goto element_oom;
    E->declaringClass = o;
    o = MakeJStringFromModifiedUTF8(thread, method->name, true);
    if (!o)
      goto element_oom;
    E->methodName = o;
    attribute_source_file *sf = method->my_class->source_file;
    o = nullptr;
    if (sf) {
      o = MakeJStringFromModifiedUTF8(thread, sf->name, true);
      if (!o)
        goto element_oom;
    }

    E->fileName = o;
    E->lineNumber = line;
    *((void **)ArrayData(stack_trace->obj) + j) = e->obj;

#if 0
    fprintf(stderr, "Stack trace element %d: %.*s.%.*s (%s:%d)\n", j, fmt_slice(method->my_class->name), fmt_slice(method->name),
            sf ? sf->name.chars : "unknown", line);
#endif
#undef E
    drop_handle(thread, e);
    frame = frame->prev;
    continue;

  element_oom:
    drop_handle(thread, e);
    goto done;
  }

done:;
  obj_header *result = stack_trace->obj;
  drop_handle(thread, stack_trace);
  return result;
}

DECLARE_NATIVE("java/lang", Throwable, fillInStackTrace, "(I)Ljava/lang/Throwable;") {
  // Called in the constructor of Throwable. We therefore need to ignore
  // frames which are constructing the current object, which we can do by
  // inspecting the stack.
  stack_frame *frame = thread->stack.top;
  while (frame) {
    if (!is_frame_part_of_throwable_construction(frame, obj->obj)) {
      break;
    }
    frame = frame->prev;
  }

  obj_header *stack_trace = make_stack_trace_elements(thread, frame);
  if (!stack_trace) // Failed to allocate
    return value_null();
  ((struct native_Throwable *)obj->obj)->depth = ArrayLength(stack_trace);
  *backtrace_object(obj->obj) = stack_trace;
  return (stack_value){.obj = obj->obj};
}

DECLARE_NATIVE("java/lang", Throwable, getStackTraceDepth, "()I") {
  DCHECK(argc == 0);
  return (stack_value){.i = ArrayLength(*backtrace_object(obj->obj))};
}

DECLARE_NATIVE("java/lang", Throwable, getStackTraceElement, "(I)Ljava/lang/StackTraceElement;") {
  DCHECK(argc == 1);
  obj_header *stack_trace = *backtrace_object(obj->obj);
  int index = args[0].i;
  if (index < 0 || index >= ArrayLength(stack_trace)) {
    return value_null();
  }
  obj_header *element = *((obj_header **)ArrayData(stack_trace) + index);
  return (stack_value){.obj = element};
}

DECLARE_NATIVE("java/lang", StackTraceElement, initStackTraceElements,
               "([Ljava/lang/StackTraceElement;Ljava/lang/Object;I)V") {
  handle *stack_trace = args[1].handle;
  int depth = ArrayLength(stack_trace->obj);
  int array_length = ArrayLength(args[0].handle->obj);
  if (array_length < depth) {
    depth = array_length;
  }
  for (int i = 0; i < depth; ++i) {
    obj_header *element = *((obj_header **)ArrayData(stack_trace->obj) + i);
    ReferenceArrayStore(args[0].handle->obj, i, element);
  }
  return value_null();
}