# About this fork

This is Shiklo's fork of [TraceJVM](https://github.com/tracecodeapp/tracejvm), used by the Java editor and tutorial
on code.shiklo.com. It stays under TraceJVM's licence (AGPL-3.0-only; third-party parts under their own licences, see
THIRD_PARTY_NOTICES.md).

- Every runtime served on code.shiklo.com is built by `.github/workflows/build-runtime.yml` and published as a
  release of this repository, tagged with the commit it was built from: that commit is its corresponding source.
- Changes made in this fork (fixes to the VM, its natives, the compiler build and the runtime images) are listed
  below with their date. Fixes that help everyone are offered back to TraceJVM.

- `shiklo/checklist/` is the Java checklist of code.shiklo.com (one program per row of the full-language checklist,
  compared with the exact output of a real JDK) with the page's engine glue; each build runs it and adds
  `checklist-report.txt` to its release.

## Changes

- 2026-10-28: added the runtime build workflow and this file.
- 2026-10-28: wall clock — `System.currentTimeMillis()` read the worker's monotonic clock (time since the worker
  started, so programs saw 1970) and `VM.getNanoTimeAdjustment` returned 0 (`Instant.now()` never moved); both now
  read the time of day (`Date.now()`), `System.nanoTime()` stays monotonic
  (`natives/share/java/lang/System.c`, `natives/share/sun/misc/VM.c`).
- 2026-10-28: `scripts/bootstrap-toolchain.mjs` puts the pinned JDK 23 first on PATH (the build host's own Java was
  used by `build-runtime.sh` before).
- 2026-10-28: NIO reads return IOStatus.EOF (-1) at the end of a file, as the JDK (`Files.readAllLines`,
  `Files.newBufferedReader`, `new Scanner(File)` failed); `FileInputStream.length0` / `position0` added
  (`readAllBytes`, `readNBytes`, `skip`, `transferTo`) (`natives/share/sun/nio/fs/UnixNativeDispatcher.c`,
  `natives/share/java/io/FileInputStream.c`).
- 2026-10-28: an exception that ends `Thread.run` of a started thread is handed to
  `Thread.dispatchUncaughtException` on that thread before it ends (as HotSpot's `JavaThread::exit`), so uncaught
  exception handlers run and the default one prints `Exception in thread "..."`; it was dropped silently
  (`vm/roundrobin_scheduler.c`).
- 2026-10-28: TeaVM javac compiles against a java.base descriptor made for each runtime profile
  (`scripts/java/PlatformDescriptor.java`, used by `build-runtime.sh`): it exports what the profile's real modules
  export. Before, javac used the TeaVM class library's descriptor, so `java.util.concurrent.locks`, `java.sql`,
  `javax.xml` and the other profile modules did not compile although their classes are in the platform jar.
- 2026-10-28: probes P7b (is the `-0.0` constant in the class file javac wrote?), P7c / P7d (javac's floating point:
  decimal and hexadecimal literals, folded string constants, folded `-0.0`), P12k (the hanging pool with a watchdog
  that prints a thread dump after 3 s), P12l (the main thread parks, another thread unparks it), P14 (stack-trace
  format) and P15a-d (every way a program ends itself).
- 2026-10-28: javac's floating point is the JDK's (bug 7; `compiler/teavm-javac/patches/0004`, overlay version 4).
  javac runs on TeaVM 0.13.1, which compiles `-x` as `0.0 - x` (`BaseWasmGenerationVisitor`, NEGATE), so a folded
  `-0.0` / `-0.0f` became `+0.0`; javac formatted folded string constants with TeaVM's `Double.toString`
  (`"" + Double.MIN_VALUE` gave `4.940656458412465E-324`); and it read literals with TeaVM's `Double.valueOf` /
  `Float.valueOf`, which give 0 for subnormals (`4.9E-324`, `1.4E-45f`: "floating-point number too small") and fail on
  hexadecimal ones, which javac then compiled as NaN (`0x1.8p1`; seen on builds 8 and 15, probes P7c-d). The patch
  makes `javac/build.gradle` edit the unpacked OpenJDK sources: `ConstFold` negates by flipping the sign bit,
  `Type.stringValue()` formats with the JDK's DoubleToDecimal / FloatToDecimal, and `JavacParser` reads literals with
  the JDK's FloatingDecimal, all copied from `jdk.internal.math` into javac as `com.sun.tools.javac.util.fp` (with
  `multiplyHigh`, a Latin-1 string helper and a float clamp, which TeaVM's class library lacks, and without CDS).
  Checked on a desktop JDK: the copies give the JDK's results in 1,150,287 checks (formatting of edge cases, random
  bit patterns and subnormals; parsing of decimal, hexadecimal, subnormal, overflowing and random literals).
