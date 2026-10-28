# TeaVM javac integration

Status: sole TraceJVM 0.4 compiler backend

TraceJVM consumes the upstream
[`konsoletyper/teavm-javac`](https://github.com/konsoletyper/teavm-javac)
browser compiler without vendoring or forking its source. The build:

1. downloads one checksum-pinned upstream source archive;
2. applies the checksum-pinned overlay in `patches/`;
3. embeds the exact OpenJDK 23.0.2+7 `javac` sources used by TraceJVM;
4. compiles every generated SDK class for Java 23; and
5. writes ignored runtime artifacts plus a checksum manifest.

The overlay makes upstream's hard-coded JDK checkout configurable, fixes
`JavaFileObject.isNameCompatible` for sources beneath package directories, and
adds an API for replacing the compiler boot platform with TraceJVM's runner
archive. That last boundary prevents javac from accepting a different Java API
than the VM can execute. The explicit
`compileClasslibEmuJava` release is a correctness guard: without it, a newer
host JDK can put classfile version 69 SDK definitions beside the version 67
compiler, causing valid sources such as records to fail during attribution.
Patch 0004 (Shiklo fork) makes javac's constant folding use the JDK's floating
point instead of TeaVM's: TeaVM compiles `-x` as `0.0 - x`, which lost the sign
of a folded `-0.0`, and its `Double.toString` / `Float.toString` differ from the
JDK's in string constants such as `"" + Double.MIN_VALUE`.

Build and verify:

```sh
JAVA_HOME=/path/to/jdk-23 pnpm build:teavm-javac
pnpm verify:teavm-javac
pnpm test:teavm-javac
```

The build requires a Java 23 host toolchain. Generated files live at
`.cache/teavm-javac/artifacts/`, remain outside Git, and are copied into the
immutable runtime-release tree under `compiler/`. TraceJVM owns the resulting
Java 23 artifact and compatibility claim; upstream remains responsible only
for its unmodified project and default release.

## Cross-browser compiler measurement

The original `measure:teavm-javac` experiment compared the OpenJDK compiler interpreted
by TraceJVM with the same compiler translated to WebAssembly GC. Each backend
compiles six changed versions of one Java 23 source using records, sealed
types, and a pattern switch. Every browser is measured in both backend orders
using a fresh browser launch for each order.

The August 3, 2026 local headless-browser measurement produced:

| Browser | Existing first compile | TeaVM first compile | Existing warm median | TeaVM warm median | Warm speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| Chromium | 2541–2542 ms | 92.6 ms | 780–784 ms | 24.1 ms | 32.4x |
| Firefox | 10193–10690 ms | 115.9 ms | 2717–2755 ms | 115.2 ms | 23.7x |
| WebKit | 2351–2354 ms | 114.3 ms | 734–739 ms | 42.0 ms | 17.5x |

Initialization remains separate from compilation and can happen during
background warmup. With the exact TraceJVM runner platform loaded, TeaVM
compiler initialization measured 527 ms in Chromium, 2.38 s in Firefox, and
827 ms in WebKit. Including initialization, the cold compiler-plus-first-compile
interval fell from about 2.76 s to 620 ms in Chromium, 10.68 s to 2.50 s in
Firefox, and 2.49 s to 941 ms in WebKit.

These are compiler-backend measurements, not full Compile-button latency.
Runner creation, consumer instrumentation, test batching, and application
rendering remain outside the interval and need a separate end-to-end gate.

## Upstream boundary

Do not propose the TraceJVM-specific JDK 23 selection as an upstream default.
After the backend passes TraceJVM's compatibility and lifecycle gates, first
ask upstream whether they want the generally useful build parameters. If they
do, the parameterization can be offered independently while Java 25 remains
their default and sole support promise.
