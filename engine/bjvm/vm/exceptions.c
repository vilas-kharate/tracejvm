#include <exceptions.h>
#include <objects.h>

void raise_exception_object(vm_thread *thread, object obj) {
  DCHECK(!thread->current_exception && "Exception is already raised");
  DCHECK(obj && "Exception object must be non-null");

  thread->current_exception = obj;

#define T ((struct native_Throwable *)obj)

  stack_frame *frame = thread->stack.top;
  if (frame) {
    if (!is_frame_native(frame)) {
      T->faulting_insn = frame->program_counter;
      T->method = frame->method;
    }
  }
  thread->current_exception = obj;

#undef T
}

[[noreturn]] static void vm_exception_was_not_initialized(slice name) {
  fprintf(stderr, "VM-generated exceptions should be initialised at VM boot\n");
  fprintf(stderr, "Raising exception of type %.*s\n", fmt_slice(name));
  abort();
}

int raise_vm_exception(vm_thread *thread, const slice exception_name, slice msg_utf8) {
  classdesc *classdesc = bootstrap_lookup_class(thread, exception_name);
  DCHECK(!thread->current_exception);
  if (unlikely(classdesc->state != CD_STATE_INITIALIZED)) {
    // Not one of the classes initialised at boot (e.g. java.net.ConnectException): initialise it now, synchronously,
    // as Class.forName0 does; it aborted the VM (Shiklo fork, 2026-10-28).
    initialize_class_t ctx = {.args = {thread, classdesc}};
    thread->stack.synchronous_depth++;
    future_t f = initialize_class(&ctx);
    thread->stack.synchronous_depth--;
    if (f.status != FUTURE_READY || ctx._result || thread->current_exception ||
        classdesc->state != CD_STATE_INITIALIZED) {
      vm_exception_was_not_initialized(exception_name);
    }
  }

  // Create the exception object
  handle *handle = make_handle(thread, new_object(thread, classdesc));
  if (msg_utf8.len > 0) {
    // Call the constructor taking in a single String value
    object msg = MakeJStringFromModifiedUTF8(thread, msg_utf8, false);
    cp_method *method = method_lookup(classdesc, STR("<init>"), STR("(Ljava/lang/String;)V"), true, false);
    call_interpreter_synchronous(thread, method, (stack_value[]){{.obj = handle->obj}, {.obj = msg}});
  } else {
    // Call the constructor taking in no arguments
    cp_method *method = method_lookup(classdesc, STR("<init>"), STR("()V"), true, false);
    call_interpreter_synchronous(thread, method, (stack_value[]){{.obj = handle->obj}});
  }

  if (!thread->current_exception)
    raise_exception_object(thread, handle->obj);
  drop_handle(thread, handle);
  return 0;
}

int raise_vm_exception_no_msg(vm_thread *thread, const slice exception_name) {
  return raise_vm_exception(thread, exception_name, null_str());
}

void raise_div0_arithmetic_exception(vm_thread *thread) {
  raise_vm_exception(thread, STR("java/lang/ArithmeticException"), STR("/ by zero"));
}

void raise_unsatisfied_link_error(vm_thread *thread, const cp_method *method) {
  // Useful for now as we have a ton of natives to implement. We'll remove it long term.
  printf("Unsatisfied link error %.*s on %.*s\n", fmt_slice(method->name), fmt_slice(method->my_class->name));

  INIT_STACK_STRING(err, 1000);
  INIT_STACK_STRING(class_name, 1000);
  exchange_slashes_and_dots(&class_name, method->my_class->name);
  bprintf(err, "Method %.*s on class %.*s with descriptor %.*s", fmt_slice(method->name), fmt_slice(class_name),
          fmt_slice(method->unparsed_descriptor));
  raise_vm_exception(thread, STR("java/lang/UnsatisfiedLinkError"), err);
}

void raise_abstract_method_error(vm_thread *thread, const cp_method *method) {
  INIT_STACK_STRING(err, 1000);
  bprintf(err, "Found no concrete implementation of %.*s", fmt_slice(method->name));
  raise_vm_exception(thread, STR("java/lang/AbstractMethodError"), err);
}

void raise_negative_array_size_exception(vm_thread *thread, int count) {
  INIT_STACK_STRING(err, 12);
  bprintf(err, "%d", count);
  raise_vm_exception(thread, STR("java/lang/NegativeArraySizeException"), err);
}

void raise_null_pointer_exception(vm_thread *thread) {
  raise_vm_exception(thread, STR("java/lang/NullPointerException"), null_str());
}

void raise_verify_error(vm_thread *thread, slice message) {
  raise_vm_exception(thread, STR("java/lang/VerifyError"), message);
}

void raise_array_store_exception(vm_thread *thread, const slice class_name) {
  INIT_STACK_STRING(name, 1024);
  exchange_slashes_and_dots(&name, class_name);
  raise_vm_exception(thread, STR("java/lang/ArrayStoreException"), name);
}

