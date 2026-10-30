import * as Effect from "effect/Effect";
import {
  TraceJVMInitializationError,
  TraceJVMOperationError,
  type TraceJVMEngineError,
} from "./errors";
import { runTraceJVMEffect } from "./run-effect";
import { validateRelativePath } from "./path-validation";
import {
  fetchVerifiedTraceJVMAsset,
  importVerifiedTraceJVMModule,
  traceJVMAssetUrl,
  type TraceJVMAssetIntegrityMap,
} from "./asset-integrity";
import {
  resolveTraceJVMResourceLimits,
  type TraceJVMResourceLimits,
  TraceJVMOutputBudget,
  validateTraceJVMCompileResources,
  validateTraceJVMGeneratedResources,
} from "./resource-limits";
import type {
  TraceJVMBinaryFile,
  TraceJVMCompileRequest,
  TraceJVMCompileResult,
  TraceJVMCompiledProgram,
} from "./engine";

export interface TraceJVMCompilerAssets {
  /** Directory containing the immutable TeaVM-javac release artifacts. */
  baseUrl: string;
  /** Trusted build-time metadata keyed by each exact asset URL. */
  integrity: TraceJVMAssetIntegrityMap;
}

export interface TraceJVMCompilerOptions {
  assets: TraceJVMCompilerAssets;
  /**
   * The runner's Java platform archive. Compilation is rejected without an
   * explicit platform so javac can never expose a different API from the VM.
   */
  platformArchiveUrl: string;
  /** Immutable APIs supplied by the host rather than by learner classpaths. */
  platformClasspath?: readonly {
    path: string;
    url: string;
  }[];
  /** Host-selected safety ceilings; defaults are suitable for browser Workers. */
  limits?: Partial<TraceJVMResourceLimits>;
}

interface TeaVMDiagnostic {
  readonly severity: "error" | "warning" | "other";
  readonly fileName?: string | null;
  readonly lineNumber?: number;
  readonly columnNumber?: number;
  readonly message: string;
}

interface TeaVMListenerRegistration {
  destroy(): void;
}

interface TeaVMCompiler {
  /** javac's options, one per line (compiler overlay patch 0005). */
  setOptions(lines: string): void;
  addSourceFile(path: string, content: string): void;
  clearSourceFiles(): void;
  addClassFile(path: string, content: Int8Array): void;
  addJarFile(content: Int8Array): void;
  clearInputClassFiles(): void;
  setSdk(content: Int8Array): void;
  addPlatformJarFile(content: Int8Array): void;
  onDiagnostic(
    listener: (diagnostic: TeaVMDiagnostic) => void,
  ): TeaVMListenerRegistration;
  compile(): boolean;
  listOutputFiles(): ArrayLike<string>;
  getOutputFile(path: string): Int8Array | null;
  clearOutputFiles(): void;
}

interface TeaVMCompilerModule {
  exports: {
    createCompiler(): TeaVMCompiler;
  };
}

interface TeaVMRuntimeModule {
  load(
    wasm: Uint8Array,
    options?: {
      stackDeobfuscator?: {
        enabled: boolean;
        path: Uint8Array;
        infoLocation: "external";
        externalInfoPath: Uint8Array;
      };
    },
  ): Promise<TeaVMCompilerModule>;
}

interface CompilerState {
  compiler: TeaVMCompiler;
  platformClasspath: readonly TraceJVMBinaryFile[];
  initializedAt: number;
  startedAt: number;
}

function abortError(message: string): Error {
  return Object.assign(new Error(message), { name: "AbortError" });
}

function formatDiagnostic(diagnostic: TeaVMDiagnostic): string {
  const location = [
    diagnostic.fileName || undefined,
    Number.isFinite(diagnostic.lineNumber)
      ? String(diagnostic.lineNumber)
      : undefined,
    Number.isFinite(diagnostic.columnNumber)
      ? String(diagnostic.columnNumber)
      : undefined,
  ].filter((part) => part !== undefined).join(":");
  const prefix = location.length > 0 ? `${location}: ` : "";
  return `${prefix}${diagnostic.severity}: ${diagnostic.message}\n`;
}

function signedBytes(content: Uint8Array): Int8Array {
  return new Int8Array(
    content.buffer,
    content.byteOffset,
    content.byteLength,
  );
}

