import {
  makeRuntimeSystem,
  type RuntimeSystem,
  type RuntimeVM,
} from "@b-jvm/bjvm2";
import * as Effect from "effect/Effect";
import {
  TraceJVMInitializationError,
  TraceJVMOperationError,
  type TraceJVMEngineError,
} from "./errors";
import { runTraceJVMEffect } from "./run-effect";
import {
  fetchVerifiedTraceJVMAsset,
  traceJVMAssetUrl,
  type TraceJVMAssetIntegrityMap,
} from "./asset-integrity";
import { removeTreeNoFollow } from "./filesystem";
import {
  validateProcessFilePath,
  validateRelativePath,
} from "./path-validation";
import {
  resolveTraceJVMResourceLimits,
  type TraceJVMResourceLimits,
  TraceJVMOutputBudget,
  validateTraceJVMRunResources,
} from "./resource-limits";
import type {
  TraceJVMAsynchronousHost,
  TraceJVMHostRequest,
  TraceJVMSynchronousHost,
} from "./host";

export interface TraceJVMSourceFile {
  path: string;
  content: string;
}

export interface TraceJVMBinaryFile {
  path: string;
  content: Uint8Array;
}

export interface TraceJVMCompileRequest {
  sources: readonly TraceJVMSourceFile[];
  /** JARs or compiled class files made visible to javac. */
  classpath?: readonly TraceJVMBinaryFile[];
  /**
   * javac's options, as on its command line: -Xlint:all, -g, -parameters, --enable-preview … (Shiklo fork,
   * 2026-10-30).
   */
  options?: readonly string[];
  signal?: AbortSignal;
  onStdout?: (chunk: string) => void;
  onStderr?: (chunk: string) => void;
}

export interface TraceJVMCompiledProgram {
  files: readonly TraceJVMBinaryFile[];
}

export interface TraceJVMCompilerDiagnostic {
  readonly severity: "error" | "warning" | "other";
  readonly fileName?: string | null;
  readonly lineNumber?: number;
  readonly columnNumber?: number;
  readonly message: string;
}

export interface TraceJVMCompileResult {
  status: "completed" | "compile-error" | "cancelled";
  exitCode: number;
  stdout: string;
  stderr: string;
  program?: TraceJVMCompiledProgram;
  diagnostics: readonly TraceJVMCompilerDiagnostic[];
  timings: {
    compilerInitMs: number;
    queueMs: number;
    compileMs: number;
    totalMs: number;
  };
}

export interface TraceJVMRunRequest {
  program: TraceJVMCompiledProgram;
  /** JARs or compiled class files made visible to the application loader. */
  classpath?: readonly TraceJVMBinaryFile[];
  /** Process-scoped files installed for this execution and restored afterward. */
  processFiles?: readonly TraceJVMBinaryFile[];
  mainClass: string;
  args?: readonly string[];
  /** Process-scoped Java system properties restored after the application exits. */
  systemProperties?: Readonly<Record<string, string>>;
  signal?: AbortSignal;
  onStdout?: (chunk: string) => void;
  onStderr?: (chunk: string) => void;
  /**
   * Default-off execution diagnostics. These counters are intended for
   * compatibility and performance campaigns, not learner-facing execution.
   */
  diagnostics?: {
    bytecodeProfile?: boolean;
  };
}

export type TraceJVMExecutionStatus =
  | "completed"
  | "runtime-error"
  | "cancelled";

export type TraceJVMIsolationStatus =
  | "not-applicable"
  | "clean"
  | "tainted";

/**
 * Describes whether the current warm VM can be reused after an operation.
 * A tainted scope is still a valid Java result, but it requires a hard Worker
 * boundary before another process is admitted.
 */
export interface TraceJVMIsolationReport {
  status: TraceJVMIsolationStatus;
  restored: readonly string[];
  taintReasons: readonly string[];
  hardBoundaryRecommended: boolean;
}

export interface TraceJVMExecuteResult {
  status: TraceJVMExecutionStatus;
  exitCode: number;
  value?: string | null;
  stdout: string;
  stderr: string;
  timings: {
    runtimeInitMs: number;
    queueMs: number;
    compileAndRunMs: number;
    totalMs: number;
  };
  isolation: TraceJVMIsolationReport;
  retirementRecommended: boolean;
  diagnostics?: TraceJVMExecutionDiagnostics;
}

export interface TraceJVMMethodProfile {
  className: string;
  methodName: string;
  descriptor: string;
  invocations: number;
  bytecodes: number;
  native: boolean;
  staticInstructions: number;
  maxStack: number;
  maxLocals: number;
  exceptionHandlers: number;
  featureMask: number;
  branches: number;
  callSites: number;
}

export interface TraceJVMOpcodeProfile {
  opcode: string;
  bytecodes: number;
}

