#include <classloader.h>
#include <natives-dsl.h>

DECLARE_NATIVE("jdk/internal/misc", VM, initialize, "()V") { return value_null(); }

DECLARE_NATIVE("jdk/internal/misc", PreviewFeatures, isPreviewEnabled, "()Z") { return (stack_value){.i = 1}; }

// The class loader of the most recent method on the stack whose loader is user-defined (not the bootstrap loader,
// not the platform loader, whose parent is the bootstrap one), or null: what ObjectInputStream.resolveClass uses to
// find the classes it reads. Was missing, so deserialization threw UnsatisfiedLinkError (Shiklo fork, 2026-10-28).
DECLARE_NATIVE("jdk/internal/misc", VM, latestUserDefinedLoader0, "()Ljava/lang/ClassLoader;") {
  for (stack_frame *frame = thread->stack.top; frame; frame = frame->prev) {
    if (!frame->method)
      continue;
    classloader *cl = frame->method->my_class->classloader;
    if (cl && !cl->is_bootstrap && cl->parent && !cl->parent->is_bootstrap && cl->java_mirror)
      return (stack_value){.obj = cl->java_mirror};
  }
  return value_null();
}
