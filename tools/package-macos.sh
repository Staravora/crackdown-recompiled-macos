#!/bin/zsh
# Makes the Mac release from the staged app: the Metal shaders bundled in, signed with a Developer ID and the hardened
# runtime, notarized, stapled, and zipped as Crackdown-v<version>-macos-arm64.zip with a .sha256 beside it, the names the
# in-app updater looks for. Stage first (tools/stage-macos.sh). Output goes to out/dist-macos.
#
# usage: package-macos.sh [--no-notarize]
# Environment:
#   CRACKDOWN_SIGN_IDENTITY  the signing identity; default: the keychain's only "Developer ID Application" one
#   CRACKDOWN_NOTARY_PROFILE the notarytool keychain profile; default zuhl-notary, shared by every app of the same team.
#                            Made once per Mac with
#                            xcrun notarytool store-credentials zuhl-notary --apple-id <you> --team-id <team>
#                            which asks for an app-specific password and keeps it in the keychain, never in a file.
#   CRACKDOWN_RELEASE_REPOSITORY the GitHub owner/name the release goes to; default Staravora/crackdown-xbox360-recompiled.
#                            The game must be built with -DCRACKDOWN_MAC_UPDATE_REPOSITORY set to it, or the release
#                            would never update itself, so the script refuses otherwise.
#   CRACKDOWN_SHADERS        the native-shaders folder to bundle; default Crackdown Data/user_data/native-shaders
set -eu
W=${0:A:h:h:h}
R="$W/crackdown"
SOURCE_APP="$W/Crackdown Recompiled.app"
NOTARIZE=true
[ "${1:-}" = "--no-notarize" ] && NOTARIZE=false
SHADERS=${CRACKDOWN_SHADERS:-"$W/Crackdown Data/user_data/native-shaders"}
PROFILE=${CRACKDOWN_NOTARY_PROFILE:-zuhl-notary}
REPOSITORY=${CRACKDOWN_RELEASE_REPOSITORY:-Staravora/crackdown-xbox360-recompiled}
IDENTITY=${CRACKDOWN_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)}
[ -n "$IDENTITY" ] || { echo "No Developer ID Application identity in the keychain." >&2; exit 1; }
grep -qaF "$REPOSITORY" "$SOURCE_APP/Contents/MacOS/crackdown" || {
  echo "This build doesn't update from $REPOSITORY. Configure the game with" >&2
  echo "  cmake -S \"$R\" -B \"$W/game-build-macos\" -DCRACKDOWN_MAC_UPDATE_REPOSITORY=$REPOSITORY" >&2
  echo "then rebuild and stage it." >&2
  exit 1
}
[ -f "$SHADERS/manifest.txt" ] && [ -f "$SHADERS/pixel-manifest.txt" ] || { echo "No Metal shaders in $SHADERS." >&2; exit 1; }
if $NOTARIZE && ! xcrun notarytool history --keychain-profile "$PROFILE" > /dev/null 2>&1; then
  echo "No notarytool profile '$PROFILE'. Make it once with:" >&2
  echo "  xcrun notarytool store-credentials $PROFILE --apple-id <your Apple ID> --team-id <your team ID>" >&2
  echo "or run with --no-notarize for a signed build that only this Mac will open without a warning." >&2
  exit 1
fi

VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$SOURCE_APP/Contents/Info.plist")
MAC_BUILD=$(/usr/libexec/PlistBuddy -c "Print CFBundleVersion" "$SOURCE_APP/Contents/Info.plist")
NAME="Crackdown-v$VERSION-macos-arm64"
OUT="$R/out/dist-macos"
APP="$OUT/Crackdown Recompiled.app"
rm -rf "$OUT"
mkdir -p "$OUT"
ditto "$SOURCE_APP" "$APP"

# The tables and the compiled shaders they name. Generated source, logs and shaders left from earlier builds stay behind.
BUNDLED="$APP/Contents/Resources/native-shaders"
mkdir -p "$BUNDLED"
cp "$SHADERS"/*.txt "$BUNDLED/"
{ awk '{print "vs_" $1 ".metallib"}' "$SHADERS/manifest.txt"; awk '{print "ps_" $1 ".metallib"}' "$SHADERS/pixel-manifest.txt"; } |
  sort -u | while read -r shader; do
    [ -f "$SHADERS/$shader" ] && cp "$SHADERS/$shader" "$BUNDLED/"
  done
echo "Bundled $(ls "$BUNDLED" | grep -c metallib) shaders"
rm -rf "$APP/Contents/MacOS/logs"
xattr -cr "$APP"

# Inside out: each library, then the app.
find "$APP/Contents/MacOS" -name '*.dylib' -print0 | while IFS= read -r -d '' library; do
  codesign --force --options runtime --timestamp -s "$IDENTITY" "$library"
done
codesign --force --options runtime --timestamp -s "$IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"
echo "Signed: $IDENTITY"

ZIP="$OUT/$NAME.zip"
if $NOTARIZE; then
  ditto -c -k --keepParent "$APP" "$OUT/notarize.zip"
  xcrun notarytool submit "$OUT/notarize.zip" --keychain-profile "$PROFILE" --wait
  rm -f "$OUT/notarize.zip"
  xcrun stapler staple "$APP"
  spctl -a -t exec -vv "$APP"
fi
ditto -c -k --keepParent "$APP" "$ZIP"
(cd "$OUT" && shasum -a 256 "$NAME.zip" > "$NAME.sha256")
echo "Made $ZIP"
cat "$OUT/$NAME.sha256"
echo "Release tag: v$VERSION+macos.$MAC_BUILD (a full release, not a pre-release, so the in-app updater sees it)"
