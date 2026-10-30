#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache_root="$repo_root/.cache"
bjvm_root="$repo_root/engine/bjvm"
runtime_root="$repo_root/runtime/assets"
build_root="$cache_root/single-vm-java23"

export LC_ALL=C
export SOURCE_DATE_EPOCH=946684800
export TZ=UTC
archive_date="2000-01-01T00:00:00Z"

temurin_version="23.0.2_7"
temurin_release="jdk-23.0.2%2B7"
linux_archive="OpenJDK23U-jdk_x64_linux_hotspot_${temurin_version}.tar.gz"
linux_sha256="870ac8c05c6fe563e7a3878a47d0234b83c050e83651d2c47e8b822ec74512dd"
temurin_base_url="https://github.com/adoptium/temurin23-binaries/releases/download/${temurin_release}"
source_revision="ff87e76b386d3f67234ccafc65049b155645ce85"
source_archive="adoptium-jdk23u-${source_revision}.tar.gz"
source_url="https://github.com/adoptium/jdk23u/archive/${source_revision}.tar.gz"
source_sha256="30dc54abfa5267b11d087aca24c8c3d94fa8e1f63059fa999b92142bfce767db"

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

download_checked() {
  local url="$1"
  local destination="$2"
  local expected_sha256="$3"
  if [[ ! -f "$destination" ]]; then
    curl --fail --location --retry 3 --output "$destination" "$url"
  fi
  local actual_sha256
  actual_sha256="$(shasum -a 256 "$destination" | awk '{print $1}')"
  if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "Checksum mismatch for $destination" >&2
    echo "expected: $expected_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 3
  fi
}

require_command curl
require_command jar
require_command javac
require_command java
require_command jimage
require_command jlink
require_command ninja
require_command node
require_command shasum

java_major="$(
  javac -version 2>&1 | sed -E 's/^javac ([0-9]+).*/\1/'
)"
if [[ "$java_major" != "23" ]]; then
  echo "The single-VM build requires JDK 23 tools; javac reports Java $java_major." >&2
  exit 2
fi
if ! command -v emcc >/dev/null 2>&1 || ! emcc --version | head -1 | grep -q "4.0.2"; then
  echo "Activate Emscripten 4.0.2 before building b-jvm." >&2
  exit 2
fi

mkdir -p "$cache_root/temurin-23-linux" "$build_root"
linux_tar="$cache_root/temurin-23-linux/$linux_archive"
download_checked "$temurin_base_url/$linux_archive" "$linux_tar" "$linux_sha256"
mkdir -p "$cache_root/runtime-sources"
download_checked \
  "$source_url" \
  "$cache_root/runtime-sources/$source_archive" \
  "$source_sha256"
linux_home="$cache_root/temurin-23-linux/jdk-23.0.2+7"
if [[ ! -d "$linux_home/jmods" ]]; then
  tar -xzf "$linux_tar" -C "$cache_root/temurin-23-linux"
fi
temurin_legal_root="$cache_root/runtime-legal/temurin-$temurin_version"
rm -rf "$temurin_legal_root"
mkdir -p "$temurin_legal_root/legal"
cp -RL "$linux_home/legal/." "$temurin_legal_root/legal/"
cp "$linux_home/NOTICE" "$temurin_legal_root/NOTICE"

rm -rf \
  "$runtime_root" \
  "$build_root/compiler" \
  "$build_root/compiler-image" \
  "$build_root/bridge" \
  "$build_root/tracekernel-api" \
  "$build_root/profiles"
mkdir -p \
  "$runtime_root/profiles" \
  "$build_root/compiler" \
  "$build_root/bridge" \
  "$build_root/tracekernel-api" \
  "$build_root/profiles"
jlink \
  --module-path "$linux_home/jmods" \
  --add-modules java.compiler,jdk.compiler,jdk.internal.opt,jdk.zipfs \
  --output "$build_root/compiler-image" \
  --no-header-files \
  --no-man-pages \
  --strip-debug \
  --compress=zip-9
jimage extract \
  --dir "$build_root/compiler" \
  "$build_root/compiler-image/lib/modules"
for module in java.compiler jdk.compiler jdk.internal.opt jdk.zipfs; do
  rm -f "$build_root/compiler/$module/module-info.class"
done

javac \
  --patch-module java.base="$repo_root/runtime/bridge" \
  -d "$build_root/bridge" \
  "$repo_root/runtime/bridge/jdk/internal/tracecode/"*.java
javac \
  --release 23 \
  -d "$build_root/tracekernel-api" \
  "$repo_root/runtime/api/io/tracecode/tracekernel/"*.java
jar --create --date="$archive_date" \
  --file "$build_root/tracekernel-api.jar" \
  -C "$build_root/tracekernel-api" .
# The java.base descriptor TeaVM javac compiles against (scripts/java/PlatformDescriptor.java; Shiklo fork).
javac \
  --release 23 --enable-preview \
  -d "$build_root/platform-descriptor" \
  "$repo_root/scripts/java/PlatformDescriptor.java"