void raise_incompatible_class_change_error(vm_thread *thread, const slice complaint) {
  raise_vm_exception(thread, STR("java/lang/IncompatibleClassChangeError"), complaint);
}

void raise_array_index_oob_exception(vm_thread *thread, int index, int length) {
  INIT_STACK_STRING(complaint, 80);
  // HotSpot's wording (Shiklo fork, 2026-10-28; was "… for array of length …").
  bprintf(complaint, "Index %d out of bounds for length %d", index, length);
  raise_vm_exception(thread, STR("java/lang/ArrayIndexOutOfBoundsException"), complaint);
}

// Where a class lives, as HotSpot's Klass::class_in_module_of_loader: "module java.base" / "unnamed module", and
// "loader 'bootstrap'", "loader 'app'" (a named loader), or "loader com.example.MyLoader @1b6d3586". An array lives where
// its element class does. TraceJVM's run loader and the Shiklo launcher's program loader, both unnamed, load what the
// JDK's application loader would: they are shown as 'app'. On failure the text stays empty.
static void describe_class_place(vm_thread *thread, const classdesc *cd, char *module, size_t module_capacity,
                                 char *loader, size_t loader_capacity) {
  while (cd->kind == CD_KIND_ORDINARY_ARRAY)
    cd = cd->one_fewer_dim;
  snprintf(module, module_capacity, "module java.base");
  snprintf(loader, loader_capacity, "'bootstrap'");
  if (cd->kind != CD_KIND_ORDINARY)
    return; // primitives and their arrays
  struct native_Class *mirror = get_class_mirror(thread, (classdesc *)cd);
  if (!mirror)
    return;
  heap_string text;
  obj_header *module_name = mirror->module ? LoadFieldObject(mirror->module, "java/lang/String", "name") : nullptr;
  if (!module_name) {
    snprintf(module, module_capacity, "unnamed module");
  } else if (read_string_to_utf8(thread, &text, module_name) == 0) {
    snprintf(module, module_capacity, "module %.*s", (int)text.len, text.chars);
    free_heap_str(text);
  }
  obj_header *class_loader = mirror->classLoader;
  if (!class_loader)
    return;
  obj_header *loader_name = ((struct native_ClassLoader *)class_loader)->name;
  slice loader_class = class_loader->descriptor->name;
  if (loader_name && read_string_to_utf8(thread, &text, loader_name) == 0) {
    snprintf(loader, loader_capacity, "'%.*s'", (int)text.len, text.chars);
    free_heap_str(text);
  } else if (utf8_equals(loader_class, "$/Run$$Program") || utf8_equals(loader_class, "java/net/FactoryURLClassLoader")) {
    snprintf(loader, loader_capacity, "'app'");
  } else {
    INIT_STACK_STRING(binary_name, 1024);
    exchange_slashes_and_dots(&binary_name, loader_class);
    snprintf(loader, loader_capacity, "%.*s @%x", fmt_slice(binary_name), (unsigned)get_object_hash_code(thread->vm, class_loader));
  }
}

// HotSpot's message (SharedRuntime::generate_class_cast_message; Shiklo fork, 2026-10-30, was "X cannot be cast to Y"):
// "class java.lang.Integer cannot be cast to class java.lang.String (java.lang.Integer and java.lang.String are in
// module java.base of loader 'bootstrap')".
void raise_class_cast_exception(vm_thread *thread, const classdesc *from, const classdesc *to) {
  INIT_STACK_STRING(complaint, 3000);
  INIT_STACK_STRING(from_str, 1000);
  INIT_STACK_STRING(to_str, 1000);

  exchange_slashes_and_dots(&from_str, from->name);
  exchange_slashes_and_dots(&to_str, to->name);

  char from_module[300], from_loader[300], to_module[300], to_loader[300];
  describe_class_place(thread, from, from_module, sizeof(from_module), from_loader, sizeof(from_loader));
  describe_class_place(thread, to, to_module, sizeof(to_module), to_loader, sizeof(to_loader));
  if (thread->current_exception) // a mirror could not be made
    return;
  if (strcmp(from_module, to_module) == 0 && strcmp(from_loader, to_loader) == 0) {
    complaint = bprintf(complaint, "class %.*s cannot be cast to class %.*s (%.*s and %.*s are in %s of loader %s)",
                        fmt_slice(from_str), fmt_slice(to_str), fmt_slice(from_str), fmt_slice(to_str), from_module,
                        from_loader);
  } else {
    complaint = bprintf(complaint,
                        "class %.*s cannot be cast to class %.*s (%.*s is in %s of loader %s; %.*s is in %s of loader %s)",
                        fmt_slice(from_str), fmt_slice(to_str), fmt_slice(from_str), from_module, from_loader,
                        fmt_slice(to_str), to_module, to_loader);
  }
  raise_vm_exception(thread, STR("java/lang/ClassCastException"), complaint);
}

void raise_illegal_monitor_state_exception(vm_thread *thread) {
  raise_vm_exception(thread, STR("java/lang/IllegalMonitorStateException"), null_str());
}