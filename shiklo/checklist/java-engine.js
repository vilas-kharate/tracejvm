// Java engine for the editor (PoC, to become the tool's java.js): TraceJVM 0.4.1 (AGPL-3.0; OpenJDK 23 javac compiled
// with TeaVM + a b-jvm VM with the real OpenJDK 23 class library) with what this site adds around it:
// - a launcher `$.Run$` compiled with every program: runs the visitor's main, gives it a System.in that never asks
//   available() (TraceJVM's fails on the keyboard), prints uncaught exceptions as the JVM does (visitor's frames only);
// - System.exit: `System.exit(` → `$.Sys$.exit(` (same length: compiler columns stay right), which tells the page;
// - the wall clock: TraceJVM's VM reads it from performance.now(); its worker starts from a module that makes
//   performance.now() count from 1970 first;
// - the project files and the standard descriptors through java-host.js.
//   const java = createJava({ base, profile }); await java.ready;
//   const c = await java.compile({ "Main.java": "…" })   → { ok, diagnostics: [{file, line, column, severity, message}], stderr, program }
//   const r = await java.run(c.program, { main, args, files, readLine, onOut, onErr, timeLimitMs })
//            → { code, error, files, ms }   (code: the exit code; error: why it stopped, if not by itself)
import { createHost } from "./java-host.js";

/** `System.exit(` / `java.lang.System.exit(` outside strings, characters, comments and text blocks → `$.Sys$.exit(`. */
export function rewriteExit(src) {
  let o = "", i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i], d = src[i + 1];
    if (c === "/" && d === "/") { const e = src.indexOf("\n", i); const j = e < 0 ? n : e; o += src.slice(i, j); i = j; continue; }
    if (c === "/" && d === "*") { const e = src.indexOf("*/", i + 2); const j = e < 0 ? n : e + 2; o += src.slice(i, j); i = j; continue; }
    if (c === '"' && src.startsWith('"""', i)) { const e = src.indexOf('"""', i + 3); const j = e < 0 ? n : e + 3; o += src.slice(i, j); i = j; continue; }
    if (c === '"' || c === "'") { let j = i + 1; while (j < n && src[j] !== c && src[j] !== "\n") j += src[j] === "\\" ? 2 : 1; o += src.slice(i, j + 1); i = j + 1; continue; }
    if (/[A-Za-z_$]/.test(c) && !/[\w$.]/.test(src[i - 1] || "")) {
      const m = /^(?:java\s*\.\s*lang\s*\.\s*)?System(\s*\.\s*)exit(?=\s*\()/.exec(src.slice(i, i + 80));
      if (m) { const whole = m[0], tail = whole.slice(whole.lastIndexOf("System") + 6); o += "$.Sys$".padStart(whole.length - tail.length, " ") + tail; i += whole.length; continue; }
      let j = i; while (j < n && /[\w$]/.test(src[j])) j++; o += src.slice(i, j); i = j; continue;
    }
    o += c; i++;
  }
  return o;
}

// Package $ is never shown to the visitor (its frames are left out of stack traces).
export const SUPPORT = {
  "$/Sys$.java": `package $;
public final class Sys$ {
    private Sys$() {}
    public static final class Exit extends Error {
        public final int status;
        Exit(int status) { super(null, null, false, false); this.status = status; }
    }
    public static void exit(int status) {
        Runtime.getRuntime().exit(status);   // the shutdown hooks, then the VM's halt stops the program (build 16 on)
        System.out.flush();                  // an older VM's halt returned: stop it here
        System.err.print("\\u001b]lmpb-exit;" + status + "\\u0007");   // the page stops the program here
        throw new Exit(status);
    }
}`,
  "$/Run$.java": `package $;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;
public final class Run$ {
    static boolean internal(StackTraceElement f) {
        String c = f.getClassName().replace('/', '.');
        return c.startsWith("$.") || c.startsWith("jdk.internal.") || c.startsWith("java.lang.reflect.") || c.startsWith("java.lang.invoke.");
    }
    static void trim(Throwable t, Set<Throwable> seen) {
        if (t == null || !seen.add(t)) return;
        List<StackTraceElement> keep = new ArrayList<>();
        for (StackTraceElement f : t.getStackTrace()) if (!internal(f)) keep.add(f);
        t.setStackTrace(keep.toArray(new StackTraceElement[0]));
        trim(t.getCause(), seen);
        for (Throwable s : t.getSuppressed()) trim(s, seen);
    }
    static Integer exitOf(Throwable t) {
        for (int i = 0; t != null && i < 20; i++, t = t.getCause()) if (t instanceof Sys$.Exit e) return e.status;
        return null;
    }
    static void uncaught(String thread, Throwable e) {
        trim(e, Collections.newSetFromMap(new IdentityHashMap<>()));
        System.out.flush();
        System.err.print("Exception in thread \\"" + thread + "\\" ");
        e.printStackTrace();
    }
    public static void main(String[] args) throws Throwable {
        FileInputStream keys = new FileInputStream(FileDescriptor.in);
        System.setIn(new BufferedInputStream(new InputStream() {
            @Override public int read() throws IOException { return keys.read(); }
            @Override public int read(byte[] b, int off, int len) throws IOException { return keys.read(b, off, len); }
            @Override public int available() { return 0; }
        }));
        Thread.setDefaultUncaughtExceptionHandler((th, e) -> { if (exitOf(e) == null) uncaught(th.getName(), e); });
        Method main = Class.forName(args[0]).getMethod("main", String[].class);
        try {
            main.invoke(null, (Object) Arrays.copyOfRange(args, 1, args.length));
        } catch (InvocationTargetException x) {
            Throwable e = x.getCause();
            if (exitOf(e) != null) return;
            uncaught("main", e);
            Sys$.exit(1);
        }
    }
}` };

