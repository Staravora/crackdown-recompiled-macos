#!/usr/bin/env bash
# Builds and runs the pure-logic tests in tests/ (no game files, no ReXGlue runtime) with clang++-20 on Linux.
#
#   tools/test.sh                  build and run every test
#   tools/test.sh user_data        only tests whose name matches
#
# CRACKDOWN_TEST_BUILD sets the build folder (default out/tests-linux); CRACKDOWN_SDK_DIR the unpacked linux-amd64 SDK
# (default tools/rexglue/linux-amd64).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in
  Darwin) PLATFORM=macos; SDK_PLATFORM=mac-arm64; GENERATOR='Unix Makefiles'; CC=clang; CXX=clang++ ;;
  Linux) PLATFORM=linux; SDK_PLATFORM=linux-amd64; GENERATOR=Ninja; CC=clang-20; CXX=clang++-20 ;;
  *) echo "ERROR: use tools/test.ps1 on Windows" >&2; exit 1 ;;
esac
BUILD="${CRACKDOWN_TEST_BUILD:-$ROOT/out/tests-$PLATFORM}"

cmake -S "$ROOT/tests" -B "$BUILD" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_CXX_COMPILER=$CXX" \
  "-DCRACKDOWN_SDK_DIR=${CRACKDOWN_SDK_DIR:-$ROOT/tools/rexglue/$SDK_PLATFORM}"
cmake --build "$BUILD" --target crackdown_tests --parallel "${CRACKDOWN_BUILD_JOBS:-4}"
if [ -n "${1:-}" ]; then
  ctest --test-dir "$BUILD" --output-on-failure -R "$1"
else
  ctest --test-dir "$BUILD" --output-on-failure
fi