export interface TraceJVMExecutionDiagnostics {
  bytecodeProfile?: {
    totalBytecodes: number;
    totalInvocations: number;
    methods: readonly TraceJVMMethodProfile[];
    opcodes: readonly TraceJVMOpcodeProfile[];
  };
  diagnosticError?: string;
}

export const TRACEJVM_RUNTIME_PROFILES = [
  "core",
  "server",
  "spring-server",
] as const;

export type TraceJVMRuntimeProfile =
  (typeof TRACEJVM_RUNTIME_PROFILES)[number];

export interface TraceJVMAssetProvider {
  /**
   * A profile URL contains jdk23.jar and the JDK 23 support tree. Profiles are
   * explicit so a consumer never silently pays for, or
   * accidentally claims, Java APIs it did not provision.
   */
  runtimeProfileBaseUrls: Readonly<
    Partial<Record<TraceJVMRuntimeProfile, string>>
  >;
  wasmUrl: string;
  /** Trusted build-time metadata keyed by each exact asset URL. */
  integrity: TraceJVMAssetIntegrityMap;
}

export interface TraceJVMOptions {
  assets: TraceJVMAssetProvider;
  /**
   * Absolute process working directory established before the Java runtime is
   * initialized. Defaults to "/".
   */
  workingDirectory?: string;
  /**
   * Route file descriptors 0, 1, and 2 through the system host instead of the
   * standalone engine callbacks.
   */
  hostStandardDescriptors?: boolean;
  /** Defaults to the minimal java.base-only runtime. */
  runtimeProfile?: TraceJVMRuntimeProfile;
  heapBytes?: number;
  retirementAfterExecutions?: number;
  /** Host-selected safety ceilings; defaults are suitable for browser Workers. */
  limits?: Partial<TraceJVMResourceLimits>;
  /**
   * Default-off engine experiments. These are intentionally excluded from
   * TraceJVM's supported compatibility contract until their release gates pass.
   */
  experiments?: {
    /** Build-time AOT for a generated allowlist of pinned OpenJDK methods. */
    hotAot?: boolean;
  };
  /**
   * Optional system boundary for hosted filesystem, descriptor, process, and
   * network capabilities. Browser consumers normally configure the Worker
   * host on TraceJVMWorkerClient instead.
   */
  host?: TraceJVMSynchronousHost & Partial<TraceJVMAsynchronousHost>;
}

export interface TraceJVM {
  initializeEffect(
    signal?: AbortSignal,
  ): Effect.Effect<{ initializeMs: number }, TraceJVMInitializationError>;
  runEffect(
    request: TraceJVMRunRequest,
  ): Effect.Effect<TraceJVMExecuteResult, TraceJVMEngineError>;
  /** Promise convenience boundary for consumers that do not use Effect. */
  initialize(signal?: AbortSignal): Promise<{ initializeMs: number }>;
  run(request: TraceJVMRunRequest): Promise<TraceJVMExecuteResult>;
  readonly executionCount: number;
  readonly retirementRecommended: boolean;
  dispose(): void;
}

export interface TraceJVMRunnerHostOptions {
  assets: TraceJVMAssetProvider;
  runtimeProfile?: TraceJVMRuntimeProfile;
  runnerHeapBytes?: number;
  retirementAfterExecutions?: number;
  limits?: Partial<TraceJVMResourceLimits>;
  experiments?: TraceJVMOptions["experiments"];
}

export interface TraceJVMProcessOptions {
  host?: TraceJVMSynchronousHost & Partial<TraceJVMAsynchronousHost>;
  workingDirectory?: string;
  hostStandardDescriptors?: boolean;
  heapBytes?: number;
  retirementAfterExecutions?: number;
}

export interface TraceJVMProcess {
  initialize(signal?: AbortSignal): Promise<{ initializeMs: number }>;
  run(request: TraceJVMRunRequest): Promise<TraceJVMExecuteResult>;
  readonly executionCount: number;
  readonly retirementRecommended: boolean;
  dispose(): void;
}

interface RuntimeBridge {
  runCompiled: (
    outputDirectory: string,
    mainClass: string,
    encodedArgs: string,
    classpath: string,
    encodedSystemProperties: string,
  ) => Promise<unknown>;
  takeLastIsolationReport: () => Promise<unknown>;
}

interface RuntimeState {
  os: RuntimeSystem;
  vm: RuntimeVM;
  bridge: RuntimeBridge;
  startedAt: number;
  initializedAt: number;
}

interface SharedRuntimeBinding {
  readonly os: RuntimeSystem;
}

interface BytecodeProfilerHandle {
  module: {
    _read_profiler(profiler: number): number;
    UTF8ToString(pointer: number): string;
    _free(pointer: number): void;
  };
  profiler: number;
}

function parseBytecodeProfile(raw: string): NonNullable<
  TraceJVMExecutionDiagnostics["bytecodeProfile"]
