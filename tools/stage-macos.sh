#!/bin/zsh
# Puts a finished build into the real app, laid out so it can be signed: programs and libraries in Contents/MacOS,
# everything else in Contents/Resources. The binary, crackdown.toml with the Mac settings added, the keycap sheet, the
# launcher art and fonts, MoltenVK's driver description, the icon and Info.plist. Refuses while the game is open. Run from anywhere.
# It doesn't build: run `cmake --build game-build-macos --target crackdown` from work/ first.
set -eu
W=${0:A:h:h:h}
R="$W/crackdown"
APP="$W/Crackdown Recompiled.app"
G="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"
if [ -n "$(pgrep -x crackdown || true)" ]; then
  echo "The game is open: close it first. Nothing was changed." >&2
  exit 1
fi
mkdir -p "$G/vulkan/lib" "$RES/glyphs" "$RES/assets/launcher" "$RES/vulkan/icd.d" "$RES/fonts"
rm -f "$G/crackdown"
cp "$W/game-build-macos/crackdown" "$G/crackdown"
cat "$R/config/crackdown.toml" "$R/config/crackdown-macos.toml" > "$RES/crackdown.toml"
cp "$R/packaging/glyphs/button_glyphs.dds" "$R/packaging/glyphs/button_glyphs.toml" "$RES/glyphs/"
cp "$R/assets/launcher/header-art.png" "$R/assets/launcher/backdrop-dusk.png" "$RES/assets/launcher/"
# The launcher's skin: Exo 2 and Michroma, with their licences (SIL OFL 1.1).
cp "$R/packaging/fonts/"*.ttf "$R/packaging/fonts/"*-OFL.txt "$RES/fonts/"
# Licences of everything the app ships.
mkdir -p "$RES/licenses"
cp "$R/LICENSE" "$RES/licenses/LICENSE.txt"
cp "$R/packaging/licenses/ReXGlue-LICENSE.txt" "$R/packaging/licenses/THIRD-PARTY-LICENSES.txt" "$R/packaging/licenses/THIRD-PARTY-LICENSES-MACOS.txt" "$RES/licenses/"
# The Vulkan loader finds MoltenVK through this, which the game points it at (UseBundledVulkanDriver).
cat > "$RES/vulkan/icd.d/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../MacOS/vulkan/lib/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON
if [ ! -f "$RES/Crackdown.icns" ]; then
  ICONS=$(mktemp -d)/Crackdown.iconset
  mkdir -p "$ICONS"
  for size in 16 32 128 256 512; do
    sips -z $size $size "$R/packaging/icons/app-icon-512.png" --out "$ICONS/icon_${size}x${size}.png" > /dev/null
    sips -z $((size * 2)) $((size * 2)) "$R/packaging/icons/app-icon-512.png" --out "$ICONS/icon_${size}x${size}@2x.png" > /dev/null
  done
  iconutil -c icns "$ICONS" -o "$RES/Crackdown.icns"
fi
VERSION=$(sed -n 's/^project(crackdown VERSION \([0-9.]*\).*/\1/p' "$R/CMakeLists.txt")
MAC_BUILD=$(sed -n 's/^set(CRACKDOWN_MAC_BUILD \([0-9]*\)).*/\1/p' "$R/CMakeLists.txt")
MINIMUM=$(otool -l "$G/crackdown" | awk '/LC_BUILD_VERSION/ {found = 1} found && /minos/ {print $2; exit}')
sed -e "s/@VERSION@/$VERSION/" -e "s/@MAC_BUILD@/$MAC_BUILD/" -e "s/@MINIMUM_MACOS@/$MINIMUM/" "$R/packaging/macos/Info.plist" > "$APP/Contents/Info.plist"
# Earlier layouts kept data beside the executable, which a signature doesn't allow.
rm -rf "$G/crackdown.toml" "$G/glyphs" "$G/assets" "$G/vulkan/share" "$G/logs"
if ! (cd "$G/vulkan/lib" && shasum -a 256 -c -s "$R/packaging/macos/vulkan-libs.sha256"); then
  echo "Warning: the Vulkan libraries in the app aren't the pinned ones in packaging/macos/vulkan-libs.sha256." >&2
fi
python3 "$R/tools/check-macos-sdk.py" --sdk "$W/rexglue-mac/mac-arm64" --staged "$G" | tail -1
echo "Staged."