# The locales of the runtime images: the languages of the site (each with its regional variants, e.g. de-DE, pt-BR,
# en-IN); English and the root locale are always kept.
locales="en,es,pt,de,fr,ja,hi"
runtime_modules_for_profile() {
  case "$1" in
    # jdk.localedata: number, date and currency formats beyond English, limited by jlink to $locales (Shiklo fork).
    core)
      echo "java.base,jdk.localedata"
      ;;
    server)
      echo "java.base,java.instrument,java.logging,java.management,java.naming,java.net.http,java.sql,java.transaction.xa,java.xml,jdk.httpserver,jdk.localedata,jdk.management,jdk.unsupported,jdk.zipfs"
      ;;
    spring-server)
      echo "java.base,java.desktop,java.instrument,java.logging,java.management,java.naming,java.net.http,java.sql,java.transaction.xa,java.xml,jdk.httpserver,jdk.localedata,jdk.management,jdk.unsupported,jdk.zipfs"
      ;;
    *)
      echo "Unknown TraceJVM runtime profile: $1" >&2
      return 2
      ;;
  esac
}

for profile in core server spring-server; do
  profile_build_root="$build_root/profiles/$profile"
  profile_runtime_root="$runtime_root/profiles/$profile"
  profile_modules="$(runtime_modules_for_profile "$profile")"
  mkdir -p \
    "$profile_build_root/classes" \
    "$profile_runtime_root/jdk23/lib/security" \
    "$profile_runtime_root/jdk23/conf/security"
  jlink \
    --module-path "$linux_home/jmods" \
    --add-modules "$profile_modules" \
    --include-locales="$locales" \
    --output "$profile_build_root/image" \
    --no-header-files \
    --no-man-pages \
    --strip-debug \
    --compress=zip-9
  jimage extract \
    --dir "$profile_build_root/classes" \
    "$profile_build_root/image/lib/modules"
  find "$profile_build_root/classes" -name module-info.class -delete
  module_map="$profile_build_root/module-packages.map"
  : > "$module_map"
  for module_directory in "$profile_build_root/classes"/*; do
    module_name="$(basename "$module_directory")"
    find "$module_directory" -type f -name '*.class' -print |
      while IFS= read -r class_file; do
        relative_path="${class_file#"$module_directory"/}"
        package_name="${relative_path%/*}"
        if [[ "$package_name" != "$relative_path" ]]; then
          printf '%s\t%s\n' "$package_name" "$module_name"
        fi
      done >> "$module_map"
  done
  printf '%s\t%s\n' "jdk/internal/tracecode" "java.base" >> "$module_map"
  sort -u "$module_map" -o "$module_map"

  profile_jar="$profile_runtime_root/jdk23.jar"
  first_module=true
  for module_directory in "$profile_build_root/classes"/*; do
    if [[ "$first_module" == "true" ]]; then
      jar --create --date="$archive_date" --file "$profile_jar" -C "$module_directory" .
      first_module=false
    else
      jar --update --date="$archive_date" --file "$profile_jar" -C "$module_directory" .
    fi
  done
  jar --update --date="$archive_date" --file "$profile_jar" \
    -C "$build_root/bridge" jdk/internal/tracecode
  # TeaVM javac takes java.base's packages from a module-info.class; without this one it used the TeaVM class
  # library's, which lacks java.util.concurrent.locks and every module outside java.base. Added with zipfile, not jar
  # (jar would treat a root module-info.class as a modular jar and check it) (Shiklo fork, 2026-10-28).
  java --enable-preview \
    -cp "$build_root/platform-descriptor" PlatformDescriptor \
    "$profile_modules" "jdk.internal.tracecode" "$profile_build_root/module-info.class"
  python3 - "$profile_jar" "$profile_build_root/module-info.class" <<'PYTHON'
import sys, zipfile
entry = zipfile.ZipInfo("module-info.class", date_time=(2000, 1, 1, 0, 0, 0))
entry.compress_type = zipfile.ZIP_DEFLATED
with open(sys.argv[2], "rb") as descriptor, zipfile.ZipFile(sys.argv[1], "a") as archive:
    archive.writestr(entry, descriptor.read())
PYTHON

  cp "$build_root/tracekernel-api.jar" \
    "$profile_runtime_root/tracekernel-api.jar"
  cp -R "$build_root/tracekernel-api" \
    "$profile_runtime_root/tracekernel-api"
  # The runner needs its module image for ClassLoader resource lookups and
  # other runtime reflection. This is runtime state, not a compiler capability.
  cp "$profile_build_root/image/lib/modules" \
    "$profile_runtime_root/jdk23/lib/modules"
  cp "$module_map" "$profile_runtime_root/jdk23/lib/module-packages.map"
  cp "$linux_home/lib/tzdb.dat" "$profile_runtime_root/jdk23/lib/tzdb.dat"
  cp "$linux_home/lib/security/default.policy" \
    "$profile_runtime_root/jdk23/lib/security/default.policy"
  cp "$linux_home/conf/security/java.security" \
    "$profile_runtime_root/jdk23/conf/security/java.security"
  cp "$linux_home/conf/security/java.policy" \
    "$profile_runtime_root/jdk23/conf/security/java.policy"
  # The JCE jurisdiction policy that java.security's crypto.policy=unlimited names (Shiklo fork, 2026-10-30): without
  # it javax.crypto could not start ("Can't read cryptographic policy directory"): Cipher, Mac and KeyGenerator failed.
  mkdir -p "$profile_runtime_root/jdk23/conf/security/policy/unlimited"
  for policy in default_US_export.policy default_local.policy; do
    cp "$linux_home/conf/security/policy/unlimited/$policy" \
      "$profile_runtime_root/jdk23/conf/security/policy/unlimited/$policy"
  done
  if [[ "$profile" != "core" ]]; then
    cp "$linux_home/conf/logging.properties" \
      "$profile_runtime_root/jdk23/conf/logging.properties"
  fi
done

# JDK tools for the Shiklo Java page's terminal (Shiklo fork, 2026-10-29): javap, jar and jshell run on the VM from
# class-path jars made from this JDK's own jmods, which the page loads only when a tool is typed. jdk-tools.jar holds
# jar, javap and javac's API (javax.tools, with a ToolProvider that finds tools on a class path); jdk-jshell.jar holds
# jshell and what it needs besides (line editing, JDI, markdown doc comments). java.prefs is not included: a class-path
# class cannot be in a java.* package; the page gives jshell its settings in memory instead.
tools_root="$runtime_root/tools"
tool_provider_root="$build_root/tool-provider"
rm -rf "$tools_root" "$tool_provider_root"
mkdir -p "$tools_root" "$tool_provider_root"
javac \
  --patch-module "java.compiler=$repo_root/shiklo/tools/src" \
  -d "$tool_provider_root" \
  "$repo_root/shiklo/tools/src/javax/tools/ToolProvider.java"
java "$repo_root/shiklo/tools/MakeToolJar.java" "$tools_root/jdk-tools.jar" "$tool_provider_root" \
  "$linux_home/jmods/java.compiler.jmod" \
  "$linux_home/jmods/jdk.internal.opt.jmod" \
  "$linux_home/jmods/jdk.compiler.jmod" \
  "$linux_home/jmods/jdk.jartool.jmod" \
  "$linux_home/jmods/jdk.jdeps.jmod"
java "$repo_root/shiklo/tools/MakeToolJar.java" "$tools_root/jdk-jshell.jar" - \
  "$linux_home/jmods/jdk.jshell.jmod" \
  "$linux_home/jmods/jdk.internal.le.jmod" \
  "$linux_home/jmods/jdk.internal.ed.jmod" \
  "$linux_home/jmods/jdk.internal.md.jmod" \
  "$linux_home/jmods/jdk.jdi.jmod"

hot_aot_generator_root="$build_root/hot-aot-generator"
mkdir -p "$hot_aot_generator_root"
javac \
  --enable-preview \
  --release 23 \
  -d "$hot_aot_generator_root" \
  "$repo_root/scripts/java/GenerateHotAot.java"
java \
  --enable-preview \
  -cp "$hot_aot_generator_root" \
  GenerateHotAot \
  "$build_root/profiles/core/classes/java.base" \
  "$build_root/compiler/jdk.compiler" \
  "$bjvm_root/vm/generated_hot_aot.inc"

cp "$bjvm_root/codegen/wasm-opt.js" "$bjvm_root/codegen/wasm-opt.cjs"
emcmake cmake \
  -S "$bjvm_root" \
  -B "$bjvm_root/build-tracecode" \
  -G Ninja \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  "-DTRACEJVM_REPRODUCIBLE_SOURCE_ROOT=$repo_root"
ninja -C "$bjvm_root/build-tracecode" bjvm_main
cp "$bjvm_root/build-tracecode/bjvm_main.wasm" \
  "$runtime_root/bjvm_main.wasm"
mkdir -p "$bjvm_root/build"
cp "$bjvm_root/build-tracecode/bjvm_main.js" "$bjvm_root/build/bjvm_main.js"
cp "$bjvm_root/build-tracecode/bjvm_main.d.ts" "$bjvm_root/build/bjvm_main.d.ts"

pnpm --dir "$repo_root" exec esbuild "$repo_root/src/browser-worker.ts" \
  --bundle --format=esm --platform=browser --target=es2022 \
  --alias:"@b-jvm/bjvm2"="$bjvm_root/js/bjvm2.ts" \
  --outfile="$repo_root/dist/browser-worker.js"
pnpm --dir "$repo_root" exec tsc -p "$repo_root/tsconfig.json" --noEmit

echo "Built TraceJVM Java 23 runtime assets."
