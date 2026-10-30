#include <errno.h>
#include <limits.h>
#include <natives-dsl.h>
#include <roundrobin_scheduler.h>
#include <stdio.h>
#include <tracejvm-host.h>

enum {
  JDK_IOS_UNAVAILABLE = -2,
  JDK_POLLIN = 0x0001,
  JDK_POLLOUT = 0x0004,
  JDK_POLLERR = 0x0008,
  JDK_POLLHUP = 0x0010,
  JDK_POLLNVAL = 0x0020,
};

static int file_descriptor_value(handle *descriptor) {
  return descriptor && descriptor->obj
      ? LoadFieldInt(descriptor->obj, "fd")
      : -1;
}

// As the JDK's Net.c (handleSocketError): the exception class follows the error, and the message is the system's text
// alone ("Connection refused"), so `catch (ConnectException e)` works (Shiklo fork, 2026-10-28; it was always a
// SocketException "connect: Connection refused").
static void raise_socket_exception(
    vm_thread *thread, [[maybe_unused]] char const *operation, int error) {
  INIT_STACK_STRING(message, 192);
  bprintf(message, "%s", strerror(error));
  switch (error) {
  case EPROTO:
    raise_vm_exception(thread, STR("java/net/ProtocolException"), message);
    break;
  case ECONNREFUSED:
  case ETIMEDOUT:
  case ENOTCONN:
    raise_vm_exception(thread, STR("java/net/ConnectException"), message);
    break;
  case EHOSTUNREACH:
    raise_vm_exception(thread, STR("java/net/NoRouteToHostException"), message);
    break;
  case EADDRINUSE:
  case EADDRNOTAVAIL:
  case EACCES:
    raise_vm_exception(thread, STR("java/net/BindException"), message);
    break;
  default:
    raise_vm_exception(thread, STR("java/net/SocketException"), message);
  }
}

static bool inet4_host(obj_header *address, char *host, size_t capacity) {
  if (!address || capacity < 16) {
    errno = EINVAL;
    return false;
  }
  obj_header *holder = LoadFieldObject(
      address, "java/net/InetAddress$InetAddressHolder", "holder");
  if (!holder || LoadFieldInt(holder, "family") != 1) {
    errno = EAFNOSUPPORT;
    return false;
  }
  unsigned int value = (unsigned int)LoadFieldInt(holder, "address");
  snprintf(host, capacity, "%u.%u.%u.%u",
           value >> 24, (value >> 16) & 0xff,
           (value >> 8) & 0xff, value & 0xff);
  return true;
}

// A new instance of a JDK class, constructed with the given arguments (args[0] is filled in), held by a handle the
// caller drops; null when an exception is pending. The class is initialized first, as the bytecode `new` does, and
// the object is kept in a handle while its constructor runs Java code (Shiklo fork, 2026-10-30: both were missing, so
// the first Inet4Address was too small and a collection during a constructor left a stale pointer).
static classdesc *initialized_class(vm_thread *thread, slice class_name) {
  classdesc *type = bootstrap_lookup_class(thread, class_name);
  if (!type)
    return nullptr;
  initialize_class_t init = {.args = {thread, type}};
  thread->stack.synchronous_depth++;
  future_t initialized = initialize_class(&init);
  thread->stack.synchronous_depth--;
  CHECK(initialized.status == FUTURE_READY);
  return thread->current_exception ? nullptr : type;
}

static handle *construct(vm_thread *thread, slice class_name, slice descriptor, stack_value *args) {
  classdesc *type = initialized_class(thread, class_name);
  if (!type)
    return nullptr;
  handle *object = make_handle(thread, new_object(thread, type));
  if (!object->obj) {
    drop_handle(thread, object);
    return nullptr;
  }
  args[0].obj = object->obj;
  cp_method *constructor = method_lookup(type, STR("<init>"), descriptor, true, false);
  call_interpreter_synchronous(thread, constructor, args);
  if (thread->current_exception) {
    drop_handle(thread, object);
    return nullptr;
  }
  return object;
}

