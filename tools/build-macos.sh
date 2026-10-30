#!/usr/bin/env bash
# Native Apple Silicon build. Requires Xcode command-line tools, CMake, Python 3,
# git and curl. Place the supported disc files in game/assets first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || {
  echo 'This build script targets Apple Silicon macOS.' >&2; exit 1;
}
RUN=false
if [ "${1:-}" = --run ]; then RUN=true; shift; fi
[ "$#" = 0 ] || { echo "Usage: $0 [--run]" >&2; exit 1; }
for tool in cmake python3 git curl clang clang++ make; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
WORK="${CRACKDOWN_MAC_WORK:-$ROOT/out/macos}"
SDK="${CRACKDOWN_SDK_DIR:-$ROOT/tools/rexglue/mac-arm64}"
SOURCE="${CRACKDOWN_MAC_SDK_SOURCE:-$WORK/rexglue-sdk}"
SDK_BUILD="${CRACKDOWN_MAC_SDK_BUILD:-$WORK/sdk-build}"
BUILD="${CRACKDOWN_MAC_BUILD:-$ROOT/out/build/mac-arm64-release}"
JOBS="${CRACKDOWN_BUILD_JOBS:-4}"
mkdir -p "$WORK" "$(dirname "$SDK")"

python3 - "$ROOT" <<'PY'
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1])
xex = root / 'game/assets/Default.xex'
if not xex.is_file():
    raise SystemExit('Missing game/assets/Default.xex: extract your supported disc first.')
expected = '7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a'
if hashlib.sha256(xex.read_bytes()).hexdigest() != expected:
    raise SystemExit('Unsupported Default.xex: the original retail executable is required.')
PY

if [ ! -f "$SDK/lib/cmake/rexglue/rexglueConfig.cmake" ]; then
  ARCHIVE="$WORK/rexglue-sdk-0.10.0-mac-arm64.zip"
  [ -f "$ARCHIVE" ] || curl -fL --retry 2 \
    https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-mac-arm64.zip -o "$ARCHIVE"
  python3 - "$ARCHIVE" "$(dirname "$SDK")" <<'PY'
import hashlib, pathlib, sys, zipfile
archive = pathlib.Path(sys.argv[1])
expected = '1192638b51a6963aa6a4f24b77ebcb90c16f7fd0b91b311e11a90100f5274326'
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('The macOS SDK archive does not match the official release checksum.')
with zipfile.ZipFile(archive) as z:
    z.extractall(sys.argv[2])
PY
  chmod +x "$SDK/bin/rexglue"
fi

if [ ! -d "$SOURCE/.git" ]; then
  git clone --depth 1 --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git "$SOURCE"
fi
[ "$(git -C "$SOURCE" rev-parse HEAD)" = f5337cdc947ff6d4c4196737e2c807a48f2a1fc2 ] || {
  echo 'The SDK source is not at the pinned v0.10.0 commit.' >&2; exit 1;
}
git -C "$SOURCE" submodule update --init --depth 1 --jobs "$JOBS"

python3 - "$ROOT" "$SOURCE" <<'PY'
import hashlib, pathlib, subprocess, sys
root, source = map(pathlib.Path, sys.argv[1:])
patches = [p for p in sorted((root / 'patches/rexglue-sdk').glob('*.patch'))
           if not p.name.endswith('.windows.patch')]
patches += sorted((root / 'patches/rexglue-sdk-macos').glob('*.patch'))
fingerprint = hashlib.sha256(b''.join(p.name.encode() + p.read_bytes() for p in patches)).hexdigest()
stamp = source / '.crackdown-macos-patches'
if stamp.exists():
    if stamp.read_text().strip() != fingerprint:
        raise SystemExit('SDK patches changed. Use a fresh CRACKDOWN_MAC_SDK_SOURCE directory; local edits will not be reset.')
else:
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=no'])
    if dirty:
        raise SystemExit('SDK source has unrecorded local changes. Use a fresh CRACKDOWN_MAC_SDK_SOURCE directory.')
    for patch in patches:
        subprocess.run(['git', '-C', str(source), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(source), 'apply', str(patch)], check=True)
    stamp.write_text(fingerprint + '\n')
PY

cmake -S "$SOURCE" -B "$SDK_BUILD" -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_C_FLAGS=-march=armv8-a -DCMAKE_CXX_FLAGS=-march=armv8-a \
  "-DREXGLUE_MAC_VULKAN_PREBUILT_ROOT=$SDK" -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$SDK_BUILD" --target rexglue rexgpu-xenos --parallel "$JOBS"
python3 - "$SOURCE" "$SDK" <<'PY'
import pathlib, shutil, sys
source, sdk = map(pathlib.Path, sys.argv[1:])
for name in ('librexruntime.dylib', 'librexgpu-xenos.dylib'):
    shutil.copy2(source / 'out/mac-arm64' / name, sdk / 'lib' / name)
shutil.copy2(source / 'out/mac-arm64/rexglue', sdk / 'bin/rexglue')
shutil.copytree(source / 'include/rex', sdk / 'include/rex', dirs_exist_ok=True)
PY
python3 "$ROOT/tools/check-macos-sdk.py" --sdk "$SDK" --record-source "$SOURCE"
git -C "$ROOT" submodule update --init --depth 1 third_party/trg-launcher
(
  cd "$ROOT"
  "$SOURCE/out/mac-arm64/rexglue" codegen --ignore-stamp crackdown_manifest.toml
)
cmake -S "$ROOT" -B "$BUILD" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
  "-DCRACKDOWN_SDK_DIR=$SDK" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$BUILD" --parallel "$JOBS"
echo "Native executable: $BUILD/crackdown"
if $RUN; then
  exec "$BUILD/crackdown" "--game_data_root=$ROOT/game/assets"
fi