/**
 * Persistent OpenJDK 23 javac compiled ahead of time with TeaVM Wasm GC.
 *
 * This component only transforms source and classpath inputs into ordinary
 * classfiles. It has no VM, process, filesystem, or TraceKernel authority.
 */
export class TraceJVMCompiler {
  private statePromise: Promise<CompilerState> | undefined;
  private disposed = false;
  private operation = Promise.resolve();
  private readonly resourceLimits: TraceJVMResourceLimits;

  constructor(private readonly options: TraceJVMCompilerOptions) {
    this.resourceLimits = resolveTraceJVMResourceLimits(options.limits);
  }

  initializeEffect(
    signal?: AbortSignal,
  ): Effect.Effect<{ initializeMs: number }, TraceJVMInitializationError> {
    return Effect.tryPromise({
      try: (effectSignal) =>
        this.initializePromise(signal ?? effectSignal),
      catch: (cause) => new TraceJVMInitializationError(cause),
    });
  }

  compileEffect(
    request: TraceJVMCompileRequest,
  ): Effect.Effect<TraceJVMCompileResult, TraceJVMEngineError> {
    return Effect.tryPromise({
      try: (effectSignal) =>
        this.compilePromise({
          ...request,
          signal: request.signal ?? effectSignal,
        }),
      catch: (cause) => new TraceJVMOperationError("compile", cause),
    });
  }

  initialize(signal?: AbortSignal): Promise<{ initializeMs: number }> {
    return runTraceJVMEffect(this.initializeEffect(signal));
  }

  compile(request: TraceJVMCompileRequest): Promise<TraceJVMCompileResult> {
    return runTraceJVMEffect(this.compileEffect(request));
  }

  dispose(): void {
    this.disposed = true;
    this.statePromise = undefined;
  }

  private async initializePromise(
    signal?: AbortSignal,
  ): Promise<{ initializeMs: number }> {
    const state = await this.state(signal);
    return { initializeMs: state.initializedAt - state.startedAt };
  }