static handle *make_inet4_address(
    vm_thread *thread, char const *host) {
  unsigned int a, b, c, d;
  char trailing;
  if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4 ||
      a > 255 || b > 255 || c > 255 || d > 255) {
    errno = EAFNOSUPPORT;
    return nullptr;
  }
  int value = (int)((a << 24) | (b << 16) | (c << 8) | d);
  return construct(thread, STR("java/net/Inet4Address"), STR("(Ljava/lang/String;I)V"),
                   (stack_value[]){{.obj = nullptr}, {.obj = nullptr}, {.i = value}});
}

// The address as a native's return value (null if it could not be made).
static stack_value inet4_address_value(vm_thread *thread, char const *host) {
  handle *address = make_inet4_address(thread, host);
  if (!address)
    return value_null();
  obj_header *result = address->obj;
  drop_handle(thread, address);
  return (stack_value){.obj = result};
}

static obj_header *make_inet_socket_address(
    vm_thread *thread, char const *host, int port) {
  handle *address = make_inet4_address(thread, host);
  if (!address)
    return nullptr;
  obj_header *result = nullptr;
  classdesc *type = initialized_class(thread, STR("java/net/InetSocketAddress"));
  if (type) {
    handle *socket_address = make_handle(thread, new_object(thread, type));
    if (socket_address->obj) {
      cp_method *constructor = method_lookup(type, STR("<init>"), STR("(Ljava/net/InetAddress;I)V"), true, false);
      call_interpreter_synchronous(thread, constructor,
                                   (stack_value[]){{.obj = socket_address->obj}, {.obj = address->obj}, {.i = port}});
      if (!thread->current_exception)
        result = socket_address->obj;
    }
    drop_handle(thread, socket_address);
  }
  drop_handle(thread, address);
  return result;
}

static bool socket_address(
    vm_thread *thread, handle *descriptor, bool peer,
    char *host, size_t host_capacity, int *port) {
  int fd = file_descriptor_value(descriptor);
  if (fd < 0 ||
      tracejvm_host_socket_address(
          fd, peer, host, host_capacity, port) != 0) {
    int error = fd < 0 ? EBADF : errno;
    raise_socket_exception(
        thread, peer ? "getpeername" : "getsockname", error);
    return false;
  }
  return true;
}

DECLARE_NATIVE("java/net", InetAddress, init, "()V") {
  return value_null();
}

DECLARE_NATIVE("java/net", InetAddress, isIPv4Available, "()Z") {
  return (stack_value){.i = true};
}

