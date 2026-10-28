// sun.nio.fs.LinuxNativeDispatcher (Shiklo fork, 2026-10-28). Its class initializer calls init(), which was
// missing, so Files.copy failed with UnsatisfiedLinkError. directCopy0 answers IOStatus.UNSUPPORTED (-4): there is no
// copy_file_range in the browser, and the JDK then copies through its own buffers.
#include <natives-dsl.h>

DECLARE_NATIVE("sun/nio/fs", LinuxNativeDispatcher, init, "()V") { return value_null(); }

DECLARE_NATIVE("sun/nio/fs", LinuxNativeDispatcher, directCopy0, "(IIJ)I") { return (stack_value){.i = -4}; }
