#!/bin/zsh
# Rebuilds the patched SDK libraries after a change to work/rexglue-sdk, and stages them: into the installed SDK the game
# builds against, and beside the game in the app. Then builds and stages the game too. The SDK source has every patch in
# patches/rexglue-sdk-macos applied, so an SDK change gets its own patch file there first. Refuses while the game is open.
# Run from anywhere.
set -eu
W=${0:A:h:h:h}
R="$W/crackdown"
SOURCE="$W/rexglue-sdk"
SDK="$W/rexglue-mac/mac-arm64"
G="$W/Crackdown Recompiled.app/Contents/MacOS"
if [ -n "$(pgrep -x crackdown || true)" ]; then
  echo "The game is open: close it first. Nothing was changed." >&2
  exit 1
fi
cmake --build "$W/sdk-build-macos" --target rexglue rexgpu-xenos --parallel 6
for name in librexruntime.dylib librexgpu-xenos.dylib; do
  rm -f "$SDK/lib/$name" "$G/$name"
  cp "$SOURCE/out/mac-arm64/$name" "$SDK/lib/$name"
  cp "$SOURCE/out/mac-arm64/$name" "$G/$name"
done
rsync -a --exclude '*.in' "$SOURCE/include/rex/" "$SDK/include/rex/"
# The same fingerprint build-macos.sh keeps, so it sees these patches as applied.
python3 - "$R" "$SOURCE" <<'PY'
import hashlib, pathlib, sys
root, source = map(pathlib.Path, sys.argv[1:])
patches = [p for p in sorted((root / 'patches/rexglue-sdk').glob('*.patch')) if not p.name.endswith('.windows.patch')]
patches += sorted((root / 'patches/rexglue-sdk-macos').glob('*.patch'))
(source / '.crackdown-macos-patches').write_text(hashlib.sha256(b''.join(p.name.encode() + p.read_bytes() for p in patches)).hexdigest() + '\n')
PY
python3 "$R/tools/check-macos-sdk.py" --sdk "$SDK" --record-source "$SOURCE"
cmake --build "$W/game-build-macos" --parallel 6
"$R/tools/stage-macos.sh"
