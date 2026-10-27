# About this fork

This is Shiklo's fork of [TraceJVM](https://github.com/tracecodeapp/tracejvm), used by the Java editor and tutorial
on code.shiklo.com. It stays under TraceJVM's licence (AGPL-3.0-only; third-party parts under their own licences, see
THIRD_PARTY_NOTICES.md).

- Every runtime served on code.shiklo.com is built by `.github/workflows/build-runtime.yml` and published as a
  release of this repository, tagged with the commit it was built from: that commit is its corresponding source.
- Changes made in this fork (fixes to the VM, its natives, the compiler build and the runtime images) are listed
  below with their date. Fixes that help everyone are offered back to TraceJVM.

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
- 2026-10-28: the runtime build workflow puts the repository's TypeScript first on PATH (emcc's `--emit-tsd`).
- 2026-10-28: assertions are off by default, as in the JDK (`Class.desiredAssertionStatus0` returned true);
  `java -ea` is for the launcher (`ClassLoader.setDefaultAssertionStatus`).
- 2026-10-28: `Class.getModifiers()` of a member class comes from its InnerClasses entry (so `static`, `private`,
  `protected` show), and ACC_SUPER is no longer reported (`natives/share/java/lang/Class.c`).
