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
  that prints a thread dump after 3 s), P12l (the main thread parks, another thread unparks it) and P14 (stack-trace
  format).
- 2026-10-28: javac's constant folding uses the JDK's floating point (bug 7; `compiler/teavm-javac/patches/0004`,
  overlay version 4). javac runs on TeaVM 0.13.1, which compiles `-x` as `0.0 - x`
  (`BaseWasmGenerationVisitor`, NEGATE), so a folded `-0.0` / `-0.0f` became `+0.0`; and javac formatted folded string
  constants with TeaVM's `Double.toString` (`"" + Double.MIN_VALUE` gave `4.940656458412465E-324`). The patch makes
  `javac/build.gradle` edit the unpacked OpenJDK sources: `ConstFold` negates by flipping the sign bit, and
  `Type.stringValue()` formats doubles and floats with the JDK's `jdk.internal.math` conversion, copied into javac as
  `com.sun.tools.javac.util.fp` (with `multiplyHigh` and a Latin-1 string helper, which TeaVM's class library lacks).
  Checked on a desktop JDK: the copied conversion gives the same text as `Double.toString` / `Float.toString` for
  800,039 values (edge cases, random bit patterns, subnormals).
- 2026-10-28: the scheduler sets a thread's `eetop` again when it queues a call on it (`vm/roundrobin_scheduler.c`).
  `free_thread_info` cleared it when the host's previous call on the main thread ended, and the host makes two calls
  per program on a reused VM, so from the second program on `unpark(main)` was dropped: an untimed `Future.get` or
  `Condition.await` on the main thread never woke (bug 12; timed waits returned at their timeout) and
  `Thread.currentThread().isAlive()` was false.
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
  `java -ea` is for the launcher (`ClassLoader.setDefaultAssertionStatus`).
- 2026-10-28: `Class.getModifiers()` of a member class comes from its InnerClasses entry (so `static`, `private`,
  `protected` show), and ACC_SUPER is no longer reported (`natives/share/java/lang/Class.c`).
