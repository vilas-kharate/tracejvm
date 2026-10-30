#!/usr/bin/env node

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  lstatSync,
  readFileSync,
  readdirSync,
  statSync,
} from "node:fs";
import { join, relative, sep } from "node:path";

const root = join(import.meta.dirname, "..");
const assetsRoot = join(root, "runtime/assets");
const manifest = JSON.parse(
  readFileSync(join(root, "runtime/manifest.json"), "utf8"),
);
const expectedProfileFiles = [
  "tracekernel-api.jar",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$ProcessIdentity.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$SessionIdentity.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$TerminalWindowSize.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$WatchdogSignal.class",
  "tracekernel-api/io/tracecode/tracekernel/TraceKernel$WatchdogStatus.class",
  "jdk23.jar",
  "jdk23/conf/security/java.policy",
  "jdk23/conf/security/java.security",
  "jdk23/conf/security/policy/unlimited/default_US_export.policy",
  "jdk23/conf/security/policy/unlimited/default_local.policy",
  "jdk23/lib/modules",
  "jdk23/lib/module-packages.map",
  "jdk23/lib/security/default.policy",
  "jdk23/lib/tzdb.dat",
];

function listFiles(directory) {
  return readdirSync(directory, { withFileTypes: true })
    .flatMap((entry) => {
      const path = join(directory, entry.name);
      assert.equal(
        lstatSync(path).isSymbolicLink(),
        false,
        `Runtime assets must not contain symlinks: ${path}`,
      );
      return entry.isDirectory() ? listFiles(path) : [path];
    })
    .sort((left, right) => left.localeCompare(right));
}

function assertMagic(path, expected, label) {
  const actual = readFileSync(path).subarray(0, expected.length);
  assert.equal(
    actual.equals(Buffer.from(expected)),
    true,
    `${label} has an invalid file signature`,
  );
}

assert.equal(manifest.javaVersion, "23.0.2+7");
assert.equal(manifest.emscriptenVersion, "4.0.2");
assert.equal(manifest.buildTools.emsdk.version, manifest.emscriptenVersion);
assert.match(manifest.buildTools.emsdk.revision, /^[0-9a-f]{40}$/);
assert.equal(manifest.buildTools.hostJdk.version, manifest.javaVersion);
assert.deepEqual(
  Object.keys(manifest.buildTools.hostJdk.platforms).sort(),
  ["darwin-arm64", "darwin-x64", "linux-arm64", "linux-x64"],
);
for (const [platform, artifact] of Object.entries(
  manifest.buildTools.hostJdk.platforms,
)) {
  assert.match(artifact.url, /^https:\/\//, `${platform} JDK URL must use HTTPS`);
  assert.match(
    artifact.sha256,
    /^[0-9a-f]{64}$/,
    `${platform} JDK archive must have a SHA-256 digest`,
  );
}
assertMagic(
  join(assetsRoot, "bjvm_main.wasm"),
  Uint8Array.from([0x00, 0x61, 0x73, 0x6d]),
  "bjvm_main.wasm",
);

for (const profile of ["core", "server", "spring-server"]) {
  const profileRoot = join(assetsRoot, "profiles", profile);
  const expectedFiles = new Set([
    ...expectedProfileFiles,
    ...(profile === "core" ? [] : ["jdk23/conf/logging.properties"]),
  ]);
  for (const relativePath of expectedProfileFiles) {
    const path = join(profileRoot, relativePath);
    assert.ok(statSync(path).isFile(), `${profile} is missing ${relativePath}`);
  }
  if (profile !== "core") {
    assert.ok(
      statSync(join(profileRoot, "jdk23/conf/logging.properties")).isFile(),
      `${profile} is missing logging.properties`,
    );
  }
  for (const jar of ["tracekernel-api.jar", "jdk23.jar"]) {
    assertMagic(
      join(profileRoot, jar),
      Uint8Array.from([0x50, 0x4b, 0x03, 0x04]),
      `${profile}/${jar}`,
    );
  }
  const actualFiles = listFiles(profileRoot).map((path) =>
    relative(profileRoot, path).split(sep).join("/"),
  );
  assert.deepEqual(
    actualFiles,
    [...expectedFiles].sort((left, right) => left.localeCompare(right)),
    `${profile} contains stale or unexpected runtime assets`,
  );
}

const files = listFiles(assetsRoot);
const digest = createHash("sha256");
let totalBytes = 0;
for (const path of files) {
  const relativePath = relative(assetsRoot, path).split(sep).join("/");
  const content = readFileSync(path);
  totalBytes += content.byteLength;
  digest.update(relativePath);
  digest.update("\0");
  digest.update(content);
  digest.update("\0");
}

console.log(
  `PASS: verified ${files.length} TraceJVM runtime assets ` +
    `(${totalBytes} bytes, tree sha256 ${digest.digest("hex")})`,
);
