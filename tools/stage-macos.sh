#!/bin/zsh
# Puts a finished build into the real app: the binary, the keycap sheet, and crackdown.toml with the Mac settings added.
# Refuses while the game is open. Run from anywhere.
set -eu
W=${0:A:h:h:h}
G="$W/Crackdown Recompiled.app/Contents/MacOS"
if [ -n "$(pgrep -x crackdown || true)" ]; then
  echo "The game is open: close it first. Nothing was changed." >&2
  exit 1
fi
rm -f "$G/crackdown"
cp "$W/game-build-macos/crackdown" "$G/crackdown"
cat "$W/crackdown/config/crackdown.toml" "$W/crackdown/config/crackdown-macos.toml" > "$G/crackdown.toml"
cp "$W/crackdown/packaging/glyphs/button_glyphs.dds" "$W/crackdown/packaging/glyphs/button_glyphs.toml" "$G/glyphs/"
python3 "$W/crackdown/tools/check-macos-sdk.py" --sdk "$W/rexglue-mac/mac-arm64" --staged "$G" | tail -1
echo "Staged."