- 2026-10-28: the scheduler sets a thread's `eetop` again when it queues a call on it (`vm/roundrobin_scheduler.c`).
  `free_thread_info` cleared it when the host's previous call on the main thread ended, and the host makes two calls
  per program on a reused VM, so from the second program on `unpark(main)` was dropped: an untimed `Future.get` or
  `Condition.await` on the main thread never woke (bug 12; timed waits returned at their timeout) and
  `Thread.currentThread().isAlive()` was false. Seen on build 15: the second program of a VM printed `alive false`
  and its `parkNanos(3 s)` ran the full 3 s although another thread unparked it after 200 ms.
- 2026-10-28: `java.util.zip` natives completed (`natives/share/java/util/zip/`): Deflater (it had none, so
  `ZipOutputStream`, `GZIPOutputStream`, `JarOutputStream` and the jar tool failed with UnsatisfiedLinkError), Adler32,
  `CRC32.update(int)` / `updateByteBuffer0`, and Inflater's direct-buffer variants, dictionaries and `getAdler`; a
  corrupt stream throws DataFormatException with zlib's message (it was reported as finished). Written from the JDK
  23 classes' contract (OpenJDK's C sources are GPLv2-only); Deflater streams are freed with `deflateEnd` at VM exit
  (`vm->z_deflate_streams`). Probes P16a-b.
- 2026-10-28: default methods are selected as JVMS 5.4.6 says (`vm/vtable.c`): of the maximally-specific
  superinterface methods, the one that is not abstract. Any two different defaults for one method counted as
  ambiguous, so a class extending AbstractCollection and implementing List (javac's own List, which jshell iterates)
  had no `spliterator()`: AbstractMethodError, although List's default is more specific than Collection's. Probe P17.
- 2026-10-29 (after build 17: checklist 37/38, probes 35/35): JDK tools for the Shiklo Java page's terminal. The
  release carries `tools/jdk-tools.jar` (jar, javap, javac's API) and `tools/jdk-jshell.jar` (jshell, line editing,
  JDI, markdown doc comments), class-path jars made from this JDK's jmods by `shiklo/tools/MakeToolJar.java`
  (`build-runtime.sh`): each module's `provides` as META-INF/services, and `shiklo/tools/src/javax/tools/ToolProvider`
  in place of the JDK's, which takes tools only from the jdk.compiler module. Reproducible (fixed entry times). On
  build 17 javap and jar work as on a desktop JDK; jshell started, compiled, and ran no snippet (next item).
- 2026-10-29: the Unsafe natives HotSpot has and the VM lacked (`natives/share/jdk/internal/misc/Unsafe.c`): char and
  float accessors, the volatile accessors of boolean, byte, char, short, float and double, compareAndExchangeInt,
  throwException, reallocateMemory0, copySwapMemory0. `Field.set` on a volatile boolean threw UnsatisfiedLinkError
  (jshell sets one before every snippet, and reported nothing), as did VarHandles on such fields and byte-order
  bulk copies of buffers. Probe P20.
- 2026-10-29: a failed file operation's UnixException carries its error number (`UnixNativeDispatcher.c`), so the JDK
  throws DirectoryNotEmptyException, NoSuchFileException, FileAlreadyExistsException … (it carried only a message, and
  every such error was a plain IOException: checklist B6b on build 17).
- 2026-10-29: the main thread runs in a "main" thread group inside "system", as in HotSpot (it was in "system").
  Probe P21.
- 2026-10-29 (after build 18): the `java.io.File` natives the VM lacked (`natives/share/java/io/UnixFileSystem.c`):
  `list0` (`File.list`, `listFiles`), `rename0` (`renameTo`), `getSpace0` (1 GiB, 768 MiB free, as statvfs0), and
  `setPermission0`, `setLastModifiedTime0`, `setReadOnly0`, which report failure (false) as the JDK does when it
  cannot make the change: the page's file system keeps neither permissions nor settable times. All threw
  UnsatisfiedLinkError.
- 2026-10-29: `invokevirtual` of a VarHandle access method uses `MethodHandles.varHandleInvoker` with the call
  site's type (`vm/bjvm.c`): the exact invoker was called without adapting to the call site, so a char field read into
  an Object (`"" + vh.getVolatile(o)`) came back as a raw char: AbstractMethodError.
- 2026-10-30: `ClassLoader.retrieveDirectives` (`natives/share/java/lang/ClassLoader.c`): no command-line assertion
  switches, the loader's default off. It was missing, so `setDefaultAssertionStatus` (how the Shiklo page's launcher
  honours `java -ea`), `setClassAssertionStatus` and `setPackageAssertionStatus` threw UnsatisfiedLinkError.