export function createJava({ base, profile = "server" }) {
  const T0 = import(base + "browser-client.js");
  // Static imports run in order (the clock first, then TraceJVM's worker) and nothing is awaited, so the client's first
  // message finds the worker's listener.
  const clock = URL.createObjectURL(new Blob([`const now = performance.now.bind(performance), origin = Date.now() - now();
performance.now = () => origin + now();
`], { type: "text/javascript" }));
  const workerUrl = URL.createObjectURL(new Blob([`import ${JSON.stringify(clock)};\nimport ${JSON.stringify(base + "browser-worker.js")};`], { type: "text/javascript" }));
  const worker = () => new Worker(workerUrl, { type: "module" });
  let T, compiler, runner = null, host = null;
  const hostProxy = { dispatch: (req) => host.dispatch(req) };
  // Every file TraceJVM downloads is checked against the size and SHA-256 its release lists (release.json): one map for
  // the runner, one for the compiler (its own files, named relative to compiler/, plus the platform jar).
  let integrity, compilerIntegrity;
  const newRunner = async () => {
    runner = new T.TraceJVMWorkerClient({ createWorker: worker, host: hostProxy, engine: {
      assets: { runtimeProfileBaseUrls: { [profile]: base + `profiles/${profile}` }, wasmUrl: base + "bjvm_main.wasm", integrity },
      runtimeProfile: profile, workingDirectory: "/home/student", hostStandardDescriptors: true } });
    await runner.initialize();
  };
  const ready = (async () => {
    T = await T0;
    const files = (await (await fetch(base + "release.json")).json()).files;
    integrity = T.createTraceJVMAssetIntegrityMap(base, files);
    compilerIntegrity = Object.freeze({ ...integrity, ...T.createTraceJVMAssetIntegrityMap(base + "compiler",
      files.filter((f) => f.path.startsWith("compiler/")).map((f) => ({ ...f, path: f.path.slice("compiler/".length) }))) });
    compiler = new T.TraceJVMCompilerWorkerClient({ compiler: { assets: { baseUrl: base + "compiler", integrity: compilerIntegrity }, platformArchiveUrl: base + `profiles/${profile}/jdk23.jar` }, createWorker: worker });
    await Promise.all([compiler.initialize(), newRunner()]);
  })();

  return {
    ready,
    async compile(files) {
      await ready;
      const sources = [
        ...Object.entries(files).filter(([p]) => p.endsWith(".java")).map(([path, content]) => ({ path, content: rewriteExit(content) })),
        ...Object.entries(SUPPORT).map(([path, content]) => ({ path, content })),
      ];
      const r = await compiler.compile({ sources });
      const diagnostics = (r.diagnostics || []).map((d) => ({ file: d.fileName, line: d.lineNumber, column: d.columnNumber, severity: d.severity, message: d.message }));
      return { ok: r.status === "completed", diagnostics, stderr: r.stderr || "", program: r.program, ms: r.timings && Math.round(r.timings.totalMs) };
    },
    async run(program, { main = "Main", args = [], files = {}, readLine = async () => null, onOut = () => {}, onErr = () => {}, timeLimitMs = 30000 } = {}) {
      await ready;
      if (!runner) await newRunner();
      const ac = new AbortController();
      let code = null, error = null;
      host = createHost({ files, onOut, onErr, readLine, onExit: (status) => { code = status; setTimeout(() => ac.abort(), 0); } });
      const t = performance.now();
      const timer = setTimeout(() => { error = `Time limit reached (${timeLimitMs / 1000} s of running time): the program was stopped.`; ac.abort(); }, timeLimitMs);
      try {
        const res = await runner.run({ program, mainClass: "$.Run$", args: [main, ...args], signal: ac.signal });
        if (code == null) code = res.exitCode;
      } catch (e) {
        if (code == null && !error) error = `The Java runtime stopped (${e && (e._tag || e.message)}${e && e.workerMessage ? `: ${e.workerMessage}` : ""}).`;
        runner = null;                        // a stopped VM is not reused: the next run starts a new one
      } finally { clearTimeout(timer); }
      return { code, error, files: host.files(), ms: Math.round(performance.now() - t) };
    },
    stop() { if (runner) { runner.terminate(); runner = null; } },
  };
}