> {
  const lines = raw.trim().split("\n").filter(Boolean);
  const opcodes = lines
    .filter((line) => line.startsWith("#opcode\t"))
    .map((line) => {
      const [, opcode, bytecodes] = line.split("\t");
      return { opcode: opcode!, bytecodes: Number(bytecodes) };
    })
    .sort((left, right) => right.bytecodes - left.bytecodes);
  const methods = lines
    .filter((line) => !line.startsWith("#"))
    .map((line) => {
      const [
        className,
        methodName,
        descriptor,
        invocations,
        bytecodes,
        native,
        staticInstructions,
        maxStack,
        maxLocals,
        exceptionHandlers,
        featureMask,
        branches,
        callSites,
      ] = line.split("\t");
      return {
        className: className!,
        methodName: methodName!,
        descriptor: descriptor!,
        invocations: Number(invocations),
        bytecodes: Number(bytecodes),
        native: native === "1",
        staticInstructions: Number(staticInstructions),
        maxStack: Number(maxStack),
        maxLocals: Number(maxLocals),
        exceptionHandlers: Number(exceptionHandlers),
        featureMask: Number(featureMask),
        branches: Number(branches),
        callSites: Number(callSites),
      };
    })
    .sort((left, right) => right.bytecodes - left.bytecodes);
  return {
    totalBytecodes: methods.reduce(
      (total, method) => total + method.bytecodes,
      0,
    ),
    totalInvocations: methods.reduce(
      (total, method) => total + method.invocations,
      0,
    ),
    methods,
    opcodes,
  };
}

function beginBytecodeProfile(
  runtime: RuntimeState,
  enabled: boolean | undefined,
): BytecodeProfilerHandle | undefined {
  if (!enabled) return undefined;
  const module = (runtime.vm as unknown as {
    _module: {
      _launch_profiler(thread: number): number;
      _read_profiler(profiler: number): number;
      UTF8ToString(pointer: number): string;
      _free(pointer: number): void;
    };
  })._module;
  const thread = (runtime.vm as unknown as {
    getActiveThread(): { ptr: number };
  }).getActiveThread();
  const profiler = module._launch_profiler(thread.ptr);
  if (!profiler) throw new Error("Could not start the bytecode profiler.");
  return { module, profiler };
}

function finishBytecodeProfile(
  handle: BytecodeProfilerHandle,
): NonNullable<TraceJVMExecutionDiagnostics["bytecodeProfile"]> {
  const pointer = handle.module._read_profiler(handle.profiler);
  if (!pointer) throw new Error("Could not read the bytecode profiler.");
  try {
    return parseBytecodeProfile(handle.module.UTF8ToString(pointer));
  } finally {
    handle.module._free(pointer);
  }
}

function describeThrowable(error: unknown): string {
  if (error instanceof Error) return error.stack ?? error.message;
  try {
    return String(error);
  } catch {
    return "Java execution failed with an unprintable throwable";
  }
}

function combineSignals(
  callerSignal: AbortSignal | undefined,
  effectSignal: AbortSignal,
): AbortSignal {
  return callerSignal
    ? AbortSignal.any([callerSignal, effectSignal])
    : effectSignal;
}

function validateClassName(className: string): void {
  if (!/^(?:[A-Za-z_$][\w$]*\.)*[A-Za-z_$][\w$]*$/u.test(className)) {
    throw new Error(`Invalid Java adapter class: ${className}`);
  }
}

/**
 * Transport process arguments through b-jvm's stable string bridge without
 * exposing a consumer-specific invocation wrapper. Each argument is UTF-8
 * encoded before Base64 transport so empty strings and arbitrary Unicode do
 * not depend on the bridge's internal Java-string representation.
 */
function encodeArguments(args: readonly string[] | undefined): string {
  const values = args ?? [];
  const encoded = values.map((value) => {
    const bytes = new TextEncoder().encode(value);
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary);
  });
  return values.length === 0 ? "0" : `${values.length}\n${encoded.join("\n")}`;
}

function encodeSystemProperties(
  properties: Readonly<Record<string, string>> | undefined,
): string {
  const entries = Object.entries(properties ?? {}).sort(([left], [right]) =>
    left.localeCompare(right),
  );
  const values = entries.flatMap(([key, value]) => [key, value]);
  return encodeArguments(values);
}

function mkdir(fs: RuntimeSystem["FS"], path: string): void {
  try {
    fs.mkdir(path);
  } catch {
    // Scratch parents may already exist.
  }
}

function setWorkingDirectory(
  fs: RuntimeSystem["FS"],
  workingDirectory: string | undefined,
): void {
  const path = validateWorkingDirectory(workingDirectory);
  let directory = "";
  for (const segment of path.split("/").filter(Boolean)) {
    directory += `/${segment}`;
    mkdir(fs, directory);
  }
  fs.chdir(directory || "/");
}

