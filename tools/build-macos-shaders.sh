#!/bin/zsh
# Builds the Metal renderer's shaders from your own game files, for a Mac build made from source; the signed download
# already has them. Without them the game draws with the emulated renderer instead.
#
# usage: build-macos-shaders.sh <game folder> <output folder> <game binary>
#   game folder    the installed game files, the folder holding Default.xex
#   output folder  where the game reads them: <data folder>/user_data/native-shaders
#   game binary    the built game (out/build/mac-arm64-release/crackdown, or the app's Contents/MacOS/crackdown), run
#                  once for a few seconds: the HUD's and menus' shaders are in the executable, which is compressed on
#                  disk, so its image is copied from memory (CRACKDOWN_DUMP_IMAGE)
#
# Needs Xcode's Metal compiler (xcrun metal) and git. It clones XenosRecomp (MIT, github.com/hedge-dev/XenosRecomp) at
# the commit this project pins, builds its translator with tools/shader-trial/xenosrecomp-crackdown.patch, translates
# every shader the game ships and compiles them. Work files go to out/shader-work. Translated shaders come from the
# game, so they stay out of the repository.
set -eu
R=${0:A:h:h}
T="$R/tools/shader-trial"
GAME=${1:?the game folder, holding Default.xex}
OUT=${2:?the output folder, <data folder>/user_data/native-shaders}
BINARY=${3:?the built game binary}
WORK="$R/out/shader-work"
command -v xcrun > /dev/null && xcrun -sdk macosx -f metal > /dev/null 2>&1 || {
  echo "Apple's Metal compiler is missing: install Xcode, then its Metal toolchain (Xcode > Settings > Components)." >&2
  exit 1
}
[ -f "$GAME/Default.xex" ] || { echo "No Default.xex in $GAME." >&2; exit 1; }
mkdir -p "$WORK" "$OUT"

# The executable's image, from a short run of the game.
IMAGE="$WORK/image.bin"
if [ ! -s "$IMAGE" ]; then
  echo "Starting the game for a few seconds to copy its executable's image..."
  CRACKDOWN_DUMP_IMAGE="$IMAGE" "$BINARY" --crackdown_skip_launcher=true "--game_data_root=$GAME" > /dev/null 2>&1 &
  game=$!
  for _ in {1..120}; do
    [ -s "$IMAGE" ] && break
    sleep 1
  done
  sleep 2
  kill $game 2> /dev/null || true
  wait $game 2> /dev/null || true
  [ -s "$IMAGE" ] || { echo "The game didn't write its image; check it starts on its own." >&2; exit 1; }
fi

echo "Extracting the game's shaders..."
rm -rf "$WORK/extracted"
python3 "$T/extract.py" "$GAME" "$WORK/extracted" "$IMAGE"

echo "Building XenosRecomp and translating..."
if [ ! -d "$WORK/XenosRecomp/.git" ]; then
  git clone -q https://github.com/hedge-dev/XenosRecomp.git "$WORK/XenosRecomp"
fi
# The pinned commit is pull request 22's.
git -C "$WORK/XenosRecomp" cat-file -e 4906992 2> /dev/null || git -C "$WORK/XenosRecomp" fetch -q origin pull/22/head
cp "$T/run-trial.sh" "$T/compile-all.sh" "$T/trial_main.cpp" "$T/xenosrecomp-crackdown.patch" "$WORK/"
zsh "$WORK/run-trial.sh"

echo "Compiling for Metal..."
python3 "$T/pull_vertices.py" "$WORK/extracted" "$WORK/translated" "$OUT"
python3 "$T/build_pixels.py" "$WORK/translated" "$OUT"
echo "Done: $OUT. The game uses them the next time it starts."