- 2026-10-30: after a user-defined loader's `loadClass` returns a class, only that loader is recorded as an initiating
  loader (JVMS 5.3.2; `vm/bjvm.c` `lookup_class`). Every loader from it up its parents to the class's defining loader
  was recorded, but `loadClass` may return a class from a loader that is not among its parents (a loader whose parent
  is the platform loader handing out a class of another application loader): the walk went past the bootstrap loader
  into a null loader and the VM stopped with "remainder by zero".
- 2026-10-30: StackWalker (`natives/share/java/lang/StackStreamFactory.c`): `callStackWalk`, `fetchStackFrames` and
  `setContinuation` were missing, so every StackWalker threw UnsatisfiedLinkError, and with it every
  `java.util.logging` call (a LogRecord finds its caller with one). As HotSpot's stackwalk.cpp: the walk starts below
  the walker's own frames, leaves out hidden frames unless asked, fills the Java side's buffer and calls back
  `doStackWalk`; a StackFrameInfo is filled whole (ResolvedMethodName, modifiers, bci, name, descriptor and its
  StackTraceElement, made by `make_stack_trace_element`, now shared with Throwable).
- 2026-10-30: `FileOutputStream.write(int)` (one byte; a DataOutputStream or ZipOutputStream straight on a file threw
  UnsatisfiedLinkError), and FileNotFoundException with the system's reason, "notes.txt (No such file or directory)"
  (`natives/share/java/io/`, `file-errors.h`).
- 2026-10-30: `Array.multiNewArray` (`Array.newInstance(int.class, 2, 3)` threw UnsatisfiedLinkError).
- 2026-10-30: array sizes are checked in 64 bits (`vm/arrays.c`): `new long[Integer.MAX_VALUE]` wrapped around to a
  small array whose zeroing wrote past it ("memory access out of bounds"); now OutOfMemoryError "Requested array size
  exceeds VM limit", or "Java heap space" beyond the heap, which the preallocated error now says too (`vm/bjvm.c`).
- 2026-10-30: a class whose static initializer failed is erroneous (JVMS 5.5): it was marked initialized and usable
  after its ExceptionInInitializerError; later uses throw NoClassDefFoundError "Could not initialize class X"
  (`vm/bjvm.c` `initialize_class`, `classdesc.initialization_failed`).
- 2026-10-30: HotSpot's exception texts: helpful NullPointerExceptions name `String` and `Object` without their
  package, write `because the return value of "X.m()" is null`, follow casts (`Map.get` unboxed into an int) and
  number parameters from 1 (`vm/extended_npe_message.c`); ClassCastException gives the classes' modules and loaders
  ("class java.lang.Integer cannot be cast to class java.lang.String (java.lang.Integer and java.lang.String are in
  module java.base of loader 'bootstrap')"; the run's and the Shiklo launcher's loaders show as 'app') (`vm/exceptions.c`).
- 2026-10-30: name lookup and socket addresses initialize `Inet4Address` / `InetSocketAddress` before making one and
  keep the objects in handles while their constructors run (`natives/share/java/net/InetAddressImpl.c`,
  `natives/share/sun/nio/ch/Net.c`): on a program's first lookup the address object was too small and its
  constructor wrote over the next allocation, so `InetAddress.getAllByName("localhost")` threw ArrayStoreException and
  HttpServer / HttpClient could not start.
- 2026-10-30: the runtime images carry the JCE jurisdiction policy that `crypto.policy=unlimited` names
  (`conf/security/policy/unlimited/*.policy`; `build-runtime.sh`, `src/engine.ts`, `verify-runtime-assets.mjs`):
  javax.crypto could not start ("Can't read cryptographic policy directory"), so Cipher, Mac and KeyGenerator failed.
- 2026-10-30: `Math.exp`, `log` and `log10` are interpreter intrinsics as `sin` / `pow` (`vm/interpreter2.c`):
  StrictMath's results are 1 ulp from HotSpot's, so `Math.exp(1)` printed 2.7182818284590455.