function validateWorkingDirectory(
  workingDirectory: string | undefined,
): string {
  const path = workingDirectory ?? "/";
  if (!path.startsWith("/")) {
    throw new Error("TraceJVM workingDirectory must be absolute.");
  }
  for (const segment of path.split("/").filter(Boolean)) {
    if (segment === "." || segment === "..") {
      throw new Error(
        "TraceJVM workingDirectory cannot contain dot segments.",
      );
    }
  }
  return path;
}

function writeBinaryFileTree(
  fs: RuntimeSystem["FS"],
  root: string,
  file: TraceJVMBinaryFile,
): void {
  const segments = file.path.split("/");
  let directory = root;
  for (const segment of segments.slice(0, -1)) {
    directory += `/${segment}`;
    mkdir(fs, directory);
  }
  fs.writeFile(`${root}/${file.path}`, file.content);
}

interface InstalledProcessFile {
  readonly path: string;
  readonly priorContent?: Uint8Array;
}

function processFileExists(
  fs: RuntimeSystem["FS"],
  path: string,
): boolean {
  const segments = path.split("/").filter(Boolean);
  const name = segments.at(-1);
  if (!name) return false;
  const parent = segments.length === 1
    ? "/"
    : `/${segments.slice(0, -1).join("/")}`;
  return fs.readdir(parent).includes(name);
}

function installProcessFiles(
  fs: RuntimeSystem["FS"],
  files: readonly TraceJVMBinaryFile[] | undefined,
): readonly InstalledProcessFile[] {
  const installed: InstalledProcessFile[] = [];
  try {
    for (const file of files ?? []) {
      validateProcessFilePath(file.path);
      const segments = file.path.split("/").filter(Boolean);
      let directory = "";
      for (const segment of segments.slice(0, -1)) {
        directory += `/${segment}`;
        mkdir(fs, directory);
      }
      let priorContent: Uint8Array | undefined;
      if (processFileExists(fs, file.path)) {
        priorContent = new Uint8Array(fs.readFile(file.path));
      }
      if (priorContent) fs.unlink(file.path);
      fs.writeFile(file.path, file.content);
      installed.push({
        path: file.path,
        ...(priorContent ? { priorContent } : {}),
      });
    }
    return installed;
  } catch (error) {
    restoreProcessFiles(fs, installed);
    throw error;
  }
}

function restoreProcessFiles(
  fs: RuntimeSystem["FS"],
  installed: readonly InstalledProcessFile[],
): void {
  for (const file of [...installed].reverse()) {
    try {
      fs.unlink(file.path);
    } catch {
      // The application may already have removed a process-scoped file.
    }
    if (file.priorContent) {
      try {
        fs.writeFile(file.path, file.priorContent);
      } catch {
        // Cleanup is best effort; isolation reporting still decides reuse.
      }
    }
  }
}

function writeClasspath(
  fs: RuntimeSystem["FS"],
  root: string,
  files: readonly TraceJVMBinaryFile[] | undefined,
): string {
  mkdir(fs, root);
  const apiTarget = `${root}/io/tracecode/tracekernel`;
  mkdir(fs, `${root}/io`);
  mkdir(fs, `${root}/io/tracecode`);
  mkdir(fs, apiTarget);
  for (const runtimeFile of TRACEKERNEL_API_RUNTIME_FILES) {
    const name = runtimeFile.slice(runtimeFile.lastIndexOf("/") + 1);
    fs.writeFile(
      `${apiTarget}/${name}`,
      fs.readFile(`/${runtimeFile}`),
    );
  }
  const entries = new Set<string>([root]);
  for (const file of files ?? []) {
    validateRelativePath(file.path, "Java classpath");
    if (file.path.startsWith("io/tracecode/tracekernel/")) {
      throw new Error(
        `Java classpath cannot replace the reserved TraceKernel API: ${file.path}`,
      );
    }
    writeBinaryFileTree(fs, root, file);
    if (file.path.endsWith(".jar")) {
      entries.add(`${root}/${file.path}`);
    } else if (!file.path.endsWith(".class")) {
      throw new Error(
        `Java classpath entries must be JARs or class files: ${file.path}`,
      );
    }
  }
  return [...entries].join(":");
}

const TRACEKERNEL_API_RUNTIME_FILES = [
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$ProcessIdentity.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$SessionIdentity.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$TerminalWindowSize.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$WatchdogSignal.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$WatchdogStatus.class",
] as const;

function decodeBridgeStatus(value: unknown): Pick<TraceJVMExecuteResult, "status" | "exitCode" | "value"> {
  const text = value == null ? null : String(value);
  if (text?.startsWith("__FAILED__:")) {
    return { status: "runtime-error", exitCode: 1 };
  }
  return { status: "completed", exitCode: 0, value: text };
}

