#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  createWriteStream,
  existsSync,
  mkdirSync,
  readFileSync,
  readlinkSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  symlinkSync,
} from "node:fs";
import { dirname, join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";

const repoRoot = join(import.meta.dirname, "..");
const manifest = JSON.parse(
  readFileSync(join(repoRoot, "runtime", "manifest.json"), "utf8"),
);
const arguments_ = process.argv.slice(2);
const rootArguments = arguments_.filter((value) => value.startsWith("--root="));
for (const argument of arguments_) {
  if (!["--build", "--offline"].includes(argument) && !argument.startsWith("--root=")) {
    fail(`Unknown argument: ${argument}.`);
  }
}
if (rootArguments.length > 1) fail("Pass --root at most once.");
const rootArgument = rootArguments[0];
if (rootArgument === "--root=") fail("--root must not be empty.");
const toolchainRoot = resolve(
  rootArgument?.slice("--root=".length) ??
    process.env.TRACEJVM_TOOLCHAIN_ROOT ??
    join(repoRoot, ".cache", "frozen-toolchain"),
);
const shouldBuild = arguments_.includes("--build");
const offline = arguments_.includes("--offline");

function fail(message) {
  throw new Error(message);
}

function pathWithoutSpaces(path, label) {
  if (!/\s/u.test(path)) return path;
  const alias = join(
    tmpdir(),
    `${label}-${createHash("sha256").update(path).digest("hex").slice(0, 12)}`,
  );
  if (existsSync(alias)) {
    if (readlinkSync(alias) !== path) {
      fail(`Build-path alias already points elsewhere: ${alias}.`);
    }
  } else {
    symlinkSync(path, alias, "dir");
  }
  return alias;
}

function run(command, commandArguments, options = {}) {
  const capture = options.capture === true;
  const result = spawnSync(command, commandArguments, {
    cwd: options.cwd ?? repoRoot,
    env: options.env ?? process.env,
    encoding: "utf8",
    stdio: capture ? "pipe" : "inherit",
  });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    fail(
      `${command} ${commandArguments.join(" ")} failed` +
        (capture ? `:\n${result.stderr || result.stdout}` : "."),
    );
  }
  return capture ? `${result.stdout}\n${result.stderr}`.trim() : "";
}

function sha256(path) {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

async function download(artifact, destination) {
  if (existsSync(destination) && sha256(destination) === artifact.sha256) return;
  if (offline) fail(`Offline bootstrap is missing ${artifact.archive}.`);
  mkdirSync(dirname(destination), { recursive: true });
  const temporary = `${destination}.partial-${process.pid}`;
  rmSync(temporary, { force: true });
  const response = await fetch(artifact.url, { redirect: "follow" });
  if (!response.ok || !response.body) {
    fail(`Download failed for ${artifact.url}: HTTP ${response.status}.`);
  }
  await pipeline(Readable.fromWeb(response.body), createWriteStream(temporary));
  const actual = sha256(temporary);
  if (actual !== artifact.sha256) {
    rmSync(temporary, { force: true });
    fail(
      `${artifact.archive} SHA-256 mismatch: expected ${artifact.sha256}, got ${actual}.`,
    );
  }
  renameSync(temporary, destination);
}

function extractArchive(archive, destination) {
  if (existsSync(destination)) return;
  const staging = `${destination}.stage-${process.pid}`;
  rmSync(staging, { recursive: true, force: true });
  mkdirSync(staging, { recursive: true });
  run("tar", ["-xzf", archive, "-C", staging]);
  const entries = readdirSync(staging).filter((entry) => entry !== ".DS_Store");
  if (entries.length !== 1 || !statSync(join(staging, entries[0])).isDirectory()) {
    fail(`Expected ${archive} to contain one root directory.`);
  }
  renameSync(join(staging, entries[0]), destination);
  rmSync(staging, { recursive: true, force: true });
}

function findJavaHome(directory, depth = 0) {
  if (existsSync(join(directory, "bin", "java"))) return directory;
  if (depth >= 4) return undefined;
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const found = findJavaHome(join(directory, entry.name), depth + 1);
    if (found) return found;
  }
  return undefined;
}