  private async compilePromise(
    request: TraceJVMCompileRequest,
  ): Promise<TraceJVMCompileResult> {
    if (this.disposed) {
      throw new Error("TraceJVM compiler has been disposed.");
    }
    validateTraceJVMCompileResources(request, this.resourceLimits);
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
      const state = await this.state(request.signal);
      if (request.signal?.aborted) {
        return this.cancelledResult(requestStartedAt, queueCompletedAt);
      }

      const compiler = state.compiler;
      compiler.clearSourceFiles();
      compiler.clearInputClassFiles();
      compiler.clearOutputFiles();
      const options = request.options ?? [];
      for (const option of options) {
        if (typeof option !== "string" || option.length === 0 || /[\r\n]/u.test(option)) {
          throw new TypeError("javac options must be non-empty strings without line breaks.");
        }
      }
      compiler.setOptions(options.join("\n"));
      for (const source of request.sources) {
        validateRelativePath(source.path, "Java source");
        compiler.addSourceFile(source.path, source.content);
      }
      for (const entry of [
        ...state.platformClasspath,
        ...(request.classpath ?? []),
      ]) {
        validateRelativePath(entry.path, "Java classpath");
        if (entry.path.toLowerCase().endsWith(".jar")) {
          compiler.addJarFile(signedBytes(entry.content));
        } else {
          compiler.addClassFile(entry.path, signedBytes(entry.content));
        }
      }

      const diagnostics: TeaVMDiagnostic[] = [];
      const diagnosticBudget = new TraceJVMOutputBudget(
        this.resourceLimits.maxOutputBytes,
      );
      const registration = compiler.onDiagnostic((diagnostic) => {
        diagnosticBudget.consume(formatDiagnostic(diagnostic));
        diagnostics.push({
          severity: diagnostic.severity,
          fileName: diagnostic.fileName,
          lineNumber: diagnostic.lineNumber,
          columnNumber: diagnostic.columnNumber,
          message: diagnostic.message,
        });
      });
      const compileStartedAt = performance.now();
      let ok = false;
      try {
        ok = compiler.compile();
      } finally {
        registration.destroy();
      }
      const completedAt = performance.now();
      if (request.signal?.aborted) {
        return this.cancelledResult(requestStartedAt, queueCompletedAt);
      }

      const stderr = diagnostics.map(formatDiagnostic).join("");
      if (stderr) request.onStderr?.(stderr);
      let program: TraceJVMCompiledProgram | undefined;
      if (ok && !diagnostics.some(({ severity }) => severity === "error")) {
        const files: TraceJVMBinaryFile[] = [];
        for (const path of Array.from(compiler.listOutputFiles())) {
          validateRelativePath(path, "Java compiler output");
          const content = compiler.getOutputFile(path);
          if (content === null) {
            throw new Error(`TeaVM javac omitted declared output: ${path}`);
          }
          files.push({
            path,
            content: new Uint8Array(content),
          });
        }
        validateTraceJVMGeneratedResources(files, this.resourceLimits);
        program = { files };
      }

      return {
        status: program ? "completed" : "compile-error",
        exitCode: program ? 0 : 1,
        stdout: "",
        stderr,
        program,
        diagnostics,
        timings: {
          compilerInitMs: state.initializedAt - state.startedAt,
          queueMs: queueCompletedAt - requestStartedAt,
          compileMs: completedAt - compileStartedAt,
          totalMs: completedAt - requestStartedAt,
        },
      };
    } finally {
      release();
    }
  }

  private cancelledResult(
    startedAt: number,
    queuedAt: number,
  ): TraceJVMCompileResult {
    const completedAt = performance.now();
    return {
      status: "cancelled",
      exitCode: 130,
      stdout: "",
      stderr: "",
      diagnostics: [],
      timings: {
        compilerInitMs: 0,
        queueMs: queuedAt - startedAt,
        compileMs: 0,
        totalMs: completedAt - startedAt,
      },
    };
  }

  private state(signal?: AbortSignal): Promise<CompilerState> {
    if (this.disposed) {
      return Promise.reject(new Error("TraceJVM compiler has been disposed."));
    }
    this.statePromise ??= this.initializeState(signal).catch((error) => {
      this.statePromise = undefined;
      throw error;
    });
    return this.statePromise;
  }

  private async initializeState(
    signal?: AbortSignal,
  ): Promise<CompilerState> {
    if (signal?.aborted) {
      throw abortError("Java compiler initialization was cancelled.");
    }
    const startedAt = performance.now();
    const baseUrl = this.options.assets.baseUrl;
    const integrity = this.options.assets.integrity;
    const runtimeUrl = traceJVMAssetUrl(baseUrl, "compiler.wasm-runtime.js");
    const [
      runtime,
      sdk,
      platform,
      platformClasspath,
      compilerWasm,
      deobfuscatorWasm,
      deobfuscatorInfo,
    ] =
      await Promise.all([
      importVerifiedTraceJVMModule<TeaVMRuntimeModule>(runtimeUrl, integrity),
      fetchVerifiedTraceJVMAsset(
        traceJVMAssetUrl(baseUrl, "compile-classlib-teavm.bin"),
        integrity,
      ),
      fetchVerifiedTraceJVMAsset(this.options.platformArchiveUrl, integrity),
      Promise.all((this.options.platformClasspath ?? []).map(
        async ({ path, url }) => {
          validateRelativePath(path, "compiler platform classpath");
          return {
            path,
            content: await fetchVerifiedTraceJVMAsset(url, integrity),
          };
        },
      )),
      fetchVerifiedTraceJVMAsset(
        traceJVMAssetUrl(baseUrl, "compiler.wasm"),
        integrity,
      ),
      fetchVerifiedTraceJVMAsset(
        traceJVMAssetUrl(baseUrl, "compiler.wasm-deobfuscator.wasm"),
        integrity,
      ),
      fetchVerifiedTraceJVMAsset(
        traceJVMAssetUrl(baseUrl, "compiler.wasm.teadbg"),
        integrity,
      ),
      ]);
    const module = await runtime.load(compilerWasm, {
      stackDeobfuscator: {
        enabled: true,
        path: deobfuscatorWasm,
        infoLocation: "external",
        externalInfoPath: deobfuscatorInfo,
      },
    });
    if (signal?.aborted) {
      throw abortError("Java compiler initialization was cancelled.");
    }
    if (this.disposed) {
      throw new Error("TraceJVM compiler was disposed during initialization.");
    }
    const compiler = module.exports.createCompiler();
    compiler.setSdk(new Int8Array(sdk.buffer, sdk.byteOffset, sdk.byteLength));
    compiler.addPlatformJarFile(
      new Int8Array(platform.buffer, platform.byteOffset, platform.byteLength),
    );
    return {
      compiler,
      platformClasspath,
      startedAt,
      initializedAt: performance.now(),
    };
  }
}
