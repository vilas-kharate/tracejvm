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
  `java -ea` is for the launcher (`ClassLoader.setDefaultAssertionStatus`).
- 2026-10-28: `Class.getModifiers()` of a member class comes from its InnerClasses entry (so `static`, `private`,
  `protected` show), and ACC_SUPER is no longer reported (`natives/share/java/lang/Class.c`).