const NOT_APPLICABLE_ISOLATION: TraceJVMIsolationReport = {
  status: "not-applicable",
  restored: [],
  taintReasons: [],
  hardBoundaryRecommended: false,
};

function decodeIsolationReport(value: unknown): TraceJVMIsolationReport {
  const lines = String(value ?? "tainted\ntaint:isolation-report-missing")
    .split("\n")
    .filter(Boolean);
  const status = lines[0] === "clean" ? "clean" : "tainted";
  const restored = lines
    .filter((line) => line.startsWith("restored:"))
    .map((line) => line.slice("restored:".length));
  const taintReasons = lines
    .filter((line) => line.startsWith("taint:"))
    .map((line) => line.slice("taint:".length));
  if (status === "tainted" && taintReasons.length === 0) {
    taintReasons.push("isolation-report-malformed");
  }
  return {
    status,
    restored,
    taintReasons,
    hardBoundaryRecommended: status === "tainted",
  };
}

async function loadOperatingSystem(
  options: Pick<TraceJVMOptions, "assets" | "runtimeProfile">,
  signal?: AbortSignal,
): Promise<RuntimeSystem> {
  const profile = options.runtimeProfile ?? "core";
  const runtimeBase = options.assets.runtimeProfileBaseUrls[profile];
  if (!runtimeBase) {
    throw new Error(
      `TraceJVM runtime profile "${profile}" was requested but no asset URL was provided.`,
    );
  }
  if (signal?.aborted) {
    throw Object.assign(
      new Error("Java initialization was cancelled."),
      { name: "AbortError" },
    );
  }
  const wasmBinary = await fetchVerifiedTraceJVMAsset(
    options.assets.wasmUrl,
    options.assets.integrity,
  );
  const os = await makeRuntimeSystem({
    runtimeUrl: runtimeBase,
    wasmBinary,
    loadRuntimeFile: (file) => fetchVerifiedTraceJVMAsset(
      traceJVMAssetUrl(runtimeBase, file),
      options.assets.integrity,
    ),
    additionalRuntimeFiles: [
      ...TRACEKERNEL_API_RUNTIME_FILES,
      "jdk23/lib/tzdb.dat",
      "jdk23/lib/module-packages.map",
      // javax.crypto's jurisdiction policy (Shiklo fork, 2026-10-30).
      "jdk23/conf/security/policy/unlimited/default_US_export.policy",
      "jdk23/conf/security/policy/unlimited/default_local.policy",
      ...(profile === "core"
        ? []
        : ["jdk23/conf/logging.properties"]),
    ],
    loadPlatformModuleImage: true,
  });
  mkdir(os.FS, "/tracejvm");
  mkdir(os.FS, "/workspace");
  const traceKernelApi = os.FS.readFile(
    "/tracekernel-api/io/tracecode/tracekernel/TraceKernel.class",
  );
  if (
    traceKernelApi.byteLength < 4 ||
    traceKernelApi[0] !== 0xca ||
    traceKernelApi[1] !== 0xfe ||
    traceKernelApi[2] !== 0xba ||
    traceKernelApi[3] !== 0xbe
  ) {
    throw new TraceJVMInitializationError(
      new Error("TraceJVM TraceKernel API asset is missing or invalid."),
    );
  }
  return os;
}

let nextRuntimeInstanceId = 0;

/**
 * Java 23 classfile execution engine with no compiler, Worker protocol,
 * TraceKernel process, application request, or consumer-generated invocation
 * assumptions.
 */
export class TraceJVMEngine implements TraceJVM {
  private runtimePromise: Promise<RuntimeState> | null = null;
  private runtimeState: RuntimeState | null = null;
  private disposed = false;
  private sequence = 0;
  private executions = 0;
  private tainted = false;
  private operation = Promise.resolve();
  private activeStdout: string[] | null = null;
  private activeStderr: string[] | null = null;
  private activeStdoutListener: ((chunk: string) => void) | null = null;
  private activeStderrListener: ((chunk: string) => void) | null = null;
  private readonly stdoutDecoder = new TextDecoder();
  private readonly stderrDecoder = new TextDecoder();
  private readonly resourceLimits: TraceJVMResourceLimits;
  private activeOutputBudget: TraceJVMOutputBudget | null = null;
  private readonly scratchRoot =
    `/tracejvm/runtime-${++nextRuntimeInstanceId}`;

  constructor(
    private readonly options: TraceJVMOptions,
    private readonly sharedRuntime?: SharedRuntimeBinding,
  ) {
    this.resourceLimits = resolveTraceJVMResourceLimits(options.limits);
  }

  get executionCount(): number {
    return this.executions;
  }

  get retirementRecommended(): boolean {
    return (
      this.tainted ||
      this.executions >= (this.options.retirementAfterExecutions ?? 8)
    );
  }

