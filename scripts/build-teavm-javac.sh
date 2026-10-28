#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/compiler/teavm-javac/manifest.json"
patches=(
  "$repo_root/compiler/teavm-javac/patches/0001-configurable-jdk-build.patch"
  "$repo_root/compiler/teavm-javac/patches/0002-nested-source-paths.patch"
  "$repo_root/compiler/teavm-javac/patches/0003-platform-archive.patch"
  "$repo_root/compiler/teavm-javac/patches/0004-shiklo-float-constants.patch"
)
cache_root="$repo_root/.cache/teavm-javac"
output_root="$cache_root/artifacts"

manifest_value() {
  node -e '
    const manifest = require(process.argv[1]);
    const value = process.argv[2].split(".")
      .reduce((current, key) => current[key], manifest);
    process.stdout.write(String(value));
  ' "$manifest" "$1"
}

sha256() {
  shasum -a 256 "$1" | awk '{print $1}'
}

verify_overlay() {
  local source_root="$1"
  while IFS=$'\t' read -r relative_path expected_sha256; do
    local actual_sha256
    actual_sha256="$(sha256 "$source_root/$relative_path")"
    if [[ "$actual_sha256" != "$expected_sha256" ]]; then
      echo "TeaVM javac overlay file checksum mismatch: $relative_path" >&2
      echo "expected: $expected_sha256" >&2
      echo "actual:   $actual_sha256" >&2
      return 3
    fi
  done < <(
    node -e '
      const manifest = require(process.argv[1]);
      for (const [path, checksum] of Object.entries(
        manifest.overlay.patchedFiles,
      )) {
        process.stdout.write(`${path}\t${checksum}\n`);
      }
    ' "$manifest"
  )
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

require_command curl
require_command node
require_command patch
require_command shasum
require_command tar

upstream_commit="$(manifest_value upstream.commit)"
archive_url="$(manifest_value upstream.archiveUrl)"
archive_sha256="$(manifest_value upstream.archiveSha256)"
overlay_version="$(manifest_value overlay.version)"
jdk_feature="$(manifest_value jdk.feature)"
jdk_version="$(manifest_value jdk.version)"
jdk_repository="$(manifest_value jdk.repository)"
jdk_archive_root="$(manifest_value jdk.archiveRoot)"
jdk_revision="$(manifest_value jdk.revision)"
jdk_archive_sha256="$(manifest_value jdk.archiveSha256)"

for index in "${!patches[@]}"; do
  expected_patch_sha256="$(
    node -e '
      const manifest = require(process.argv[1]);
      process.stdout.write(manifest.overlay.patches[Number(process.argv[2])].sha256);
    ' "$manifest" "$index"
  )"
  if [[ "$(sha256 "${patches[$index]}")" != "$expected_patch_sha256" ]]; then
    echo "TeaVM javac overlay checksum does not match manifest.json: ${patches[$index]}" >&2
    exit 3
  fi
done

java_home="${TRACEJVM_JAVA23_HOME:-${JAVA_HOME:-}}"
if [[ \
  -z "$java_home" ||
  ! -x "$java_home/bin/java" ||
  ! -x "$java_home/bin/javac" \
]]; then
  echo "Set TRACEJVM_JAVA23_HOME or JAVA_HOME to JDK $jdk_version." >&2
  exit 2
fi
java_major="$("$java_home/bin/javac" -version 2>&1 | sed -E 's/^javac ([0-9]+).*/\1/')"
if [[ "$java_major" != "$jdk_feature" ]]; then
  echo "TeaVM javac build requires JDK $jdk_feature; javac reports $java_major." >&2
  exit 2
fi
java_runtime_version="$(
  "$java_home/bin/java" -XshowSettings:properties -version 2>&1 |
    sed -n -E 's/^[[:space:]]*java\.runtime\.version = (.*)$/\1/p'
)"
if [[ "$java_runtime_version" != "$jdk_version" ]]; then
  echo "TeaVM javac build requires JDK $jdk_version." >&2
  echo "java.runtime.version reports $java_runtime_version." >&2
  exit 2
fi

mkdir -p "$cache_root/downloads" "$output_root"
archive="$cache_root/downloads/teavm-javac-$upstream_commit.tar.gz"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 --output "$archive" "$archive_url"
fi
actual_archive_sha256="$(sha256 "$archive")"
if [[ "$actual_archive_sha256" != "$archive_sha256" ]]; then
  echo "TeaVM javac source archive checksum mismatch." >&2
  echo "expected: $archive_sha256" >&2
  echo "actual:   $actual_archive_sha256" >&2
  exit 3
fi

source_root="$cache_root/source-$upstream_commit-overlay-$overlay_version"
if [[ ! -d "$source_root" ]]; then
  staging="$(mktemp -d "$cache_root/source-staging.XXXXXX")"
  cleanup_staging() {
    if [[ -n "${staging:-}" && "$staging" == "$cache_root"/source-staging.* ]]; then
      rm -rf "$staging"
    fi
  }
  trap cleanup_staging EXIT
  tar -xzf "$archive" -C "$staging" --strip-components=1
  for patch_file in "${patches[@]}"; do
    patch -d "$staging" -p1 -i "$patch_file" -N -t
  done
  mv "$staging" "$source_root"
  staging=""
  trap - EXIT
fi
verify_overlay "$source_root"

export JAVA_HOME="$java_home"
export LC_ALL=C
export SOURCE_DATE_EPOCH=946684800
export TZ=UTC

"$source_root/gradlew" \
  -p "$source_root" \
  :compiler:build \
  --no-daemon \
  "-Pjdk.revision=$jdk_revision" \
  "-Ptracejvm.jdk.feature=$jdk_feature" \
  "-Ptracejvm.jdk.repository=$jdk_repository" \
  "-Ptracejvm.jdk.archive-root=$jdk_archive_root"

generated="$source_root/compiler/build/generated/teavm/wasm-gc"
classlib="$source_root/compiler/build/classlib"
jdk_source_archive="$source_root/javac/build/jdk-$jdk_revision.zip"
if [[ "$(sha256 "$jdk_source_archive")" != "$jdk_archive_sha256" ]]; then
  echo "OpenJDK compiler source archive checksum mismatch." >&2
  exit 3
fi
mkdir -p "$repo_root/.cache/runtime-sources"
install -m 0644 \
  "$jdk_source_archive" \
  "$repo_root/.cache/runtime-sources/openjdk-jdk23u-$jdk_revision.zip"
install -m 0644 \
  "$archive" \
  "$repo_root/.cache/runtime-sources/teavm-javac-$upstream_commit.tar.gz"
for name in \
  compiler.wasm \
  compiler.wasm-runtime.js \
  compiler.wasm-deobfuscator.wasm \
  compiler.wasm.teadbg; do
  install -m 0644 "$generated/$name" "$output_root/$name"
done
install -m 0644 \
  "$classlib/compile-classlib-teavm.bin" \
  "$output_root/compile-classlib-teavm.bin"

node "$repo_root/scripts/teavm-javac-artifacts.mjs" write
