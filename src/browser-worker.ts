/// <reference lib="webworker" />

import {
  TraceJVMEngine,
} from "./engine";
import { TraceJVMCompiler } from "./compiler";
import type {
  TraceJVMWorkerRequest,
  TraceJVMWorkerResponse,
} from "./worker-protocol";
import {
  makeTraceJVMSynchronousHost,
} from "./host-channel";
import type { TraceJVMHostRequest } from "./host";

const scope = self as unknown as DedicatedWorkerGlobalScope;
let engine: TraceJVMEngine | undefined;
let compiler: TraceJVMCompiler | undefined;
let nextHostCallId = 0;
const operations = new Map<number, AbortController>();
const pendingHostCalls = new Map<
  number,
  {
    resolve: (value: unknown) => void;
    reject: (error: Error) => void;
  }
>();

function post(message: TraceJVMWorkerResponse, transfer: Transferable[] = []): void {
  scope.postMessage(message, transfer);
}

function describeThrowable(error: unknown): string {
  // A tagged error sets its message after its stack was taken, so the stack alone can lack it (Shiklo fork,
  // 2026-10-30: a compile failure arrived as "TraceJVMOperationError at catch ..." without its cause).
  if (error instanceof Error) {
    const stack = error.stack ?? "";
    if (error.message && !stack.includes(error.message)) {
      return `${error.name}: ${error.message}\n${stack}`;
    }
    return stack || error.message;
  }
  try {
    return String(error);
  } catch {
    return "TraceJVM failed with an unprintable error";
  }
}

function dispatchHostAsync(
  request: TraceJVMHostRequest,
): Promise<unknown> {
  const id = ++nextHostCallId;
  return new Promise((resolve, reject) => {
    pendingHostCalls.set(id, { resolve, reject });
    post({
      id,
      type: "host-call-async",
      request,
    });
  });
}

function rejectPendingHostCalls(message: string): void {
  for (const pending of pendingHostCalls.values()) {
    pending.reject(new Error(message));
  }
  pendingHostCalls.clear();
}

function getEngine(
  request: Extract<TraceJVMWorkerRequest, { type: "initialize" }>,
): TraceJVMEngine {
  if (compiler) {
    throw new Error(
      "TraceJVM Worker is already initialized as a compiler.",
    );
  }
  engine ??= new TraceJVMEngine({
    ...request.options,
    ...(request.hostChannel
      ? {
          host: {
            ...makeTraceJVMSynchronousHost(
              request.hostChannel,
              (hostRequest) => post({
                id: ++nextHostCallId,
                type: "host-call",
                request: hostRequest,
              }),
            ),
            dispatch: dispatchHostAsync,
          },
        }
      : {}),
  });
  return engine;
}

scope.onmessage = async (event: MessageEvent<TraceJVMWorkerRequest>) => {
  const message = event.data;
  if (message.type === "host-response-async") {
    const pending = pendingHostCalls.get(message.id);
    if (!pending) return;
    pendingHostCalls.delete(message.id);
    if (message.response.ok) {
      pending.resolve(message.response.value);
    } else {
      const failure = message.response.error;
      const error = new Error(failure.message);
      error.name = failure.name;
      if (failure.code) {
        Object.defineProperty(error, "code", {
          configurable: true,
          enumerable: true,
          value: failure.code,
        });
      }
      pending.reject(error);
    }
    return;
  }
  if (message.type === "cancel") {
    operations.get(message.targetId)?.abort();
    return;
  }
  if (message.type === "dispose") {
    engine?.dispose();
    engine = undefined;
    compiler?.dispose();
    compiler = undefined;
    operations.clear();
    rejectPendingHostCalls("TraceJVM disposed during a host call.");
    post({ id: message.id, type: "disposed" });
    return;
  }

  const controller = new AbortController();
  operations.set(message.id, controller);
  const stream = {
    signal: controller.signal,
    onStdout: (chunk: string) =>
      post({ id: message.id, type: "stream", stream: "stdout", chunk }),
    onStderr: (chunk: string) =>
      post({ id: message.id, type: "stream", stream: "stderr", chunk }),
  };

  try {
    if (message.type === "initialize") {
      const runtime = getEngine(message);
      const result = await runtime.initialize(controller.signal);
      post({ id: message.id, type: "initialized", result });
      return;
    }
    if (message.type === "initialize-compiler") {
      if (engine) {
        throw new Error(
          "TraceJVM Worker is already initialized as a runner.",
        );
      }
      compiler ??= new TraceJVMCompiler(message.options);
      const result = await compiler.initialize(controller.signal);
      post({ id: message.id, type: "compiler-initialized", result });
      return;
    }
    if (message.type === "compile") {
      if (!compiler) {
        throw new Error("TraceJVM compiler must be initialized before use.");
      }
      const result = await compiler.compile({
        ...message.request,
        ...stream,
      });
      const transfer =
        result.program?.files.map((file) => file.content.buffer) ?? [];
      post({
        id: message.id,
        type: "compile-result",
        result,
      }, transfer);
      return;
    }
    if (!engine) {
      throw new Error("TraceJVM must be initialized before use.");
    }
    const result = await engine.run({ ...message.request, ...stream });
    post({ id: message.id, type: "run-result", result });
  } catch (error) {
    post({ id: message.id, type: "error", error: describeThrowable(error) });
  } finally {
    operations.delete(message.id);
  }
};