  initializeEffect(
    signal?: AbortSignal,
  ): Effect.Effect<{ initializeMs: number }, TraceJVMInitializationError> {
    return Effect.tryPromise({
      try: (effectSignal) =>
        this.initializePromise(combineSignals(signal, effectSignal)),
      catch: (cause) => new TraceJVMInitializationError(cause),
    });
  }

  runEffect(
    request: TraceJVMRunRequest,
  ): Effect.Effect<TraceJVMExecuteResult, TraceJVMEngineError> {
    return Effect.tryPromise({
      try: (effectSignal) =>
        this.runPromise({
          ...request,
          signal: combineSignals(request.signal, effectSignal),
        }),
      catch: (cause) => new TraceJVMOperationError("run", cause),
    });
  }

  initialize(signal?: AbortSignal): Promise<{ initializeMs: number }> {
    return runTraceJVMEffect(this.initializeEffect(signal));
  }

  run(request: TraceJVMRunRequest): Promise<TraceJVMExecuteResult> {
    return runTraceJVMEffect(this.runEffect(request));
  }

  private async initializePromise(
    signal?: AbortSignal,
  ): Promise<{ initializeMs: number }> {
    const runtime = await this.runtime(signal);
    return { initializeMs: runtime.initializedAt - runtime.startedAt };
  }

  private async runPromise(
    request: TraceJVMRunRequest,
  ): Promise<TraceJVMExecuteResult> {
    if (this.disposed) throw new Error("Java engine has been disposed.");
    validateTraceJVMRunResources(request, this.resourceLimits);
    const requestStartedAt = performance.now();
    const prior = this.operation;
    let release!: () => void;
    this.operation = new Promise<void>((resolve) => {
      release = resolve;
    });
    await prior;
    const queueCompletedAt = performance.now();
    try {
      if (request.signal?.aborted) {
        return this.cancelledResult(requestStartedAt, queueCompletedAt);
      }
      const runtime = await this.runtime(request.signal);
      if (request.signal?.aborted) {
        return this.cancelledResult(requestStartedAt, queueCompletedAt);
      }

      validateClassName(request.mainClass);
      const sequence = ++this.sequence;
      const outputRoot = `${this.scratchRoot}/run-classes-${sequence}`;
      const classpathRoot =
        `${this.scratchRoot}/run-classpath-${sequence}`;
      mkdir(runtime.os.FS, outputRoot);
      for (const file of request.program.files) {
        validateRelativePath(file.path, "Java class artifact");
        writeBinaryFileTree(runtime.os.FS, outputRoot, file);
      }
      const classpath = writeClasspath(
        runtime.os.FS,
        classpathRoot,
        request.classpath,
      );
      const processFiles = installProcessFiles(
        runtime.os.FS,
        request.processFiles,
      );

      const output = this.beginOutputCapture(request);
      const operationStartedAt = performance.now();
      let value: unknown;
      let diagnostics: TraceJVMExecutionDiagnostics | undefined;
      let profiler: BytecodeProfilerHandle | undefined;
      let isolation: TraceJVMIsolationReport = {
        status: "tainted",
        restored: [],
        taintReasons: ["java-bridge-failed-before-isolation-report"],
        hardBoundaryRecommended: true,
      };
      try {
        try {
          profiler = beginBytecodeProfile(
            runtime,
            request.diagnostics?.bytecodeProfile,
          );
        } catch (error) {
          diagnostics = { diagnosticError: describeThrowable(error) };
        }
        value = await runtime.bridge.runCompiled(
          outputRoot,
          request.mainClass,
          encodeArguments(request.args),
          classpath,
          encodeSystemProperties(request.systemProperties),
        );
        isolation = decodeIsolationReport(
          await runtime.bridge.takeLastIsolationReport(),
        );
      } catch (error) {
        output.stderr.push(describeThrowable(error));
        value = "__FAILED__:JavaScriptBridgeError";
      } finally {
        if (profiler) {
          try {
            diagnostics = {
              ...diagnostics,
              bytecodeProfile: finishBytecodeProfile(profiler),
            };
          } catch (error) {
            diagnostics = {
              ...diagnostics,
              diagnosticError: describeThrowable(error),
            };
          }
        }
        try {
          this.finishOutputCapture(request, output);
        } finally {
          removeTreeNoFollow(runtime.os.FS, outputRoot);
          removeTreeNoFollow(runtime.os.FS, classpathRoot);
          restoreProcessFiles(runtime.os.FS, processFiles);
        }
      }
      this.executions += 1;
      this.tainted ||= isolation.status === "tainted";
      const completedAt = performance.now();
      const decoded = decodeBridgeStatus(value);
      return {
        ...decoded,
        stdout: output.stdout.join(""),
        stderr: output.stderr.join(""),
        timings: {
          runtimeInitMs: runtime.initializedAt - runtime.startedAt,
          queueMs: queueCompletedAt - requestStartedAt,
          compileAndRunMs: completedAt - operationStartedAt,
          totalMs: completedAt - requestStartedAt,
        },
        isolation,
        retirementRecommended: this.retirementRecommended,
        ...(diagnostics ? { diagnostics } : {}),
      };
    } finally {
      release();
    }
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.runtimeState?.vm.dispose();
    if (this.runtimeState) {
      removeTreeNoFollow(this.runtimeState.os.FS, this.scratchRoot);
    }
    this.runtimeState = null;
    this.runtimePromise = null;
    this.activeStdout = null;
    this.activeStderr = null;
    this.activeStdoutListener = null;
    this.activeStderrListener = null;
    this.activeOutputBudget = null;
  }

