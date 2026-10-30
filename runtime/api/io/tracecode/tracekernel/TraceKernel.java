package io.tracecode.tracekernel;

import java.io.IOException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/**
 * Explicit access to process controls that have no portable Java SE equivalent.
 *
 * <p>Filesystem, descriptor, process, and socket behavior remains available
 * through ordinary Java APIs. This class is intentionally limited to
 * TraceKernel-specific watchdog and Unix process-topology controls.
 */
public final class TraceKernel {
  private static final List<Runnable> WINDOW_SIZE_LISTENERS =
      new ArrayList<>();

  private TraceKernel() {}

  public enum WatchdogSignal {
    SIGTERM(0),
    SIGKILL(1);

    private final int code;

    WatchdogSignal(int code) {
      this.code = code;
    }
  }

  public record WatchdogStatus(
      boolean armed,
      long timeoutMillis,
      long deadlineMillis,
      WatchdogSignal signal) {}

  public record ProcessIdentity(long pid, long parentPid, long processGroupId, long sessionId) {}

  public record SessionIdentity(long sessionId, long processGroupId) {}

  public record TerminalWindowSize(long rows, long columns) {}

  public static ProcessIdentity currentProcess() throws IOException {
    long[] identity = identity0();
    requireLength(identity, 4, "identity");
    return new ProcessIdentity(identity[0], identity[1], identity[2], identity[3]);
  }

  public static WatchdogStatus armWatchdog(
      Duration timeout,
      WatchdogSignal signal) throws IOException {
    Objects.requireNonNull(timeout, "timeout");
    Objects.requireNonNull(signal, "signal");
    long timeoutMillis;
    try {
      timeoutMillis = timeout.toMillis();
    } catch (ArithmeticException error) {
      throw new IllegalArgumentException("Watchdog timeout is too large.", error);
    }
    if (timeoutMillis <= 0) {
      throw new IllegalArgumentException("Watchdog timeout must be positive.");
    }
    return watchdogStatus(watchdog0(1, timeoutMillis, signal.code));
  }

  public static WatchdogStatus petWatchdog() throws IOException {
    return watchdogStatus(watchdog0(2, 0, 0));
  }

  public static WatchdogStatus disarmWatchdog() throws IOException {
    return watchdogStatus(watchdog0(3, 0, 0));
  }

  public static WatchdogStatus watchdogStatus() throws IOException {
    return watchdogStatus(watchdog0(4, 0, 0));
  }

  public static SessionIdentity createSession() throws IOException {
    long[] identity = setsid0();
    requireLength(identity, 2, "setsid");
    return new SessionIdentity(identity[0], identity[1]);
  }

  /**
   * Applies Unix {@code setpgid(2)} semantics. Zero selects the current
   * process for {@code pid} and that process's PID for {@code processGroupId}.
   */
  public static long setProcessGroup(long pid, long processGroupId)
      throws IOException {
    return setpgid0(pid, processGroupId);
  }

  public static long terminalForegroundProcessGroup(int descriptor)
      throws IOException {
    return tcgetpgrp0(descriptor);
  }

  public static long setTerminalForegroundProcessGroup(
      int descriptor,
      long processGroupId) throws IOException {
    return tcsetpgrp0(descriptor, processGroupId);
  }

  public static TerminalWindowSize terminalWindowSize(int descriptor)
      throws IOException {
    return terminalWindowSize(tcgetwinsize0(descriptor));
  }

  public static TerminalWindowSize setTerminalWindowSize(
      int descriptor,
      long rows,
      long columns) throws IOException {
    if (rows <= 0 || columns <= 0) {
      throw new IllegalArgumentException(
          "Terminal rows and columns must be positive.");
    }
    TerminalWindowSize size =
        terminalWindowSize(tcsetwinsize0(descriptor, rows, columns));
    dispatchPendingSignals();
    return size;
  }

  /**
   * Marks (true) or unmarks (false) the calling method as the start of the program on this thread: stack traces and
   * StackWalker end above it, as under a JDK launcher, which has no Java frames below {@code main}. For a launcher
   * that calls a program's {@code main} itself (Shiklo fork, 2026-10-30).
   */
  public static void markProgramBase(boolean mark) {
    markProgramBase0(mark);
  }

  public static synchronized void addWindowSizeListener(Runnable listener) {
    WINDOW_SIZE_LISTENERS.add(Objects.requireNonNull(listener, "listener"));
  }

  public static synchronized boolean removeWindowSizeListener(
      Runnable listener) {
    return WINDOW_SIZE_LISTENERS.remove(
        Objects.requireNonNull(listener, "listener"));
  }

  private static void dispatchPendingSignals() throws IOException {
    if (pollSignal0() != 28) {
      return;
    }
    List<Runnable> listeners;
    synchronized (TraceKernel.class) {
      listeners = List.copyOf(WINDOW_SIZE_LISTENERS);
    }
    for (Runnable listener : listeners) {
      listener.run();
    }
  }

  private static TerminalWindowSize terminalWindowSize(long[] values)
      throws IOException {
    requireLength(values, 2, "terminal window size");
    if (values[0] <= 0 || values[1] <= 0) {
      throw new IOException(
          "TraceKernel returned an invalid terminal window size.");
    }
    return new TerminalWindowSize(values[0], values[1]);
  }

  private static WatchdogStatus watchdogStatus(long[] values)
      throws IOException {
    requireLength(values, 4, "watchdog");
    if (values[0] == 0) {
      return new WatchdogStatus(false, 0, 0, null);
    }
    WatchdogSignal signal = switch ((int) values[3]) {
      case 0 -> WatchdogSignal.SIGTERM;
      case 1 -> WatchdogSignal.SIGKILL;
      default -> throw new IOException(
          "TraceKernel returned an invalid watchdog signal.");
    };
    return new WatchdogStatus(true, values[1], values[2], signal);
  }

  private static void requireLength(long[] values, int length, String operation)
      throws IOException {
    if (values == null || values.length != length) {
      throw new IOException(
          "TraceKernel returned an invalid " + operation + " response.");
    }
  }

  private static native long[] identity0() throws IOException;

  private static native void markProgramBase0(boolean mark);

  private static native long[] watchdog0(
      int action,
      long timeoutMillis,
      int signal) throws IOException;

  private static native long[] setsid0() throws IOException;

  private static native long setpgid0(long pid, long processGroupId)
      throws IOException;

  private static native long tcgetpgrp0(int descriptor) throws IOException;

  private static native long tcsetpgrp0(int descriptor, long processGroupId)
      throws IOException;

  private static native long[] tcgetwinsize0(int descriptor)
      throws IOException;

  private static native long[] tcsetwinsize0(
      int descriptor,
      long rows,
      long columns) throws IOException;

  private static native int pollSignal0() throws IOException;
}