function prepareEmsdk(destination) {
  const emsdk = manifest.buildTools.emsdk;
  if (!existsSync(join(destination, ".git"))) {
    if (offline) fail("Offline bootstrap is missing the pinned Emsdk checkout.");
    mkdirSync(dirname(destination), { recursive: true });
    const staging = `${destination}.stage-${process.pid}`;
    rmSync(staging, { recursive: true, force: true });
    mkdirSync(staging, { recursive: true });
    run("git", ["init"], { cwd: staging });
    run("git", ["remote", "add", "origin", emsdk.repository], { cwd: staging });
    run("git", ["fetch", "--depth=1", "origin", emsdk.revision], { cwd: staging });
    run("git", ["checkout", "--detach", "FETCH_HEAD"], { cwd: staging });
    renameSync(staging, destination);
  }
  const actual = run("git", ["rev-parse", "HEAD"], {
    cwd: destination,
    capture: true,
  });
  if (actual !== emsdk.revision) {
    fail(`Emsdk checkout mismatch: expected ${emsdk.revision}, got ${actual}.`);
  }
  const emcc = join(destination, "upstream", "emscripten", "emcc");
  const environmentScript = join(destination, "emsdk_env.sh");
  if (offline) {
    if (!existsSync(emcc) || !existsSync(environmentScript)) {
      fail(`Offline bootstrap has not installed Emscripten ${emsdk.version}.`);
    }
  } else {
    run("python3", [join(destination, "emsdk.py"), "install", emsdk.version], {
      cwd: destination,
    });
    run("python3", [join(destination, "emsdk.py"), "activate", emsdk.version], {
      cwd: destination,
    });
  }
  const versionOutput = run(
    "bash",
    [
      "-c",
      'source "$1" >/dev/null && exec emcc --version',
      "tracejvm-toolchain-bootstrap",
      environmentScript,
    ],
    { capture: true },
  );
  if (!versionOutput.includes(`emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) ${emsdk.version}`)) {
    fail(`Installed Emscripten does not report ${emsdk.version}.`);
  }
  return environmentScript;
}

function platformKey() {
  if (!["darwin", "linux"].includes(process.platform)) {
    fail(`Unsupported TraceJVM build host: ${process.platform}-${process.arch}.`);
  }
  const architecture =
    process.arch === "x64" ? "x64" : process.arch === "arm64" ? "arm64" : undefined;
  if (!architecture) fail(`Unsupported TraceJVM build architecture: ${process.arch}.`);
  return `${process.platform}-${architecture}`;
}

function runWithEmsdk(environmentScript, buildScript, environment) {
  run(
    "bash",
    [
      "-c",
      'source "$1" >/dev/null && exec bash "$2"',
      "tracejvm-toolchain-bootstrap",
      environmentScript,
      buildScript,
    ],
    { env: environment },
  );
}

const platform = platformKey();
const jdkArtifact = manifest.buildTools.hostJdk.platforms[platform];
if (!jdkArtifact) fail(`No frozen TraceJVM host JDK for ${platform}.`);
mkdirSync(toolchainRoot, { recursive: true });
const buildToolchainRoot = pathWithoutSpaces(toolchainRoot, "tracejvm-toolchain");
const buildRepoRoot = pathWithoutSpaces(repoRoot, "tracejvm-source");

const archive = join(toolchainRoot, "downloads", jdkArtifact.archive);
await download(jdkArtifact, archive);
const jdkRoot = join(buildToolchainRoot, "jdk-23.0.2+7");
extractArchive(archive, jdkRoot);
const javaHome = findJavaHome(jdkRoot);
if (!javaHome) fail(`${jdkArtifact.archive} did not contain a JDK home.`);
const runtimeVersion = run(
  join(javaHome, "bin", "java"),
  ["-XshowSettings:properties", "-version"],
  { capture: true },
);
if (!runtimeVersion.includes(`java.runtime.version = ${manifest.buildTools.hostJdk.version}`)) {
  fail(`Host JDK does not report ${manifest.buildTools.hostJdk.version}.`);
}

const emsdkRoot = join(buildToolchainRoot, "emsdk");
const emsdkEnvironment = prepareEmsdk(emsdkRoot);
// The pinned JDK goes first on PATH: build-runtime.sh calls javac, jlink and jimage by name, and jlink must be the
// exact JDK of the modules it packs (a build host's own Java, e.g. 17 on GitHub's Ubuntu runners, was found first).
const environment = {
  ...process.env,
  JAVA_HOME: javaHome,
  TRACEJVM_JAVA23_HOME: javaHome,
  PATH: `${join(javaHome, "bin")}:${process.env.PATH ?? ""}`,
};

if (shouldBuild) {
  runWithEmsdk(
    emsdkEnvironment,
    join(buildRepoRoot, "scripts", "build-teavm-javac.sh"),
    environment,
  );
  runWithEmsdk(
    emsdkEnvironment,
    join(buildRepoRoot, "scripts", "build-runtime.sh"),
    environment,
  );
  run("node", [join(buildRepoRoot, "scripts", "verify-runtime-assets.mjs")], {
    env: environment,
  });
  run("node", [join(buildRepoRoot, "scripts", "teavm-javac-artifacts.mjs"), "verify"], {
    env: environment,
  });
}

console.log(
  JSON.stringify(
    {
      platform,
      root: toolchainRoot,
      buildToolchainRoot,
      buildRepoRoot,
      javaHome,
      emsdkRoot,
      emsdkRevision: manifest.buildTools.emsdk.revision,
      emscriptenVersion: manifest.buildTools.emsdk.version,
      built: shouldBuild,
    },
    null,
    2,
  ),
);