  private cancelledResult(startedAt: number, queueCompletedAt: number): TraceJVMExecuteResult {
    return {
      status: "cancelled",
      exitCode: 130,
      stdout: "",
      stderr: "",
      timings: {
        runtimeInitMs: 0,
        queueMs: queueCompletedAt - startedAt,
        compileAndRunMs: 0,
        totalMs: performance.now() - startedAt,
      },
      isolation: NOT_APPLICABLE_ISOLATION,
      retirementRecommended: this.retirementRecommended,
    };
  }

  private beginOutputCapture(
    request: Pick<TraceJVMRunRequest, "onStdout" | "onStderr">,
  ): { stdout: string[]; stderr: string[] } {
    const output = { stdout: [] as string[], stderr: [] as string[] };
    this.activeStdout = output.stdout;
    this.activeStderr = output.stderr;
    this.activeStdoutListener = request.onStdout ?? null;
    this.activeStderrListener = request.onStderr ?? null;
    this.activeOutputBudget = new TraceJVMOutputBudget(
      this.resourceLimits.maxOutputBytes,
    );
    return output;
  }

  private finishOutputCapture(
    request: Pick<TraceJVMRunRequest, "onStdout" | "onStderr">,
    output: { stdout: string[]; stderr: string[] },
  ): void {
    const finalStdout = this.stdoutDecoder.decode();
    const finalStderr = this.stderrDecoder.decode();
    try {
      if (finalStdout) {
        this.activeOutputBudget?.consume(finalStdout);
        output.stdout.push(finalStdout);
        request.onStdout?.(finalStdout);
      }
      if (finalStderr) {
        this.activeOutputBudget?.consume(finalStderr);
        output.stderr.push(finalStderr);
        request.onStderr?.(finalStderr);
      }
    } finally {
      this.activeStdout = null;
      this.activeStderr = null;
      this.activeStdoutListener = null;
      this.activeStderrListener = null;
      this.activeOutputBudget = null;
    }
  }

  private runtime(signal?: AbortSignal): Promise<RuntimeState> {
    if (this.disposed) return Promise.reject(new Error("Java engine has been disposed."));
    this.runtimePromise ??= this.initializeRuntime(signal).then(
      (runtime) => {
        if (this.disposed) {
          runtime.vm.dispose();
          throw new Error("Java engine was disposed during initialization.");
        }
        this.runtimeState = runtime;
        return runtime;
      },
      (error) => {
        this.runtimePromise = null;
        throw error;
      },
    );
    return this.runtimePromise;
  }

  private async initializeRuntime(signal?: AbortSignal): Promise<RuntimeState> {
    const startedAt = performance.now();
    const os = this.sharedRuntime?.os ??
      await loadOperatingSystem(this.options, signal);
    if (signal?.aborted) {
      throw Object.assign(
        new Error("Java initialization was cancelled."),
        { name: "AbortError" },
      );
    }
    mkdir(os.FS, this.scratchRoot);
    const workingDirectory =
      validateWorkingDirectory(this.options.workingDirectory);
    if (!this.sharedRuntime) {
      setWorkingDirectory(os.FS, workingDirectory);
    }
    const vm = os.makeVM({
      classpath: "/tracekernel-api:/",
      heapSize: this.options.heapBytes ?? (64 << 20),
      experimentalHotAot: this.options.experiments?.hotAot,
      workingDirectory,
      stdout: (bytes) => {
        if (this.activeStdout) {
          const chunk = this.stdoutDecoder.decode(bytes, { stream: true });
          if (chunk) {
            this.activeOutputBudget?.consume(chunk);
            this.activeStdout.push(chunk);
            this.activeStdoutListener?.(chunk);
          }
        }
      },
      stderr: (bytes) => {
        if (this.activeStderr) {
          const chunk = this.stderrDecoder.decode(bytes, { stream: true });
          if (chunk) {
            this.activeOutputBudget?.consume(chunk);
            this.activeStderr.push(chunk);
            this.activeStderrListener?.(chunk);
          }
        }
      },
      hostDispatchSync: this.options.host
        ? (request) =>
            this.options.host!.dispatchSync(
              request as TraceJVMHostRequest,
            )
        : undefined,
      hostDispatchAsync: this.options.host?.dispatch
        ? (request) =>
            this.options.host!.dispatch!(
              request as TraceJVMHostRequest,
            )
        : undefined,
      hostStandardDescriptors:
        this.options.hostStandardDescriptors === true,
    });
    // The VM indexed and copied these archives while constructing its classpath.
    if (!this.sharedRuntime) {
      os.FS.unlink("/jdk23.jar");
    }
    vm.setPreemptionFrequencyUs(1_000);
    const bridge = vm.loadClass(
      "jdk/internal/tracecode/TraceJVMRunner" as never,
    ) as RuntimeBridge;
    return { os, vm, bridge, startedAt, initializedAt: performance.now() };
  }
}