- 2026-10-30: javac's options and notes (TeaVM javac overlay patch 0005, version 5; `TraceJVMCompileRequest.options`):
  `-Xlint…`, `-g…`, `-parameters`, `--enable-preview` … go through javac's own Arguments (the compiler had no options:
  no warnings could be turned on, no preview feature tried), and the deferred notes ("Note: Main.java uses unchecked
  or unsafe operations.") are reported after compiling, as javac does when it closes.
- 2026-10-30 (after build 21): javac's options are set as `Option.process` does (Arguments needs classes TeaVM lacks),
  `-Xlint:` lists included, and diagnostics are formatted by javac's own rich formatter with the `[lint category]`
  prefix; mandatory warnings report as warnings (patch 0005, regenerated).
- 2026-10-30: a program base (`vm_thread.program_base`, `TraceKernel.markProgramBase`): a launcher that calls a
  program's `main` itself marks its frame, and stack traces, `Thread.getStackTrace` and StackWalker end above it, as
  under a JDK launcher. Native `MethodHandle` frames are hidden like LambdaForms (`is_frame_hidden`, `vm/bjvm.h`).
- 2026-10-30: helpful NullPointerException sources for `checkcast` (`vm/analysis.c`): `int v = map.get(k)` named a
  `<parameter1>` instead of `"java.util.Map.get(Object)"`'s return value.
- 2026-10-30: `java.lang.reflect.Array` get/set of each primitive type (`Array.setInt`, `getDouble` …, declared one
  by one: the native scanner reads source text, not macros) — they threw UnsatisfiedLinkError.
- 2026-10-30: `FileInputStream.available()` on a host-backed descriptor uses the host's fstat and seek (0 for a pipe
  or socket).
