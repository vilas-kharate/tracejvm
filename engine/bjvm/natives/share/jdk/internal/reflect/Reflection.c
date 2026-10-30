#include "natives-dsl.h"

// As HotSpot's Method::is_ignored_by_security_stack_walk (Shiklo fork, 2026-10-30): Method.invoke, the reflection
// accessors, LambdaForms (hidden classes here, or the pregenerated java/lang/invoke/*$Holder classes) and the
// MethodHandle intrinsics. Every java/lang/invoke class was skipped, so MethodHandles.lookup() inside one gave its
// caller's lookup: LazyInitializingVarHandle could not find its own method, and SSLContext.getDefault (HttpClient)
// failed with InternalError "uncaught exception".
bool ignore_frame_for_get_caller_class(stack_frame *frame) {
  classdesc *cd = frame->method->my_class;
  if (is_frame_hidden(frame))
    return true;
  if (strncmp(cd->name.chars, "jdk/internal/reflect/", 21) == 0)
    return true;
  if (utf8_equals(cd->name, "java/lang/reflect/Method") && utf8_equals(frame->method->name, "invoke"))
    return true;
  if (strncmp(cd->name.chars, "java/lang/invoke/", 17) == 0 && cd->name.len > 7 &&
      memcmp(cd->name.chars + cd->name.len - 7, "$Holder", 7) == 0)
    return true;
  return false;
}

DECLARE_NATIVE("jdk/internal/reflect", Reflection, getCallerClass, "()Ljava/lang/Class;") {
  // Look a couple frames before the current frame
  int i = 2;
  stack_frame *frame = thread->stack.top;
  while (frame && i > 0) {
    frame = frame->prev;
    --i;
  }
  while (frame && ignore_frame_for_get_caller_class(frame))
    frame = frame->prev;
  if (frame == nullptr)
    return value_null();
  return (stack_value){.obj = (void *)get_class_mirror(thread, frame->method->my_class)};
}

DECLARE_NATIVE("jdk/internal/reflect", Reflection, getClassAccessFlags, "(Ljava/lang/Class;)I") {
  obj_header *obj_ = args[0].handle->obj;
  classdesc *classdesc = unmirror_class(obj_);
  return (stack_value){.i = classdesc->access_flags};
}

DECLARE_NATIVE("jdk/internal/reflect", Reflection, areNestMates, "(Ljava/lang/Class;Ljava/lang/Class;)Z") {
  // TODO
  return (stack_value){.i = 1};
}