class TraceJVMProcessLease implements TraceJVMProcess {
  private disposed = false;

  constructor(
    private readonly engine: TraceJVMEngine,
    private readonly kernelBound: boolean,
    private readonly onDispose: () => void,
  ) {}

  get executionCount(): number {
    return this.engine.executionCount;
  }

  get retirementRecommended(): boolean {
    return this.engine.retirementRecommended;
  }

  initialize(signal?: AbortSignal): Promise<{ initializeMs: number }> {
    if (this.disposed) {
      return Promise.reject(new Error("TraceJVM process has been disposed."));
    }
    return this.engine.initialize(signal);
  }

  run(request: TraceJVMRunRequest): Promise<TraceJVMExecuteResult> {
    if (this.disposed) {
      return Promise.reject(new Error("TraceJVM process has been disposed."));
    }
    if (this.kernelBound && request.processFiles?.length) {
      return Promise.reject(
        new Error(
          "TraceJVM runner host processes use TraceKernel as the filesystem " +
            "authority; provision process files through the bound kernel process.",
        ),
      );
    }
    return this.engine.run(request);
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.engine.dispose();
    this.onDispose();
  }
}

/**
 * Persistent immutable runtime substrate for disposable runner JVMs.
 *
 * This host has no compiler and cannot accept Java source. Embedders may pair
 * it with any compiler lifecycle without coupling runner retirement to that
 * compiler.
 */
export class TraceJVMRunnerHost {
  private initialization:
    Promise<{ initializeMs: number; os: RuntimeSystem }> | undefined;
  private readonly processes = new Set<TraceJVMProcessLease>();
  private disposed = false;

  constructor(private readonly options: TraceJVMRunnerHostOptions) {}

  async initialize(
    signal?: AbortSignal,
  ): Promise<{ initializeMs: number }> {
    const initialized = await this.ensureInitialized(signal);
    return { initializeMs: initialized.initializeMs };
  }

  async createProcess(
    options: TraceJVMProcessOptions,
    signal?: AbortSignal,
  ): Promise<TraceJVMProcess> {
    const { os } = await this.ensureInitialized(signal);
    if (this.disposed) {
      throw new Error("TraceJVM runner host has been disposed.");
    }
    const engine = new TraceJVMEngine(
      {
        assets: this.options.assets,
        runtimeProfile: this.options.runtimeProfile,
        heapBytes: options.heapBytes ?? this.options.runnerHeapBytes ??
          (16 << 20),
        retirementAfterExecutions:
          options.retirementAfterExecutions ??
          this.options.retirementAfterExecutions,
        experiments: this.options.experiments,
        limits: this.options.limits,
        workingDirectory: options.workingDirectory,
        hostStandardDescriptors: options.hostStandardDescriptors,
        host: options.host,
      },
      { os },
    );
    let lease!: TraceJVMProcessLease;
    lease = new TraceJVMProcessLease(
      engine,
      options.host !== undefined,
      () => this.processes.delete(lease),
    );
    this.processes.add(lease);
    try {
      await lease.initialize(signal);
      return lease;
    } catch (error) {
      lease.dispose();
      throw error;
    }
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    for (const process of [...this.processes]) process.dispose();
    this.initialization = undefined;
  }

  private ensureInitialized(
    signal?: AbortSignal,
  ): Promise<{ initializeMs: number; os: RuntimeSystem }> {
    if (this.disposed) {
      return Promise.reject(
        new Error("TraceJVM runner host has been disposed."),
      );
    }
    this.initialization ??= this.initializeRuntime(signal).catch((error) => {
      this.initialization = undefined;
      throw error;
    });
    return this.initialization;
  }

  private async initializeRuntime(
    signal?: AbortSignal,
  ): Promise<{ initializeMs: number; os: RuntimeSystem }> {
    const startedAt = performance.now();
    const os = await loadOperatingSystem(this.options, signal);
    if (this.disposed) {
      throw new Error(
        "TraceJVM runner host was disposed during initialization.",
      );
    }
    return {
      initializeMs: performance.now() - startedAt,
      os,
    };
  }
}