DECLARE_NATIVE("java/net", InetAddress, isIPv6Supported, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("java/net", Inet4Address, init, "()V") {
  return value_null();
}

DECLARE_NATIVE("java/net", Inet6Address, init, "()V") {
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, initIDs, "()V") {
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, isIPv6Available0, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("sun/nio/ch", Net, isReusePortAvailable0, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("sun/nio/ch", Net, isExclusiveBindAvailable, "()I") {
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/ch", Net, shouldSetBothIPv4AndIPv6Options0, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("sun/nio/ch", Net, canIPv6SocketJoinIPv4Group0, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("sun/nio/ch", Net, canJoin6WithIPv4Group0, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("sun/nio/ch", Net, canUseIPv6OptionsWithIPv4LocalAddress0, "()Z") {
  return (stack_value){.i = false};
}

DECLARE_NATIVE("sun/nio/ch", Net, socket0, "(ZZZZ)I") {
  if (args[0].i || !args[1].i) {
    raise_socket_exception(thread, "socket", EAFNOSUPPORT);
    return value_null();
  }
  int fd = tracejvm_host_socket();
  if (fd < 0) {
    raise_socket_exception(thread, "socket", errno);
    return value_null();
  }
  return (stack_value){.i = fd};
}

DECLARE_NATIVE("sun/nio/ch", Net, bind0,
               "(Ljava/io/FileDescriptor;ZZLjava/net/InetAddress;I)V") {
  int fd = file_descriptor_value(args[0].handle);
  char host[16];
  int bound_port;
  if (fd < 0 || !args[3].handle ||
      !inet4_host(args[3].handle->obj, host, sizeof(host)) ||
      tracejvm_host_bind(fd, host, args[4].i, &bound_port) != 0) {
    int error = fd < 0 ? EBADF : errno;
    raise_socket_exception(thread, "bind", error);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, listen,
               "(Ljava/io/FileDescriptor;I)V") {
  int fd = file_descriptor_value(args[0].handle);
  if (fd < 0 || tracejvm_host_listen(fd, args[1].i) != 0) {
    raise_socket_exception(thread, "listen", fd < 0 ? EBADF : errno);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, connect0,
               "(ZLjava/io/FileDescriptor;Ljava/net/InetAddress;I)I") {
  int fd = file_descriptor_value(args[1].handle);
  char host[16];
  if (fd < 0 || !args[2].handle ||
      !inet4_host(args[2].handle->obj, host, sizeof(host)) ||
      tracejvm_host_connect(fd, host, args[3].i) != 0) {
    int error = fd < 0 ? EBADF : errno;
    if (error == EINPROGRESS || error == EAGAIN)
      return (stack_value){.i = JDK_IOS_UNAVAILABLE};
    raise_socket_exception(thread, "connect", error);
    return value_null();
  }
  return (stack_value){.i = 1};
}

DECLARE_ASYNC_NATIVE(
    "sun/nio/ch", Net, accept,
    "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;"
    "[Ljava/net/InetSocketAddress;)I",
    locals(
      int call_id;
      int fd;
      int accepted;
      int port;
      char host[16];
      rr_wakeup_info wakeup_info;
    ),
    invoked_methods()) {
  DEBUG_PEDANTIC_YIELD(self->wakeup_info);

  self->fd = file_descriptor_value(args[0].handle);
  if (self->fd < 0) {
    raise_socket_exception(thread, "accept", EBADF);
    ASYNC_RETURN(value_null());
  }
  if (!args[1].handle || !args[1].handle->obj) {
    raise_socket_exception(thread, "accept", EINVAL);
    ASYNC_RETURN(value_null());
  }

  if (!tracejvm_host_is_remote_fd(self->fd)) {
    self->accepted = tracejvm_host_accept(
        self->fd, self->host, sizeof(self->host), &self->port);
  } else {
    if (self->call_id == 0) {
      self->call_id = tracejvm_host_accept_begin(self->fd);
      if (self->call_id < 0) {
        raise_socket_exception(thread, "accept", errno);
        ASYNC_RETURN(value_null());
      }
    }
    while (true) {
      self->accepted = tracejvm_host_accept_poll(
          self->call_id, self->host, sizeof(self->host), &self->port);
      if (self->accepted != 0)
        break;
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + 1000;
      ASYNC_YIELD((void *)&self->wakeup_info);
      DEBUG_PEDANTIC_YIELD(self->wakeup_info);
    }
  }

  if (self->accepted < 0) {
    int error = self->fd < 0 ? EBADF : errno;
    if (error == EAGAIN)
      ASYNC_RETURN((stack_value){.i = JDK_IOS_UNAVAILABLE});
    raise_socket_exception(thread, "accept", error);
    ASYNC_RETURN(value_null());
  }
  StoreFieldInt(args[1].handle->obj, "fd", self->accepted);
  if (args[2].handle && args[2].handle->obj &&
      ArrayLength(args[2].handle->obj) > 0) {
    obj_header *address =
        make_inet_socket_address(thread, self->host, self->port);
    if (!address) {
      tracejvm_host_close(self->accepted);
      StoreFieldInt(args[1].handle->obj, "fd", -1);
      if (!thread->current_exception)
        raise_socket_exception(thread, "accept", errno);
      ASYNC_RETURN(value_null());
    }
    ReferenceArrayStore(args[2].handle->obj, 0, address);
  }
  ASYNC_END((stack_value){.i = 1});
}

DECLARE_NATIVE("sun/nio/ch", Net, shutdown,
               "(Ljava/io/FileDescriptor;I)V") {
  int fd = file_descriptor_value(args[0].handle);
  if (fd < 0 || args[1].i < 0 || args[1].i > 2 ||
      tracejvm_host_shutdown(fd, args[1].i) != 0) {
    raise_socket_exception(thread, "shutdown", fd < 0 ? EBADF : errno);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, localPort,
               "(Ljava/io/FileDescriptor;)I") {
  char host[16];
  int port = 0;
  if (!socket_address(
          thread, args[0].handle, false, host, sizeof(host), &port))
    return value_null();
  return (stack_value){.i = port};
}

DECLARE_NATIVE("sun/nio/ch", Net, localInetAddress,
               "(Ljava/io/FileDescriptor;)Ljava/net/InetAddress;") {
  char host[16];
  int port = 0;
  if (!socket_address(
          thread, args[0].handle, false, host, sizeof(host), &port))
    return value_null();
  return inet4_address_value(thread, host);
}

DECLARE_NATIVE("sun/nio/ch", Net, remotePort,
               "(Ljava/io/FileDescriptor;)I") {
  char host[16];
  int port = 0;
  if (!socket_address(
          thread, args[0].handle, true, host, sizeof(host), &port))
    return value_null();
  return (stack_value){.i = port};
}

DECLARE_NATIVE("sun/nio/ch", Net, remoteInetAddress,
               "(Ljava/io/FileDescriptor;)Ljava/net/InetAddress;") {
  char host[16];
  int port = 0;
  if (!socket_address(
          thread, args[0].handle, true, host, sizeof(host), &port))
    return value_null();
  return inet4_address_value(thread, host);
}

DECLARE_NATIVE("sun/nio/ch", Net, getIntOption0,
               "(Ljava/io/FileDescriptor;ZII)I") {
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/ch", Net, setIntOption0,
               "(Ljava/io/FileDescriptor;ZIIIZ)V") {
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, poll,
               "(Ljava/io/FileDescriptor;IJ)I") {
  int fd = file_descriptor_value(args[0].handle);
  int events = args[1].i;
  long long requested_timeout = args[2].l;
  int timeout = requested_timeout < 0
      ? -1
      : requested_timeout > INT_MAX ? INT_MAX : (int)requested_timeout;
  int readiness = fd < 0
      ? -1
      : tracejvm_host_poll(
          fd, (events & JDK_POLLIN) != 0,
          (events & JDK_POLLOUT) != 0, timeout);
  if (readiness < 0) {
    raise_socket_exception(thread, "poll", fd < 0 ? EBADF : errno);
    return value_null();
  }
  int result = 0;
  if (readiness & 1) result |= JDK_POLLIN;
  if (readiness & 2) result |= JDK_POLLOUT;
  if (readiness & 4) result |= JDK_POLLERR;
  if (readiness & 8) result |= JDK_POLLHUP;
  if (readiness & 16) result |= JDK_POLLNVAL;
  return (stack_value){.i = result};
}

DECLARE_NATIVE("sun/nio/ch", Net, pollConnect,
               "(Ljava/io/FileDescriptor;J)Z") {
  int fd = file_descriptor_value(args[0].handle);
  int readiness =
      fd < 0 ? -1 : tracejvm_host_poll(fd, false, true, (int)args[1].l);
  if (readiness < 0) {
    raise_socket_exception(thread, "pollConnect", fd < 0 ? EBADF : errno);
    return value_null();
  }
  return (stack_value){.i = (readiness & (2 | 4 | 8)) != 0};
}

DECLARE_NATIVE("sun/nio/ch", Net, available,
               "(Ljava/io/FileDescriptor;)I") {
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/ch", Net, pollinValue, "()S") {
  return (stack_value){.i = JDK_POLLIN};
}

DECLARE_NATIVE("sun/nio/ch", Net, polloutValue, "()S") {
  return (stack_value){.i = JDK_POLLOUT};
}

DECLARE_NATIVE("sun/nio/ch", Net, pollerrValue, "()S") {
  return (stack_value){.i = JDK_POLLERR};
}

DECLARE_NATIVE("sun/nio/ch", Net, pollhupValue, "()S") {
  return (stack_value){.i = JDK_POLLHUP};
}

DECLARE_NATIVE("sun/nio/ch", Net, pollnvalValue, "()S") {
  return (stack_value){.i = JDK_POLLNVAL};
}

DECLARE_NATIVE("sun/nio/ch", Net, pollconnValue, "()S") {
  return (stack_value){.i = JDK_POLLOUT};
}

DECLARE_ASYNC_NATIVE(
    "sun/nio/ch", SocketDispatcher, read0,
    "(Ljava/io/FileDescriptor;JI)I",
    locals(
      int call_id;
      int fd;
      ssize_t result;
      rr_wakeup_info wakeup_info;
    ),
    invoked_methods()) {
  DEBUG_PEDANTIC_YIELD(self->wakeup_info);

  self->fd = file_descriptor_value(args[0].handle);
  if (self->fd < 0 || args[2].i < 0) {
    raise_socket_exception(
        thread, "read", self->fd < 0 ? EBADF : EINVAL);
    ASYNC_RETURN(value_null());
  }
  if (args[2].i == 0)
    ASYNC_RETURN((stack_value){.i = 0});

  if (!tracejvm_host_routes_fd(self->fd)) {
    self->result = tracejvm_host_read(
        self->fd, (void *)args[1].l, (size_t)args[2].i, 0, false);
  } else {
    if (self->call_id == 0) {
      self->call_id =
          tracejvm_host_read_begin(self->fd, (size_t)args[2].i);
      if (self->call_id < 0) {
        raise_socket_exception(thread, "read", errno);
        ASYNC_RETURN(value_null());
      }
    }
    while (true) {
      self->result = tracejvm_host_read_poll(
          self->call_id, (void *)args[1].l, (size_t)args[2].i);
      if (self->result != TRACEJVM_HOST_ASYNC_PENDING)
        break;
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + 1000;
      ASYNC_YIELD((void *)&self->wakeup_info);
      DEBUG_PEDANTIC_YIELD(self->wakeup_info);
    }
  }

  if (self->result < 0) {
    if (errno == EAGAIN)
      ASYNC_RETURN((stack_value){.i = JDK_IOS_UNAVAILABLE});
    raise_socket_exception(
        thread, "read", self->fd < 0 ? EBADF : errno);
    ASYNC_RETURN(value_null());
  }
  ASYNC_END((stack_value){
      .i = self->result == 0 ? -1 : (int)self->result});
}

DECLARE_NATIVE("sun/nio/ch", SocketDispatcher, write0,
               "(Ljava/io/FileDescriptor;JI)I") {
  int fd = file_descriptor_value(args[0].handle);
  ssize_t result = fd < 0
      ? -1
      : tracejvm_host_write(
          fd, (void const *)args[1].l, (size_t)args[2].i, 0, false);
  if (result < 0) {
    if (errno == EAGAIN)
      return (stack_value){.i = JDK_IOS_UNAVAILABLE};
    raise_socket_exception(thread, "write", fd < 0 ? EBADF : errno);
    return value_null();
  }
  return (stack_value){.i = (int)result};
}

// A gathering write (Shiklo fork, 2026-10-30; HttpClient's requests failed with UnsatisfiedLinkError): the buffers
// one after another until one is not written whole. address holds len entries laid out as IOVecWrapper writes them,
// a base and a length of Unsafe.addressSize bytes each.
typedef struct {
  void const *base;
  size_t length;
} jdk_iovec;

DECLARE_NATIVE("sun/nio/ch", SocketDispatcher, writev0,
               "(Ljava/io/FileDescriptor;JI)J") {
  int fd = file_descriptor_value(args[0].handle);
  jdk_iovec const *buffers = (jdk_iovec const *)args[1].l;
  s64 total = 0;
  for (int i = 0; i < args[2].i; ++i) {
    if (buffers[i].length == 0)
      continue;
    ssize_t result = fd < 0
        ? -1
        : tracejvm_host_write(fd, buffers[i].base, buffers[i].length, 0, false);
    if (result < 0) {
      if (total > 0)
        break;
      if (errno == EAGAIN)
        return (stack_value){.l = JDK_IOS_UNAVAILABLE};
      raise_socket_exception(thread, "write", fd < 0 ? EBADF : errno);
      return value_null();
    }
    total += result;
    if ((size_t)result < buffers[i].length)
      break;
  }
  return (stack_value){.l = total};
}

DECLARE_NATIVE("sun/nio/ch", UnixDispatcher, init, "()V") {
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixDispatcher, close0,
               "(Ljava/io/FileDescriptor;)V") {
  int fd = file_descriptor_value(args[0].handle);
  if (fd >= 0 && tracejvm_host_close(fd) != 0) {
    raise_socket_exception(thread, "close", errno);
  } else if (args[0].handle && args[0].handle->obj) {
    StoreFieldInt(args[0].handle->obj, "fd", -1);
  }
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixDispatcher, preClose0,
               "(Ljava/io/FileDescriptor;)V") {
  return value_null();
}
