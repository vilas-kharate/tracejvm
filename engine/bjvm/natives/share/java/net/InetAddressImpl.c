// Name lookup and network interfaces in the browser (Shiklo fork, 2026-10-28).
//
// There is no DNS in a browser tab. "localhost" (also the name getLocalHostName gives) is the loopback address
// 127.0.0.1, the network a program really has here: sockets between its threads and between programs on the page.
// Any other name is an UnknownHostException, as on a computer without a network. NetworkInterface reports no
// interfaces (the JDK's seed generator, used by SecureRandom and UUID.randomUUID, handles that itself).
// These natives were missing: InetAddress.getByName("localhost") threw UnsatisfiedLinkError and the VM aborted.
#include <natives-dsl.h>
#include <strings.h>

static bool is_local_name(vm_thread *thread, obj_header *name) {
  if (!name)
    return false;
  heap_string chars;
  if (read_string_to_utf8(thread, &chars, name))
    return false;
  bool local = (chars.len == 9 && strncasecmp(chars.chars, "localhost", 9) == 0) ||
               (chars.len == 21 && strncasecmp(chars.chars, "localhost.localdomain", 21) == 0);
  free_heap_str(chars);
  return local;
}

// { Inet4Address(host, 127.0.0.1) }
static stack_value loopback_addresses(vm_thread *thread, handle *host) {
  classdesc *inet4 = bootstrap_lookup_class(thread, STR("java/net/Inet4Address"));
  classdesc *inet = bootstrap_lookup_class(thread, STR("java/net/InetAddress"));
  if (!inet4 || !inet)
    return value_null();
  // Initialized (so linked) before an instance is made, as the bytecode `new` does (2026-10-30: on a program's first
  // lookup Inet4Address had no field layout yet, the object was too small and its constructor wrote over the next
  // allocation: getAllByName("localhost") threw ArrayStoreException, so HttpServer and HttpClient could not start).
  initialize_class_t init = {.args = {thread, inet4}};
  thread->stack.synchronous_depth++;
  future_t initialized = initialize_class(&init);
  thread->stack.synchronous_depth--;
  CHECK(initialized.status == FUTURE_READY);
  if (thread->current_exception)
    return value_null();
  handle *address = make_handle(thread, new_object(thread, inet4));
  cp_method *constructor = method_lookup(inet4, STR("<init>"), STR("(Ljava/lang/String;I)V"), true, false);
  call_interpreter_synchronous(
      thread, constructor, (stack_value[]){{.obj = address->obj}, {.obj = host->obj}, {.i = 0x7f000001}});
  obj_header *array = nullptr;
  if (!thread->current_exception) {
    array = CreateObjectArray1D(thread, inet, 1);
    if (array)
      ReferenceArrayStore(array, 0, address->obj);
  }
  drop_handle(thread, address);
  return (stack_value){.obj = array};
}

static stack_value lookup(vm_thread *thread, handle *name) {
  if (is_local_name(thread, name->obj))
    return loopback_addresses(thread, name);
  heap_string chars;
  if (name->obj && !read_string_to_utf8(thread, &chars, name->obj)) {
    raise_vm_exception(thread, STR("java/net/UnknownHostException"), hslc(chars));
    free_heap_str(chars);
  } else {
    raise_vm_exception_no_msg(thread, STR("java/net/UnknownHostException"));
  }
  return value_null();
}

static stack_value host_by_address(vm_thread *thread, obj_header *address) {
  if (address && ArrayLength(address) == 4 && ByteArrayLoad(address, 0) == 127)
    return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
  raise_vm_exception_no_msg(thread, STR("java/net/UnknownHostException"));
  return value_null();
}

static bool is_loopback(obj_header *address) {
  return address && ArrayLength(address) == 4 && ByteArrayLoad(address, 0) == 127;
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, getLocalHostName, "()Ljava/lang/String;") {
  return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, lookupAllHostAddr, "(Ljava/lang/String;)[Ljava/net/InetAddress;") {
  return lookup(thread, args[0].handle);
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, getHostByAddr, "([B)Ljava/lang/String;") {
  return host_by_address(thread, args[0].handle->obj);
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, isReachable0, "([BI[BI)Z") {
  return (stack_value){.i = is_loopback(args[0].handle->obj)};
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, getLocalHostName, "()Ljava/lang/String;") {
  return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, lookupAllHostAddr, "(Ljava/lang/String;I)[Ljava/net/InetAddress;") {
  return lookup(thread, args[0].handle);
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, getHostByAddr, "([B)Ljava/lang/String;") {
  return host_by_address(thread, args[0].handle->obj);
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, isReachable0, "([BII[BII)Z") {
  return (stack_value){.i = is_loopback(args[0].handle->obj)};
}

DECLARE_NATIVE("java/net", NetworkInterface, init, "()V") { return value_null(); }

DECLARE_NATIVE("java/net", NetworkInterface, getAll, "()[Ljava/net/NetworkInterface;") {
  classdesc *type = bootstrap_lookup_class(thread, STR("java/net/NetworkInterface"));
  return (stack_value){.obj = type ? CreateObjectArray1D(thread, type, 0) : nullptr};
}

DECLARE_NATIVE("java/net", NetworkInterface, getByName0, "(Ljava/lang/String;)Ljava/net/NetworkInterface;") {
  return value_null();
}

DECLARE_NATIVE("java/net", NetworkInterface, getByIndex0, "(I)Ljava/net/NetworkInterface;") { return value_null(); }

DECLARE_NATIVE("java/net", NetworkInterface, getByInetAddress0, "(Ljava/net/InetAddress;)Ljava/net/NetworkInterface;") {
  return value_null();
}

DECLARE_NATIVE("java/net", NetworkInterface, boundInetAddress0, "(Ljava/net/InetAddress;)Z") {
  return (stack_value){.i = 0};
}