- 2026-10-30: the spring-server image adds `java.prefs` and `java.datatransfer` (`java.util.prefs` and the AWT
  clipboard types; javac's descriptor lacked them).
- 2026-10-30 (after build 22): `Reflection.getCallerClass` skips the frames HotSpot skips (`Method.invoke`, the
  reflection accessors, LambdaForms, MethodHandle intrinsics) instead of every `java.lang.invoke` class
  (`natives/share/jdk/internal/reflect/Reflection.c`): `MethodHandles.lookup()` inside `LazyInitializingVarHandle`
  got its caller's lookup, so `SSLContext.getDefault()`, and with it every `HttpClient`, failed with InternalError
  "uncaught exception".
- 2026-10-30: sorting as interpreter intrinsics (`vm/interpreter2.c`): `DualPivotQuicksort.sort` of each primitive
  array type (Java's order: -0.0 before 0.0, NaN last), and `ComparableTimSort.sort` / `TimSort.sort` with natural or
  reverse order when every element is an Integer, a Long or a String (a stable merge sort, as TimSort); any other
  call runs the Java code. Sorting 300,000 Integers took 3 s, 300,000 ints 1.4 s.
- 2026-10-29 (after build 16, 36/38): dynamic constants (`CONSTANT_Dynamic`) as bootstrap arguments (`vm/bjvm.c`,
  `vm/classfile.c`). javac makes them for pattern switches (a record pattern with primitive components under a
  guard: `ConstantBootstraps.primitiveClass`; enum labels: EnumDesc constants); they were never linked to their
  bootstrap method nor resolved (UNREACHABLE, seen as "Unsupported method handle kind"; checklist A6, probes P8d-e).
  Resolved as HotSpot does, through `MethodHandleNatives.linkDynamicConstant`, synchronously (they nest).
- 2026-10-29: `UnixFileSystem.bufferedCopy0` (`Files.copy`; after build 16's statvfs0 the copy reached it: B6b).
- 2026-10-29: thread states: `Thread.holder.threadStatus` is kept as HotSpot does (start, sleep, park, Object.wait,
  monitor enter, end), so `Thread.getState()` is right (every thread was NEW) and starting a thread twice throws
  IllegalThreadStateException. `getStackTrace0` returns the thread's own trace (JDK 23 does not fall back to
  dumpThreads). Probes P18, P19.
- 2026-10-29: the checklist pages end a stopped VM's worker (`shiklo/checklist/java-engine.js`): a program that calls
  System.exit now leaves its VM waiting for ever, and several of them crashed the probe page in build 16's CI run.
- 2026-10-28: `Thread.dumpThreads` and `Thread.getStackTrace0` (`natives/share/java/lang/Thread.c`; both missing:
  `Thread.getAllStackTraces()` and another thread's `getStackTrace()` threw UnsatisfiedLinkError). The element
  building of `Throwable.fillInStackTrace` is now `make_stack_trace_elements`, which walks any thread's frames.
  Probe P18.
- 2026-10-28: `Shutdown.halt0` ends the program (`natives/share/java/lang/Runtime.c`). It returned, so `System.exit`
  from code the page does not rewrite (the JDK's own tools, `Runtime.getRuntime().exit` / `halt`, `System::exit`) went
  on with the program. With a routed error stream it writes the page's exit signal (`ESC ] lmpb-exit;<status> BEL`),
  which the host never answers, so the VM stops there; shutdown hooks have run by then (probes P15a-d).
- 2026-10-28: method-handle constants of kind getStatic, putField and putStatic (JVMS 5.4.3.5) — only getField was
  supported, and the others aborted the VM ("Unsupported method handle kind"), e.g. in a `switch` on a record pattern
  with a `when` guard; field handles resolve the field's class, put kinds make setters (`vm/bjvm.c`).
- 2026-10-28: `raise_vm_exception` initialises an exception class the VM did not initialise at boot (it aborted the
  VM, e.g. for `ConnectException`) (`vm/exceptions.c`); `UnixNativeDispatcher.statvfs0` added (`Files.copy`,
  `FileStore` sizes; the in-memory file system is described as 1 GiB).
- 2026-10-28: stack traces as HotSpot prints them: binary class names with dots, the module
  (`java.base/java.lang.Integer.parseInt`), "Unknown Source" when a class has no SourceFile (it showed the method's
  name), "Native Method", and frames of hidden classes left out (`natives/share/java/lang/Throwable.c`).
- 2026-10-28: `Class.forName` of an array name with a class loader (`"[I"`, `"[Ljava.lang.String;"`) loads the
  element class and builds the array class, as HotSpot does; it asked `loader.loadClass("[I")`, which always fails,
  so deserializing any array field broke the stream (`natives/share/java/lang/Class.c`).
- 2026-10-28: socket errors throw the JDK's classes with the system's text alone: `ConnectException: Connection
  refused`, `NoRouteToHostException`, `BindException`, `ProtocolException`, else `SocketException`
  (`natives/share/sun/nio/ch/Net.c`); `LinuxNativeDispatcher.posix_fadvise` added (a hint; `Files.copy`).
- 2026-10-28: the checklist runner prints the page's and workers' console errors after the report, and a crashed
  run's line carries the worker's message.
- 2026-10-28: `Unsafe.park` consumes the unpark permit as `LockSupport.park` does (and returns at once for an
  interrupted thread); the permit was never consumed, so after a thread's first unpark its waits spun
  (`natives/share/jdk/internal/misc/Unsafe.c`).
- 2026-10-28: `Runtime.totalMemory` / `freeMemory` added and `maxMemory` made real (the heap's capacity and use;
  `maxMemory` was Long.MAX_VALUE), `VM.latestUserDefinedLoader0` added (deserialization),
  `LinuxNativeDispatcher.init` / `directCopy0` added (`Files.copy`), and ArrayIndexOutOfBoundsException says
  "Index 2 out of bounds for length 2" as HotSpot does.
- 2026-10-28: records — the class-file parser reads the Record attribute (`vm/classfile.c`, `vm/classfile.h`), and
  `Class.isRecord0` / `getRecordComponents0` are implemented (`natives/share/java/lang/Class.c`); they were missing,
  so `isRecord()`, `getRecordComponents()` and record serialization threw UnsatisfiedLinkError.
- 2026-10-28: name lookup and network interfaces (`natives/share/java/net/InetAddressImpl.c`, new): "localhost" is
  127.0.0.1, other names are an UnknownHostException (there is no DNS in a browser tab), NetworkInterface reports
  none. The natives were missing, so `InetAddress.getByName("localhost")` aborted the VM and `UUID.randomUUID()` /
  `SecureRandom` failed in `NetworkInterface.init`.
- 2026-10-28: the runtime images carry `jdk.localedata` for en, es, pt, de, fr, ja, hi (jlink `--include-locales`),
  so `Locale.GERMANY`, `Locale.FRANCE` … format numbers, dates and currencies as the JDK does (they fell back to
  English).
- 2026-10-28: the runtime build workflow puts the repository's TypeScript first on PATH (emcc's `--emit-tsd`).
- 2026-10-28: assertions are off by default, as in the JDK (`Class.desiredAssertionStatus0` returned true);
  `java -ea` is for the launcher (`ClassLoader.setDefaultAssertionStatus`, which works from 2026-10-30).
- 2026-10-28: `Class.getModifiers()` of a member class comes from its InnerClasses entry (so `static`, `private`,
  `protected` show), and ACC_SUPER is no longer reported (`natives/share/java/lang/Class.c`).
